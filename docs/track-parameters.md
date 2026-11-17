# Track parameter words

What the 58 per-track parameter words of [track control](api.md#track-control) mean.
The SDK exposes them by index only: `opxy::control::track_parameters` is 58 and
`Input::values[track][index]` and `control::add(overlay, track, index, value)` take a
bare index. [`api.md`](api.md) says the indices 0..7 are the engine's eight controls
and the rest are "stock's own track parameters, in stock's units", and that the SDK
does not tell a machine a parameter's label, range or type. A machine that overlays
stock's words, such as Omnifilter (`enum { kCutoff = 16, kResonance = 17, kAttack = 8,
kRelease = 11 }`), takes its indices from the stock firmware; nothing in the SDK
defines them.

This page collects what [`abi-1.2.1.md`](abi-1.2.1.md) and the stock preset layout
establish. It is a reading of OS 1.2.1, not a manufacturer specification.

## Confirmed by the ABI notes

| Words | Meaning | Where it is stated |
| --- | --- | --- |
| 0..7 | The engine's eight controls; Shift selects 4..7 | `api.md`, [alternate controls](abi-1.2.1.md#alternate-controls) |
| 8..11 | Amp envelope: attack, decay, sustain, release | [Screens](abi-1.2.1.md#screens): stock's envelope page draws words 8..11 as the amp envelope |
| 12 | Play mode | [Synthesis drum transport](abi-1.2.1.md#synthesis-drum-transport): stock's drum note on and off read it |
| 15 | The track's engine volume | [Per-key sends](abi-1.2.1.md#per-key-sends): record word 15 |
| 16..19 | Track filter: cutoff, resonance, envelope amount, key amount | [The track filter](abi-1.2.1.md#the-track-filter-on-kit-voices); stock's benchmark writes cutoff and resonance to words 16 and 17 |
| 22, 23 | FX I and FX II send knobs | [Per-key sends](abi-1.2.1.md#per-key-sends) |
| 32..35 | Filter envelope: attack, decay, sustain, release (M2 with Shift) | [Screens](abi-1.2.1.md#screens), [step locks of a kit's Shift fields](abi-1.2.1.md#step-locks-of-a-kits-shift-fields) |
| 36, 37 | Unused by stock. The SDK keeps Shift rows and [step-lock handles](abi-1.2.1.md#step-lock-handles) here | the same sections |
| 38 | Note-start pan term, centre `0x40000000` | [Step locks of a kit's Shift fields](abi-1.2.1.md#step-locks-of-a-kits-shift-fields) |

Omnifilter's four constants are all in this table: 16 cutoff, 17 resonance, 8 amp
attack and 11 amp release.

## Other words noted in the ABI notes

- 20 and 21 are terms of the track level (`(1 - w20 (1 - m)) (1 - w21 (1 - g))`).
- Stock's aux controller edits 24..27, and the track menu's scramble randomizes 24..31.
- On aux tracks stock keeps defaults in 29..33. Words 39..41, 48 and 55..57 are read by
  the drum renderer, and word 46 has GUI readers.
- Word 57 carries the SDK's mark while a machine holds aux track 12, and is never a
  stored value ([aux tracks](abi-1.2.1.md#aux-tracks)).
- Only words 0..41 have a per-step lock slot. A lock stores the top 16 bits of the word.

## Not confirmed

A stock preset's JSON groups its settings as `engine.params[8]`, `envelope.amp`,
`envelope.filter`, `fx.params[8]` and `lfo.params[8]`. That suggests 16..23 are the
filter group and 24..31 the LFO group, but the ABI notes do not say so. Not checked:
words 13, 14, 20 and 21 individually, 24..31, 39..57, and the units of each word. Every
live value is non-negative; the engine's controls run 0..32767, and stock's own words are
in stock's units.

## Gap

A machine that overlays stock's words has to rediscover this table. The SDK could define
named constants for the confirmed words and list them in `api.md`.
