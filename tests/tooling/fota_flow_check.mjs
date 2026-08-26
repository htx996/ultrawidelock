/* SPDX-License-Identifier: ISC */

/*
 * fota_flow_check.mjs -- drive web/flasher/fota.js end to end against a fake
 * board, with no browser and no radio.
 *
 *   node tests/tooling/fota_flow_check.mjs <repo-root>
 *
 * fota_wire_check.mjs proves the BYTES match the Python. This proves the ORDER
 * of operations is right, which is the part a wire check cannot see and the
 * part that decides whether a real update works:
 *
 *   - the board is identified BEFORE anyone is asked to press anything
 *   - a board the index has no entry for is told so, and nothing is sent
 *   - a board already on the latest image is told so, and nothing is sent
 *   - rc=11 (window shut) is waited out rather than treated as a failure
 *   - the upload restarts cleanly at offset 0 across those retries
 *   - reset is sent, the board is re-read, and the new hash is CHECKED
 *
 * The last one is the reason this file exists. `make fota-done` is a manual
 * step on the command line precisely because a delta is cut against the exact
 * bytes on the board; a page that reported success without re-reading the hash
 * would be guessing, and the next update would be built from the wrong base.
 *
 * TWO FAKES AND NOTHING ELSE. A virtual clock, because verifyCdk waits 17 s for
 * MCUboot and a test must not; and a fake GATT characteristic that speaks real
 * SMP -- real 8-byte headers, real CBOR, real indefinite-length maps, the same
 * zcbor shape a Zephyr board emits. fota.js and smp.js are imported unmodified.
 */

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import path from "node:path";

const repoRoot = process.argv[2];
if (!repoRoot) {
  console.error("usage: fota_flow_check.mjs <repo-root>");
  process.exit(2);
}
const flasher = path.join(repoRoot, "web", "flasher");

let failures = 0;
let checks = 0;

function check(name, ok, detail = "") {
  checks += 1;
  if (ok) {
    console.log(`  ok   ${name}`);
  } else {
    failures += 1;
    console.log(`  FAIL ${name}  ${detail}`);
  }
}

/* ---- a virtual clock ------------------------------------------------------ */

/*
 * fota.js sleeps 17 s before its first reconnect and 1.5 s between window
 * retries, and measures both with Date.now(). Real timers would make this suite
 * take a minute to prove something that is pure sequencing, so time is faked --
 * ordering preserved exactly, duration collapsed to nothing.
 */
const realSetTimeout = globalThis.setTimeout;
let now = 0;
let seq = 0;
let timers = [];

globalThis.setTimeout = (fn, ms = 0) => {
  const id = ++seq;
  timers.push({ id, at: now + ms, order: id, fn });
  return id;
};
globalThis.clearTimeout = (id) => {
  timers = timers.filter((t) => t.id !== id);
};
Date.now = () => now;

/** Let every already-resolvable promise settle, without advancing the clock. */
const flush = () => new Promise((r) => setImmediate(r));

/**
 * Run until the work under test finishes, firing timers in time order.
 *
 * Bounded by a step count rather than by wall time: a flow that never settles
 * is a bug to report, not a suite to hang.
 */
async function runUntil(promise, label) {
  let done = false;
  let error = null;
  promise.then(() => { done = true; }, (e) => { done = true; error = e; });

  for (let step = 0; step < 200000 && !done; step++) {
    await flush();
    if (done) break;
    if (!timers.length) {
      await flush();
      if (!done && !timers.length) break;
      continue;
    }
    timers.sort((a, b) => (a.at - b.at) || (a.order - b.order));
    const next = timers.shift();
    now = Math.max(now, next.at);
    next.fn();
  }
  await flush();
  if (error) throw error;
  if (!done) throw new Error(`${label}: never settled`);
}

/* ---- a fake board that speaks SMP ----------------------------------------- */

const smp = await import(pathToFileURL(path.join(flasher, "smp.js")).href);
const serialMod = await import(pathToFileURL(path.join(flasher, "serial.js")).href);

const hex = (u8) => Buffer.from(u8).toString("hex");
const unhex = (s) => new Uint8Array(Buffer.from(s, "hex"));

/* Indefinite-length maps, as zcbor emits them -- Zephyr does not define
 * ZCBOR_CANONICAL, so this is the shape every real reply arrives in. */
function indefMap(pairs) {
  const out = [0xbf];
  for (const [k, v] of pairs) {
    out.push(...smp.cborEncode(k));
    out.push(...(v instanceof Uint8Array && v.raw ? v : smp.cborEncode(v)));
  }
  out.push(0xff);
  return new Uint8Array(out);
}
function raw(bytes) {
  const u = new Uint8Array(bytes);
  u.raw = true;
  return u;
}
function indefArray(items) {
  const out = [0x9f];
  for (const i of items) out.push(...i);
  out.push(0xff);
  return new Uint8Array(out);
}

class FakeBoard {
  constructor(runningHex) {
    this.running = runningHex;
    this.windowOpen = false;
    this.received = [];
    this.total = 0;
    this.resetCount = 0;
    this.uploadCalls = 0;
    this.restarts = 0;
    this.listeners = [];
    this.applyTo = null;
    this.connected = true;
  }

  addEventListener(_type, fn) { this.listeners.push(fn); }
  removeEventListener(_type, fn) { this.listeners = this.listeners.filter((f) => f !== fn); }
  async startNotifications() { return this; }

  _notify(frame) {
    /* Asynchronous, like a real notification: the write resolves first. */
    setTimeout(() => {
      for (const fn of this.listeners) {
        fn({ target: { value: { buffer: frame.buffer.slice(frame.byteOffset,
                                                            frame.byteOffset + frame.byteLength) } } });
      }
    }, 1);
  }

  _reply(seqNo, group, cmdId, body) {
    const frame = new Uint8Array(8 + body.length);
    const dv = new DataView(frame.buffer);
    dv.setUint8(0, 3);            /* OP_WRITE_RSP; the page matches on seq only */
    dv.setUint16(2, body.length);
    dv.setUint16(4, group);
    dv.setUint8(6, seqNo);
    dv.setUint8(7, cmdId);
    frame.set(body, 8);
    this._notify(frame);
  }

  async writeValueWithoutResponse(frame) {
    if (!this.connected) throw new Error("disconnected");
    const dv = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const group = dv.getUint16(4);
    const seqNo = dv.getUint8(6);
    const cmdId = dv.getUint8(7);
    const body = frame.slice(8);
    const req = body.length ? smp.cborDecode(body)[0] : {};

    if (group === 1 && cmdId === 0) {
      /* image list -- never window-gated, which is what lets the page identify
       * a board before asking anyone to press a button. */
      const entry = raw(indefMap([
        ["slot", 0],
        ["version", "0.3.0"],
        ["hash", unhex(this.running)],
        ["bootable", true],
        ["active", true],
        ["confirmed", true],
      ]));
      this._reply(seqNo, group, cmdId,
                  indefMap([["images", raw(indefArray([entry]))]]));
      return;
    }

    if (group === 1 && cmdId === 1) {
      this.uploadCalls += 1;
      if (!this.windowOpen) {
        this._reply(seqNo, group, cmdId, indefMap([["rc", 11]]));
        return;
      }
      if (req.off === 0) {
        this.received = [];
        this.total = req.len;
        this.restarts += 1;
      }
      if (req.off !== this.received.length) {
        /* Resync, exactly as the firmware does: report the real cursor. */
        this._reply(seqNo, group, cmdId, indefMap([["off", this.received.length]]));
        return;
      }
      this.received.push(...req.data);
      this._reply(seqNo, group, cmdId, indefMap([["off", this.received.length]]));
      return;
    }

    if (group === 0 && cmdId === 5) {
      this.resetCount += 1;
      /* A real board reboots without answering; the page tolerates that. It
       * comes back running the new image if the transfer was complete. */
      if (this.applyTo && this.received.length === this.total) {
        this.running = this.applyTo;
      }
      return;
    }

    throw new Error(`fake board got an unexpected command ${group}/${cmdId}`);
  }
}

/*
 * The same board, reached down a wire instead of a radio.
 *
 * WHY IT WRAPS FakeBoard RATHER THAN REIMPLEMENTING IT. The point of the serial
 * path is that everything above the transport is the same code -- same header,
 * same CBOR, same window gate, same upload loop. A second fake board would let
 * those drift apart silently, which is the exact failure this suite exists to
 * catch. So the bytes are framed on the way in, unframed on the way out, and
 * the board in the middle cannot tell which transport it is on. That is the
 * claim under test.
 */
function fakeSerialPort(board, serial) {
  const queue = [];
  let wake = null;
  const framer = new serial.Framer();

  board.addEventListener("characteristicvaluechanged", (ev) => {
    queue.push(serial.encodePacket(new Uint8Array(ev.target.value.buffer)));
    if (wake) { const w = wake; wake = null; w(); }
  });

  const port = {
    closed: false,
    /* requestPort() opens the port before handing it over, exactly as a real
     * one has to be opened before it has a readable or a writable. */
    opened: null,
    async open(options) { port.opened = options; },
    readable: {
      getReader: () => ({
        async read() {
          for (;;) {
            if (queue.length) return { value: queue.shift(), done: false };
            if (port.closed) return { value: undefined, done: true };
            await new Promise((r) => { wake = r; });
          }
        },
        releaseLock() {},
        async cancel() {
          port.closed = true;
          if (wake) { const w = wake; wake = null; w(); }
        },
      }),
    },
    writable: {
      getWriter: () => ({
        async write(bytes) {
          /* Unframe exactly as the firmware would, then hand the board the SMP
           * frame it would have seen over GATT. */
          for (const frame of framer.feed(bytes)) {
            await board.writeValueWithoutResponse(frame);
          }
        },
        releaseLock() {},
      }),
    },
    async close() { port.closed = true; },
  };
  return port;
}

/* ---- a DOM the page can drive --------------------------------------------- */

function fakeElement(id) {
  return {
    id,
    dataset: {},
    style: {},
    hidden: true,
    disabled: false,
    value: "",
    textContent: "",
    innerHTML: "",
    children: [],
    firstElementChild: null,
    handlers: {},
    append(...kids) { this.children.push(...kids); },
    setAttribute(k, v) { this.dataset[`attr_${k}`] = v; },
    addEventListener(type, fn) { this.handlers[type] = fn; },
    /* What the status region actually said, in order. The page's whole job is
     * to tell someone the truth about their board, so the transcript IS the
     * thing under test. */
    get text() { return this.children.map((c) => c.textContent).join(" | "); },
  };
}

let dom;
let statusLog;

function installDom() {
  const out = fakeElement("fota-out");
  const bar = fakeElement("fota-bar");
  bar.firstElementChild = fakeElement("fill");
  dom = {
    "fota-out": out,
    "fota-bar": bar,
    "fota-go": fakeElement("fota-go"),
    "fota-target": fakeElement("fota-target"),
    "fota-how": fakeElement("fota-how"),
    "fota-how-row": fakeElement("fota-how-row"),
  };
  statusLog = [];

  /* Record every status the page sets, then let it clear the region as it
   * normally would. */
  const realAppend = out.append.bind(out);
  out.append = (...kids) => {
    realAppend(...kids);
    statusLog.push({ kind: out.dataset.kind, text: kids.map((k) => k.textContent).join(" ") });
  };

  globalThis.document = {
    getElementById: (id) => dom[id] || null,
    createElement: () => fakeElement("new"),
  };
  globalThis.window = { isSecureContext: true };
  /* node already provides globalThis.crypto, and it is getter-only. uwldfu.js
   * is the only module that wants it, and this suite drives the SMP path. */
}

const said = (needle) => statusLog.some((s) => s.text.includes(needle));
const lastKind = () => (statusLog.length ? statusLog[statusLog.length - 1].kind : null);

/* ---- the fixtures --------------------------------------------------------- */

const OLD = "aa".repeat(32);
const NEW = "cc".repeat(32);
const STRANGER = "77".repeat(32);
const DELTA = new Uint8Array(11264).fill(7);

const INDEX = {
  schema: 1,
  targets: {
    dwm3001cdk: {
      transport: "smp",
      name: "DWM3001CDK",
      dir: "dwm3001cdk",
      latest: { version: "0.3.1", sha256: NEW },
      updates: {
        [OLD]: {
          file: "ultrawidelock-aaaaaaaa-to-cccccccc.bin",
          size: DELTA.length,
          to: NEW,
          version: "0.3.1",
        },
      },
    },
  },
};

function installFetch() {
  globalThis.fetch = async (url) => {
    if (url.includes("ota-index.json")) {
      return { ok: true, status: 200, json: async () => INDEX };
    }
    if (url.includes(".bin")) {
      return { ok: true, status: 200, arrayBuffer: async () => DELTA.buffer.slice(0) };
    }
    return { ok: false, status: 404 };
  };
}

/**
 * Load a fresh copy of fota.js. It calls init() at import, so the cache is
 * busted per scenario to get a clean run rather than a re-entered one.
 */
async function loadPage(board, how = "ble", capture = (p) => p) {
  /* defineProperty, not assignment: node ships its own getter-only
   * globalThis.navigator, and a plain assignment throws. */
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    writable: true,
    value: {
    serial: how === "serial" ? {
      requestPort: async () => capture(fakeSerialPort(board, serialMod)),
    } : undefined,
    bluetooth: {
      requestDevice: async () => ({
        name: "ultrawidelock",
        gatt: {
          connect: async () => ({
            getPrimaryService: async () => ({ getCharacteristic: async () => board }),
          }),
          disconnect: () => { board.connected = false; },
        },
      }),
    },
    },
  });
  const url = pathToFileURL(path.join(flasher, "fota.js")).href + `?v=${++seq}`;
  await import(url);
  await flush();
  if (how === "serial") dom["fota-how"].value = "serial";
}

/** Click the page's button and run the whole flow to completion. */
async function clickAndRun(label) {
  const handler = dom["fota-go"].handlers.click;
  if (!handler) throw new Error("the page wired no click handler");
  await runUntil(handler(), label);
}

/* ---- scenario 1: an update applies, is installed, and is verified ---------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  await loadPage(board);

  check("dropdown built from the index", dom["fota-target"].children.length === 1);
  dom["fota-target"].value = "dwm3001cdk";

  /* The window is shut. The page must wait it out, not fail -- and the moment
   * it asks, someone walks over and presses SW2. */
  let pressedAfter = -1;
  const out = dom["fota-out"];
  const before = out.append;
  out.append = (...kids) => {
    before(...kids);
    if (out.dataset.kind === "prompt" && !board.windowOpen) {
      board.windowOpen = true;
      pressedAfter = board.uploadCalls;
    }
  };

  await clickAndRun("scenario 1");

  check("identified the board before prompting",
        said("Update available"), statusLog.map((s) => s.text).join(" / "));
  check("asked for SW2 only after the board refused", pressedAfter >= 1);
  check("waited the window out instead of failing", board.windowOpen);
  check("the whole delta arrived", board.received.length === DELTA.length);
  check("the bytes are the ones that were served",
        Buffer.from(board.received).equals(Buffer.from(DELTA)));
  check("the transfer restarted cleanly at offset 0", board.restarts === 1);
  check("the board was reset", board.resetCount === 1);
  check("the board was re-read after the reset", board.running === NEW);
  check("reported success", said("Updated to 0.3.1"));
  check("final state is ok", lastKind() === "ok", `kind=${lastKind()}`);
}

/* ---- scenario 2: a board running something we ship no update from ---------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(STRANGER);
  board.windowOpen = true;      /* even so, nothing may be sent */
  await loadPage(board);
  dom["fota-target"].value = "dwm3001cdk";

  await clickAndRun("scenario 2");

  check("unknown board: says no update applies", said("No over-the-air update applies"));
  check("unknown board: explains the single slot", said("one MCUboot slot"));
  check("unknown board: points at the J-Link", said("J-Link"));
  check("unknown board: sent nothing", board.received.length === 0);
  check("unknown board: did not reset", board.resetCount === 0);
  check("unknown board: warns rather than errors", lastKind() === "warn", `kind=${lastKind()}`);
}

/* ---- scenario 3: a board already on the latest image ----------------------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(NEW);
  board.windowOpen = true;
  await loadPage(board);
  dom["fota-target"].value = "dwm3001cdk";

  await clickAndRun("scenario 3");

  check("up-to-date board: says so", said("Already up to date"));
  check("up-to-date board: names the version it is on", said("0.3.1"));
  check("up-to-date board: sent nothing", board.received.length === 0);
  check("up-to-date board: did not reset", board.resetCount === 0);
  check("up-to-date board: reports ok", lastKind() === "ok", `kind=${lastKind()}`);
}

/* ---- scenario 3b: latest, AND an entry claiming to update it ---------------- */

/*
 * Regression. A build that does not change between two releases produces a
 * delta whose starting image is also its destination, and a published set can
 * contain one. Looking the running hash up in `updates` before checking whether
 * it is already the latest finds that entry and offers the board an update to
 * what it is already running -- and the verify afterwards PASSES, because the
 * hash it waits for is the hash that never moved. Success, reported for
 * nothing, with a flash erase and a reboot spent on it.
 *
 * Seen for real on 2026-08-27: a `make fota` after an edit outside the firmware
 * sources rebuilt a byte-identical image and produced a 7,391 B self-patch.
 */
{
  installDom();
  const selfPatch = {
    schema: 1,
    targets: {
      dwm3001cdk: {
        transport: "smp",
        name: "DWM3001CDK",
        dir: "dwm3001cdk",
        latest: { version: "0.3.1", sha256: NEW },
        updates: {
          [NEW]: { file: "self.bin", size: DELTA.length, to: NEW, version: "0.3.1" },
        },
      },
    },
  };
  globalThis.fetch = async (url) => {
    if (url.includes("ota-index.json")) {
      return { ok: true, status: 200, json: async () => selfPatch };
    }
    return { ok: true, status: 200, arrayBuffer: async () => DELTA.buffer.slice(0) };
  };

  const board = new FakeBoard(NEW);
  board.windowOpen = true;
  await loadPage(board);
  dom["fota-target"].value = "dwm3001cdk";

  await clickAndRun("scenario 3b");

  check("self-patch: reports up to date, not an update", said("Already up to date"));
  check("self-patch: sent nothing", board.received.length === 0);
  check("self-patch: did not reset", board.resetCount === 0);
}

/* ---- scenario 4: the update does not take ---------------------------------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.windowOpen = true;
  board.applyTo = null;         /* MCUboot declines it; the old image boots */
  await loadPage(board);
  dom["fota-target"].value = "dwm3001cdk";

  await clickAndRun("scenario 4");

  check("failed apply: does not claim success", !said("Updated to"));
  check("failed apply: says the board came back on something else",
        said("came back running something else"));
  check("failed apply: says it is not bricked", said("not bricked"));
  check("failed apply: warns", lastKind() === "warn", `kind=${lastKind()}`);
}

/* ---- scenario 5: the same update, down a cable ------------------------------
 *
 * The claim being tested is that the transport is the ONLY difference. Same
 * index, same delta, same window gate, same verification -- so this scenario
 * asserts the same facts as scenario 1 and additionally that the bytes really
 * did go through the serial framing on the way, rather than the page quietly
 * falling back to Bluetooth.
 */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  let openedPort = null;
  const capture = (port) => { openedPort = port; return port; };
  await loadPage(board, "serial", capture);

  check("serial: the transport picker is offered when both APIs exist",
        dom["fota-how"].children.length === 2,
        `${dom["fota-how"].children.length} options`);
  check("serial: the picker is shown rather than hidden",
        dom["fota-how-row"].hidden === false);

  dom["fota-target"].value = "dwm3001cdk";

  let pressedAfter = -1;
  const out = dom["fota-out"];
  const before = out.append;
  out.append = (...kids) => {
    before(...kids);
    if (out.dataset.kind === "prompt" && !board.windowOpen) {
      board.windowOpen = true;
      pressedAfter = board.uploadCalls;
    }
  };

  await clickAndRun("scenario 5");

  check("serial: asked for the port, not the radio", said("serial port"),
        statusLog.map((s) => s.text).join(" / "));
  check("serial: identified the board before prompting", said("Update available"));
  check("serial: asked for SW2 only after the board refused", pressedAfter >= 1);
  check("serial: waited the window out instead of failing", board.windowOpen);
  check("serial: the whole delta arrived", board.received.length === DELTA.length);
  check("serial: the bytes are the ones that were served",
        Buffer.from(board.received).equals(Buffer.from(DELTA)));
  check("serial: the transfer restarted cleanly at offset 0", board.restarts === 1);
  check("serial: the board was reset", board.resetCount === 1);
  check("serial: the board was re-read after the reset", board.running === NEW);
  check("serial: reported success", said("Updated to 0.3.1"));
  check("serial: final state is ok", lastKind() === "ok",
        `kind=${lastKind()} :: ${statusLog.map((x) => x.text).join(" / ")}`);

  /* The cable is worth having only if it is actually faster. Same delta, so
   * fewer requests is the whole benefit, and it is a number rather than a
   * hope: 11,264 bytes at 384 a time against 105 a time. */
  /* MCUboot's serial recovery and the application both run uart0 at 115200,
   * and a port opened at the wrong rate fails as silence rather than as an
   * error -- the worst shape a bug can take on this path. */
  check("serial: opened the port at 115200",
        openedPort && openedPort.opened && openedPort.opened.baudRate === 115200,
        JSON.stringify(openedPort && openedPort.opened));
  check("serial: the port was closed when the page let go", openedPort.closed);

  const overBle = Math.ceil(DELTA.length / smp.BLE_CHUNK);
  const overSerial = Math.ceil(DELTA.length / serialMod.APP_CHUNK);
  check("serial: sent the delta in fewer requests than Bluetooth would",
        overSerial < overBle, `${overSerial} vs ${overBle}`);
}

/* ---- verdict --------------------------------------------------------------- */

globalThis.setTimeout = realSetTimeout;

if (failures) {
  console.log(`\n  ${failures} of ${checks} checks FAILED\n`);
  process.exit(1);
}
console.log(`\n  the page's flow holds against a fake board`);
console.log(`RESULT: PASS (${checks} checks)\n`);
