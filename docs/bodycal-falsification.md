# BodyCal: the capture that decides whether per-carry-mode numbers exist

BodyCal is one sentence of product: **the door should trigger at the same
distance whether the phone is in a hand, a pocket, or a bag.** The question is
whether that needs a constant per carry mode, or a widening whose size the
installer picks.

The mechanism is in the tree and defaults to doing nothing. This capture decides
what goes in it, and what result would say to put nothing in it at all.

## A falsification, not a calibration

The obvious version of BodyCal was tried and refuted. RESULTS.md Result 19
measured a body in the path as a constant **+84.5 cm**, and the controller
briefly subtracted it to recover a "true" range.
Result 21 repeated that capture with a **second body** and measured **+127.0 cm
[+109, +136]** against the original **+82.0 [+62, +93]**: intervals that do not
overlap. The obstruction replicated cleanly (about **-10 dB** of first-path
power in both sessions), so the classifier is sound and only the number was not.

The **sign** survived; the **magnitude** died. So `ultrawidelock_approach`
widens `unlock_cm` rather than correcting the range, the widening is `0` by
default, and nothing in `modules/` will ever ship a centimetre figure for it.
The full argument is in
`modules/ultrawidelock_cred/include/ultrawidelock_approach.h` around
`range_correct_en` and `nlos_widen_cm`.

Splitting "obstructed" into three carry modes does not repair a magnitude that
failed to replicate across bodies; it asks the same question three times.

## The protocol

### Fixed geometry, one variable

**Ground truth is 40 cm, fixed, and the phone does not move between modes.**

Mount the reader at normal install height. Mark a floor position where the phone
sits **40 cm** from the reader antenna. Use a tripod where the mode allows it;
where it does not (pocket, bag), the *subject* stands on the mark and the phone
sits where that carry mode puts it, with the offset from the mark measured and
recorded, never assumed zero.

40 cm rather than Result 19's 100 cm: the widening is a threshold decision at
`unlock_cm`, whose default is 100 cm, and a measurement at the threshold cannot
distinguish "this class needs widening" from "this class is at the boundary".
40 cm puts every mode's *true* position inside, so only what the radio reports
varies.

The reader stays fixed for the whole session. Result 19's antenna-delay offset
(**25.5 cm**, this board, uncalibrated `DW3000` antenna delay) is common to every
mode and cancels out of a per-mode comparison. Do not correct for it.

### The four modes

| Mode | Class | Placement |
|---|---|---|
| Clear | `ULTRAWIDELOCK_ML_CARRY_CLEAR` | Phone held out, direct line of sight, body out of the path |
| In-hand obstructed | `ULTRAWIDELOCK_ML_CARRY_HAND` | Phone in hand, subject's body between hand and reader |
| Pocket | `ULTRAWIDELOCK_ML_CARRY_POCKET` | Front trouser pocket, screen inward, subject facing away |
| Bag | `ULTRAWIDELOCK_ML_CARRY_BAG` | Shoulder bag or backpack, phone in the main compartment |

**Two subjects minimum, different people.** Result 21: one body produced a
number that looked measured and was not. A per-class figure that does not
replicate across subjects is not a per-class figure. Two is the floor, not the
target.

**Both phone orientations per mode where the mode permits one.** Result 9 found
`d = +0.06` on `fp_pwr` between held-behind-back and back-pocket, which is
nothing, but those were two geometries of one obstruction class; pocket versus
bag is the question here.

### How much to capture

Result 19 worked from **399** tripod receptions, enough to separate clear from
obstructed at a 47 cm interquartile spread. The per-class question is finer, so
aim for **at least 150 receptions per (mode, subject) cell**: 8 cells, roughly
1,200 receptions, about 4 minutes of ranging per cell at the ~192 ms block rate.

Capture each cell as one continuous run and record the cell boundaries. Holding
out a whole capture is the only split that means anything; a random split across
one run measures autocorrelation, not generalisation. That is how the shipped
model's honest score (**0.7729** held-out-capture against **0.8800** on the
mixed split) was obtained.

## Pass and fail

**Compute, per (mode, subject) cell:** the residual `reported_cm - 40`, its
median, and its standard deviation.

### PASS: per-class constants are supportable

Both of these must hold:

1. **Per-class residual std ≤ ~15 cm**, in every cell.
2. **The per-class medians replicate across subjects**: the two subjects'
   medians for the same mode agree within roughly one std of each other.

Result 19's obstructed captures had interquartile spreads of **38** and **47.5**
cm against **7** and **11.5** for clear; a spread in that range is wider than
the margin any threshold decision defends, so a constant correction would move
noise rather than remove bias. 15 cm is roughly the widest spread at which a
per-class constant is worth more than the error it introduces.

On a pass, fill `cfg.nlos_widen_class_cm[]` per install from that install's own
medians. A pass means per-class constants are a *kind of thing that exists*, not
that these numbers are portable: the 25.5 cm antenna offset alone is this board
and this antenna, and session drift of about 2.9 dB on `fp_pwr` an hour apart is
on record.

### FAIL: widen-only, and the table stays at zero

If either condition fails, and **failure is the outcome Result 21 predicts**:

- Leave `nlos_widen_class_cm[]` at its `0` default. Every entry falling through
  to `nlos_widen_cm` is exactly the behaviour that shipped.
- Keep using the **sign**: the window says obstructed, the threshold widens, the
  installer picks the number by walking their own door.
- **Do not** re-open the subtraction. Do not feed a corrected range into the
  Kalman filter. The refutation stands until a capture overturns it, and this
  capture failing is not that.

A partial result, say pocket and bag replicating but in-hand not, is a fail for
in-hand and a pass for the other two. The table is per class, so tune what
replicated and leave the rest at `0`.

## How the data gets there

### Capture: the `[FEAT]` CSV

The reader emits a `[FEAT]` line per reception from
`apps/dwm3001cdk-lock/src/ml_feed.c`, header row first, under `make mlgate`. The
header, in order:

```
[FEAT] t_ms,n,cm,cls,conf_c,dis,fp_resid_c,rx_pwr_c,fp_pwr_c,delta_p_c,
       fp_peak_c,fp_idx_q6,peak_idx,peak_amp,f1,f2,f3,area,acc
```

Every dB column is the value times 100 as an integer: printing floats needs
`CONFIG_CBPRINTF_FP_SUPPORT`, a global switch that grows every printf in an
image whose only console is an 8 KB RTT ring. `fp_idx_q6` is the first-path index
in Q10.6, handed over unconverted: its integer and fractional halves answer
different questions.

**The carry-mode label is not on the line and cannot be.** Nothing on the device
knows which pocket the phone was in; the operator supplies it per cell.

The columns BodyCal needs, all carried by the line:

- The five Ipatov CIA registers, exactly as `struct ultrawidelock_ml_cia` names
  them: `f1`, `f2`, `f3`, `acc`, `area`.
- The reported range in centimetres, `cm`.
- `delta_p_c`, which is `rx_pwr - fp_pwr` in centi-dB, for the vendor rule and
  the disagreement counter `dis`.
- Two columns the shipped model does not read and a fitted one might:
  `fp_idx_q6` and `fp_peak_c`, the first-path-to-peak ratio. **No noise floor**,
  not an oversight: the DW3000's diagnostic struct has no noise member this repo
  can name, so the raw peak, `f1..f3`, `area` and `acc` ship instead, and the
  column can be derived offline once its source is established.

**The range lags by a round, correctly.** A DS-TWR round yields its distance when
the round completes, while diagnostics are read per reception, so a caller
classifying every reception holds the previous round's range. The shipped model
was trained on that lag. Do not "fix" it in the capture: a de-lagged capture
trains a model the firmware cannot reproduce.

### Training: the external `tinyml` repo

`tinyml/gen_model.py` consumes the `[FEAT]` CSV and emits the generated headers.
That repo is **not** vendored here. A four-class run must emit, beside the
existing pair:

- `ultrawidelock_ml_carry_tree.h`, defining
  `ultrawidelock_ml_carry_tree_predict(const int16_t *features, int32_t features_length)`
  returning a leaf index in `[0, 4)` in the enum's order.
- `ultrawidelock_ml_carry_scaler.h`, defining `ultrawidelock_ml_carry_lo[]` and
  `ultrawidelock_ml_carry_scale[]`.

Both go in `modules/ultrawidelock_ml/src/`. **That is the entire integration.**
`ultrawidelock_ml_los.c` picks them up with `__has_include`, needing no edit
there, in `CMakeLists.txt` or in `Kconfig`, and
`ultrawidelock_ml_los_carry_trained()` flips to `true` on its own.

**Class order is part of the model.** The generator emits leaf indices, not names,
so training labels must be encoded `clear=0, hand=1, pocket=2, bag=3`.
`ultrawidelock_approach.c` static-asserts its mirror of the enum against
`ultrawidelock_ml.h`, but nothing can check the *generator's* label encoding from
C: a reordered training label compiles and hands a pocket the bag's widening.

The generator's own gates are weaker than they read: sized for the public set's
42,000 samples, they run on 544. They prove the generated C classifies
identically to the trained model, over very little ground.

## What is in the tree today

| Piece | Where | State |
|---|---|---|
| Carry-class enum and fold | `modules/ultrawidelock_ml/include/ultrawidelock_ml.h` | Shipped |
| `ultrawidelock_ml_los_carry_classify()` | `modules/ultrawidelock_ml/src/ultrawidelock_ml_los.c` | Shipped, **binary underneath** |
| Four-class model pickup | same file, `__has_include` seam | Waiting on this capture |
| `nlos_widen_class_cm[]` | `modules/ultrawidelock_cred/include/ultrawidelock_approach.h` | Shipped, all entries `0` |
| `ultrawidelock_approach_feed_carry()` | `modules/ultrawidelock_cred/src/ultrawidelock_approach.c` | Shipped |
| Host coverage | `tests/host/test_approach.c`, group `per-carry-mode widening (BodyCal)` | Shipped |
| `[FEAT]` CSV emitter | `apps/dwm3001cdk-lock/src/ml_feed.c` | Shipped, `make mlgate` only |
| First-path index and peak | `modules/ultrawidelock_uwb/include/uwb_cirdiag.h` | Shipped. Free: they come out of a burst whose length was already fixed |
| `ultrawidelock_ml_los_diag()` | `modules/ultrawidelock_ml/src/ultrawidelock_ml_los.c` | Shipped, capture only. No decision reads it |
| The capture itself | nobody has run one | **This is the missing step** |

**The shipped model is two-class.** `ultrawidelock_ml_los_carry_classify()`
returns only `CLEAR` or `HAND`, so `POCKET` and `BAG` table entries are
unreachable and an installer tuning them is tuning nothing. Ask
`ultrawidelock_ml_los_carry_trained()` before offering them in any UI.

All of it is behind `CONFIG_ULTRAWIDELOCK_ML_LOS`, off by default, so an image
that does not ask for the classifier is byte-identical to the one that shipped
before any of this existed.

## The gates that do not move

Whatever this capture returns, these hold:

1. The widening only ever **adds** permission at the threshold comparison. It
   never touches the range, and the estimator never sees a corrected number.
2. Three gates stay in front of it: classified obstructed, confidence over
   `nlos_conf_min`, and a **strict majority of the five-sample window** whose
   votes age out at `MEDIAN_STALE_MS`. Per-class widening rides those same
   three; it does not get its own.
3. Each entry is clamped so `unlock_cm + entry` stays strictly under
   `approach_cm`. Without it a widened sample would arm the trajectory gate and
   fire it on the same sample, which is the hole `574dbb91` closed.
4. A tie in the carry vote resolves to the **smallest** widening. A window that
   cannot identify the geometry gets the stricter threshold.
5. Widening is **not** a safety improvement. Widening to 220 opens for a
   misclassified walk-by at 220 exactly as subtracting 120 would. The benefit is
   narrower: the estimator stays clean, and a policy number invites the tuning it
   needs where a measured-looking constant invites unearned trust.
