# Game relay: our own transport under HotA multiplayer

HotA's TCP/IP multiplayer talks to DirectPlay 4 through two COM objects created with
`CoCreateInstance`: `IDirectPlay4A` (`CLSID_DirectPlay`) and `IDirectPlayLobby3A`
(used only to build the TCP/IP address). The engine's network wrapper (vtable
`00640D14`) maps its methods 1:1 onto those interfaces and handles the 14 documented
DirectPlay system messages.

The adapter replaces both objects with its own implementation
(`native/dplay_relay.cpp`) when the owned game copy contains `HeroesPost.relay.ini`:

```ini
host=127.0.0.1
port=5090
name=red
```

It patches the game executable's `ole32!CoCreateInstance` import; every other class
passes through. Microsoft's `dplayx.dll` is never loaded. All sessions, players,
groups, game messages and chat go to the relay endpoint of `HeroesLobby.Server`
(`Relay:Port`, default 5090, `-1` disables), which holds the authoritative session
state (`src/HeroesLobby.Server/Relay/`). Each client mirrors that state and turns relay
notifications into the DirectPlay system messages the engine expects. The wire format is
documented at the top of `RelayProtocol.cs` and `dplay_relay.cpp`.

The game, HD's simultaneous turns and every game message are unchanged; only the
transport differs. HotA's native TCP/IP join screen lists the relay's sessions.

## Verification, 2026-10-25

- `Native.RelayTest.exe <host> <port>` (built by `native/test.ps1`) drives two
  replacement DirectPlay objects through an independently declared `IDirectPlay4A`
  vtable against a live server: create/enumerate/join, host server player DPID 1,
  system messages with pointers into the caller's buffer, ordered delivery of 200
  messages, a 64 KiB message, chat, groups, player data, buffer-size queries and
  leave notification. 34/34 checks passed.
- Two real isolated HotA clients with the adapter (`artifacts/native/relay-x86`)
  hosted and joined `Judgement Day` through the relay, started with simultaneous turns,
  reached day 1 as Red and Blue, ended both turns and both advanced to day 2 with
  simultaneous turns still active. Relay counters showed 555/93 and 92/548
  messages sent/delivered. No DirectPlay provider was used.
- Existing C# suite: 34/34 passed.

## Away seats, rejoin and the server stand-in

Once the host transmits the started world (engine message `0x3E8`), the relay holds
seats: a client that disconnects, closes or destroys its player becomes *away*. The
other clients are not told, so their engines never run HotA's departure, save-and-exit
or AI-conversion paths. The relay logs every game message delivered to each player.

A fresh HotA process takes a held seat back by resuming a capture of that seat:

1. A live client's seat is captured either at its native End Turn (`watch-completion
   <pid> snapshot`) or at an idle adventure-map moment (`capture-current <pid>`). The
   manifest records `relay_consumed`, the number of relay game messages its engine had
   taken.
2. The new process's `HeroesPost.relay.ini` adds `rejoin=<session name>`, `seat=<player
   name>` and `from=<relay_consumed>` (`NativeHost resume` writes `from`). The existing
   resume transaction loads the captured world, both baselines, simultaneous-turn state
   and roster into a single-player scaffold. Its own session setup then reaches the
   relay as `Rejoin` instead of creating a session, and its `CreatePlayer` receives the
   held seat's original player ID.
3. The relay replays the seat's messages from `from` onward. The client withholds them
   from the engine until the resume audit has passed. It then attaches the adventure
   manager's network handler, which native `Start` skipped because the scaffold had no
   transport, and restores the human players' names from the relay. After that it is
   an ordinary participant: it answers the day barrier, receives and merges worlds, and
   broadcasts like the original client.

The same mechanism is the server-side stand-in. When a player leaves after End Turn,
the server resumes that player's completion capture in its own HotA copy, and the
stand-in completes the day for the seat. When the player returns, the stand-in is
captured and closed, and the player's client resumes from that capture.

### Verification, 2026-10-25

All runs used two humans on Judgement Day with simultaneous turns, driven by
`.scratch/relay-dev/scenario.sh` and `handover.sh`, and ended turns without moving heroes:

- Blue was captured mid-turn on day 2 and its process killed; Red ended its turn and
  waited at the barrier with no departure dialog. A fresh process resumed Blue's seat
  and ended its turn. Red sent its world, the resumed Blue merged and broadcast, and
  both reached day 3, then day 4.
- Handover: Blue ended day 2 and quit. A stand-in in a third game copy resumed Blue's
  completion capture, and Red ended its turn; both reached day 3. The stand-in was
  captured and closed, and Blue's own copy resumed it. Both ended day 3 and reached
  day 4 with correct player names.
- C# suite 38/38, including relay seat-hold and rejoin tests. Native suite passed;
  `Native.RelayTest.exe` 35/35.

### Deferred messages

While players move simultaneously, HD parks other players' hero-action messages
(`0x41A`, `0x3FD`, `0x422`, ...) in its simultaneous-turn queue instead of applying them.
A seat captured after another player acted that day therefore carries a non-empty queue.
The capture already records those messages. `NativeHost resume` writes them to
`HeroesPost.deferred.bin` and adds `deferred=` to the relay ini. The rejoining client
delivers them first, through the normal receive path, once the resume audit releases game
messages, so HD queues them again itself. They are not counted in `relay_consumed`, which
stays an index into the relay's log for the seat. Without a relay rejoin, a capture with
queued messages is still refused.

### Real play across handovers, 2026-10-25

The development adapter adds `experiment_click_tile_X_Y_Z`. It hovers the tile through
the native mouse-move handler (`0040E2C0`) and then presses it (`0040A530`, map item
`0x25`). One press plots the selected hero's path; a second walks it. Battles used
HotA's auto-combat key. Every run compared both clients' heroes (position, experience,
army), resources and remaining monsters at each new day. Scripts:
`.scratch/relay-dev/leave.sh`, `battle.sh`.

| Run | Map | Who leaves | Result |
|---|---|---|---|
| Moves, resource pickups | Judgement Day | Blue after End Turn on day 2 | Consistent through day 5, including the returned client's moves |
| Host seat | Judgement Day | Red (relay host, ST coordinator) on day 2 | Stand-in completed the day; consistent through day 4 |
| Day 1 | Judgement Day | Blue on day 1; Red on day 1 | Consistent through day 4 |
| Queued messages | Judgement Day | Blue on day 2 after Red had moved (9 queued) | Re-delivered; consistent through day 5 |
| Battles | Too Many Monsters | Blue on day 2; stand-in started late | Consistent through day 4 |

In the battle run Red won a battle on day 1 and lost its hero in another on day 2, while
Blue's seat was away. The stand-in for Blue was started only after Red had already ended
day 2; it replayed the relay's messages and completed the day. Blue then returned and lost
its hero in a battle on the resumed client. Experience, armies, lost heroes, removed
monsters and the AI players' battles agreed on both clients at every new day.

Fixed on the way:

- The relay now keeps the host role with the host's seat and gives it to whoever
  reclaims that seat.
- A resumed client opened with the map view where the single-player scaffold had put it,
  often in unexplored darkness, and its first map click did nothing. It now selects the
  local player's current hero (`00417A80`), which centres the view and rebuilds the path
  map.

Observed, not fixed:

- After each new day, a client shows the other player's heroes with the movement points
  they had at the end of the previous day; their own client shows full movement. Positions
  and everything else agree. This is probably the order of HD's world exchange and new-day
  processing rather than the relay, but it has not been compared against stock DirectPlay,
  which is not available on the test machine.
- A capture costs about 0.35-0.45 s end to end, including starting `NativeHost`, and about
  1.2 MB (110 KB gzipped) on a 36x36 map.

## Playing by hand

The handover no longer needs scripts.

- **Play.** The app's **Play HotA** writes `HeroesPost.relay.ini` with the player's lobby
  token as `ticket=` and opens their HotA copy. Players host and join in HotA's own
  New Game, Multiplayer, TCP/IP screens; the relay lists every session on the server.
  Chat is HotA's own. The relay binds each seat to the account whose ticket created it.
- **Saving.** While HotA runs, the app's seat keeper (`SeatKeeper`) uploads captures to
  `POST /api/relay/games/{id}/captures`: at the start of each day, at most every 20 seconds
  after the player has sent game messages, and at End Turn, through a completion capture
  armed in advance. A capture taken mid-turn no longer disarms the End Turn capture. The
  server keeps the newest captures per seat (`Relay:CaptureDirectory`) and refuses one that
  consumed fewer relay messages than the stored one.
- **Stand-ins.** `StandInManager` watches the relay. Ten seconds after a seat goes away
  (`StandIn:GraceSeconds`), it resumes that seat's latest capture in a free server copy
  (`StandIn:Copies`, separated by `;`, with `StandIn:NativeDirectory`). The stand-in
  authenticates with a ticket of its own, which may take any held seat. It fights battles
  with auto-combat and accepts dialogs with Enter. It stores a capture at the start of each
  day. If the seat's turn is not ended when the game's turn limit has passed since the day
  began, it arms a completion capture and ends the turn. The limit defaults to
  `StandIn:TurnSeconds` (600) and can be changed per game in the app. Failed stand-ins
  back off before the next attempt.
- **Continue.** `POST /api/relay/games/{id}/claim` captures and closes the seat's stand-in,
  unless its turn was already ended and stored. It then holds off new stand-ins for three
  minutes. The app downloads the latest capture, writes `rejoin=`/`seat=` and resumes it,
  and the seat keeper takes over.
- **Maps.** Captures record the scenario file (`map_file`, from `world+0x1F6D9`), and resume
  starts its scaffold from that map instead of Judgement Day. The map must be installed in
  the resuming copy, including the server's stand-in copies.

Relay ini values are written in Latin-1: HotA's session names contain a non-ASCII separator
between the game name and the host's name, and the relay compares raw bytes. Captures and
rejoins also carry the seat's sent-message count (`relay_sent`, `sent=`). The relay logs a
warning when a capture had sent fewer messages than the seat did, because the player's later
actions are then missing from it.

### Verification, 2026-10-25

Two players on Judgement Day, launched with `NativeHost play`. That command runs the same
`RelayPlay` and `SeatKeeper` code as the app's buttons. The game itself was started with
the scripted scenario setup, not by hand in HotA's menus (`.scratch/relay-dev/e2e-run.sh`).
The server had two stand-in copies and a 60-second turn limit.

1. Both seat keepers uploaded a day-1 capture once the map appeared. Each seat was bound to
   its player's account.
2. Blue moved, ended the turn and was killed. Its End Turn capture reached the server first.
   A stand-in resumed it and claimed Blue's seat, and the relay replayed messages 533..556.
   Red moved and ended day 1. On day 2 the stand-in matched Red in heroes, armies,
   resources and monsters; movement points differed as before.
3. Red ended day 2. The stand-in stored its day-2 capture and ended Blue's turn once the
   limit had passed, with its own completion capture stored. On day 3 both clients matched.
4. Blue continued: the claim captured and closed the stand-in, and Blue's HotA resumed the
   capture and rejoined. Blue matched Red. Both moved and ended day 3, and on day 4 they
   matched again.

The C# suite passes, 37 tests. They include the relay's account-bound seats, per-game turn
limits, and a headless app test: Play, the game list and the turn limit, with a fake HotA
launcher. The native suites pass.

Observed, not explained: on Continue the relay warned that the stand-in's capture had sent
174 of the seat's 176 messages. The worlds still matched on days 3 and 4. The two missing
messages may be traffic the stand-in sent after its capture and before it closed. This has
not been checked.

## In-game lobby (game patch)

The adapter can also run as a patch inside the player's own HotA installation, with no
launcher and no desktop app:

- **Loading.** `version.dll` from `artifacts/native/patch-x86` sits in the HotA folder next to
  `HeroesPost.Native.dll`. Windows loads it instead of the system `version.dll`; it forwards
  the 17 real exports and loads the adapter in patch mode. Patch mode still checks the
  supported HotA build, but needs no owned-copy marker, and enables its hooks itself once
  the main menu is up.
- **Relay only for Heroes Post games.** The `CoCreateInstance` redirect is installed at
  start-up but passes every call through to DirectPlay. The lobby switches it on just
  before it drives HotA's own TCP/IP screens. Choosing a connection type by hand switches
  it off again, so normal TCP/IP and the HD+ online lobby are unchanged.
- **Server and identity.** `HeroesPost.ini` in the HotA folder holds `server=http://host:port`.
  It is created empty on first start. The first time the lobby opens, the game registers
  the name from HotA's multiplayer screen (`POST /api/players`) and writes `token=` and
  `token_server=` back to the same file. If the server no longer knows the token, it
  registers again.
- **The button.** A Heroes Post button sits below Direct Connection in HotA's multiplayer
  screen. Its art is put together at run time from the player's own files: the frame of
  the connection buttons, plain gold from the Cancel button, and the spell scroll
  artifact. No game art is stored in this repository.
- **The window** (`native/lobby_ui.cpp`). It uses HD+'s blue background and gold border,
  its sunken panels, gold bars and 16-pixel icons, and HotA's fonts. It shows:
  - the connection line;
  - **Your games**, the relay seats held by this account: setting up, you are in, away
    with the seat kept, or the server playing your turn;
  - **Games on this server**: name, host and seats, a lock icon for passwords, "playing"
    once started;
  - lobby chat;
  - a hint bar;
  - the buttons **Create**, **Join**, **Continue**, **Turn limit** and **Close**.

  A worker thread (`native/lobby_client.cpp`, WinHTTP) polls `GET /api/relay/lobby?after=<chat id>`
  every 2 seconds while the window is open. It posts a mouse move to the game window when
  anything changes, and the window repaints only then.
- **Create** turns the relay on, then sends HotA's multiplayer dialog the clicks it would
  get from the player: TCP/IP, then Host. HotA's own session-name dialog and scenario setup
  follow. The "Your IP Address" caption is replaced with the server address.
- **Join** turns the relay on with a filter, so session enumeration reports only the
  chosen game. It clicks TCP/IP, waits up to 10 seconds for the session to be listed,
  then clicks its row and Join. Double-clicking a game does the same.
- **Turn limit** cycles through 2 min, 5 min, 10 min, 30 min, 1 h, 4 h and 1 day for the
  selected game (`PUT /api/relay/games/{id}/turn-limit`).
- **Chat**: `POST /api/relay/chat`. The server keeps the newest 200 lines in memory; a
  newly opened window gets the last 30.
- Text conversion: HotA text is in the ANSI code page and is sent as UTF-8. Relay session
  and seat names are raw bytes, which the server keeps as Latin-1 and splits at HotA's
  `0xFA` separator into game and host.
- HotA reports mouse buttons to dialogs as message `0x30A`, with the Windows message
  (`0x201` down, `0x202` up) in the subtype and the position in game coordinates.

### Verification, 2026-10-25

Two patched copies on one machine against a local server, driven with real mouse clicks.
The test moves the cursor over HotA's window and posts the button messages; HD reads the
position from the cursor, which is why posted clicks failed before.

1. Red opened the lobby from the multiplayer screen's new button. It signed in by itself
   as the name typed in HotA. A chat line typed in the lobby reached the server and showed
   in both lobbies.
2. Red pressed **Create**. HotA showed its session-name dialog over the TCP/IP screen.
   After OK, the relay log had `Open create My Game<FA>SigmaGoidaSkibid`, and the server
   listed the game as open with 1 of 8 seats.
3. Blue's lobby listed the game. Blue selected it and pressed **Join**, and landed in
   HotA's scenario setup next to Red (`Open join`, `CreatePlayer BluePlayer`).
4. Red pressed Begin; both accepted HotA's PvP options and reached the adventure map.
   Blue's HotA was then killed. On restart, Blue's lobby showed the game under **Your games**
   as away with the seat kept, and **Turn limit** changed it on the server from 600 to
   1800 seconds.

### Not yet done

- **Continue from the in-game lobby.** The button is there but disabled. Returning to an
  away seat still needs the desktop app or `NativeHost continue`. The patch would have to
  claim the seat, download the capture and restart HotA into the resumed seat.
- Seat saving still runs in the desktop app's seat keeper; the patch does not upload
  captures yet. Without the app, a stand-in starts from the last capture the server has.
- Stand-in HotA windows are still visible on the server's desktop.
- The relay keeps sessions, seats and delivery logs only in memory. Restarting the server
  ends every running game. Captures survive on disk, but nothing can rejoin them.
- A player who quits mid-turn loses actions taken since the last capture, at most about
  20 seconds of play. The relay detects this and logs a warning.
- Not yet tested: contact between human players (which ends simultaneous turns); more than
  two humans; large maps; a stand-in's battles against a human.
- Stand-ins need a Windows server with prepared HotA copies; the Docker image has none.
  Without stand-ins, an away seat's turn waits for its player.
- The player's lobby token is stored in plain text in the relay ini inside their own HotA
  copy.
- HD's start-up update check once replaced `HD_HOTA.dll` in a test copy. The launcher
  now sets `<Update.CheckAtStart> = 0` in owned copies.
