/* UltraWideLock — the landing hero: a working DS-TWR round.
 *
 * The page opens with the thing the firmware actually does, not a description
 * of it. Drag the phone and the measured distance, the tick count and the
 * verdict all move with it. Arm the relay and the measurement gets *longer*,
 * which is the whole argument for time-of-flight in one gesture: a relay can
 * add delay, and adding delay can only push you further away. There is no
 * attack that makes light arrive early.
 *
 * Every constant here is the firmware's own, cited to the line it lives on.
 * They are checked by tools/check_hero_constants.py, which re-reads each cited
 * line and fails the build if the value has moved.
 *
 * Progressive by construction: the SVG in the page is already a complete,
 * labelled, static round. This file adds interaction. With scripting off, or
 * before this file arrives, the hero is a diagram rather than a broken widget.
 *
 * The travelling pulses are CSS animations on transform, so they run on the
 * compositor. JavaScript only runs when the reader moves something, plus one
 * short walk-up of the lock above the instrument when the page opens.
 */
(function () {
  "use strict";

  var svg = document.getElementById("hero-scope");
  var slider = document.getElementById("hs-range");
  if (!svg || !slider) return;

  /* ---- the firmware's numbers ------------------------------------------ */
  /* Same cited-constant format the twin uses, and checked the same way:
     `NAME: value, // path:line`. web/site/check_hero_constants.py re-reads
     each cited line from the C tree and fails if the value has moved off it.
     Keep the format; it is load-bearing, not decoration. */
  var FW = {
    UNLOCK_RANGE_CM: 100,     // apps/esp32-matter-lock/main/app_main.cpp:146
    CHIP_FREQ_HZ: 499200000,  // modules/ultrawidelock_dw3000/dwt_uwb_driver/deca_device_api.h:61
    TICKS_PER_CYCLE: 128,     // modules/ultrawidelock_dw3000/dwt_uwb_driver/deca_device_api.h:59
  };

  /* Speed of light in vacuum, SI exact. Not from the firmware -- it is not
     the firmware's to define. */
  var C_M_S = 299792458;

  /* One timestamp tick is 1/(499.2 MHz x 128) = 15.65 ps, which light crosses
     in 4.6917 mm. That number is the resolution of the whole product. */
  var TICK_S = 1 / (FW.CHIP_FREQ_HZ * FW.TICKS_PER_CYCLE);
  var MM_PER_TICK = TICK_S * C_M_S * 1000;
  var BOUND_M = FW.UNLOCK_RANGE_CM / 100;

  /* What a relay costs its operator. A repeater cannot subtract propagation
     time; it can only add its own. This is a deliberately modest figure --
     even a very fast relay lands outside a one-metre bound. */
  var RELAY_ADD_M = 1.20;

  /* ---- geometry -------------------------------------------------------- */
  /* The lock is the origin of the scale, because the lock is what the
     distance is measured from. The ruler in the SVG is drawn to these exact
     numbers: 0 m at x=84, 3 m at x=648, so 188 user units per metre and a
     tick every half metre. Change one and change the other. */
  var LOCK_X = 96, MAX_X = 888;
  var MIN_M = 0.10, MAX_M = 3.00;
  var PX_PER_M = (MAX_X - LOCK_X) / MAX_M;          /* 264 */
  var MIN_X = LOCK_X + MIN_M * PX_PER_M;            /* 122.4 */

  var el = {
    phone: document.getElementById("hs-phone"),
    rail: document.getElementById("hs-rail"),
    dim: document.getElementById("hs-dim"),
    dimTick: document.getElementById("hs-dim-tick"),
    dimCap: document.getElementById("hs-dim-cap"),
    relay: document.getElementById("hs-relay"),
    lanes: document.getElementById("hs-lanes"),
    laneLines: document.querySelectorAll(".hs-lane-line"),
    metres: document.getElementById("hs-m"),
    ticks: document.getElementById("hs-ticks"),
    nanos: document.getElementById("hs-ns"),
    truth: document.getElementById("hs-true"),
    verdict: document.getElementById("hs-verdict"),
    relayBtn: document.getElementById("hs-relay-btn"),
    scope: svg,
    phaseUwb: document.getElementById("hs-phase-uwb"),
    phaseGate: document.getElementById("hs-phase-gate"),
    phaseMatter: document.getElementById("hs-phase-matter")
  };

  var relayOn = false;

  /* ---- the grant ------------------------------------------------------- */
  /* Crossing inside the bound is the one event here worth marking, so it is
     the only thing that animates on demand. This function's entire job is a
     class on the block that encloses both the diagram and the readout; the
     stamp, the ring and the dimension flash are CSS on that class, so the
     crossing still costs no per-frame JavaScript.
     Seeded from the markup's own data-state, so the page does not celebrate
     its own first paint: an arrival has to be arrived at. */
  var band = svg.closest(".scope-in");
  var lastState = svg.getAttribute("data-state");

  function markState(state) {
    if (state === lastState) return;
    lastState = state;
    if (!band) return;
    band.classList.remove("granted");
    if (state !== "grant") return;
    /* Reading a layout value flushes the removal, so a second crossing
       restarts the animations instead of continuing the first. Once per
       crossing, never during one. */
    void band.offsetWidth;
    band.classList.add("granted");
  }

  function setPhase(node, state) {
    if (!node) return;
    node.classList.toggle("is-done", state === "done");
    node.classList.toggle("is-active", state === "active");
    node.classList.toggle("is-alert", state === "alert");
  }

  function drawPhases(state) {
    setPhase(el.phaseUwb, state === "grant" ? "done" : "active");
    setPhase(el.phaseGate, state === "grant" ? "done" :
      state === "relay" ? "alert" : "active");
    setPhase(el.phaseMatter, state === "grant" ? "active" : "idle");
  }

  function xToM(x) { return (x - LOCK_X) / PX_PER_M; }
  function mToX(m) { return LOCK_X + m * PX_PER_M; }
  function fmt(n, dp) { return n.toFixed(dp); }

  /* ---- render ---------------------------------------------------------- */
  function draw() {
    var trueM = xToM(+slider.value);
    var seenM = trueM + (relayOn ? RELAY_ADD_M : 0);
    var x = mToX(trueM);

    /* The phone and its rail sit at the true position. The dimension line
       spans the *measured* distance, so when the relay is armed you can see
       the measurement overshoot the phone. */
    el.phone.setAttribute("transform", "translate(" + x.toFixed(1) + ",0)");
    el.rail.setAttribute("transform", "translate(" + x.toFixed(1) + ",0)");

    var seenX = Math.min(mToX(seenM), MAX_X + 46);
    el.dim.setAttribute("x2", seenX.toFixed(1));
    el.dimTick.setAttribute("transform", "translate(" + seenX.toFixed(1) + ",0)");
    el.dimCap.setAttribute("x", ((LOCK_X + seenX) / 2).toFixed(1));
    el.dimCap.textContent = fmt(seenM, 2) + " m";

    /* Pulses travel the real gap, so the animation is the geometry. */
    el.lanes.style.setProperty("--travel", (x - LOCK_X).toFixed(1) + "px");
    /* The lanes end where the phone is. Drawn to a fixed width they implied
       the exchange continued past the device it was talking to. */
    for (var i = 0; i < el.laneLines.length; i++) {
      el.laneLines[i].setAttribute("x2", x.toFixed(1));
    }

    if (el.relay) {
      el.relay.style.display = relayOn ? "" : "none";
      if (relayOn) {
        var mid = (LOCK_X + x) / 2;
        el.relay.setAttribute("transform", "translate(" + mid.toFixed(1) + ",0)");
      }
    }

    var ticks = Math.round(seenM * 1000 / MM_PER_TICK);
    var ns = seenM / C_M_S * 1e9;

    el.metres.textContent = fmt(seenM, 2);
    el.ticks.textContent = ticks.toLocaleString("en");
    el.nanos.textContent = fmt(ns, 2);

    var granted = seenM <= BOUND_M;
    var state = relayOn ? "relay" : granted ? "grant" : "hold";
    el.scope.setAttribute("data-state", state);
    markState(state);
    drawPhases(state);

    if (relayOn) {
      el.verdict.textContent = "refused · added delay";
      el.truth.textContent = "phone is really at " + fmt(trueM, 2) +
        " m — the relay could only make that longer";
      el.truth.removeAttribute("hidden");
    } else {
      el.verdict.textContent = granted ? "unlock" : "held · out of range";
      el.truth.setAttribute("hidden", "");
    }

    slider.setAttribute("aria-valuetext",
      fmt(seenM, 2) + " metres measured, " + ticks + " ticks, " +
      (relayOn ? "refused, relay adds delay"
               : granted ? "inside the one metre bound, unlock"
                         : "outside the one metre bound, held"));
  }

  /* ---- drag ------------------------------------------------------------ */
  /* The slider is the real control: it carries the value, the keyboard
     support and the accessible name. Dragging the phone just writes to it,
     so there is exactly one source of truth and no state to keep in step. */
  function pointerToValue(evt) {
    var box = svg.getBoundingClientRect();
    var vb = svg.viewBox.baseVal;
    var x = (evt.clientX - box.left) / box.width * vb.width;
    return Math.max(MIN_X, Math.min(MAX_X, x));
  }

  var dragging = false;
  var target = document.getElementById("hs-hit") || svg;

  target.addEventListener("pointerdown", function (e) {
    dragging = true;
    target.setPointerCapture(e.pointerId);
    svg.classList.add("dragging");
    slider.value = pointerToValue(e);
    draw();
    e.preventDefault();
  });
  target.addEventListener("pointermove", function (e) {
    if (!dragging) return;
    slider.value = pointerToValue(e);
    draw();
  });
  function endDrag(e) {
    if (!dragging) return;
    dragging = false;
    svg.classList.remove("dragging");
    try { target.releasePointerCapture(e.pointerId); } catch (err) {}
  }
  target.addEventListener("pointerup", endDrag);
  target.addEventListener("pointercancel", endDrag);

  slider.addEventListener("input", draw);

  if (el.relayBtn) {
    el.relayBtn.addEventListener("click", function () {
      relayOn = !relayOn;
      el.relayBtn.setAttribute("aria-pressed", relayOn ? "true" : "false");
      el.relayBtn.textContent = relayOn ? "Relay armed" : "Arm a relay";
      el.relayBtn.classList.toggle("btn-warn", relayOn);
      draw();
      /* On a narrow screen the instrument scrolls inside its own frame, so
         bring the measured end into view: the relay's push is the point,
         and it should be seen rather than sit past the edge. */
      if (frame.scrollWidth > frame.clientWidth + 1) {
        var k = svg.getBoundingClientRect().width / svg.viewBox.baseVal.width;
        var end = relayOn ? Math.min(mToX(xToM(+slider.value) + RELAY_ADD_M), MAX_X)
                          : LOCK_X;
        /* Just past the end, and never further than the lock unless the
           end cannot otherwise be shown: the measurement needs its origin. */
        frame.scrollTo({
          left: Math.max(0, (end + 24) * k - frame.clientWidth),
          behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches
            ? "auto" : "smooth"
        });
      }
    });
  }

  /* The frame fades its right edge while there is more instrument past it. */
  var frame = svg.parentNode;
  function edge() {
    frame.classList.toggle("more",
      frame.scrollLeft + frame.clientWidth < frame.scrollWidth - 4);
  }
  frame.addEventListener("scroll", edge, { passive: true });
  window.addEventListener("resize", edge);

  /* ---- first paint ----------------------------------------------------- */
  /* Start just inside the bound: the opening state should be the one the
     product is for, not an error condition. */
  slider.value = mToX(0.82);
  draw();
  edge();
  svg.classList.add("live");

  /* ---- the bar ------------------------------------------------------- */
  /* Part of the opening frame until the reader leaves it. */
  var nav = document.querySelector(".topbar");
  function materializeNav() {
    if (nav) nav.classList.toggle("is-scrolled", window.scrollY > 12);
  }
  window.addEventListener("scroll", materializeNav, { passive: true });
  materializeNav();

  /* ---- the orb --------------------------------------------------------- */
  /* The walk-up in one object. On a device with a fine pointer the cursor is
     the phone: the rings come in as it nears, and the lock opens once the
     distance it stands for crosses the firmware's own unlock bound (BOUND_M,
     from the FW table above), holds, then re-locks after it leaves. Touch
     devices get a timed walk-up instead; reduced motion gets a still lock.
     CSS derives every visual from two things written here: --prox and
     data-phase. */
  var orb = document.getElementById("orb");
  var reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  if (!orb) return;

  var orbLabel = document.getElementById("orb-label");
  var orbRead = document.getElementById("orb-read");
  var FAR_M = 4.20;            /* where a walk-up starts                     */
  var DETECT_M = 4.0;          /* the rings respond inside this              */
  var NEAR_M = 0.82;           /* where a walk-up ends: the scope's opening  */
  var RELOCK_MS = 350;         /* grace after the pointer leaves the bound   */
  var phase = "locked";

  function setOrb(p) {
    if (p === phase) return;
    phase = p;
    orb.setAttribute("data-phase", p);
    var name = p === "unlocked" ? "Unlocked" : p === "ranging" ? "Ranging" : "Locked";
    orbLabel.textContent = name;
    orb.setAttribute("aria-label", "Lock status: " + name.toLowerCase());
  }
  function setProx(v) { orb.style.setProperty("--prox", v.toFixed(3)); }
  function setRead(m) { orbRead.textContent = fmt(m, 2) + " m"; }
  function proxOf(m) {
    return Math.max(0, Math.min(1, (DETECT_M - m) / (DETECT_M - BOUND_M)));
  }

  setRead(FAR_M);
  if (reduced) return;

  /* A walk-up from FAR_M to NEAR_M on an ease-out, as a person slows at a
     door. Unlocks at the bound crossing, not at the end: the bound is the
     decision, not the doorstep. Calls done() once settled. */
  var walkFrame = 0;
  function walk(ms, done) {
    var t0 = 0;
    setOrb("ranging");
    var step = function (now) {
      if (!t0) t0 = now;
      var t = Math.min(1, (now - t0) / ms);
      var m = FAR_M + (NEAR_M - FAR_M) * (1 - Math.pow(1 - t, 3));
      setRead(m);
      setProx(proxOf(m));
      if (m <= BOUND_M) setOrb("unlocked");
      if (t < 1) walkFrame = requestAnimationFrame(step);
      else if (done) done();
    };
    walkFrame = requestAnimationFrame(step);
  }
  function rest() { setOrb("locked"); setProx(0); setRead(FAR_M); }

  var finePointer = window.matchMedia("(hover: hover) and (pointer: fine)").matches;

  if (!finePointer) {
    /* Touch: a gentle loop while the orb is on screen. */
    var loopTimer = 0, onScreen = true;
    var cycle = function () {
      if (!onScreen) return;
      walk(2200, function () { loopTimer = setTimeout(function () {
        rest(); loopTimer = setTimeout(cycle, 2000); }, 2600); });
    };
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (e) {
        var was = onScreen;
        onScreen = e[0].isIntersecting;
        if (onScreen && !was) cycle();
        if (!onScreen) { clearTimeout(loopTimer); cancelAnimationFrame(walkFrame); rest(); }
      }).observe(orb);
    }
    loopTimer = setTimeout(cycle, 900);
    return;
  }

  /* Fine pointer: one walk-up on arrival so the moment is seen without being
     discovered, cancelled the instant the pointer moves. */
  var introTimers = [];
  var intro = true;
  introTimers.push(setTimeout(function () {
    walk(1600, function () {
      introTimers.push(setTimeout(function () { if (intro) rest(); }, 1300));
    });
  }, 650));

  /* Pointer distance from the orb's centre, in orb half-widths, mapped onto
     metres so the orb's edge sits just inside the bound and the outer ring
     just outside it. */
  var px = -1e6, py = -1e6, raf = 0, relock = 0;
  function metresAt() {
    var r = orb.getBoundingClientRect();
    var half = r.width / 2 || 1;
    var d = Math.hypot(px - (r.left + half), py - (r.top + r.height / 2)) / half;
    return Math.max(0.10, Math.min(9.99, (d - 0.62) * 1.7));
  }
  function apply() {
    raf = 0;
    var m = metresAt();
    setProx(proxOf(m));
    setRead(Math.min(m, 9.99));
    if (m <= BOUND_M) {
      clearTimeout(relock); relock = 0;
      setOrb("unlocked");
    } else if (phase === "unlocked") {
      if (!relock) relock = setTimeout(function () {
        relock = 0;
        setOrb(metresAt() < DETECT_M ? "ranging" : "locked");
      }, RELOCK_MS);
    } else {
      setOrb(m < DETECT_M ? "ranging" : "locked");
    }
  }
  function track(x, y) {
    if (intro) {
      intro = false;
      introTimers.forEach(clearTimeout);
      cancelAnimationFrame(walkFrame);
    }
    px = x; py = y;
    if (!raf) raf = requestAnimationFrame(apply);
  }
  window.addEventListener("pointermove", function (e) { track(e.clientX, e.clientY); },
                          { passive: true });
  document.addEventListener("pointerleave", function () { track(-1e6, -1e6); });
  window.addEventListener("blur", function () { track(-1e6, -1e6); });
})();
