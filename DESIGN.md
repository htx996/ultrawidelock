---
name: UltraWideLock
description: A product page whose hero is a working lock that ranges the visitor and opens at the firmware's own bound.
colors:
  ground: "#ffffff"
  surface: "#ffffff"
  card: "#ffffff"
  panel: "#f5f5f7"
  raise: "#f2f2f5"
  ink: "#3a3a3f"
  strong: "#1d1d1f"
  muted: "#56565c"
  faint: "#6e6e73"
  inverse: "#ffffff"
  line: "rgba(0,0,0,.1)"
  hairline: "rgba(0,0,0,.065)"
  line-strong: "rgba(0,0,0,.2)"
  accent: "#00a383"
  accent-ink: "#007a62"
  accent-hover: "#006b56"
  accent-solid: "#00806a"
  warn: "#9a5b00"
  ok: "#0d7a4d"
  danger: "#c41e3a"
  focus: "#00806a"
  glow-a: "rgba(0,214,166,.3)"
  glow-b: "rgba(64,160,255,.28)"
  ground-dark: "#0c0e10"
  surface-dark: "#111416"
  card-dark: "#171b1e"
  raise-dark: "#1d2224"
  sunken-dark: "#08090b"
  ink-dark: "#d6dbd9"
  strong-dark: "#f2f4f3"
  muted-dark: "#a8b0ad"
  faint-dark: "#8c9491"
  inverse-dark: "#04100c"
  accent-dark: "#2ee6b8"
  accent-ink-dark: "#5fecc4"
  accent-hover-dark: "#8ff5d8"
  warn-dark: "#ffb340"
  ok-dark: "#4cd38a"
  danger-dark: "#ff6961"
  focus-dark: "#7ff0d4"
  glow-a-dark: "rgba(46,230,184,.2)"
  glow-b-dark: "rgba(72,150,255,.17)"
typography:
  display:
    fontFamily: "-apple-system, BlinkMacSystemFont, \"Segoe UI Variable Text\", \"Segoe UI\", Roboto, \"Helvetica Neue\", Helvetica, system-ui, sans-serif"
    fontSize: "clamp(2.75rem, 8vw, 5.25rem)"
    fontWeight: 700
    lineHeight: 1.02
    letterSpacing: "-0.04em"
  headline:
    fontFamily: "-apple-system, BlinkMacSystemFont, \"Segoe UI Variable Text\", \"Segoe UI\", Roboto, \"Helvetica Neue\", Helvetica, system-ui, sans-serif"
    fontSize: "clamp(2.1rem, 5vw, 3.4rem)"
    fontWeight: 700
    lineHeight: 1.05
    letterSpacing: "-0.035em"
  title:
    fontFamily: "-apple-system, BlinkMacSystemFont, \"Segoe UI Variable Text\", \"Segoe UI\", Roboto, \"Helvetica Neue\", Helvetica, system-ui, sans-serif"
    fontSize: "1.375rem"
    fontWeight: 700
    lineHeight: 1.2
    letterSpacing: "-0.022em"
  lede:
    fontFamily: "-apple-system, BlinkMacSystemFont, \"Segoe UI Variable Text\", \"Segoe UI\", Roboto, \"Helvetica Neue\", Helvetica, system-ui, sans-serif"
    fontSize: "clamp(1.04rem, 1.7vw, 1.2rem)"
    fontWeight: 400
    lineHeight: 1.55
    letterSpacing: "-0.011em"
  body:
    fontFamily: "-apple-system, BlinkMacSystemFont, \"Segoe UI Variable Text\", \"Segoe UI\", Roboto, \"Helvetica Neue\", Helvetica, system-ui, sans-serif"
    fontSize: "1rem"
    fontWeight: 400
    lineHeight: 1.65
    letterSpacing: "-0.011em"
  label:
    fontFamily: "-apple-system, BlinkMacSystemFont, \"Segoe UI Variable Text\", \"Segoe UI\", Roboto, \"Helvetica Neue\", Helvetica, system-ui, sans-serif"
    fontSize: "0.75rem"
    fontWeight: 500
    lineHeight: 1.5
    letterSpacing: "-0.011em"
  mono:
    fontFamily: "ui-monospace, \"SF Mono\", \"JetBrains Mono\", \"Cascadia Mono\", Menlo, Consolas, monospace"
    fontSize: "0.8125rem"
    fontWeight: 400
    lineHeight: 1.6
    letterSpacing: "0"
rounded:
  xs: "4px"
  sm: "8px"
  md: "12px"
  lg: "18px"
  xl: "28px"
  pill: "999px"
spacing:
  sp-1: "0.235rem"
  sp-2: "0.47rem"
  sp-3: "0.705rem"
  sp-4: "0.94rem"
  sp-5: "1.176rem"
  sp-6: "1.41rem"
  sp-8: "1.88rem"
  sp-10: "2.35rem"
  sp-12: "2.82rem"
  sp-16: "3.76rem"
  sp-20: "4.7rem"
  sp-28: "6.58rem"
  sp-40: "9.4rem"
components:
  button-primary:
    backgroundColor: "{colors.accent-solid}"
    textColor: "{colors.inverse}"
    rounded: "{rounded.pill}"
    padding: "0.47rem 1.176rem"
    height: "2.45rem"
  button-primary-hover:
    backgroundColor: "{colors.accent-hover}"
    textColor: "{colors.inverse}"
  button-primary-lg:
    backgroundColor: "{colors.accent-solid}"
    textColor: "{colors.inverse}"
    rounded: "{rounded.pill}"
    padding: "0.705rem 1.41rem"
    height: "2.95rem"
  button-secondary:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.strong}"
    rounded: "{rounded.pill}"
    padding: "0.47rem 1.176rem"
    height: "2.45rem"
  button-secondary-hover:
    backgroundColor: "{colors.raise}"
    textColor: "{colors.strong}"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.muted}"
    rounded: "{rounded.pill}"
    padding: "0.47rem 1.176rem"
  pill:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.muted}"
    typography: "{typography.mono}"
    rounded: "{rounded.pill}"
    padding: "0.235rem 0.705rem"
  navlink:
    textColor: "{colors.muted}"
    rounded: "{rounded.pill}"
    padding: "0.235rem 0.705rem"
  navlink-active:
    backgroundColor: "{colors.raise}"
    textColor: "{colors.strong}"
  card:
    backgroundColor: "{colors.card}"
    rounded: "{rounded.lg}"
    padding: "1.41rem"
  panel:
    backgroundColor: "{colors.panel}"
    rounded: "{rounded.xl}"
    padding: "clamp(1.1rem, 3vw, 2.2rem)"
  field:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.ink}"
    rounded: "{rounded.md}"
    padding: "0.47rem 0.705rem"
---

# Design System: UltraWideLock

## Overview

**Creative North Star: "The Lock That Opens At The Bound"**

An Apple-grade product page on neutral ground: white with `#f5f5f7` panels by day, a cool charcoal by night. The page is quiet so that one thing can speak: a mint signal that means a trusted measurement and an open lock. Everything visual that moves is a ranging pulse, an impulse leaving an anchor and decaying outward, because that is what the firmware does. The hero is a working lock orb that treats the pointer as a phone and opens at the firmware's own unlock bound (`UNLOCK_RANGE_CM`, 100 cm, read by `hero.js`).

Theme follows the OS (`prefers-color-scheme`); `data-theme` on `<html>` overrides it; `.tone-dark` forces a dark band locally inside a light page. The theme toggle does not cut, it sweeps: a view transition reveals the new theme as one expanding circle from the button, its edge tinted with the accent.

This world explicitly refuses the dark instrument-dashboard look of the retired "Drawn To Scale" system. Instruments still exist (the DS-TWR scope, the twin), but they sit inside soft panels on a light page, not as the page itself.

**Key Characteristics:**
- Neutral ground in both themes; one mint signal, one amber counter-signal, nothing else chromatic outside status.
- Platform SF type with tight display tracking; monospace only for measurements, identifiers and code.
- Soft, object-like forms: 28px panels, 18px cards, capsule controls.
- Depth from tonal panels, faint shadows and two slow blurred lights, never from heavy drop shadows.
- One physics: exponential ease-out arrivals, spring presses, ranging rings that decay.

## Colors

Neutral grey ground with a single mint signal and an amber counter-signal; every chromatic value carries a measurement meaning.

### Primary
- **Ranging Mint** (`accent`, `accent-dark`): the signal. Open lock, trusted range, the first-path trace, the orb at the bound, the `.lit` second line of display headings, ranging rings. On white it is a mark colour (3.2:1), so it appears as text only at display size.
- **Deep Mint Ink** (`accent-ink`, `accent-ink-dark`): the accent as readable text: links, arrow links, verdict text, active TOC entries, numbered step badges.
- **Solid Mint** (`accent-solid`): the accent as a filled background. Its own token because themes disagree about what is safe: white on `#00806a` holds 4.9:1 on light; on dark the fill is the bright mint carrying `inverse-dark` at 12.1:1.
- **Mint Hover** (`accent-hover`, `accent-hover-dark`): darker on light, lighter on dark; hover for links and the primary button.

### Secondary
- **Relay Amber** (`warn`, `warn-dark`): the late path. A relay's added delay, an obstructed or suspect measurement, the relay button and its note. It is never decorative.

### Tertiary
- **Sky Light** (`glow-b`, `glow-b-dark`): the second of the two atmosphere lights, a cool blue blurred blob paired with the mint light (`glow-a`). Atmosphere only; never on text, borders or controls.

### Neutral
- **Paper White** (`ground`, `surface`, `card`): page ground, the top bar's vibrancy base, card fill on light.
- **Apple Light Grey** (`panel`, `#f5f5f7`, also `sunken` on light): panels set into the page (the scope, feature visuals, tiles, next-step links).
- **Raised Grey** (`raise`): hover fills, active nav capsule, inline code.
- **Cool Charcoal** (`ground-dark`, `surface-dark`, `card-dark`, `raise-dark`, `sunken-dark`): the night ground in five steps; on dark `panel` equals `surface-dark`, one step off the ground.
- **Graphite text** (`strong`, `ink`, `muted`, `faint` and their `-dark` pairs): headings, body, secondary copy, captions. `faint` holds at least 4.5:1 on its ground in both themes.
- **Hairlines** (`hairline`, `line`, `line-strong`): translucent black on light, translucent white on dark (`rgba(255,255,255,.06/.1/.2)`). Panels take `hairline`; cards and controls take `line`.
- **Status** (`ok`, `danger` and pairs): success and failure in tools (flasher, callouts, `btn-danger`). Not part of the ranging vocabulary.

### Named Rules
**The Two Signals Rule.** `--path-first` (aliasing the accent) is a direct, trusted first path and an open lock; `--path-late` (aliasing warn) is a late, obstructed or relayed path. Anywhere the site distinguishes a good measurement from a suspect one it uses this pair and only this pair, with the same meaning on the landing hero, in the twin and in the guides. A trace takes one state from one attribute, so it is never half mint and half amber.

**The Mark-Not-Text Rule.** On light, `accent` is a mark colour (dots, rings, strokes, display-size `.lit` text at 3.2:1). Body-size text in the signal colour uses `accent-ink`; fills that carry text use `accent-solid`.

**The Neutral Ground Rule.** Everything that is not a measurement is grey. The signal reads as signal because nothing around it competes.

## Typography

**Display Font:** the platform UI face (SF Pro on Apple devices, Segoe UI Variable, Roboto) via `-apple-system, BlinkMacSystemFont, …, system-ui, sans-serif`
**Body Font:** the same stack
**Label/Mono Font:** `ui-monospace, "SF Mono", "JetBrains Mono", …` with JetBrains Mono self-hosted (latin subset, `fonts/jetbrains-mono.woff2`) as the fallback where the system has no good monospace

**Character:** the page is set in the same face as the Wallet sheet the key lives in, so on the devices this firmware pairs with it reads as native. Display is bold and tightly tracked; body is slightly negative-tracked for an Apple feel. The root size is 17px, so `1rem` is 17px everywhere.

### Hierarchy
- **Display** (700, `clamp(2.75rem, 8vw, 5.25rem)`, 1.02, -0.04em): the hero claim, two lines, second line in mint (`.lit`); the closer uses the same treatment at `clamp(2.4rem, 6vw, 4.25rem)`. Steps down to `clamp(2.75rem, 7vw, 4.6rem)` on laptop heights so the CTA stays above the fold.
- **Headline** (700, `clamp(2.1rem, 5vw, 3.4rem)`, 1.05, -0.035em): section and feature headings (features use `clamp(2rem, 4.2vw, 3.1rem)`). Docs `h1` uses the token scale (`--fs-900`, 2.5rem) with the same display tracking.
- **Title** (700, 1.375rem, 1.2, -0.022em): docs `h3`; tile and board titles run larger in bold at -0.03em.
- **Lede** (400, `clamp(1.04rem, 1.7vw, 1.2rem)`, 1.45 to 1.6): the paragraph under every display or headline, in `muted`, held to 30 to 40ch or rem.
- **Body** (400, 1rem / 17px, 1.65, -0.011em): prose, capped at the `--measure` of 46rem.
- **Label** (500, 0.75rem or 0.735rem): status pills, scope header, fine print, captions.
- **Mono** (400, 0.8125rem, 1.6): code, version tags, chip names, numbered steps, every number in the instrument, readouts at `--fs-700` with `tabular-nums`.

### Named Rules
**The Platform Face Rule.** No webfont for prose or display. The system UI face is the brand face; only the monospace fallback is self-hosted, and never from a third party.

**The Measurements Are Mono Rule.** Monospace marks things that are measured or literal: distances, times, percentages, identifiers, code. Prose, headings and button labels stay in the sans. Figures set in sans (the facts row, the flasher percentage) still take `tabular-nums`.

## Layout

A single centred column with a `--page-max` of 70rem and `--sp-8` (1.88rem) gutters (`--sp-5` below 44rem). The spacing scale steps on a 0.235rem (about 4px at the 17px root) unit, from `--sp-1` to `--sp-40`.

The landing runs as full-height chapters: a `100svh` centred hero, sections padded `clamp(5rem, 11vw, 8.5rem)` top and bottom, features at `clamp(4.5rem, 9vw, 7rem)`. Features are editorial pairs, copy beside a visual (`1fr / 1.12fr`), swapping sides on `.flip` and stacking below 64rem. Sibling features are separated by a hairline that fades out at both ends. Rows of facts and boards are divided by vertical hairlines that become horizontal on a phone.

The instrument panel is wider than any text column (max 64rem) because it is drawn to scale; below its natural width it keeps a 38rem minimum and scrolls inside its own frame.

The docs are a two-column app: a 17.5rem sidebar beside a `--measure` (46rem) article and a 14rem TOC rail. Below 64rem the sidebar collapses behind a toggle and the TOC is dropped. The top bar is 3.5rem; the nav links become a sheet below 52rem.

**The Dark Band Rule.** `.tone-dark` re-resolves every token against the night palette inside a light page. It is used for the get-started terminal band, inset from the viewport edge with `clamp(1.5rem, 3vw, 2.5rem)` corners and a faint mint radial at its top. It is the one place the page is a terminal.

## Elevation & Depth

Tonal first, shadows faint. Panels are a grey step off the page with a hairline, not lifted. Shadows exist in three steps plus an inset highlight, and are much heavier on dark only because charcoal swallows them. The signature depth is atmosphere: two large blurred lights (mint `glow-a`, sky `glow-b`, `blur(48px)`) drifting slowly behind the hero and the closer, feathered top and bottom by a mask so they never end on a line.

### Shadow Vocabulary
- **Rest** (`--shadow-1`: `0 1px 2px rgba(0,0,0,.05)` light): buttons, cards, the orb status pill.
- **Float** (`--shadow-2`: `0 1px 2px rgba(0,0,0,.04), 0 12px 32px -14px rgba(0,0,0,.16)` light): the mobile nav sheet, icon tiles, the flasher card.
- **Lift** (`--shadow-3`: `0 2px 6px rgba(0,0,0,.04), 0 30px 70px -28px rgba(0,0,0,.24)` light): hovered tiles, product shots, the terminal windows.
- **Inset highlight** (`--shadow-inset`: `inset 0 1px 0 rgba(255,255,255,.8)` light): a top-edge highlight on cards.
- **Signal bloom** (`0 8px 28px -10px var(--glow-a)`, growing to `0 14px 38px -10px` on hover): under the primary button, the lock icon, the orb body (`0 30px 80px -30px` of the orb colour).

### Named Rules
**The Bloom Not Shadow Rule.** Things in the signal colour cast light of their own colour, not grey shadow. A primary action glows mint beneath it.

**The Vibrancy Over Atmosphere Rule.** Backdrop blur is reserved for chrome that floats over the atmosphere or the page: the scrolled top bar (`saturate(180%) blur(20px)` over 72% surface), the hero pill and orb status (`blur(12px)`), the orb body (`blur(18px)`). Each has a solid-surface fallback under `prefers-reduced-transparency`. Blurred layers move by translate only; scaling one re-rasterises the blur every frame.

## Shapes

Soft and continuous, the way the hardware it pairs with is shaped: small parts stay crisp, panels read as objects.

- **Panels** (28px, `rounded.xl`): the scope, feature visuals, tiles. Shrinks to 18px on a phone.
- **Cards and windows** (18px, `rounded.lg`): cards, feature links, next-step links, the terminal, the flasher card, the command palette.
- **Fields and blocks** (12px, `rounded.md`): inputs, code blocks, tables, readout cells.
- **Small parts** (4 to 8px): kbd, inline code, callouts, focus ring, icon badges.
- **Capsules** (`rounded.pill`): every button, nav link, pill, verdict and status chip, sliders.
- **Circles**: ranging rings, status dots, step numbers, the atmosphere lights. The orb body is a squircle (30% radius).

Borders are 1px hairlines; the 3px `--bw-rule` is used only for callout and blockquote left edges and coverage bars.

## Components

### Buttons
Capsules that compress on a spring.
- **Shape:** full capsule (999px), min height 2.45rem (`btn-sm` 1.95rem, `btn-lg` 2.95rem).
- **Primary:** `accent-solid` fill, `inverse` text, weight 500, no border, carrying the signal bloom.
- **Hover / Focus:** primary lifts 1px and scales to 1.015 with a larger bloom and `accent-hover` fill; every button presses to `scale(.96)` on `--ease-spring` over 240ms. Focus is the global ring: a 2px ground-coloured gap then a 4px `focus` ring, with a transparent outline kept for forced-colours mode.
- **Secondary:** surface fill, `line` border, `strong` text, rest shadow; hover goes to `raise` with `line-strong` border.
- **Ghost:** transparent and `muted`, `raise` on hover. **Warn** (relay) and **Danger** take the amber or red tint, line and text.
- **Arrow link:** the tertiary action, medium weight in `accent-ink` (`strong` in the hero) with a stroked arrow that moves 3px toward where it goes on hover.

### Chips
- **Pill:** capsule, `line` border, surface fill, mono 0.75rem in `muted`; the accent pill takes `accent-tint` fill, `accent-line` border, `accent-ink` text.
- **Status pill:** sans label with a dot (7px) in the state colour, translucent surface with backdrop blur.

### Cards / Containers
- **Corner Style:** 18px cards, 28px panels.
- **Background:** `card` for cards, `panel` (`#f5f5f7` / charcoal step) for panels.
- **Shadow Strategy:** cards at rest shadow plus inset highlight; panels flat with a `hairline` border; tiles lift 4px to the Lift shadow on hover over 900ms `--ease-out-expo`.
- **Border:** `line` on cards, `hairline` on panels.
- **Internal Padding:** `--sp-6` (1.41rem) on cards; `clamp(1.6rem, 3.5vw, 2.5rem)` on tiles and visuals.

### Inputs / Fields
- **Style:** `ground` fill, `line` border, 12px radius, `--fs-400`.
- **Range slider:** drawn, not native: a 4px capsule track in `line-strong`, the travelled part in the trace colour, a 1.25rem white thumb with a soft shadow.
- **Focus:** the global focus ring.

### Navigation
A sticky 3.5rem top bar. On the landing it starts transparent as part of the opening frame and becomes vibrancy (72% surface, saturate and blur, hairline bottom) once scrolled. The wordmark is the ranging mark (an anchor and two decaying arcs, in mint) beside the name in semibold. Links are medium-weight capsules in `muted`, `strong` on hover, `raise` fill when current. Below 52rem the links become a sheet that drops 4px and scales from .98 on the shared overlay curve; the current link there takes the accent tint.

### Lock Orb (signature)
The hero lock, ranging. `hero.js` writes `--prox` (0 far, 1 at the bound) and `data-phase`; the orb colour is mixed in OKLab from `muted` toward the accent as the pointer-phone closes, and becomes the accent outright when granted. Three slow rings and two faster near-field rings (whose strength is their opacity, so the loop never restarts) pulse out from a blurred squircle body holding a stroked lock glyph; the shackle swaps to open at the bound and the body settles from 1.06 on a spring. A status pill under it reads the state and the distance in tabular figures. The whole orb floats 12px on a 7s cycle.

### DS-TWR Scope (signature)
The live instrument in a 28px panel: an SVG drawn to scale with mono labels and numbers, a dashed mint bound line, pulses that travel the real gap, a slider alternative to drag, three readout cells and a capsule verdict whose colour follows the single `data-state` attribute (grant mint, hold muted, relay amber). Crossing inside the bound fires a ring, a verdict stamp and a dimension flash.

### Motion
**The One Physics Rule.** Every arrival uses `--ease-out-expo` (`cubic-bezier(.16,1,.3,1)`). Scroll reveals rise 28px out of an 8px blur over 900ms with a 90ms stagger; presses and grants overshoot on `--ease-spring`; released things coast on `--ease-decel`. Rings always expand and fade, never contract. Under `prefers-reduced-motion` durations collapse, decorative loops stop, and the instrument parks its pulses mid-flight so the diagram still reads.

## Do's and Don'ts

### Do:
- **Do** use mint only for a trusted measurement, an open lock, or the ranging pulse, and amber only for the late path or a relay's delay (The Two Signals Rule).
- **Do** set body-size signal text in `accent-ink` and filled signal surfaces in `accent-solid`; reserve raw `accent` for marks and display-size `.lit` lines.
- **Do** put instruments and visuals in 28px `panel` containers with a hairline, and cards at 18px.
- **Do** make every button a capsule, and give the primary its mint bloom rather than a grey shadow.
- **Do** give every backdrop blur a solid fallback under `prefers-reduced-transparency` and every loop a reduced-motion state.
- **Do** read product numbers (the unlock bound, timings) from the firmware's constants rather than inventing them in the page.

### Don't:
- **Don't** return to the dark instrument-dashboard look as the page itself; dark is the night theme and the terminal band, not the identity.
- **Don't** introduce a third chromatic signal, or use mint or amber as decoration.
- **Don't** set prose, headings or button labels in monospace.
- **Don't** load prose or display webfonts, or any font from a third-party host.
- **Don't** place uppercase kickers or eyebrows above headings; uppercase tracked caps are for small data labels (table heads, result kinds) only.
- **Don't** scale a blurred layer; translate it.
