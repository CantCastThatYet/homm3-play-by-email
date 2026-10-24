# Completed multiplayer turn across logout

Status: prepared experiment; not executed. The earlier Hot Seat save/load/day-transition checks passed, but do not answer this question.

## Question

Can a human finish a normal HotA multiplayer turn, close their game, and later recover their reserved slot while the other humans keep their actual turns and the native engine advances the day exactly once?

Keep the normal session turn settings. Test simultaneous turns explicitly when enabled; a sequential-only result cannot establish support for them. Use isolated copies, a small disposable two-human map with an AI, and distinct player identities. No public lobby protocol changes or real campaigns are needed.

## Evidence to collect

Record the pinned client/module versions and turn settings; each player's authenticated identity, color, human flags and native transport ID; the native date; a recognizable hero move/resource change; and native completion/acknowledgement state. Capture the world and required HD streams at the same demonstrated boundary, with hashes and revision ownership. Existing save observers alone do not establish that End Turn produces a complete recovery checkpoint.

Static evidence already distinguishes temporary outages from terminal departure and shows that terminal departure can unlink a participant and clear human ownership. See [disconnect-state-lifecycle.md](../re/disconnect-state-lifecycle.md) and [reconnect-deep-dive.md](../re/reconnect-deep-dive.md). Those are reasons to observe these fields, not proof that logout is supported.

## Sequence and acceptance

1. Establish a normal two-client multiplayer session. Record the initial state on both clients and verify the expected human assignments.
2. Have one human make a recognizable move and end their turn while the other is unfinished. Identify the native completion boundary and the data needed to recover it. A save-upload receipt alone is not turn completion.
3. Close the finished player's client. The other player's state must retain the absent human, submitted actions, and completion status without AI conversion, abandonment, or forced session exit. If stock behavior fails, retain the trace; it defines the adapter work rather than passing the experiment.
4. Reconnect the finished player before the other finishes. They must recover the waiting state and cannot replay their completed turn. Then close that client again.
5. Finish the remaining human turn. Observe native merge/AI/day progression as applicable; verify no missing or repeated actions, movement renewal or income. Reconnect the absent player into the correct next-turn state.
6. Repeat with the host as the first player to finish and leave. Host absence must not require a permanently running player client.
7. At a demonstrated durable boundary, close all clients and restart the lightweight server. Reconnect and verify the same revision, human ownership, completion states and next playable turn. Duplicate submission/reconnect must not advance the day again.

Offline-human combat requires its own test: pause before any combat outcome, notify both players, and resume with both human participants. Passing this completed-turn experiment does not pass that requirement.

## Current prerequisites and limits

The bridge currently observes save/load/turn routines and captures existing HD streams after a native save. It does not yet persist an authoritative multiplayer completion boundary, restore HD coordination state, suppress terminal departure, or rebind transport identity. Implement and verify those mechanisms against the observed failure points; do not satisfy this experiment by changing the session to Hot Seat or inventing server-side day advancement.

Native UI capture has failed on this machine. Prepare instrumentation before requesting a separate two-client run; the user has already completed the last manual step of the Hot Seat check. No further manual action is needed for that completed check.
