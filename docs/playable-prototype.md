# Playable prototype: agreed scope

HotA runs locally on each player's PC, using the original client and rules. The server must remain lightweight: session coordination, durable checkpoint/turn storage, and notifications. It will not run or stream a separate game client for each player.

The lobby delivered first is a foundation, not completion of the requested playable prototype. The prototype is accepted only when two people can create/join a session, start the actual game, make real moves, close their clients, resume their reserved slots, complete a native turn/day transition, and repeat. An offline human defender must cause a pause and notifications, never automatic AI defense. Native contact rules still apply; unrestricted simultaneous interaction is not promised.

### Turn-flow clarification, 2026-10-24

The user's requirement is normal multiplayer play: do your turn, end it, wait for the others, then play the next turn, with the added ability to log off after finishing and return at any time. This is not approval to replace multiplayer with sequential Hot Seat file passing. Preserve the session's native turn settings, including simultaneous turns when enabled and HotA's normal restrictions when players interact.

The standalone app must configure the map and players, launch the patched client, and create/join/resume through the adapter without asking players to repeat setup in HotA's menus. The user explicitly reiterated this expectation during the native experiment. Native menu operation is a temporary verification technique, not the delivered Start/Join flow. Reuse HotA's setup and transport lifecycle behind the adapter; a direct save load alone does not establish a multiplayer session.

Finishing a turn and being connected are separate states. A completed turn must remain submitted after its player closes the game; an offline player must retain their human seat. Rejoining while others are still playing must show the waiting state without allowing the submitted turn to be played twice. Rejoining after advancement must recover the correct next playable state. The session must survive every player closing their game, without a native game process on the server. Notifications must follow a durable, playable state, not merely a lobby counter.

The first required logout boundary is after End Turn. Recovery of arbitrary unfinished actions is additional work and is not established by this clarification or the completed Hot Seat tests. The next multiplayer experiment is specified in [multiplayer-completed-turn-check.md](multiplayer-completed-turn-check.md).

## Current implementation

The updated Windows desktop now connects both players' Ready controls to native startup. The real two-client lobby flow reaches the map with simultaneous turns enabled; see [startup instructions and evidence](lobby-native-startup.md). This supports the verified small map and default starting choices. It does not establish safe logout or native multiplayer recovery.

- C# Avalonia lobby, ASP.NET Core service, SQLite identities/rosters/readiness.
- Authenticated durable opaque checkpoint storage with revision checks and idempotent retries. See [checkpoint-api.md](checkpoint-api.md). A storage receipt does not signify a completed turn.
- Experimental x86 bridge for one inspected HotA/HD build, and a C# controller in `HeroesLobby.NativeHost`.
- Native observation of save/load/turn calls, and bounded capture of existing HD memory streams and coordination metadata after a successful native save.
- Native completed-turn marker hooks, plus an explicit experimental game-thread snapshot command. Hook installation has been verified in two concurrent isolated clients; actual multiplayer completion capture/recovery remains to be exercised.
- A persistent client upload outbox that verifies capture hashes and retains request identity through lost responses and process restarts. Storage receipts remain separate from native completed-turn admission.

The bridge has no direct load, merge, advance, or branch-resume command. Those require a demonstrated native lifecycle; the API refuses to imply they exist. A captured envelope is explicitly marked `complete_restore: false`. Ordinary saved world bytes and captured coordination data must still be paired and tested. Existing streams may be stale or absent, and native pointer values cannot be restored into another process.

## Run the native integration experiment

The first user-assisted save/server-transfer/cold-load and Red-to-Green/day-transition checks have now passed for one Hot Seat map. See [the exact runtime results and remaining limitations](../re/native-save-runtime-proof.md). Simultaneous-turn recovery and offline PvP are still unproved.

Use Windows and the pinned installed build. The C# app/server remain cross-platform; the original game's bridge is Windows x86. Native compilation requires MSVC's x86 C++ tools and the Windows SDK. The native code uses the game's existing `patcher_x86` library; its header notice is retained under `native/vendor/`.

```powershell
./native/build.ps1 -Test
dotnet run --project src/HeroesLobby.NativeHost -- prepare C:/Games/Heroes3_HotA C:/projects/homm3-play-by-email/.scratch/playable-hota
dotnet run --project src/HeroesLobby.NativeHost -- launch C:/projects/homm3-play-by-email/.scratch/playable-hota C:/projects/homm3-play-by-email/artifacts/native/win-x86
```

`prepare` requires a new destination, excludes the original Games/Saves directories, and never overwrites the installed game. It verifies the inspected executable/DLL hashes before and after copying. On this development machine that isolated copy is already prepared; skip the prepare command rather than overwrite it. Substitute your own paths on another machine. The local SDK `.tools/dotnet/dotnet.exe` may replace `dotnet` here.

The controller launches only that marked copy, loads the adapter, verifies the actual loaded modules (including HD's substituted HotA DLL), requests chained telemetry hooks on the game's own thread before a world is loaded, and arms capture for the next native save. The loader checks the target executable, creation time, isolated-copy marker, and common live launcher parent. It does not attach to an unrelated running game. Distinct marked copies may run concurrently; another client using the same installation, an unmarked installation, or an uninspectable process is refused. See [the native two-client evidence](../re/two-local-clients-plan.md).

Use the normal HotA UI to initialize a small test game. Record its date, active player, hero tile/movement, resources, and armies. Perform an identifiable move, arm capture if necessary, then make a manual native save with a unique short name. Autosaves can consume an armed capture, so check the observed native save name and flags; do not assume the first capture is your manual save.

```powershell
dotnet run --project src/HeroesLobby.NativeHost -- probe <game-pid>
dotnet run --project src/HeroesLobby.NativeHost -- probe <game-pid> capture_next_save
```

The probe reports the completed manifest path and native state. Payloads are copied on the game thread at the observed save boundary; a worker writes detached bytes and publishes the manifest after the files are written. Capture output is under the native build directory, separated by process ID and creation time. Missing/busy/invalid streams and file-write failures remain visible in the result.

Native startup is now connected to the lobby, with its limitations shown in the app. Before enabling logout/resume, prove the simultaneous-turn baseline/branch recovery and merge lifecycle, native day advancement exactly once, and offline PvP suspension. Opening an executable, uploading a file, or incrementing a server counter does not pass those recovery tests.

The current bridge is a development experiment, not yet a playable asynchronous release. Do not use it with an existing campaign or distribute bundled game assets. The service does not treat these capture files as authoritative game state.
