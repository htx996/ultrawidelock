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
    go.textContent = on ? "Working…" : "Find my board over Bluetooth";
  }
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

/* ---- the DWM3001CDK flow -------------------------------------------------- */

async function runCdk(target) {
  say("busy", "Pick your board in the chooser…",
      "It advertises as “ultrawidelock”. If it is not listed, it may be in a Matter " +
      "session — power-cycle it and try again.");

  const { device, smp: session } = await smp.connect();
  say("busy", `Connected to ${device.name || "the board"}.`, "Asking what it is running…");

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
    device.gatt.disconnect();
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
    device.gatt.disconnect();
    return;
  }

  say("busy",
      `Update available: ${update.version}.`,
      `${(update.size / 1024).toFixed(1)} KB, applying to ${running.slice(0, 16)}…`);

  const blob = new Uint8Array(await fetchUpdate(target, update));

  await session.upload(blob, {
    onProgress: progress,
    onWindowClosed: () => {
      say("prompt", "Press SW2 on the board now.",
          "The board accepts nothing until an update window is open, and the window is " +
          "the only thing standing between this lock and anyone else in radio range. " +
          "D10 blinks twice a second while it is open. This page keeps asking for three " +
          "minutes.");
    },
  });

  hideProgress();
  say("busy", "Staged. Restarting the board…",
      "MCUboot applies the update during boot. It is off the air for 17-31 seconds.");

  await session.reset();
  session.detach();

  await verifyCdk(device, update);
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
async function verifyCdk(device, update) {
  const deadline = Date.now() + APPLY_GIVE_UP_MS;
  await new Promise((r) => setTimeout(r, APPLY_FIRST_TRY_MS));

  for (;;) {
    try {
      const session = await smp.attach(device);
      const running = await session.runningHash();
      session.detach();

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
      device.gatt.disconnect();
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

    if (target.transport === "smp") await runCdk(target);
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

  if (!navigator.bluetooth) {
    /* Say which browsers, not "unsupported". Web Bluetooth is Chromium-only:
     * Safari has never shipped it, so no iPhone can do this, and Firefox has
     * not either. That is a fact about the browser, not about the board, and
     * someone reading this needs to know which one to go and get. */
    go_.disabled = true;
    say("warn", "This browser cannot do Bluetooth updates.",
        "Web Bluetooth exists only in Chrome and Edge on a computer, and Chrome on " +
        "Android. Safari has never shipped it, so no iPhone or iPad can update a board " +
        "from this page; Firefox has not shipped it either. Everything else on this page " +
        "still works.");
    return;
  }

  if (!window.isSecureContext) {
    go_.disabled = true;
    say("warn", "Bluetooth needs a secure page.",
        "Open this page over https (or localhost) and reload.");
    return;
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
