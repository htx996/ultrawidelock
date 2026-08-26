/* SPDX-License-Identifier: ISC */

/*
 * uwldfu.js -- the native framed DFU protocol over Web Bluetooth.
 *
 * A port of scripts/ultrawidelock_push.py. Same service, same characteristic,
 * same little-endian frames, same error table. The firmware half already
 * exists on two ports -- ports/zephyr/dfu/dfu_ble_zephyr.c and
 * ports/freertos-nrf52833/dfu/dfu_ble_freertos.c -- and the ESP32 port speaks
 * it too, which is the point: one wire protocol, one page, two chips.
 *
 * The CDK is served by smp.js instead, because a released CDK already speaks
 * mcumgr and a phone can drive that. This file is for the ESP32, where the
 * payload is a whole application image rather than an 11 KB delta.
 *
 * THE CHUNK SIZE IS PROBED, NOT ASSUMED, and unlike smp.js that is worth the
 * complexity here. Web Bluetooth will not report the ATT MTU, and a whole
 * ESP32 image is ~2 MB: at 105 bytes a frame that is roughly twice the
 * transfer time of 244. DATA frames carry their own offset and the receiver is
 * idempotent on them, so a write that the link rejects for being too long
 * costs nothing but the attempt -- which makes probing downward safe in a way
 * it would not be on a stream protocol.
 *
 * Expect minutes, not seconds. The protocol is strictly lock-step -- one write,
 * one notification, then the next -- so throughput is bounded by the connection
 * interval rather than by the radio, and 2 MB is roughly eight thousand round
 * trips. That is the price of not needing Wi-Fi, a Matter fabric or a cable.
 */

export const DFU_SVC_UUID = "d3b5a140-9e23-4b3a-8be4-6b1ee5f980a3";
export const DFU_CHR_UUID = "d3b5a141-9e23-4b3a-8be4-6b1ee5f980a3";

const OP_BEGIN = 0x11, OP_DATA = 0x12, OP_COMMIT = 0x13, OP_ABORT = 0x14;
const RSP_OK = 0x81, RSP_ERR = 0x82;

/* modules/ultrawidelock_dfu/include/ultrawidelock_dfu_rx.h, enum
 * ultrawidelock_dfu_err. Deliberately coarse on the wire; the prose is ours. */
const ERRORS = {
  1: "no update window is open -- double-click the board's button to open one",
  2: "out of sequence",
  3: "too large for the update partition",
  4: "the signature did not verify -- this image was not signed for this board",
  5: "length or CRC disagreed at commit",
  6: "a flash write or erase failed",
  7: "malformed frame",
  8: "another update is already in progress",
};

/*
 * Candidate payload sizes, largest first.
 *
 * THE CEILING IS THE FIRMWARE, NOT THE LINK. Every port flattens an incoming
 * write into a 256-byte frame buffer (DFU_MTU in dfu_ble_esp32.c,
 * dfu_ble_zephyr.c and dfu_ble_freertos.c) and answers anything longer with
 * ATT_ERR_INVALID_ATTR_VALUE_LEN. With the 9-byte DATA preamble that leaves 247
 * payload bytes, so 244 is the largest candidate worth trying -- a bigger one
 * could only ever cost a round trip and a refusal, on every single transfer.
 *
 * Below that: 180 is the Python script's default, and 20 is the floor every ATT
 * implementation must support (a 23-byte MTU), so the last candidate cannot
 * fail for being too long.
 */
const CHUNK_CANDIDATES = [244, 180, 128, 64, 20];

/** Bytes of frame ahead of the payload in a DATA frame: opcode + id + offset. */
const DATA_PREAMBLE = 9;

/* ---- frames ---------------------------------------------------------------
 *
 * Exported as free functions rather than kept private to DfuSession so that
 * tests/tooling/fota_wire_check.sh can compare them against
 * scripts/ultrawidelock_push.py's struct.pack() without needing a radio, a
 * characteristic, or a mock of either. The bytes are the whole contract with
 * the firmware; anything that cannot be checked without hardware will not be.
 */

/** BEGIN: `<BII` -- opcode, transfer id, total length. */
export function beginFrame(transferId, total) {
  const frame = new Uint8Array(9);
  const dv = new DataView(frame.buffer);
  dv.setUint8(0, OP_BEGIN);
  dv.setUint32(1, transferId, true);
  dv.setUint32(5, total, true);
  return frame;
}

/** DATA: `<BII` + payload -- opcode, transfer id, offset, bytes. */
export function dataFrame(transferId, offset, payload) {
  const frame = new Uint8Array(DATA_PREAMBLE + payload.length);
  const dv = new DataView(frame.buffer);
  dv.setUint8(0, OP_DATA);
  dv.setUint32(1, transferId, true);
  dv.setUint32(5, offset, true);
  frame.set(payload, DATA_PREAMBLE);
  return frame;
}

/** COMMIT: `<BI` -- opcode, transfer id. */
export function commitFrame(transferId) {
  const frame = new Uint8Array(5);
  const dv = new DataView(frame.buffer);
  dv.setUint8(0, OP_COMMIT);
  dv.setUint32(1, transferId, true);
  return frame;
}

/** ABORT: `<BI` -- opcode, transfer id. */
export function abortFrame(transferId) {
  const frame = new Uint8Array(5);
  const dv = new DataView(frame.buffer);
  dv.setUint8(0, OP_ABORT);
  dv.setUint32(1, transferId, true);
  return frame;
}

/**
 * Decode a reply frame.
 *
 * @returns {{ok: true, transferId: number, offset: number}
 *          | {ok: false, code: number}}
 */
export function parseReply(rsp) {
  if (!rsp.length) throw new DfuError("empty reply");
  if (rsp[0] === RSP_ERR) {
    return { ok: false, code: rsp.length > 1 ? rsp[1] : 0 };
  }
  if (rsp[0] !== RSP_OK) throw new DfuError(`unexpected reply 0x${rsp[0].toString(16)}`);
  if (rsp.length !== 9) throw new DfuError(`malformed OK reply (${rsp.length} bytes)`);
  const dv = new DataView(rsp.buffer, rsp.byteOffset, rsp.byteLength);
  return { ok: true, transferId: dv.getUint32(1, true), offset: dv.getUint32(5, true) };
}

/** Human prose for an error code, for callers that have one in hand. */
export function errorText(code) {
  return ERRORS[code] || `the board refused it (error ${code})`;
}

export class DfuError extends Error {
  constructor(message, code) {
    super(message);
    this.name = "DfuError";
    this.code = code;
  }
}

/** Raised by begin() while the board is still refusing for want of a window. */
export class WindowClosed extends DfuError {
  constructor() {
    super(ERRORS[1], 1);
    this.name = "WindowClosed";
  }
}

/** One update conversation: write a frame, wait for its reply. */
export class DfuSession {
  /** @param {BluetoothRemoteGATTCharacteristic} chr the DFU characteristic, notifying. */
  constructor(chr) {
    this.chr = chr;
    this.replies = [];
    this.waiters = [];
    /* Never zero: the receiver treats 0 as "no transfer", so a zero id would
     * be indistinguishable from an idle board. Matches secrets.randbits(32) or 1
     * in ultrawidelock_push.py. */
    this.transferId = crypto.getRandomValues(new Uint32Array(1))[0] || 1;
    this.chunk = 0;
    this._onValue = (ev) => {
      const frame = new Uint8Array(ev.target.value.buffer);
      const waiter = this.waiters.shift();
      if (waiter) waiter(frame); else this.replies.push(frame);
    };
    chr.addEventListener("characteristicvaluechanged", this._onValue);
  }

  detach() {
    this.chr.removeEventListener("characteristicvaluechanged", this._onValue);
  }

  _nextReply(timeoutMs) {
    const queued = this.replies.shift();
    if (queued) return Promise.resolve(queued);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const at = this.waiters.indexOf(fulfil);
        if (at >= 0) this.waiters.splice(at, 1);
        reject(new DfuError("the board stopped answering"));
      }, timeoutMs);
      const fulfil = (frame) => { clearTimeout(timer); resolve(frame); };
      this.waiters.push(fulfil);
    });
  }

  /**
   * Send one frame, return the board's next expected offset.
   *
   * @param {Uint8Array} frame
   * @param {object} opts
   * @param {number[]} opts.tolerate error codes to return as a negative number
   *                                 rather than throw on
   * @param {number} opts.expectedOffset  drop OK replies that do not carry it
   */
  async call(frame, { tolerate = [], expectedOffset = null, timeoutMs = 20000 } = {}) {
    await this.chr.writeValueWithResponse(frame);

    for (;;) {
      const reply = parseReply(await this._nextReply(timeoutMs));

      if (!reply.ok) {
        if (tolerate.includes(reply.code)) return -reply.code;
        throw new DfuError(errorText(reply.code), reply.code);
      }
      const { transferId, offset: nextOffset } = reply;

      /* A notification whose first delivery timed out can arrive behind its
       * retry and poison the following DATA call. The transfer id rejects
       * another uploader; the expected cursor rejects this transfer's late
       * reply without retransmitting. */
      if (transferId !== this.transferId) continue;
      if (expectedOffset !== null && nextOffset !== expectedOffset) continue;
      return nextOffset;
    }
  }

  /** BEGIN. Throws WindowClosed while the board's update window is shut. */
  async begin(total) {
    const got = await this.call(beginFrame(this.transferId, total),
                                { tolerate: [1], expectedOffset: 0 });
    if (got < 0) throw new WindowClosed();
  }

  /**
   * Keep asking to BEGIN until someone opens the update window.
   *
   * The board refuses everything until the button is held, so a push that
   * started first would otherwise just fail. Asking repeatedly costs the board
   * a comparison and a two-byte notification: no flash, no state.
   *
   * @param {number} total          bytes about to be sent
   * @param {number} timeoutMs      give up after this long
   * @param {() => void} onWaiting  called once, when the first refusal lands
   */
  async waitForWindow(total, timeoutMs, onWaiting) {
    const deadline = Date.now() + timeoutMs;
    let prompted = false;
    for (;;) {
      try {
        await this.begin(total);
        return;
      } catch (err) {
        if (!(err instanceof WindowClosed)) throw err;
        if (!prompted && onWaiting) { onWaiting(); prompted = true; }
        if (Date.now() > deadline) {
          throw new DfuError("no update window was opened in time");
        }
        await new Promise((r) => setTimeout(r, 1500));
      }
    }
  }

  /**
   * Find the largest DATA frame this link will carry, by sending the first one.
   *
   * The frame written here is a real DATA frame at offset 0, not a probe: a
   * rejected write never reached the board, and an accepted one is progress we
   * keep. Returns the payload size that worked.
   */
  async _probeChunk(blob) {
    for (const candidate of CHUNK_CANDIDATES) {
      const payload = blob.slice(0, Math.min(candidate, blob.length));
      const frame = this._dataFrame(0, payload);
      try {
        const next = await this.call(frame, { expectedOffset: payload.length });
        this.chunk = payload.length;
        return next;
      } catch (err) {
        /* A DfuError is the board talking -- it got the frame and disliked it,
         * so a smaller one will not help. Only the browser refusing to write
         * (NotSupportedError, or the platform's own length check) means "too
         * long for this link". */
        if (err instanceof DfuError) throw err;
        if (candidate === CHUNK_CANDIDATES[CHUNK_CANDIDATES.length - 1]) throw err;
      }
    }
    throw new DfuError("could not find a frame size this link accepts");
  }

  _dataFrame(offset, payload) {
    return dataFrame(this.transferId, offset, payload);
  }

  /**
   * Send the whole image.
   *
   * @param {Uint8Array} blob
   * @param {(sent: number, total: number) => void} onProgress
   */
  async send(blob, onProgress) {
    let sent = await this._probeChunk(blob);
    if (onProgress) onProgress(sent, blob.length);

    while (sent < blob.length) {
      const payload = blob.slice(sent, sent + this.chunk);
      const expected = sent + payload.length;
      const got = await this.call(this._dataFrame(sent, payload), { expectedOffset: expected });
      if (got !== expected) {
        throw new DfuError(`the board asked for offset ${got} after ${expected} B were sent`);
      }
      sent = got;
      if (onProgress) onProgress(sent, blob.length);
    }
  }

  /** COMMIT. The board reboots on success, so the disconnect that follows is
   *  the expected ending rather than a failure. */
  async commit(total) {
    await this.call(commitFrame(this.transferId), { expectedOffset: total });
  }

  /** ABORT. Best effort: a board that has already gone is not an error here. */
  async abort() {
    try {
      await this.call(abortFrame(this.transferId), { timeoutMs: 3000 });
    } catch { /* nothing useful to do */ }
  }
}

/*
 * What an ESP32 lock actually puts in its advertisement, which is not what you
 * would guess and is why the filters below are not just a name:
 *
 *   commissioned, with an Aliro reader key provisioned
 *       26 bytes of 0xFFF2 service data and NO LOCAL NAME AT ALL
 *   commissioned, no reader key yet
 *       the 0xFFF2 UUID plus the GAP name, which CHIP owns and sets to
 *       "MATTER-<discriminator>" -- never "ultrawidelock"
 *   still commissionable
 *       Matter's own 0xFFF6 advertisement
 *
 * A name filter therefore finds a commissioned lock never, and an
 * uncommissioned one under a name nobody would think to type. The credential
 * service UUID is the one thing present in every state, so it leads.
 *
 * Filters are OR'd, so the name entries stay as a fallback for a board whose
 * advertisement is shaped differently -- the DWM3001CDK's is.
 */
const CRED_SVC_UUID = 0xfff2;
const MATTER_SVC_UUID = 0xfff6;

/**
 * Put up the browser's device chooser and connect.
 *
 * The DFU service itself is NOT advertised -- the advertising set belongs to
 * Matter and then to the credential reader, and both are full -- so it can
 * never be a filter. It is declared optional instead, which is what permits
 * getPrimaryService() to reach it once connected.
 */
/* "ultrawide", not "ultrawidelock": the advertised local name is shortened when
 * service data fills the 31-byte advertisement, and namePrefix matches only
 * from the start of what was actually emitted. See SCAN_NAME in smp.js. */
export async function connect(name = "ultrawide") {
  if (!navigator.bluetooth) throw new DfuError("this browser has no Web Bluetooth");
  const device = await navigator.bluetooth.requestDevice({
    filters: [
      { services: [CRED_SVC_UUID] },
      { services: [MATTER_SVC_UUID] },
      { namePrefix: name },
      { namePrefix: "MATTER-" },
    ],
    optionalServices: [DFU_SVC_UUID],
  });
  return { device, session: await attach(device) };
}

/** Connect (or reconnect) to a device we already hold, and start notifications. */
export async function attach(device) {
  const server = await device.gatt.connect();
  const svc = await server.getPrimaryService(DFU_SVC_UUID);
  const chr = await svc.getCharacteristic(DFU_CHR_UUID);
  await chr.startNotifications();
  return new DfuSession(chr);
}
