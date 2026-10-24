# Native branch storage and resume

The desktop exposes **Resume HotA** for a member of a started game (`Playing`/`Started`). It does not wait for another player, presence, or lobby readiness. The server requires an existing native startup record for that member and a validated member-owned capture. Legacy lobby records and opaque checkpoint uploads are not silently migrated into playable branches.

All routes require the same bearer player credential as the lobby. Responses contain only the requesting member's branch. The server never launches HotA.

| Route | Request | Result |
|---|---|---|
| `GET /api/games/{id}/branch` | — | Own authoritative `NativeBranchInfo`, or a specific conflict explaining what is missing |
| `POST /api/games/{id}/resume/acquire` | `{ expectedRevision, initialize: false }` | Exclusive lease token, expiry, authoritative descriptor |
| `POST /api/games/{id}/resume/renew` | `{ token }` | Renewed lease and current descriptor |
| `POST /api/games/{id}/resume/release` | `{ token }` | Release result; an old token cannot release a newer lease |
| `POST /api/games/{id}/branch` | `{ expectedRevision, leaseToken, checkpointId, launchId, sessionName, adapterBuild }` | New authoritative descriptor |

Initial branch creation acquires revision zero with `initialize: true` after that member's actual native startup report. Publishing references a checkpoint already uploaded through the checkpoint API. The checkpoint must belong to the authenticated player and game. Publication validates the archive's format, file hashes, limits, local color/date, participant roster, fresh world-save metadata and completion marker where applicable. The launch ID and session name must equal the game's frozen native launch. The adapter build is a SHA256 identity supplied by the client, not remote execution attestation.

Branch revision is separate from lobby revision. Atomic SQLite transactions serialize lease acquisition, renewal and publication. Lease tokens are random 256-bit values; only their hashes are persisted. Leases last two minutes and the desktop renews them every 25 seconds while tracking a running client. A stale revision, expired/wrong lease, older checkpoint or regressing date is rejected. A repeated identical publication after a lost response returns the prior descriptor without creating another revision. Both branch and lease state survive server restart.

The desktop downloads the exact checkpoint, verifies its descriptor's length/hash, and calls the native bridge's restoration path. It reports the bridge's real error when restoration is unsupported or fails verification. It releases a failed launch's lease only when no launch was attempted, the owned process is stopped, or its native input fence has been confirmed. A launcher that throws without returning a process ID leaves the lease reserved until expiry, because absence of a process has not been established. Startup captures and fresh completion captures are published automatically by the tracked native client. Local ZIP archives remain under the profile's `branches/{gameId}` directory.

The native client uses a 60-second gameplay lease deadline, shorter than the server's 120-second lease. Every 25 seconds, the desktop renews the server lease first, then sends `renew_lease_fence`. On tracking or renewal failure it sends `hold_session`; it releases the server lease only after confirmed fencing or process exit. If the pipe is unavailable, the existing native deadline holds gameplay before another server lease becomes available. A crashed app cannot keep the native lease renewed. This guarantee depends on the actual native gate and its runtime proof, not merely successful HTTP renewal. Process IDs are paired with their creation time to reject PID reuse.

`RestoreCapability = "unverified"` deliberately describes the server's knowledge: archive validation is not engine restoration proof. Likewise, storing a completed local branch does not prove shared-day merge, delivery to other players, cold transport rejoin, or offline PvP behavior. The runtime must establish its own restored state before reporting success. The server lease serializes authoritative writes; the native deadline supplies the disconnected executable's input gate.

The server binds the submitted bytes to the authenticated member and frozen session; it does not attest that an untrusted client really executed HotA. No bearer credentials are written into branch archives or descriptors.
