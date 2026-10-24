# Engine checkpoint preparation API

This API durably stores opaque bytes from a local game client. The server does not parse or validate a Heroes save, run HotA, complete a player's turn, merge turns, or advance a day. Uploading leaves the game's status and revision unchanged.

Every route requires the existing `Authorization: Bearer <player token>` header. The authenticated player must currently belong to the requested game. Any current member may list and download that game's checkpoints. Checkpoint ownership always comes from the authenticated identity, never from request metadata.

## Upload

`POST /api/games/{gameId}/checkpoints?baseRevision={revision}`

Headers:

- `Authorization: Bearer <player token>`
- `Idempotency-Key: <unique request key, 1–128 characters>`
- `Content-Type: application/octet-stream`

The body is the raw checkpoint bytes. Content-Type is descriptive; the server stores arbitrary nonempty bytes and makes no engine-validity claim. `baseRevision` is the current lobby/game record revision, not a native game date or a turn number.

A successful response is HTTP 200 with `CheckpointInfo`:

```json
{
  "id": "server-generated checkpoint ID",
  "gameId": "game ID",
  "ownerPlayerId": "authenticated uploader ID",
  "baseRevision": 3,
  "sha256": "lowercase SHA-256 of the exact uploaded bytes",
  "length": 12345,
  "createdAt": "2026-10-24T12:00:00+00:00"
}
```

Keys are scoped to `(gameId, ownerPlayerId)`. Repeating the same key, base revision, and content returns the original receipt without another row, including after a later game revision change. Reusing a key for different bytes or a different base revision returns 409. A new key with a stale base revision also returns 409. Clients should retain the original request key and bytes until the outcome is known, then retry the same request if the connection fails.

The server checks membership before reading the body and again in the committing SQLite transaction. A player who leaves during upload cannot commit. A game revision change during a new upload causes a conflict rather than attaching bytes to a silently changed roster/settings revision.

## List and download

- `GET /api/games/{gameId}/checkpoints` returns a list of receipts ordered by creation time and ID.
- `GET /api/games/{gameId}/checkpoints/{checkpointId}/content` returns the exact bytes as `application/octet-stream`, with a generated `{checkpointId}.bin` attachment name, SHA-256 ETag, and `Cache-Control: no-store`.

The download query binds both game and checkpoint IDs. A checkpoint from a different game returns 404 even if the caller belongs to the URL's game. A former member cannot retrieve checkpoints just because they uploaded them. Downloads verify stored length and SHA-256 before returning bytes; an integrity failure returns 500.

## Limits and persistence

`Checkpoints:MaxBytes` (environment variable `Checkpoints__MaxBytes`) defaults to 67,108,864 bytes (64 MiB). The supported configuration range is 1 through 268,435,456. Empty uploads return 400; oversized bodies return 413. The upload route overrides Kestrel's ordinary 16 KiB lobby request limit only for that request and still counts bytes when Content-Length is absent. Invalid/missing authentication returns 401; a nonmember returns 403.

Metadata, content hash, and content bytes live together in the existing `Lobby:Database` SQLite database, in the `checkpoints` table. No client-supplied filename or path is opened. A single transaction commits the BLOB and receipt, with `synchronous=FULL` on checkpoint-store connections. WAL recovery exposes a complete committed row or no row; there is no separate payload-file rename or orphan-file recovery step. Copying/backing up a running database must retain SQLite's normal WAL consistency requirements.

An interrupted body or cancellation before commit writes no checkpoint row. Cancellation or connection failure after a commit can still prevent the client from seeing its receipt; the idempotent retry resolves that ambiguity. Rows are immutable through this API. Game deletion prevents access to retained rows but does not currently delete their bytes.

This prototype buffers each bounded upload before its transaction and uses a byte-array copy for the SQLite BLOB; peak per-upload memory is larger than payload size. Download also buffers the BLOB. Aggregate storage quotas, retention/cleanup, and upload concurrency limits are not implemented. These limits concern storage preparation; they do not establish a playable native-engine resume protocol.

## Validation

`CheckpointTests` exercises restart recovery, unchanged game status/revision, concurrent idempotent retries, stale/conflicting requests, member and cross-game isolation, leaving during upload, empty/oversized/interrupted/cancelled bodies, and corrupt-content refusal. A real local Kestrel test accepts a 32 KiB checkpoint above the normal lobby limit and rejects one above the configured checkpoint limit.

## Client capture outbox

`HeroesLobby.NativeHost` now packages native capture manifests and their explicitly referenced files into deterministic ZIPs. It checks each published length/hash, rejects traversal/linked paths, caps the archive at 64 MiB, and preserves `complete_restore: false`. It does not assume an unrelated GM file belongs to the same native boundary.

`CheckpointOutbox` persists immutable archive bytes and their server/player/game/base-revision request before upload. The key derives from that exact identity and archive hash. Credentials are supplied by the authenticated caller and are not written to the outbox. A matching server receipt is published separately after validation. Restart/retry uses the same key and bytes; 409 does not silently rebase an old capture. Callers must disable HTTP redirects. Partially prepared directories are ignored and retained for inspection; cleanup/retention is not yet automated.

Development commands (these are not the final app workflow):

```text
queue-capture <manifest-path> <server> <game-id> <player-id> <base-revision> <outbox-directory>
send-pending <outbox-directory> <server> <player-id>
```

The second command reads `HEROES_POST_TOKEN` from its environment, keeping it out of command-line arguments. Its receipt only confirms storage, not safe logout or native next-turn readiness. `CheckpointOutboxTests` covers a lost response after real server commit, restart/idempotent recovery, byte-for-byte download, changed local payloads, wrong identity/server, bad file references and stale-revision retention. `re/test-native-outbox.py` also exercised an actual HotA capture envelope against the isolated loopback proof server, retaining results in `artifacts/completion-proof/native-outbox-roundtrip.json`.
