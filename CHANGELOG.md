# Changelog

What changed for players and server hosts in each release of the Heroes Post patch and server.
The release workflow publishes each version's section with its GitHub release.

## [0.1.6] - 2026-10-27

### Fixed
- A host who leaves mid-turn and continues can end that turn again. Continue left Next Hero,
  Sleep, Move and End Turn disabled; whether it did depended on leftover processor state, which
  in testing was always so for the host and never for a guest.

## [0.1.5] - 2026-10-27

### Changed
- Your seat is saved a second after you stop acting (at most every three seconds) instead of at
  most every ten, so quitting or a crash loses at most the last few seconds.
- Saves go to the server as differences from the previous one. On a 72x72 map a save of about
  2 MB now sends about 150 bytes when nothing changed, 0.5-1.5 KB after a move, a pickup, a
  chest, hiring or a building, and 12-15 KB at a new day. A save takes 20-40 ms in the game.

### Server
- Update the server to take differences. Older servers keep working with this patch: it then
  sends whole saves, as before.

### Known issues
- A host who leaves mid-turn and continues cannot end that turn: HotA's End Turn button stays
  disabled. A guest can. Also in 0.1.4.

## [0.1.4] - 2026-10-26

### Fixed
- Two HotAs of one Windows user signed in as different players: the first one's saves went to the
  second player's seat, and its own seat had nothing to continue from. Each HotA now saves with
  the account it joined the game with.
- Sign out in one HotA no longer breaks a game running in another: it forgets the sign-in on this
  PC and leaves it valid on the server.

### Known issues
- A host who leaves mid-turn and continues cannot end that turn: HotA's End Turn button stays
  disabled. A guest can.

## [0.1.3] - 2026-10-26

### Fixed
- Games on a random map can be continued. Continue looked for the map file, which only the host
  who generated it may have; it now starts on a map that ships with HotA or the game of the same
  size and underground, and then loads the saved game. Not yet possible for Huge, Extra Huge and
  Giant maps without underground, which no shipped map matches.

## [0.1.2] - 2026-10-26

### Fixed
- The day no longer sticks when the host ends the turn and leaves before the last player ends it.
  On Continue the host's start of the day change was discarded as a repeat; the returning game now
  takes messages it never answered only after it has caught up, so its answers are delivered.

### Server
- Needs the 0.1.2 server, which keeps how far each seat's answers were heard. A game saved by an
  older server counts every message as answered.

## [0.1.1] - 2026-10-26

### Added
- Support for HD Mod 5.8 R24, which HD Mod updated itself to. 5.8 R22 still works.

### Changed
- For any other HotA or HD Mod version the installer says to update both through HotA's launcher.
  A fresh install from the official HotA 1.8.1 installer has HD 5.8 R7 and is not supported.

## [0.1.0] - 2026-10-26

### Added
- The first player download: `HeroesPost-Setup` finds HotA, checks its version, installs the patch
  and registers in Add or Remove Programs; it also uninstalls. English or Russian.
- The Heroes Post lobby on HotA's multiplayer screen: accounts, your games, the server's games,
  chat, Create, Join and Continue. The server is chosen in the game.
- Simultaneous-turn games through the server that you can leave and continue at any time. Your
  seat is saved at End Turn, at the start of each day and after you act.
- A server that runs on Linux in Docker, keeps games across restarts, and lets players reconnect
  after a dropped connection without HotA noticing.

### Known issues
- Supports HD Mod 5.8 R22 only. HD Mod updates itself to 5.8 R24 through HotA's launcher; that
  needs 0.1.1 or later.
- Two HotAs on one PC damage each other's drawing when focus switches. The cause is unknown.
- The Linux server has no stand-ins: an away player's seat is not played, and the turn limit is
  not enforced.
