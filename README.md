# Strata

A small live-coding language for text-sequenced, monophonic instruments. You type commands into a
terminal (serial, USB, whatever your host wires up) and the instrument reacts immediately -- no
recompile, no reflash.

Strata's core (`strata.h` + `strata.cpp`) has **zero knowledge of any specific instrument**. It
knows how to sequence a melodic "note" track and any number of named numeric "parameter" lanes, and
that's it. A project wires it to a real device by implementing one small interface, `strata::Host`,
that answers three questions: what parameters does this instrument have, what are they called, and
what do I do when one is set. Everything else -- pattern syntax, scheduling, scales, probability,
arithmetic in command arguments -- lives in the core and never changes between projects.

This repository is the language on its own. For a working example of a `Host` implementation wired
to real hardware, see [Archean Live](https://github.com/nakedboards/archean-live) (a synthesizer
firmware built on Strata).

## Why

Most live-coding languages assume a computer running a full OS, talking to a software synth or
sending MIDI. Strata is for the opposite case: a small microcontroller **is** the instrument, and
the language has to run on it directly -- no heap allocation, small, static memory footprint,
predictable timing. It was built first for a patchable analog/digital hardware synthesizer, but
nothing in the core is specific to that.

## Quick taste

```
tempo 96
mel = c3 e3 g3 ~ [c4 e4]*2 <g3 b3> (c3|e3) d3?
play mel | q minor c
lane cutoff "0 .3 .6 .9" | slow 4
```

See [LANGUAGE.md](LANGUAGE.md) for the full syntax reference.

## Using it in a project

1. Copy `strata.h` and `strata.cpp` into your project (Arduino-style builds need the sources inside
   the sketch folder; a CMake/PlatformIO project can just add them as sources).
2. Implement `strata::Host` (see the comments in `strata.h`): `gate()`/`pitch()` for the melodic
   track, `paramCount()`/`paramName()`/`setParam()`/`releaseParam()` for whatever your instrument can
   control, `print()` for your terminal, `reseed()` if you have anything that uses randomness beyond
   the language's own pattern engine.
3. `strata::Interpreter interp(yourHost);` and call `interp.tick(micros())` as often as possible from
   your main loop, and `interp.feed(ch)` for every byte read from your terminal/serial input.

That's the whole integration surface. A minimal host for an instrument with one parameter ("cutoff")
looks like:

```cpp
struct MyHost : strata::Host {
  float cutoff = 0.5f;
  void gate(bool on) override { /* drive your gate output */ }
  void pitch(float midi) override { /* drive your pitch output, clamp to your device's range here */ }
  int paramCount() const override { return 1; }
  const char* paramName(int) const override { return "cutoff"; }
  void setParam(int, float v) override { cutoff = v; /* clamp/apply */ }
  void releaseParam(int) override { /* hand control back to a physical knob, if you have one */ }
  void reseed(uint32_t) override {}
  void print(const char* s) override { Serial.print(s); }
};
```

With that, `cutoff 0.8`, `lane cutoff "0 .5 1"`, and `free cutoff` all work immediately -- the core
never had to be told "cutoff" exists ahead of time.

## Design notes

* **No dynamic allocation.** Every pattern, track, and queued event lives in a fixed-size array
  sized at compile time (`MAX_NODES`, `MAX_NAMED`, `MAX_PARAMS`, `MAX_QUEUE` in `strata.h`). Safe to
  run on a microcontroller with no heap fragmentation risk over long uptimes.
* **The host clamps, not the core.** `setParam()` receives whatever number the person typed (after
  arithmetic evaluation); the core has no idea what range is valid for "cutoff" vs "attack" vs
  "wave", so it's the host's job to clamp or otherwise sanitize. This keeps the core free of any
  per-parameter knowledge.
* **Named values are per-parameter, resolved only where the target is already known.** A parameter
  can accept words in addition to plain numbers (`wave sine` as well as `wave 3`) via
  `Host::paramValues()`. This works in a direct `<parameter> <value>` command and in an inline
  pattern attached straight to a lane (`lane wave "morph land tri"`) -- in both cases the target
  parameter is fixed at the point the word is read, so it resolves through that parameter's own
  table with no ambiguity. It does not work in a *named* pattern (`name = ...`), since that pattern
  isn't tied to any one parameter until it's later attached to a lane.
* **One melodic voice.** The "note" track is singular by design -- this matches many small hardware
  synths (including the one this was first built for), which are themselves monophonic. A project
  wanting polyphony would need a different core; Strata doesn't attempt it.

## Status

Early, unreleased-grade software (first public version). The language surface is reasonably settled;
expect the parameter-table API in `strata::Host` to stay stable, since it's the integration point
other projects build against. See [CHANGELOG.md](CHANGELOG.md).

## License

MIT (matching the rest of the Nakedboards ecosystem).
