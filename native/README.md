# Native client adapter

This x86 adapter runs inside a player's isolated original HotA HD client. It observes native save/load/turn calls and the native simultaneous-turn completion marker. It can capture existing HD memory after a save or at that marker. An explicit experimental command can also call native serialization at the marker. It does not load a save, advance a turn, resume a branch, merge branches, or submit anything to a server. The capture format always declares `complete_restore: false`; it does **not** contain an external GM world file.

Build from the repository root with installed MSVC x86 tools:

```powershell
./native/build.ps1 -Test
# Other installed Visual Studio layout:
./native/build.ps1 -VcVarsPath 'C:\path\to\VC\Auxiliary\Build\vcvars32.bat' -Test
```

Outputs are under `artifacts/native/win-x86`: `HeroesPost.Native.dll`, `HeroesPost.Injector.exe`, and test executables. `native/test.ps1` rebuilds only the capture test executable and runs synthetic-memory and non-game IPC tests. It can be used without overwriting the adapter binaries. Do not rebuild the DLL while an owned game has it loaded.

## Ownership and loading

The launcher must create an isolated game copy containing `.heroespost-owned-copy` with exactly `HeroesPost.Native.v1` (a trailing newline is accepted), start `h3hota HD.exe` there, and start the injector as its own child. The game and injector must share the same live parent process. The original installation is never edited.

```text
HeroesPost.Injector.exe --pid DECIMAL_PID
  --expected-exe ABSOLUTE_ISOLATED_EXE
  --dll ABSOLUTE_ADAPTER_DLL_OUTSIDE_GAME_COPY
  --created-filetime DECIMAL_UTC_FILETIME
  --isolated-root ABSOLUTE_COPY_ROOT
```

`--created-filetime` is the game's creation time as an exact unsigned decimal FILETIME (C#: `process.StartTime.ToUniversalTime().ToFileTimeUtc()`). Paths are resolved using file handles before comparison, and the executable hash is pinned. The injector calls LoadLibraryW and the adapter's explicit `HeroesPostInitialize` export; DllMain only records its module handle. It never calls an engine world function. An injection timeout is reported without terminating the target; a pending loader's argument memory remains allocated until process exit.

The DLL is process-lifetime. No hot-unload operation is exposed. Startup and save/load/turn observation were exercised in the original client; see `re/native-save-runtime-proof.md`. The additional completion hooks installed successfully in two concurrent isolated clients, but execution of completed-turn capture and experimental serialization still requires a real multiplayer turn. Passing synthetic tests is not that proof.

## Local protocol

Per-PID named pipe: `\\.\pipe\HeroesPost.Native.v1.<pid>`. Current-user-only DACL, remote clients rejected, one connection at a time. Send one UTF-8 JSON object terminated by newline (maximum 4096 bytes), read one newline response, then close the connection. The parser accepts exactly one `command` string; duplicate keys, additional fields and trailing objects are rejected.

```json
{"command":"probe"}
{"command":"enable_telemetry"}
{"command":"capture_next_save"}
{"command":"capture_next_completion"}
{"command":"experiment_snapshot_next_completion"}
```

All responses have `protocol: 1` and `ok`. Errors have `error: {code, message}`. `probe` remains available on unsupported/not-ready builds and reports actual loaded paths/hashes, `fingerprintAccepted`, telemetry counts, best-effort current color/date, capture state and unsupported capabilities. Probe memory reads are observational and not an atomic world snapshot. `mainPresent` does not prove a match is loaded.

`enable_telemetry` requires matching loaded executable/HD/HW/loader/patcher/HotA-fix hashes and the isolated marker. It queues installation on the Heroes III window's game thread through a one-shot WH_GETMESSAGE callback. Both enqueue and installation require no allocated world-map pointer; install from the initial menu before creating/loading a game. `pending_game_thread` is not success: poll `probe` until `telemetry.status` is `active`, or handle a reported failure. No hooks are installed from the pipe worker. Unknown window class or an unpumped message queue remains a reported/pending condition, not permission to install on an arbitrary thread.

The three high hooks use the existing `patcher_x86.dll` chain and `GetDefaultFunc()`:

| Base address | ABI checked from engine bytes | Observation |
|---|---|---|
| `004BEB60` | thiscall, name + four stack flags, ret14h | entry/return count; bounded original name bytes; raw EAX and low return byte |
| `004BEFF0` | thiscall, name + two stack arguments, ret0Ch | load entry/return count only |
| `004C6CA0` | thiscall, no stack arguments | turn entry/return count and nesting depth |

The save success/error paths write **AL**, so capture uses the low return byte, never full-EAX truthiness. Engine return values are preserved. Save metadata allocation/capture failures are isolated from the native call and counted/reported. Initialization rejects unsupported files; no broad byte patch is applied over another mod's entry JMP.

Two additional low hooks at HD static addresses `0117AF35` and `0117AFCC` run just after the native local-done store and before the corresponding send. Both surrounding byte sequences are checked after relocation. The observer leaves registers/flags untouched, preserves floating-point/SSE state, returns `EXEC_DEFAULT`, and owns only its own hook handles. The native callback itself is not a completed durable submission.

`capture_next_completion` copies existing streams and metadata at the next confirmed marker. It does not serialize the world and explicitly reports `fresh_world_serialized: false`. `experiment_snapshot_next_completion` additionally calls the chained native Save entry with `RMTGEXBS.BIN,0,0,0,1` on the game thread. It requires a single outer turn frame, no active save/load, matching current/local identity, and unchanged observed native phase/participants across serialization. It then requires a fresh closed local stream and a stable capture. The existing HD receive flag is an eligibility value that can persist; it is not treated as a call-depth flag. Native save preparation can change selection/animation state, so this command remains an opt-in development experiment requiring gameplay comparison and restart/merge proof. Neither command intercepts departures or makes closing the game safe.

`capture_next_save` arms one capture. The next native save returning a nonzero low byte copies bounded existing HD buffers on the game thread. A separate worker writes detached bytes under `captures/<pid>-<creation-filetime>/<sequence>` next to the adapter DLL, outside the game copy. The worker creates its parent directories, reports disk errors, hashes payloads, flushes files and publishes `manifest.json` last. `captureStatus: written` means evidence files were written, **not** that the whole game can be restored. `lastCaptureManifest` can refer to the preceding successful capture while a new capture is armed/failed; inspect status/error too.

`native_save` in the manifest records the observed name, exact original bytes in hex, four flags, raw return value, low return byte, and `world_artifact_included: false`. The newly written `Games/*.GM?` must be paired independently during the first runtime experiment. Existing HD streams may be absent, busy, invalid or from an earlier synchronization stage; their individual status and warnings are preserved.

The copied `vendor/patcher_x86.hpp` retains its original Barinov Alexander/baratorch copyright and free-distribution notice. The adapter does not redistribute original game binaries.

## Native session setup experiment

`stage-session <isolated-root> Host|Join <player-name> <session-name> <IPv4> <pid> [map.h3m] [local-port]` writes a bounded UTF-16 request and queues `bootstrap_session`. The adapter closes the native main menu through its own dispatch path, initializes native TCP/IP, hosts or joins the named session, and optionally selects a fixed map on the host through the patched native selector. `cancel_bootstrap` cancels only a queued request. A completed bootstrap currently means `awaiting_scenario_configuration`, not a started game.

Port zero preserves DirectPlay's default. An explicit port from 1024 through 65535 adds the supported `DPAID_INetPort` WORD element to the native compound address. Separate local ports 24777 and 24778 were verified on this PC; no OS port reservations, firewall settings or provider binary were changed. Native discovery and the host-advertised address connected the peer. WAN/NAT connectivity remains unverified.

Setup commands require the pinned build, an owned isolated installation, the main menu without a loaded world, and validated game-thread boundaries. Fixed map selection searches the current native filtered list; hidden/unavailable or ambiguous maps are refused. Evidence and ABI checks are in `re/native-session-bootstrap.md` and `re/directplay-fixed-port.md`.

`start-session <root> <pid> HostStart|PeerAccept <peer-id-1> <peer-id-2> <map.h3m> <date-MWD>` arms the separate native Start/options helper. Arm the peer first and require `peer_armed` with `setupReady: true`; then queue HostStart with the same two native IDs, map and date. The helper checks native assignments and pending map transfers, sends one native Start event, sets ST through native controls, and accepts both sides' real options handshake. Decimal date `211` means month 2, week 1, day 1. Native startup was verified through this route on both clients; see `re/automatic-native-start-proof.md`. No turn-ending or logout functionality is implied by this command.
