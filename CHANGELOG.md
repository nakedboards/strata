# Changelog

Strata follows [Semantic Versioning](https://semver.org/) once it reaches 1.0: breaking changes to
the `strata::Host` interface or the command syntax bump the major version; new, backward-compatible
commands or `Host` methods bump the minor version; fixes bump the patch version. Before 1.0 (0.x),
minor-version bumps may still include small breaking changes, called out explicitly below.

## [0.1.0] -- first public version

Initial release. Core language: pattern mini-notation (notes, rests, ties, subdivision, alternation,
random choice, probability, repeat), a generic host-defined parameter system (no instrument-specific
names in the core), scales and transposition, arithmetic in command arguments, modulation-friendly
`lane`/`play`/`free` model with automatic hand-back to physical controls. Named values (e.g. `wave
sine`) are supported in direct commands and in inline `lane`/`play` patterns, resolved per-parameter
through `Host::paramValues()` at the point the target is known -- not in named patterns (`name =
...`), since those aren't tied to one parameter until later attached to a lane.
