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
  stopTimed();
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
  stopTimed();
  const bar = el("fota-bar");
  if (bar) bar.hidden = true;
}

/* ---- the bar during the part with no bytes to count ------------------------
 *
 * WHY THIS EXISTS. Uploading has a numerator and a denominator, so the bar
 * above is honest for free. Applying does not: the board is rebooting, MCUboot
 * is rewriting flash, and nothing reports back until it finishes. That is
 * 17-31 seconds on this board -- the LONGEST single stretch of the whole
 * operation -- and the page used to show nothing at all through it, which is
 * exactly where somebody starts wondering whether it has died. Reported on the
 * first real run, and fair.
 *
 * IT NEVER REACHES 100%, and that is the design rather than a rounding
 * artefact. This is an estimate against a known range, not a measurement, so it
 * creeps toward a cap and waits there. A bar that sits at 100% while nothing
 * happens has lied; a bar that sits at 94% saying "any moment now" has not.
 * Only the board coming back and reporting the right hash finishes it.
 */
let timedTimer = null;

function progressTimed(label, expectMs, capPct = 94) {
  stopTimed();
  const bar = el("fota-bar");
  const fill = bar && bar.firstElementChild;
  if (!bar || !fill) return;

  bar.hidden = false;
  const started = Date.now();

  const tick = () => {
    const elapsed = Date.now() - started;
    /* Eases off as it approaches the cap, so overrunning the estimate looks
     * like patience rather than like a stall at a hard stop. */
    const pct = Math.min(capPct, capPct * (1 - Math.exp(-2.2 * (elapsed / expectMs))));
    fill.style.width = `${pct.toFixed(1)}%`;
    bar.setAttribute("aria-valuenow", String(Math.round(pct)));
    const left = Math.max(0, Math.round((expectMs - elapsed) / 1000));
    bar.dataset.label = left
      ? `${label}  ·  about ${left}s left`
      : `${label}  ·  any moment now`;
  };

  tick();
  timedTimer = setInterval(tick, 250);
}

function stopTimed() {
  if (timedTimer) { clearInterval(timedTimer); timedTimer = null; }
}

/** The one place the bar is allowed to reach the end. */
function progressDone(label) {
  stopTimed();
  const bar = el("fota-bar");
  const fill = bar && bar.firstElementChild;
  if (!bar || !fill) return;
  bar.hidden = false;
  fill.style.width = "100%";
  bar.setAttribute("aria-valuenow", "100");
  bar.dataset.label = label;
}

/**
 * @param {boolean} on
 * @param {string} [label] what the disabled button should say instead of "Working…"
 *
 * THE LABEL EXISTS BECAUSE "Working…" WAS A LIE for the longest part of the
 * flow. Both choosers -- Web Bluetooth's device picker and WebSerial's port
 * picker -- are native browser dialogs that block until somebody clicks
 * something, and Chrome draws them as a small panel under the address bar that
 * is genuinely easy to miss. So the page sat there saying "Working…" while the
 * browser sat there waiting for the operator, and the honest reading of the
 * screen was "this has hung". Observed on the first real run of this page.
 */
function busy(on, label) {
  const go = el("fota-go");
  if (go) {
    go.disabled = on;
    go.textContent = on ? (label || "Working…") : (go.dataset.idle || "Find my board");
  }
}

/** The button, while the thing being waited on is a person. */
const WAITING_FOR_YOU = "Waiting for you…";

/* Where the dialog actually is. Not decoration: a modal nobody can find is
 * indistinguishable from a page that has stopped responding. */
const CHOOSER_WHERE =
  "Chrome shows this as a panel just under the address bar — it is easy to miss if " +
  "the window is scrolled. Nothing has been sent yet and nothing is written until " +
  "you pick a device.";

/**
 * The transports that could be used on this board, in this browser.
 *
 * TARGET-DEPENDENT, not just browser-dependent: `recover` needs a whole image
 * in the index, and offering it when none was published would be an option
 * that can only ever fail.
 */
function usableFor(target) {
  return Object.keys(HOW).filter((k) => {
    if (!HOW[k].available()) return false;
    return !HOW[k].needs || (target && HOW[k].needs(target));
  });
}

/** Which transport the panel is set to, falling back to whatever is usable. */
function chosenHow(target) {
  const usable = usableFor(target);
  const sel = el("fota-how");
  const key = sel && sel.value;
  if (key && usable.includes(key)) return key;
  return usable[0] || null;
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
const CHOOSER_EMPTY_BLE =
  "If you cancelled, nothing was sent and nothing changed. If the list was empty: " +
  "a board with no firmware on it yet is not advertising anything, so it cannot show " +
  "up here — install over a cable first (ESP32) or over the J-Link (DWM3001CDK), then " +
  "come back. An already-flashed board that is missing may be mid-commissioning; " +
  "power-cycle it and retry.";

const CHOOSER_EMPTY_SERIAL =
  "If you cancelled, nothing was sent and nothing changed. If no likely port was " +
  "listed: the DWM3001CDK's port is the J-Link one, and it is there whenever the " +
  "board is plugged in — it belongs to the probe, so it appears even when the " +
  "board's own software is not running at all. The second USB socket is not it, and " +
  "shows nothing outside provisioning mode.";

const HOW = {
  ble: {
    label: "Bluetooth — no cable",
    available: () => !!navigator.bluetooth,
    run: (target) => runCdk(target, linkBle),
    button: "Find my board over Bluetooth",
    empty: CHOOSER_EMPTY_BLE,
  },
  serial: {
    label: "USB cable — faster",
    available: () => !!navigator.serial,
    run: (target) => runCdk(target, linkSerial),
    button: "Find my board over USB",
    empty: CHOOSER_EMPTY_SERIAL,
  },
  /*
   * THE ONLY WHOLE-IMAGE PATH THIS BOARD HAS, and the only one that works when
   * the application does not.
   *
   * Everything else here is a delta, because one MCUboot slot means an update
   * has to be computed against exactly the bytes already on the part. That
   * requires a running application to ask what those bytes are -- so a board
   * whose image does not boot is beyond every other option on this page.
   *
   * MCUboot's serial recovery is the exception: it is not running the
   * application, so the whole slot is free, and it listens on the same uart0.
   * CONFIG_BOOT_SERIAL_NO_APPLICATION=y means a board with no valid image stays
   * there rather than jumping into erased flash, which is what makes a torn
   * update recoverable instead of fatal.
   *
   * Offered only when the index actually carries a recovery image; see
   * `usableFor` below.
   */
  recover: {
    label: "USB cable — reinstall everything",
    available: () => !!navigator.serial,
    needs: (target) => !!target.recovery,
    run: (target) => runCdkRecovery(target),
    button: "Reinstall over USB",
    empty: CHOOSER_EMPTY_SERIAL,
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
  say("prompt", "Pick your board in the chooser…",
      "It advertises as “ultrawidelock”. " + CHOOSER_WHERE + " If it is listed but " +
      "greyed out, or not listed at all, it may be in a Matter session — power-cycle " +
      "it and try again.");
  busy(true, WAITING_FOR_YOU);

  const { device, smp: session } = await smp.connect();
  busy(true);

  /* The session is replaced on every reconnect, and the one being replaced has
   * a listener on a characteristic that no longer exists. Verification retries
   * in a loop, so dropping it is the difference between a bounded number of
   * dead listeners and one per retry. */
  let current = session;

  return {
    session,
    what: device.name || "the board",
    gone: "or it may have gone out of range",
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
  say("prompt", "Pick the board's serial port…",
      "On the DWM3001CDK this is the J-Link port, not the second one — the probe owns " +
      "uart0 and the application talks down it. It is usually named for SEGGER. " +
      CHOOSER_WHERE);
  busy(true, WAITING_FOR_YOU);

  /* The port is not kept: `release` goes through the session, whose transport
   * owns closing it. Two handles onto one port is how it gets closed twice. */
  const { smp: session } = await smp.connectSerial(serial.APP_CHUNK);
  busy(true);
  return {
    session,
    what: "the board over the cable",
    gone: "or the cable may have been unplugged",
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
  try {
    await cdkFlow(target, link);
  } catch (err) {
    /*
     * RELEASE ON EVERY EXIT. This used to be three `link.release()` calls on
     * the three paths somebody thought of; the ones nobody thought of -- the
     * board falling silent, a size mismatch, the update window never opening,
     * a write past the MTU -- left a WebSerial port open. An open port cannot
     * be reopened: requestPort() hands back the same object and open() rejects
     * with InvalidStateError, so the retry fails for a reason that has nothing
     * to do with the board and only closing the tab clears it. A radio link
     * forgives being dropped on the floor. A cable does not.
     */
    link.release();
    throw err;
  }
}

async function cdkFlow(target, link) {
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

  /* `|| {}` because a hand-edited or pre-recovery index can omit the key, and
   * "Cannot read properties of undefined" is not a sentence anyone can act on.
   * ota-index.py always emits it, even as {}. */
  const update = (target.updates || {})[running];

  if (!update) {
    /* A single MCUboot slot means a delta or nothing. This is the honest end
     * of the road, not a retryable error, so it must not read like one. */
    /* WHAT TO DO INSTEAD DEPENDS ON WHAT WAS PUBLISHED. Sending someone to find
     * a J-Link while "Reinstall over USB" sits in the dropdown beside them is
     * the worst version of this message, and it is what it used to say. */
    const fallback = target.recovery
      ? `Pick “${HOW.recover.label}” above instead — it writes the whole image ` +
        `through the bootloader, so it needs no starting image to work from.`
      : `Use the J-Link and the release bundle.`;
    say("warn", "No over-the-air update applies to this board.",
        `It is running ${running.slice(0, 16)}…, which is not an image we publish an ` +
        `update from. The DWM3001CDK has one MCUboot slot, so an update is a delta ` +
        `against exactly the bytes already on the part. ` + fallback);
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

  say("busy", "Staged. Restarting the board…",
      "MCUboot applies the update during boot. It is off the air for 17-31 seconds, " +
      "and nothing can be asked of it until it comes back.");
  progressTimed("applying on the board", 24000);

  await session.reset();
  link.settle();

  await verifyCdk(link, update);
}

/* ---- the DWM3001CDK, whole image, through MCUboot -------------------------- */

/** How long to keep asking the bootloader whether it is there yet. */
const RECOVERY_PROBE_MS = 120000;
const RECOVERY_PROBE_EVERY_MS = 1000;

/**
 * Reinstall the whole image over MCUboot's serial recovery.
 *
 * DIFFERENT IN EVERY WAY THAT MATTERS from the delta flow above, which is why
 * it is a separate function rather than a branch:
 *
 *   nothing is identified first   There may be no application to ask. That is
 *                                 the case this exists for.
 *   no index lookup               A whole image applies to any board, because
 *                                 it does not subtract from anything.
 *   no update window              The window is the application's gate, and the
 *                                 application is not running. What stands in
 *                                 for it is physical: this needs a cable.
 *   it overwrites in place        One slot. A torn transfer leaves no
 *                                 application -- recoverable, because MCUboot
 *                                 stays in recovery, but not nothing.
 */
async function runCdkRecovery(target) {
  const rec = target.recovery;
  if (!rec) {
    say("warn", "No reinstall image is published for this board.",
        "The release that produced this index did not include a whole-image build, so " +
        "only the delta updates above are available.");
    return;
  }

  say("prompt", "Put the board into recovery mode first.",
      "Hold SW2 for five seconds while the board is running: it restarts into the " +
      "bootloader and waits there. If the board's software is already broken it is " +
      "waiting there anyway, and there is nothing to press. Then pick the J-Link port. " +
      CHOOSER_WHERE);

  busy(true, WAITING_FOR_YOU);
  const { smp: session } = await smp.connectSerial(serial.MCUBOOT_CHUNK);
  busy(true);

  try {
    say("busy", "Waiting for the bootloader…",
        "The board answers here only while it is in recovery. If nothing happens, hold " +
        "SW2 for five seconds again — the request is one-shot and is cleared on the " +
        "boot that consumes it.");

    await waitForBootloader(session);

    const url = `ota/${target.dir}/${rec.file}`;
    const r = await fetch(url, { cache: "no-cache" });
    if (!r.ok) throw new Error(`the index names ${rec.file}, which is not published`);
    const blob = new Uint8Array(await r.arrayBuffer());

    if (blob.length !== rec.size) {
      throw new Error(
        `${rec.file} is ${blob.length.toLocaleString()} B but the index says ` +
        `${rec.size.toLocaleString()} B; refusing to write it`);
    }

    say("busy", `Writing ${rec.version} — ${(blob.length / 1024).toFixed(0)} KB.`,
        "This overwrites the running image in place, because the board has one slot. " +
        "Do not unplug it. If the transfer is cut, the board stays in recovery and you " +
        "can start again — it does not become unrecoverable.");

    /*
     * WAITING OUT THE UPDATE WINDOW IS WRONG HERE, which is why the timeout is
     * negative rather than long.
     *
     * MCUboot has no window: it is not the application, and the application's
     * gate is the application's. So an rc=11 on this path does not mean "press
     * the button" -- it means the APPLICATION answered, and therefore that the
     * board never entered recovery. Waiting would leave someone pressing SW2
     * over and over at the one thing that was never going to accept it. A
     * negative deadline makes the first refusal final, and the catch below
     * turns it into the sentence that is actually true.
     */
    try {
      await session.upload(blob, { onProgress: progress, windowTimeoutMs: -1 });
    } catch (err) {
      if (err && err.rc === 11) {
        throw new Error(
          "the application answered, not the bootloader — so the board is running " +
          "normally and did not enter recovery. Hold SW2 for five seconds until it " +
          "restarts, then try again. (If you only wanted an update, the other two " +
          "options are the ones you want.)");
      }
      throw err;
    }
    hideProgress();

    say("busy", "Written. Restarting the board…",
        "MCUboot verifies the image before it hands over.");
    progressTimed("verifying and booting", 20000);
    await session.reset();

    await verifyRecovery(session, rec);
  } finally {
    session.detach();
  }
}

/**
 * Poll until something answers an image-list read.
 *
 * The read is the probe because it is the one command that is never gated: the
 * application serves it ungated (ports/zephyr/dfu/dfu_smp_img.c) and MCUboot in
 * recovery has no gate at all. So an answer means "somebody is listening", and
 * that is all this needs to know before it starts writing.
 */
async function waitForBootloader(session) {
  const deadline = Date.now() + RECOVERY_PROBE_MS;
  for (;;) {
    try {
      await session.imageState();
      return;
    } catch (err) {
      if (Date.now() > deadline) {
        throw new Error(
          "the bootloader did not answer. Hold SW2 for five seconds and try again; " +
          "if it never answers, the board needs its J-Link and the release bundle. " +
          `(${err.message})`);
      }
      await new Promise((r) => setTimeout(r, RECOVERY_PROBE_EVERY_MS));
    }
  }
}

/**
 * Confirm the board came back on the image that was just written.
 *
 * The port survives this, unlike the Bluetooth link: it belongs to the J-Link
 * OB rather than to the nRF52833, so the same session can simply ask again.
 */
async function verifyRecovery(session, rec) {
  const deadline = Date.now() + APPLY_GIVE_UP_MS;
  await new Promise((r) => setTimeout(r, APPLY_FIRST_TRY_MS));

  for (;;) {
    try {
      const running = await session.runningHash();
      if (running === rec.sha256) {
        progressDone(`reinstalled ${rec.version}`);
        say("ok", `Reinstalled ${rec.version}.`,
            `The board came back reporting ${running.slice(0, 16)}…, which is the image ` +
            `that was written. It is on the current release, so ordinary updates apply ` +
            `to it again.`);
      } else {
        hideProgress();
        say("warn", "The board came back running something else.",
            `Expected ${rec.sha256.slice(0, 16)}…, got ${running.slice(0, 16)}…. Hold ` +
            `SW2 for five seconds and reinstall; if it repeats, use the J-Link.`);
      }
      return;
    } catch (err) {
      if (Date.now() > deadline) {
        say("warn", "The board did not come back in time.",
            `It may still be verifying. Reconnecting will say what it is running. ` +
            `(${err.message})`);
        return;
      }
      await new Promise((r) => setTimeout(r, APPLY_RETRY_MS));
    }
  }
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
        progressDone(`installed ${update.version}`);
        say("ok", `Updated to ${update.version}.`,
            `The board came back reporting ${running.slice(0, 16)}…, which is the image ` +
            `this update produces. Nothing else to do.`);
      } else {
        hideProgress();
        say("warn", "The board came back running something else.",
            `Expected ${update.to.slice(0, 16)}…, got ${running.slice(0, 16)}…. The ` +
            `update did not apply. The old image is still bootable, so the board is not ` +
            `bricked — try again, and if it repeats, reflash over the J-Link.`);
      }
      link.release();
      return;
    } catch (err) {
      if (Date.now() > deadline) {
        hideProgress();
        say("warn", "The board did not come back in time.",
            `It may still be applying, ${link.gone}. Reconnecting will say what it is ` +
            `running. (${err.message})`);
        link.release();
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

  /* Remembered because the chooser throws BEFORE any link exists, and the
   * message for "nothing picked" is different for a device chooser and a port
   * chooser. Defaults to the Bluetooth wording, which is what the ESP32 path
   * uses -- it has no chooser of its own to pick from. */
  let how = null;

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
      how = chosenHow(target);
      if (!how) throw new Error("this browser has neither Web Bluetooth nor WebSerial");
      await HOW[how].run(target);
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
      /* Both choosers throw this, and they mean different things by it. A
       * device chooser was empty because nothing was advertising; a port
       * chooser lists the J-Link whether or not the board is running anything,
       * so "install firmware first" would be actively wrong there -- and on the
       * recovery path it would send someone to fetch the probe that path exists
       * to make unnecessary. */
      const spec = (how && HOW[how]) || HOW.ble;
      say("idle", how === "ble" || !how ? "No board picked." : "No port picked.",
          spec.empty);
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

  /*
   * The transport picker only appears when there is a choice to make. One
   * option in a dropdown is a decision the reader cannot get wrong and should
   * not be asked to make.
   *
   * REBUILT WHEN THE BOARD CHANGES, because what is on offer depends on the
   * board and not only on the browser: an ESP32 has no mcumgr transports at
   * all, and "reinstall everything" exists only where the index carries a whole
   * image to reinstall.
   */
  const how = el("fota-how");
  const howRow = el("fota-how-row");

  /* Falls back to a NEUTRAL label, not to the browser's first transport. The
   * old fallback was HOW[usable[0]], which is browser-wide -- so on a Chromium
   * build with WebSerial and no Bluetooth adapter, selecting an ESP32 target
   * (which has no mcumgr transports at all, so `how.value` is "") labelled the
   * button "Find my board over USB" for a path that is Bluetooth-only. */
  const relabel = () => {
    const spec = HOW[how && how.value];
    const label = spec ? spec.button : "Find my board";
    go_.dataset.idle = label;
    if (!go_.disabled) go_.textContent = label;
  };

  function rebuildHow(target) {
    /* Only the mcumgr board has a choice. For anything else the picker is put
     * away rather than shown with one entry. */
    const forTarget = target && target.transport === "smp" ? usableFor(target) : [];
    if (!how) {
      go_.dataset.idle = HOW[usable[0]].button;
      return;
    }
    how.innerHTML = "";
    for (const key of forTarget) {
      const opt = document.createElement("option");
      opt.value = key;
      opt.textContent = HOW[key].label;
      how.append(opt);
    }
    /* Set explicitly rather than relying on a select defaulting to its first
     * option, so that reading `how.value` back is defined from here on. */
    how.value = forTarget[0] || "";
    if (howRow) howRow.hidden = forTarget.length < 2;
    relabel();
  }

  if (how) how.addEventListener("change", relabel);

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
    select.value = targets[0][0];
    select.addEventListener("change",
                            () => rebuildHow(index.targets[select.value]));
    rebuildHow(index.targets[select.value]);
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
