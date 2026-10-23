# Phase 0 recovery boundary

This phase creates only disposable test bags. There is no production recovery
command, RecordingCoordinator state machine or cleanup service yet. Do not treat
probe bags or the identity executable as an implementation of the recovery
requirements in the frozen design.

Probes refuse to overwrite existing bag destinations; the C++ writer test places
each run in a new child directory. Failed tests retain their logs and generated
files for diagnosis. Never infer a successful operation from an incomplete bag.

`tools/inspect-mcap-schema.py --expect-custom FILE.mcap` checks actual MCAP summary
Schema/Channel/Statistics content. It does not repair data, decode damaged chunks
or replace PlotJuggler field validation. The committed AC-55 fixtures, hashes and
plugin results allow independent reproduction without the original message package.

ASan/TSan failures must be preserved verbatim. In particular, a failure reproduced
by `phase0_ros_allocator_reproducer` without the pool is evidence of a baseline
runtime issue, not permission to suppress it or to declare the full sanitizer
gate passed. See `implementation_notes.md` for the issue/alternative/AC mapping.
