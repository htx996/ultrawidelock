/* SPDX-License-Identifier: ISC */

/*
 * fota.js -- the over-the-air half of this page.
 *
 * The wired half above it writes a whole image over WebSerial and needs a
 * cable. This half needs neither cable nor probe: it finds the board over
 * Bluetooth, asks what it is running, and sends the one update that applies.
 *
 * THE ORDER OF OPERATIONS IS THE DESIGN. The board is identified BEFORE
 * anything is asked of the person at the keyboard, because on the DWM3001CDK
 * the image-list read is not window-gated while the upload is
 * (ports/zephyr/dfu/dfu_smp_img.c). So the page can say "you are running X,
 * here is the update to Y, press the button now" instead of "press the button
 * and hope". Someone whose board has no update path finds out before they go
 * and press anything.
 *
 * Transports live in smp.js (mcumgr, the CDK) and uwldfu.js (the native framed
 * protocol, the ESP32). This file knows about neither wire format; it knows
 * about the index, the DOM and the failure prose.
 */

import * as smp from "./smp.js";
import * as serial from "./serial.js";
import * as uwldfu from "./uwldfu.js";

const INDEX_URL = "ota/ota-index.json";

/* How long to wait after reset before expecting the board back.
 *
 * MCUboot takes 17-31 s to apply a delta on the CDK (mk/cdk.mk:648 says ~30 s
 * and the firmware's own comment says 17-31). The first reconnect is tried at
 * the short end and then retried, rather than sleeping for the long end and
 * making every success feel slow. */
const APPLY_FIRST_TRY_MS = 17000;
const APPLY_RETRY_MS = 3000;
const APPLY_GIVE_UP_MS = 90000;

const el = (id) => document.getElementById(id);

/* ---- status reporting ----------------------------------------------------- */

/*
 * One live region, four states. Everything the flow wants to say goes through
 * here so the page cannot end up with a stale success line under a fresh
 * error, which is the classic way a status area lies.
 */
function say(kind, headline, detail = "") {
  const out = el("fota-out");
  if (!out) return;
  out.dataset.kind = kind;
  out.innerHTML = "";

  const h = document.createElement("strong");
  h.textContent = headline;
  out.append(h);

  if (detail) {
    const p = document.createElement("p");
    /* textContent, not innerHTML: some of this prose is a board-supplied error
     * string, and none of it is worth an injection sink. */
    p.textContent = detail;
    out.append(p);
  }
}

function progress(sent, total) {
  const bar = el("fota-bar");
  const fill = bar && bar.firstElementChild;
  if (!bar || !fill) return;
  bar.hidden = false;
  const pct = total ? Math.round((sent / total) * 100) : 0;
  fill.style.width = `${pct}%`;
  bar.setAttribute("aria-valuenow", String(pct));
  bar.dataset.label = `${pct}%  ·  ${sent.toLocaleString()} / ${total.toLocaleString()} B`;
}

function hideProgress() {
  const bar = el("fota-bar");
  if (bar) bar.hidden = true;
}

function busy(on) {
  const go = el("fota-go");
  if (go) {
    go.disabled = on;
    go.textContent = on ? "Working…" : (go.dataset.idle || "Find my board");
  }
}

/** Which transport the panel is set to, falling back to whatever exists. */
function chosenHow() {
  const sel = el("fota-how");
  const key = sel && sel.value;
  if (key && HOW[key] && HOW[key].available()) return key;
  return Object.keys(HOW).find((k) => HOW[k].available()) || null;
}

/* ---- the index ------------------------------------------------------------ */

let indexPromise = null;

function loadIndex() {
  if (!indexPromise) {
    indexPromise = fetch(INDEX_URL, { cache: "no-cache" }).then((r) => {
      if (!r.ok) throw new Error(`the update index is not published (HTTP ${r.status})`);
      return r.json();
    });
  }
  return indexPromise;
}

/* ---- how to reach the board ------------------------------------------------
 *
 * Only the DWM3001CDK has a choice here. The ESP32's cable path is the whole
 * -image installer further down this page -- a different protocol to a
 * different thing (the ROM bootloader), already shipped -- so the only decision
 * this panel offers is which way to reach an mcumgr board.
 *
 * BOTH APIS ARE CHROMIUM-ONLY, but not identically: desktop Chrome and Edge
 * have both, Chrome on Android has Web Bluetooth and NOT WebSerial. So the two
 * are probed separately and the panel offers whichever exist, rather than
 * treating "no Bluetooth" as "no updates".
 */
const HOW = {
  ble: {
    label: "Bluetooth — no cable",
    available: () => !!navigator.bluetooth,
    open: linkBle,
    button: "Find my board over Bluetooth",
  },
  serial: {
    label: "USB cable — faster",
    available: () => !!navigator.serial,
    open: linkSerial,
    button: "Find my board over USB",
  },
};

/* ---- links ----------------------------------------------------------------
 *
 * The CDK flow below is written against a "link" rather than against a radio,
 * because the two ways into that board differ in four small places and nowhere
 * else: how the operator picks it, what to say when the update window is shut,
 * how to get a session back after the reset, and how to let go at the end.
 * Everything between -- identify, look up, fetch, upload, verify -- is one
 * piece of code serving both.
 */

/** Web Bluetooth: the no-cable path. */
async function linkBle() {
  say("busy", "Pick your board in the chooser…",
      "It advertises as “ultrawidelock”. If it is not listed, it may be in a Matter " +
      "session — power-cycle it and try again.");

  const { device, smp: session } = await smp.connect();

  /* The session is replaced on every reconnect, and the one being replaced has
   * a listener on a characteristic that no longer exists. Verification retries
   * in a loop, so dropping it is the difference between a bounded number of
   * dead listeners and one per retry. */
  let current = session;

  return {
    session,
    what: device.name || "the board",
    /*
     * A RECONNECT, because the board drops the link when it reboots and the
     * device handle survives it. No new user gesture is needed for this, which
     * is the only reason verification after an apply is possible at all.
     */
    reattach: async () => {
      current.detach();
      current = await smp.attach(device);
      return current;
    },
    /* The reset already tore the GATT link down; this drops the listener that
     * was pointed at the characteristic it used to have. */
    settle: () => current.detach(),
    release: () => { try { device.gatt.disconnect(); } catch { /* already gone */ } },
    windowHint:
      "The board accepts nothing until an update window is open, and the window is " +
      "the only thing standing between this lock and anyone else in radio range. " +
      "D10 blinks twice a second while it is open. This page keeps asking for three " +
      "minutes.",
  };
}

/** WebSerial: uart0, which is the J-Link OB's VCOM. */
async function linkSerial() {
  say("busy", "Pick the board's serial port…",
      "On the DWM3001CDK this is the J-Link port, not the second one — the probe owns " +
      "uart0 and the application talks down it. It is usually named for SEGGER.");

  /* The port is not kept: `release` goes through the session, whose transport
   * owns closing it. Two handles onto one port is how it gets closed twice. */
  const { smp: session } = await smp.connectSerial(serial.APP_CHUNK);
  return {
    session,
    what: "the board over the cable",
    /*
     * THE SAME SESSION, and that is a property of the hardware rather than a
     * shortcut. The USB device here belongs to the J-Link OB, not to the
     * nRF52833 -- uart0 is wired between the two. So resetting the nRF does not
     * re-enumerate anything and the port never closes underneath us, where a
     * radio link has to be rebuilt. The Framer discards whatever the board
     * printed while it was rebooting.
     */
    reattach: async () => session,
    /* Nothing to settle: the port did not go anywhere. */
    settle: () => {},
    release: () => session.detach(),
    windowHint:
      "The board accepts nothing until an update window is open — the cable does not " +
      "bypass that, and is not meant to. Press SW2; D10 blinks twice a second while " +
      "the window is open. This page keeps asking for three minutes.",
  };
}

/* ---- the DWM3001CDK flow -------------------------------------------------- */

async function runCdk(target, openLink) {
  const link = await openLink();
  const session = link.session;
  say("busy", `Connected to ${link.what}.`, "Asking what it is running…");

  const running = await session.runningHash();

  /* ALREADY-LATEST IS CHECKED FIRST, ahead of the index lookup, and the order
   * is not cosmetic. A published set can contain an update whose starting image
   * IS the latest -- a build that did not change between two releases produces
   * exactly that -- and looking the hash up first would find it and cheerfully
   * offer the board an update to what it is already running. The verify step
   * afterwards would even pass, because the hash it checks for is the hash that
   * was already there. ota-index.py now refuses to publish such an entry, and
   * this is the second line of defence for a set that predates that check. */
  if (running === target.latest.sha256) {
    say("ok", "Already up to date.",
        `This board is running ${target.latest.version} ` +
        `(${running.slice(0, 16)}…), which is the current release.`);
    link.release();
    return;
  }

  const update = target.updates[running];

  if (!update) {
    /* A single MCUboot slot means a delta or nothing. This is the honest end
     * of the road, not a retryable error, so it must not read like one. */
    say("warn", "No over-the-air update applies to this board.",
        `It is running ${running.slice(0, 16)}…, which is not an image we publish an ` +
        `update from. The DWM3001CDK has one MCUboot slot, so what travels over the ` +
        `air is a delta against exactly the bytes already on the part — there is no ` +
        `whole-image path to fall back on. Use the J-Link and the release bundle.`);
    link.release();
    return;
  }

  say("busy",
      `Update available: ${update.version}.`,
      `${(update.size / 1024).toFixed(1)} KB, applying to ${running.slice(0, 16)}…`);

  const blob = new Uint8Array(await fetchUpdate(target, update));

  await session.upload(blob, {
    onProgress: progress,
    onWindowClosed: () => {
      say("prompt", "Press SW2 on the board now.", link.windowHint);
    },
  });

  hideProgress();
  say("busy", "Staged. Restarting the board…",
      "MCUboot applies the update during boot. It is off the air for 17-31 seconds.");

  await session.reset();
  link.settle();

  await verifyCdk(link, update);
}

async function fetchUpdate(target, update) {
  const url = `ota/${target.dir}/${update.file}`;
  const r = await fetch(url, { cache: "no-cache" });
  if (!r.ok) throw new Error(`the index names ${update.file}, which is not published`);
  const buf = await r.arrayBuffer();
  /* The index and the file are published together, so a size mismatch means one
   * of them is stale -- and installing on a stale index is how a board ends up
   * running something nobody can identify later. */
  if (buf.byteLength !== update.size) {
    throw new Error(
      `${update.file} is ${buf.byteLength} B but the index says ${update.size} B. ` +
      `Reload the page; if it persists the published set is inconsistent.`);
  }
  return buf;
}

/*
 * Reconnect after the apply and check the board really is running the new image.
 *
 * This is the step `make fota-done` exists for on the command line, and it is
 * not decoration: the board is the only witness to whether the update landed,
 * and a delta built against the wrong base is refused by the next update. A
 * page that said "done" without asking would be guessing.
 */
async function verifyCdk(link, update) {
  const deadline = Date.now() + APPLY_GIVE_UP_MS;
  await new Promise((r) => setTimeout(r, APPLY_FIRST_TRY_MS));

  for (;;) {
    try {
      const session = await link.reattach();
      const running = await session.runningHash();

      if (running === update.to) {
        say("ok", `Updated to ${update.version}.`,
            `The board came back reporting ${running.slice(0, 16)}…, which is the image ` +
            `this update produces. Nothing else to do.`);
      } else {
        say("warn", "The board came back running something else.",
            `Expected ${update.to.slice(0, 16)}…, got ${running.slice(0, 16)}…. The ` +
            `update did not apply. The old image is still bootable, so the board is not ` +
            `bricked — try again, and if it repeats, reflash over the J-Link.`);
      }
      link.release();
      return;
    } catch (err) {
      if (Date.now() > deadline) {
        say("warn", "The board did not come back in time.",
            `It may still be applying, or it may have gone out of range. Reconnecting ` +
            `will say what it is running. (${err.message})`);
        return;
      }
      await new Promise((r) => setTimeout(r, APPLY_RETRY_MS));
    }
  }
}

/* ---- the ESP32 flow ------------------------------------------------------- */

async function runEsp(target) {
  say("busy", "Pick your board in the chooser…", "It advertises as “ultrawidelock”.");

  const { device, session } = await uwldfu.connect();
  say("busy", `Connected to ${device.name || "the board"}.`, "Fetching the image…");

  const image = target.image;
  const r = await fetch(`ota/${target.dir}/${image.file}`, { cache: "no-cache" });
  if (!r.ok) throw new Error(`the index names ${image.file}, which is not published`);
  const blob = new Uint8Array(await r.arrayBuffer());

  /* No delta, so no base to match -- but also no way to know what it was
   * running, and the size is why anyone would want to. Say the cost up front. */
  say("busy", `Sending ${(blob.length / 1048576).toFixed(2)} MB.`,
      "A whole application image travels over Bluetooth here, because the ESP32 has two " +
      "full slots and no delta path. Expect several minutes. Keep this tab open and the " +
      "board in range.");

  await session.waitForWindow(blob.length, 180000, () => {
    say("prompt", "Double-click the board's button now.",
        "The board accepts nothing until the update window is open. A long press is the " +
        "commissioning window instead, which is a different thing. This page keeps asking " +
        "for three minutes.");
  });

  const started = Date.now();
  await session.send(blob, (sent, total) => {
    progress(sent, total);
    const rate = sent / Math.max(1, (Date.now() - started) / 1000);
    const left = Math.round((total - sent) / Math.max(1, rate));
    const bar = el("fota-bar");
    if (bar) {
      bar.dataset.label =
        `${Math.round((sent / total) * 100)}%  ·  ${(rate / 1024).toFixed(1)} KB/s  ·  ` +
        `about ${Math.ceil(left / 60)} min left`;
    }
  });

  await session.commit(blob.length);
  session.detach();
  hideProgress();

  say("ok", `Sent ${image.version}.`,
      "The board is rebooting into the new image. If it does not come back, it rolls " +
      "itself back to the previous one at the next boot.");
}

/* ---- entry point ---------------------------------------------------------- */

async function go() {
  const select = el("fota-target");
  const key = select ? select.value : "";

  busy(true);
  hideProgress();

  try {
    const index = await loadIndex();
    const target = index.targets[key];

    if (!target) {
      say("warn", "No update is published for that board yet.",
          "The index this page reads has no entry for it. That means a release has not " +
          "been cut with over-the-air artifacts for that target.");
      return;
    }

    if (target.transport === "smp") {
      const how = chosenHow();
      if (!how) throw new Error("this browser has neither Web Bluetooth nor WebSerial");
      await runCdk(target, HOW[how].open);
    }
    else if (target.transport === "uwldfu") await runEsp(target);
    else throw new Error(`the index asks for a transport this page does not have (${target.transport})`);
  } catch (err) {
    hideProgress();
    /* NotFoundError covers two very different things, and the browser will not
     * say which: the chooser was dismissed, or it listed nothing to dismiss.
     * The second is what a first-time owner hits -- a board with no firmware on
     * it is not advertising, so it cannot appear here -- and "no board picked"
     * would send them looking for a fault that is not there. Cancelling is a
     * decision and must not be dressed up as a failure either, so the message
     * has to carry both readings without alarming the first one. */
    if (err && err.name === "NotFoundError") {
      say("idle", "No board picked.",
          "If you cancelled, nothing was sent and nothing changed. If the list was " +
          "empty: a board with no firmware on it yet is not advertising anything, so " +
          "it cannot show up here — install over a cable first (ESP32) or over the " +
          "J-Link (DWM3001CDK), then come back. An already-flashed board that is " +
          "missing may be mid-commissioning; power-cycle it and retry.");
    } else {
      say("err", "That did not work.", err && err.message ? err.message : String(err));
    }
  } finally {
    busy(false);
  }
}

/* ---- wiring --------------------------------------------------------------- */

export function init() {
  const go_ = el("fota-go");
  const select = el("fota-target");
  if (!go_ || !select) return;

  /* Whichever of the two this browser has. Neither is a stopper on its own:
   * Chrome on Android has Web Bluetooth and no WebSerial, and a machine with a
   * cable in it wants the serial path anyway. */
  const usable = Object.keys(HOW).filter((k) => HOW[k].available());

  if (!usable.length) {
    /* Say which browsers, not "unsupported". Both APIs are Chromium-only:
     * Safari has never shipped either, so no iPhone can do this, and Firefox
     * has not either. That is a fact about the browser, not about the board,
     * and someone reading this needs to know which one to go and get. */
    go_.disabled = true;
    say("warn", "This browser cannot update a board.",
        "Web Bluetooth and WebSerial both exist only in Chrome and Edge on a computer " +
        "(and Web Bluetooth alone in Chrome on Android). Safari has never shipped " +
        "either, so no iPhone or iPad can update a board from this page; Firefox has " +
        "not shipped either. Everything else on this page still works.");
    return;
  }

  if (!window.isSecureContext) {
    go_.disabled = true;
    say("warn", "This needs a secure page.",
        "Both Web Bluetooth and WebSerial refuse to run otherwise. Open this page over " +
        "https (or localhost) and reload.");
    return;
  }

  /* The transport picker only appears when there is a choice to make. One
   * option in a dropdown is a decision the reader cannot get wrong and should
   * not be asked to make. */
  const how = el("fota-how");
  const howRow = el("fota-how-row");
  if (how) {
    how.innerHTML = "";
    for (const key of usable) {
      const opt = document.createElement("option");
      opt.value = key;
      opt.textContent = HOW[key].label;
      how.append(opt);
    }
    /* Set explicitly rather than relying on a select defaulting to its first
     * option, so that reading `how.value` back is defined from here on. */
    how.value = usable[0];
    if (howRow) howRow.hidden = usable.length < 2;
    const relabel = () => {
      go_.dataset.idle = (HOW[how.value] || HOW[usable[0]]).button;
      if (!go_.disabled) go_.textContent = go_.dataset.idle;
    };
    how.addEventListener("change", relabel);
    relabel();
  } else {
    go_.dataset.idle = HOW[usable[0]].button;
  }

  loadIndex().then((index) => {
    /* The dropdown is built from the index, so it can never offer a board the
     * published set has no update for -- the same reason the wired flasher
     * builds its board list out of manifest.json. */
    const targets = Object.entries(index.targets || {});
    if (!targets.length) {
      go_.disabled = true;
      say("warn", "No over-the-air updates are published yet.", "");
      return;
    }
    select.innerHTML = "";
    for (const [key, target] of targets) {
      const opt = document.createElement("option");
      opt.value = key;
      opt.textContent = `${target.name} — ${target.latest.version}`;
      select.append(opt);
    }
    say("idle", "Ready.",
        "Your board needs to be powered and in range. Nothing is written until you " +
        "open the update window on the board itself.");
  }).catch((err) => {
    go_.disabled = true;
    say("warn", "No over-the-air updates are published yet.",
        `${err.message} A release has to be cut with update artifacts before this ` +
        `section can do anything.`);
  });

  go_.addEventListener("click", go);
}

init();
