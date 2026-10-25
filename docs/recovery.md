# Recovery

The Recorder performs conservative startup recovery before entering IDLE.

## Recorder-only output ownership

The standalone `ros2_bag_recorder` acquires a nonblocking POSIX advisory
exclusive lock on `.ros2_bag_recorder.lock` inside its canonical output root
**before** it modifies recovery state or creates the Recorder. The descriptor
remains held for the Recorder process lifetime. A second standalone process
with the same resolved output root fails without running recovery.

The lock file must **not** be unlinked on shutdown: another process could then
lock a newly-created inode while the original process still holds its lock.
The lock protects cooperating standalone Recorder processes on a local
filesystem with working advisory locking semantics. Direct library callers
(including test harnesses) are responsible for coordinating shared output
directories themselves; the library does not acquire this process lock.

The former UDS Session ownership, PID/socket cleanup and `bag_cli` control
scheme have been removed. They are historical behavior only.

## Segment recovery

`DELETING` segments are removed again on startup. A segment is changed to
`REMOVED` only after filesystem deletion succeeds.

Unexpectedly interrupted `WRITING` or `FINALIZING` segments become
`INCOMPLETE`; they are retained and are never auto-deleted. A `recovery.json`
diagnostic records `UNEXPECTED_TERMINATION` and topic operations that have a
durable PREPARED event without a durable terminal event.

## Inspection evidence and limits

For every retained `INCOMPLETE` segment, recovery attempts to open its MCAP
directory through `rosbag2_cpp::Reader`, enumerates available Topic metadata
and reads each available message. `recovery.json` records `reader_opened`, the
metadata and message counts, and any reader exception. This evidence can show
that a segment contains readable recorded data even when a durable
`TOPIC_ADDED` event is missing.

Recovery also scans visible raw MCAP Schema and Channel records in every
`.mcap` file below a retained segment. `recovery.json` records unique Schema and
Channel counts plus whether every nonzero Channel schema ID has a matching
Schema record. The scan does not decompress Chunk payloads, so it is evidence
for direct or summary records only and must not be treated as a complete MCAP
closure proof for compressed data.

Recovery never infers that an uncertain operation succeeded, and it never
deletes an unreadable or corrupted segment. Preserve `manifest.json`,
`topic_events.jsonl`, `recovery.json`, and the affected segment before making
an operational decision.

See `docs/archive/phase1_validation_report.md` for the executed recovery tests and the
remaining G1/G2 gates.
