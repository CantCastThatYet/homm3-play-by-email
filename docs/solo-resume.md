# Solo resume prototype

The Windows desktop can restore a member's native HotA checkpoint without another player online. The server stores bytes and leases; it runs no game process. Original HotA executes the turn locally.

## Use it

1. Extract the complete updated Windows package. Connect to your server with your existing profile. Same-PC testing uses separate Red/Blue profiles and isolated HotA copies.
2. Start a supported two-human Judgement Day game from both lobbies. Wait for each lobby to confirm its initial checkpoint is saved.
3. Close the native clients. Select the started game in either lobby and press **Resume HotA**. Only that member needs to be online.
4. Keep the lobby running while playing. End the turn and wait for **Turn saved** before closing HotA.

The resume button downloads the authenticated member's branch, verifies its hashes, launches an isolated client and loads the actual native world. It verifies the date, resources, hero records and human membership before enabling input. A failed recovery closes its owned client and records diagnostics, instead of leaving an inert game window.

A branch has an exclusive renewable lease. If the lobby closes, loses the server or cannot renew that lease, native gameplay is held. This prevents two clients from publishing conflicting versions of the same seat. Recover by closing that client and resuming after its lease is released or expires.

## Current limits

- Initial and completed-turn checkpoints are uploaded automatically. Mid-turn edits are not automatically persisted to the server. Closing before completion can restore the last server checkpoint and lose those edits.
- Completed turns reopen held. The server does not yet merge simultaneous branches or advance the shared day. Opening a completed checkpoint does not grant an extra turn.
- Offline PvP, contact rollback, inbound peer-event delivery, notifications and arbitrary maps are not implemented. The proof covers isolated startup turns and End Turn, not every adventure interaction or combat scenario.
- Initial startup still needs both native clients. Solo resume requires a checkpoint created by this build; an old lobby record alone has no world to restore.
- The native adapter is Windows x86 and accepts only the pinned installed HotA/HD binaries. The C# lobby/server architecture remains cross-platform; other native game platforms are unverified.

## Verification

The real desktop coordinator started two native clients, saved both branches through the HTTP API, closed both, disconnected Red, and restored Blue alone. Blue's real native End Turn produced a completion checkpoint, published as server branch revision 2. No server-side game process was used. Both source-audit verification and completion-tail verification passed; the game stayed on day 1.

Local evidence: `artifacts/solo-lobby-proof/run-v2/blue-solo-restored-probe.json`, `blue-after-end-turn.json`, and `blue-server-completed-branch.json`. Separate cold Red tests verified unfinished restoration, End Turn, and reopening a completed turn without replaying it. These local captures contain proprietary game state and are excluded from release archives.

See [branch API and leases](native-resume-api.md) for the server contract.
