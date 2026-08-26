/* SPDX-License-Identifier: ISC */

/*
 * serial_frame_check.mjs -- web/flasher/serial.js against stdlib-built vectors.
 *
 * Three things get asserted here, and they are not the same kind of claim:
 *
 *   1. THE BYTES. Every frame serial.js emits, compared to what
 *      serial_frame_vectors.py builds with binascii.crc_hqx and base64 -- which
 *      that file has ALREADY checked ultrawidelock_smp.py against, byte for
 *      byte. So this is the third leg of a three-way agreement: the standard
 *      library, the CLI client that gets pointed at a real board, and the
 *      browser. Independent implementations, so it catches the wrong CRC
 *      variant, a flipped byte order, or base64 padding that drifts.
 *   2. THE ROUND TRIP, fed one byte at a time. A serial port delivers whatever
 *      it has when it has it, so a reassembler that only works on whole frames
 *      works on the bench and fails on the board. One byte at a time is the
 *      worst case and therefore the only honest one.
 *   3. WHAT HAPPENS TO RUBBISH. uart0 carries MCUboot's banner and anything
 *      the previous session left half-written. A framer that wedges on a line
 *      it does not recognise would leave the page hanging on a board that is
 *      answering perfectly.
 *
 * What none of it proves is that a DWM3001CDK accepts these bytes -- no test on
 * a machine with no board attached can prove that. It proves the encoder and
 * the decoder agree with an outside authority on the parts that have one.
 *
 *   node tests/tooling/serial_frame_check.mjs <vectors.json> <repo-root>
 */

import { readFileSync } from "node:fs";

const [vectorsPath, root] = process.argv.slice(2);
if (!vectorsPath || !root) {
  console.error("usage: serial_frame_check.mjs <vectors.json> <repo-root>");
  process.exit(2);
}

const serial = await import(`${root}/web/flasher/serial.js`);
const V = JSON.parse(readFileSync(vectorsPath, "utf8"));

let pass = 0;
let fail = 0;

function ok(name, cond, detail = "") {
  if (cond) { pass += 1; console.log(`  ok   ${name}`); }
  else { fail += 1; console.log(`  FAIL ${name}${detail ? `  ${detail}` : ""}`); }
}

const hex = (bytes) => Buffer.from(bytes).toString("hex");
const unhex = (s) => Uint8Array.from(Buffer.from(s, "hex"));

/* ---- 1. the constants the C states outright ------------------------------- */

ok("MAX_FRAME matches serial.h:28", serial.MAX_FRAME === V.max_frame,
   `${serial.MAX_FRAME} vs ${V.max_frame}`);
ok("first frame carries ((127-3)>>2)*3", serial.FIRST_INPUT === V.first_input,
   `${serial.FIRST_INPUT} vs ${V.first_input}`);
ok("later frames carry three fewer", serial.NEXT_INPUT === V.next_input,
   `${serial.NEXT_INPUT} vs ${V.next_input}`);

/* ---- 2. the CRC variant --------------------------------------------------- */

ok("crc16 is CRC-16/XMODEM (check value 0x31C3)",
   serial.crc16(new TextEncoder().encode("123456789")) === V.crc_check_value,
   `got 0x${serial.crc16(new TextEncoder().encode("123456789")).toString(16)}`);
ok("crc16 of nothing is zero", serial.crc16(new Uint8Array(0)) === 0);

/* ---- 3. the frames, byte for byte ----------------------------------------- */

for (const v of V.vectors) {
  const body = unhex(v.body);

  ok(`crc agrees with binascii.crc_hqx · ${v.name}`,
     serial.crc16(body) === v.crc,
     `0x${serial.crc16(body).toString(16)} vs 0x${v.crc.toString(16)}`);

  const wire = serial.encodePacket(body);
  ok(`frames match the stdlib build · ${v.name}`,
     hex(wire) === v.wire,
     hex(wire) === v.wire ? "" : `\n       got ${hex(wire)}\n       want ${v.wire}`);

  /* No line, markers and newline included, may exceed the one stated limit. */
  let longest = 0;
  let start = 0;
  for (let i = 0; i < wire.length; i++) {
    if (wire[i] === 0x0a) { longest = Math.max(longest, i - start + 1); start = i + 1; }
  }
  ok(`no frame over ${V.max_frame} bytes · ${v.name}`, longest <= V.max_frame,
     `longest ${longest}`);

  /* One byte at a time: what a serial port actually does. */
  const framer = new serial.Framer();
  const out = [];
  for (const b of wire) out.push(...framer.feed(Uint8Array.of(b)));
  ok(`round trip, one byte at a time · ${v.name}`,
     out.length === 1 && hex(out[0]) === v.body,
     `${out.length} packets`);

  /* And all at once, which is the other thing a port does. */
  const bulk = new serial.Framer().feed(wire);
  ok(`round trip, one read · ${v.name}`,
     bulk.length === 1 && hex(bulk[0]) === v.body);
}

/* ---- 4. rubbish on the wire ----------------------------------------------- */

const good = serial.encodePacket(Uint8Array.of(1, 2, 3, 4, 5, 6, 7, 8));

{
  /* MCUboot prints this before it starts listening. It must not become a
   * packet, and it must not stop the next one from becoming one. */
  const f = new serial.Framer();
  const noise = new TextEncoder().encode("*** Booting Zephyr OS build v3.7.0 ***\r\n");
  const out = [...f.feed(noise), ...f.feed(good)];
  ok("a boot banner is dropped, and the next packet still parses",
     out.length === 1 && hex(out[0]) === "0102030405060708");
}

{
  /* A corrupted frame must take only itself down. */
  const bad = Uint8Array.from(good);
  const at = 6;
  bad[at] = bad[at] === 0x41 ? 0x42 : 0x41;
  const f = new serial.Framer();
  const out = [...f.feed(bad), ...f.feed(good)];
  ok("a corrupt packet is dropped, and the next one still parses",
     out.length === 1 && hex(out[0]) === "0102030405060708");
}

{
  /* A continuation frame with nothing to continue is not a packet. */
  const f = new serial.Framer();
  const orphan = Uint8Array.from([0x04, 0x14, ...new TextEncoder().encode("AAAA"), 0x0a]);
  const out = [...f.feed(orphan), ...f.feed(good)];
  ok("an orphan continuation frame is dropped",
     out.length === 1 && hex(out[0]) === "0102030405060708");
}

{
  /* A line longer than any real frame must be abandoned rather than buffered,
   * or a chatty port is a memory leak. */
  const f = new serial.Framer();
  const flood = new Uint8Array(10000).fill(0x41);
  const out = [...f.feed(flood), ...f.feed(good)];
  ok("an over-long line is abandoned, and the next packet still parses",
     out.length === 1 && hex(out[0]) === "0102030405060708");
}

{
  /* A truncated packet leaves the framer waiting, not wedged: the bytes that
   * follow are a fresh packet and must be read as one. */
  const f = new serial.Framer();
  const partial = serial.encodePacket(new Uint8Array(200).fill(7));
  const firstLine = partial.slice(0, partial.indexOf(0x0a) + 1);
  const out = [...f.feed(firstLine), ...f.feed(good)];
  ok("a packet cut off mid-way does not swallow the next one",
     out.length === 1 && hex(out[0]) === "0102030405060708");
}

{
  /* A declared length of 1 cannot describe a packet: there is no room for the
   * two CRC bytes. Before the guard, `full.length - 2` was -1 here and the
   * subarray produced an empty "packet" that the caller then had to ignore. */
  const f = new serial.Framer();
  const body = Uint8Array.of(0x00, 0x01, 0x00);      /* len=1, one byte after */
  const line = new Uint8Array([0x06, 0x09,
    ...new TextEncoder().encode(Buffer.from(body).toString("base64")), 0x0a]);
  const out = [...f.feed(line), ...f.feed(good)];
  ok("a declared length of 1 is not a packet",
     out.length === 1 && hex(out[0]) === "0102030405060708",
     `${out.length} packets, first ${out[0] ? hex(out[0]) : "-"}`);
}

/* ---- 5. the chunk sizes stay inside the MTU the overlay sets --------------- */

{
  /* CONFIG_MCUMGR_TRANSPORT_UART_MTU caps a whole SMP frame. The payload plus
   * the 8-byte header plus the CBOR map has to fit under it, and the overlay
   * and this constant are edited in different files by different people. */
  const MTU = 512;
  const CBOR_OVERHEAD = 40;   /* generous: image/off/len keys and their values */
  ok("APP_CHUNK leaves room under the 512-byte UART MTU",
     serial.APP_CHUNK + 8 + CBOR_OVERHEAD <= MTU,
     `${serial.APP_CHUNK} + 48 vs ${MTU}`);
  ok("MCUBOOT_CHUNK is no larger than APP_CHUNK",
     serial.MCUBOOT_CHUNK <= serial.APP_CHUNK);
}

/* ---- 6. the property that actually matters, under random noise -------------
 *
 * WHY A FUZZ AND NOT MORE HAND-WRITTEN CASES. The hand-written noise cases
 * above pin behaviours somebody thought of, and that is exactly their limit: a
 * differential run against a Framer with the resync removed found a stream none
 * of them covers, and a run against one with the `overrun` flag removed found
 * NONE -- which is how it was established that the resync is the load-bearing
 * fix and the flag is a bound rather than a behaviour. That finding is written
 * into serial.js, and this is what keeps it honest.
 *
 * The invariant is the strong one and the one a board depends on: every packet
 * the framer emits must be a packet that was really sent. Dropping a packet in
 * the middle of arbitrary rubbish is acceptable -- rubbish and a frame on one
 * unterminated line are genuinely inseparable. INVENTING one is not.
 *
 * Deterministic: a fixed seed and a hand-rolled LCG, so a failure is a bug
 * report rather than a coin toss.
 */
{
  let seed = 20260827;
  const rand = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);

  let emitted = 0;
  let invented = 0;
  let framesSeen = 0;

  for (let trial = 0; trial < 400; trial++) {
    const parts = [];
    const sent = new Set();

    for (let i = 0; i < 10; i++) {
      const kind = rand(4);
      if (kind === 0) {
        const body = new Uint8Array(rand(300)).map(() => rand(256));
        parts.push(serial.encodePacket(body));
        sent.add(hex(body));
        framesSeen += 1;
      } else if (kind === 1) {
        /* printable rubbish, newline-terminated: a log line */
        const n = rand(400);
        const a = new Uint8Array(n + 1);
        for (let j = 0; j < n; j++) a[j] = 0x20 + rand(90);
        a[n] = 0x0a;
        parts.push(a);
      } else if (kind === 2) {
        /* printable rubbish with NO newline: the case the bound is about */
        const n = rand(600);
        const a = new Uint8Array(n);
        for (let j = 0; j < n; j++) a[j] = 0x20 + rand(90);
        parts.push(a);
      } else {
        /* a packet cut off after its first frame */
        const w = serial.encodePacket(new Uint8Array(200).fill(rand(256)));
        parts.push(w.slice(0, w.indexOf(0x0a) + 1));
      }
    }

    const total = parts.reduce((n, x) => n + x.length, 0);
    const bytes = new Uint8Array(total);
    let o = 0;
    for (const x of parts) { bytes.set(x, o); o += x.length; }

    /* Random read sizes, because that is what a serial port does. */
    const f = new serial.Framer();
    for (let i = 0; i < bytes.length;) {
      const n = 1 + rand(64);
      for (const got of f.feed(bytes.subarray(i, i + n))) {
        emitted += 1;
        if (!sent.has(hex(got))) invented += 1;
      }
      i += n;
    }
  }

  ok("fuzz: never emits a packet that was not sent", invented === 0,
     `${invented} of ${emitted} emitted packets were invented`);
  ok("fuzz: still emits real packets out of the noise", emitted > 0,
     `${emitted} emitted across ${framesSeen} sent`);
}

console.log(`\n  the serial framing agrees with the standard library`);
console.log(`RESULT: ${fail ? "FAIL" : "PASS"} (${pass + fail} checks)`);
process.exit(fail ? 1 : 0);
