# Starting HotA from the lobby

The updated desktop app and server now connect readiness to the native startup adapter. This is a playable native startup build; it does not yet enable safe logout, native multiplayer reconnection, asynchronous day completion or email notifications.

## On this development PC

The server at `http://localhost:5080` has been rebuilt and restarted using the existing Docker data volume. Existing lobby records remain in that volume. The previous arbitrary map label cannot select a native map; use a new game for this test.

1. Open `artifacts/Play-Red.cmd` and `artifacts/Play-Blue.cmd`. These launch the updated app with separate profiles under `.scratch/lobby-profiles/` and separate prepared HotA copies. They do not replace the original saved player identities.
2. Connect both apps. Red creates a new game with the default `Judgement Day.h3m` map and two seats. Blue joins it.
3. Keep host Red, guest Blue, and Random town, hero and bonus. Mark both ready. The apps open HotA and complete native setup automatically; allow about a minute on this PC.

These launchers are local conveniences with workspace paths. The portable app package is `artifacts/HeroesPost-win-x64.zip`; extract all files, including `native/`. On another PC, use **Set up HotA** to choose the supported installation. The app checks fingerprints and prepares a separate game copy; it never bundles or downloads game assets. Ordinary launches retain one player identity. Use distinct `HEROES_POST_PROFILE` directories when simulating two players on one PC.

The host PC address in setup is separate from the lobby server address. `127.0.0.1` is for two clients on this PC only. Cross-PC players need a reachable host IPv4 address. WAN/NAT traversal has not been validated. The native game still uses direct TCP/IP; the lobby server is not yet a relay or asynchronous engine.

Both players must use the updated app and mark ready after HotA setup. Older readiness flags have no native capability flag and do not silently launch games. Unsupported maps/choices, missing files, an old server, native errors and timeouts are surfaced in the app. Launch failures do not automatically restart or replace a running game. Fix the reported issue and create a new lobby; failed clients are left available for inspection.

## Coordination and duplicate protection

The host starts one authenticated, persistent launch attempt. Only members can fetch its native session/address. Each client launches its own owned game copy. Host establishes the native session before the guest opens HotA, avoiding the main-menu readiness failure observed with concurrent launches. Clients report stable distinct native IDs; the guest arms the real native options approval before the host invokes Start. Each reports Started only after native world/map presence, setup closure, expected local/current color and active simultaneous turns are observed.

Repeated host requests return the same attempt. Server transitions reject wrong actors, stale attempt IDs, duplicate native IDs and premature Start reports. A local exclusive attempt marker prevents another app instance from launching the same attempt twice. This marker does not implement recovery; restarting the companion during an unfinished launch reports that the attempt was already handled rather than recreating it.

## Verification, 2026-10-24

- 24 C# tests pass, including two headless Avalonia windows whose Ready controls trigger the real HTTP coordination flow with a recording native runtime, and server authorization/idempotency/transition/persistence checks.
- Native x86 capture, completion parsing, MemFile replacement, request parser, DirectPlay ABI and unsupported-build smoke checks pass.
- The same two-window readiness test was rerun with the actual native runtime. It passed in 69 seconds. No development host/join/start commands were issued during that test: the desktop coordinator performed those operations.
- Real host PID7328 and peer PID340 reached day 1 on Judgement Day as Red and Blue, both with allocated adventure maps and simultaneous turns active. Both completion-marker counts were zero. They were left open; no turn was ended.
- Evidence: `artifacts/completion-proof/lobby-native-start-v2.log`, `lobby-native-start.json`, `lobby-native-7328.json`, and `lobby-native-340.json`. The UI test used headless Avalonia; it is not an OS screenshot or manual visual inspection of the published app.
- Windows x64 Native AOT publication succeeded with the native adapter included. The portable ZIP is about 17 MB. Original game assets are not included.

The first live run exposed the concurrent-launch menu-readiness failure and is retained in `lobby-native-start.log`; the successful rerun used host-first sequencing. That correction is in the shipped coordinator.

## Completed-turn logout investigation

The published package still uses ordinary native departure behavior. Blue
ending its turn and then exiting can force Red into HotA's save-and-exit flow.
The user reproduced this; it is not a setup error.

Separate development builds now prove a fresh native completion snapshot,
byte-exact server transfer, and interception before human-seat removal for both
abrupt process loss and the game's native transport teardown. They deliberately
pause the remaining client. Continued play and cold rejoin remain unimplemented,
so these experimental builds have not replaced the published package.
See [the evidence and exact limitations](../re/completed-departure-test-status.md).
