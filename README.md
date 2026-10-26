# Heroes Post

A native desktop companion for self-hosted Heroes III communities. Written in C# with Avalonia, ASP.NET Core, SignalR, and SQLite on .NET 10.

**The Windows prototype plays HotA multiplayer through this server.** HotA's DirectPlay transport is replaced by a relay in the server, so players host and join in HotA's own multiplayer screens and chat in HotA. A player can leave after ending a turn, or mid-turn, and come back later. Meanwhile the server plays their seat with its own HotA copy. It finishes a turn they ended, and ends any other turn after the game's turn limit. See [the relay and its verification](docs/relay-transport.md).

## Try it

Start the server from this directory. Stand-ins need Windows and prepared HotA copies; see [Playing by hand](docs/relay-transport.md#playing-by-hand):

```sh
dotnet run --project src/HeroesLobby.Server -- --urls http://0.0.0.0:5080
```

Each player opens the desktop app, enters the server address and a name, connects, and clicks **Set up HotA** once. **Play HotA** opens HotA connected to the server. In HotA choose New Game, Multiplayer, TCP/IP, then Host or Join, and turn on simultaneous turns in the game options. Keep the app open while playing: it saves your position on the server. After you leave, your game appears under **Your games** with **Continue**; the turn limit for away seats is chosen there too.

**In-game lobby (no app).** Players run `HeroesPost-Setup.exe` (see [Player download](#player-download)), or copy `version.dll` and `HeroesPost.Native.dll` into the HotA folder by hand. HotA's multiplayer screen then has a Heroes Post button. The first time, it asks for the server's address; the lobby's Server button changes it later. It then asks you to sign in or register an account (a name and a password that work from any computer), then opens a lobby with your games, the server's games, chat, Create, Join and Continue; see [In-game lobby](docs/relay-transport.md#in-game-lobby-game-patch) and [Accounts](docs/relay-transport.md#accounts).

The relay listens on TCP port 5090 (`Relay:Port`) next to the HTTP port. Players' copies connect to the server's host name on that port. Only the supported HotA build is accepted; see `NativeBridge.Fingerprints`. Hosting by hand in HotA's menus has not been verified yet, and other limits are listed in the relay document.

The Windows native package is `artifacts/HeroesPost-win-x64.zip` when built locally. Extract the whole folder and run `HeroesLobby.Desktop.exe`; no .NET installation is required. Keep the DLLs beside the executable.

## Develop

Install the [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0). From two terminals:

```sh
dotnet run --project src/HeroesLobby.Server
```

```sh
dotnet run --project src/HeroesLobby.Desktop
```

On the development machine used to create this prototype, `.tools/dotnet/dotnet.exe` is a local SDK and can replace `dotnet` in these commands. That SDK is not part of the source distribution.

```sh
dotnet test HeroesPost.slnx
```

Tests cover authentication and passwords, concurrent joins, revision conflicts, readiness/locking, persistent identity after server restart, host transfer, live change hints, and the desktop flow against a real local HTTP server. The desktop test also renders previews to `artifacts/screenshots/` using Avalonia's headless platform.

## Host your own server

There is no central service or account provider. The server runs on Linux; `docker compose up -d --build` on a Linux host is enough. `compose.yaml` publishes HTTP on port 5080 and the game relay on TCP port 5090, stores state in the named `lobby-data` volume, and restarts the container automatically.

Linux specifics, checked 2026-10-26 with the image on Ubuntu 24.04 and two HotAs on Windows:
- Two players registered, hosted, joined and played a day change.
- One left and came back with Continue, and both reached the next day with identical games.
- A container restart mid-game was resumed by both clients without a gap.

Keep in mind:
- **Relay port:** the relay must be reachable directly on the same port number the server listens on (`Relay__Port`), because the server tells clients its own port. It is plain TCP, not HTTP, so it does not go through a reverse proxy. It is also not encrypted, including the sign-in token it carries.
- **No stand-ins:** stand-ins need Windows and prepared HotA copies, so on Linux nobody plays an away player's seat. The turn limit is not enforced either. The others wait until that player continues. Change `Lobby__Name` to name your community. `docker compose down` retains the volume; adding `-v` deletes it.

Without Docker:

```sh
dotnet run --project src/HeroesLobby.Server -- --urls http://0.0.0.0:5080
```

Configuration can be supplied through environment variables:

| Variable | Purpose |
| --- | --- |
| `URLS` | Listening addresses; the source default is `http://127.0.0.1:5080` |
| `Lobby__Name` | Community name shown in the app |
| `Lobby__Database` | SQLite path; source default is `data/lobby.db` relative to the server's working directory |

Use HTTPS for an Internet-facing server: put it behind a TLS reverse proxy, forward HTTP and WebSocket traffic, and point clients directly at the HTTPS URL. The client deliberately rejects HTTP redirects. `/health` is the health endpoint. Proxy deployments currently share the write limit of 60 requests per minute per observed IP; forwarded client addresses are not trusted automatically.

Run one server process per database. Back up the data volume while the server is stopped, or use SQLite's backup mechanism; copying only the live `.db` file can omit WAL data. This prototype has open player registration and no operator console, quotas, account recovery, or abuse moderation. It is intended for small trusted communities.

## Identity and privacy

The app remembers a separate identity for each server URL. On Windows, credentials live in `%LOCALAPPDATA%/HeroesPost/profile.json`. Other platforms use .NET's local application data directory. `HEROES_POST_PROFILE` overrides the profile directory, which is useful for trying two players locally.

Back up this private file: its bearer tokens are the keys to your reserved seats. Display names are labels, not authentication. Changing your name does not create a new identity on a server you already joined. Changing the server hostname/port creates a different profile entry. There is no recovery or credential rotation UI yet. Do not share the profile or commit it to source control.

Game passwords control joining. Game names, map labels, rosters, and preferences are visible to anyone who can reach the server. Passwords are salted and hashed on the server; player tokens are stored there as hashes. The local profile contains the actual tokens and relies on your user account's filesystem protection.

## Project layout

| Project | Responsibility |
| --- | --- |
| `HeroesLobby.Contracts` | Versioned lobby DTOs and source-generated JSON metadata |
| `HeroesLobby.Server` | Identities, game relay, seat captures, stand-ins, HTTP API; the older lobby API |
| `HeroesLobby.Desktop` | Avalonia desktop app: per-server identity, Play HotA, Your games, Continue |
| `HeroesLobby.NativeHost` | HotA adapter control, captures, resume, relay API client and seat keeper |
| `HeroesLobby.Tests` | Server integration and headless desktop tests |

Writes are transactional and carry a revision where stale changes matter. Settings or roster changes clear readiness. Disconnecting never leaves a game; leaving is an explicit action. The last player leaving removes the game. Leaving as host transfers hosting to the remaining roster. Locked lobbies must be reopened before anyone can change preferences or leave.

The server still serves the earlier lobby API (games, readiness, scripted native launch and solo-resume branches), but the app no longer uses it: games are created in HotA itself.

## Player download

`native/package.ps1` builds what players download, in `artifacts/release`:

- `HeroesPost-Setup-<version>.exe`, a small installer carrying both DLLs. It finds HotA (the HotA installer's uninstall entry, a running HotA, the usual folders, or Browse) and checks the six files the patch is pinned to (`native/supported_build.hpp`). It refuses an unsupported HotA build, another mod's `version.dll`, or a running HotA. It installs the two DLLs, adds an Add or Remove Programs entry for the Windows user, and has Uninstall. It asks for administrator rights only when Windows refuses a write. English or Russian, after the Windows language. `/S /D=<folder> [/uninstall] [/lang=en|ru] [/log=<file>]` runs it without a window.
- `HeroesPost-<version>.zip`, the same two DLLs and a readme for installing by hand.
- `SHA256SUMS.txt`.

The installer does not set the server: the game asks for it. Pushing a tag `v1.2.3` runs `.github/workflows/release.yml`, which builds these and publishes a GitHub release. The files are not code-signed, so Windows SmartScreen warns about the installer on first run.

Checked 2026-10-26 on a copy of a real HotA 1.8.1 installation:
- A silent install wrote the two DLLs and the Add or Remove Programs entry, and no server.
- In HotA, Heroes Post opened the server window. A pasted address was saved and led to sign-in and the lobby.
- An invalid address was refused. Switching servers and back from the lobby and the sign-in window found the earlier sign-in.
- A changed HotA file, another mod's `version.dll`, a folder without HotA, and uninstalling while HotA ran were each refused with the reason.
- Uninstalling from the copy in the HotA folder removed every Heroes Post file, that copy included.
- Not tested: the request for administrator rights, and the release workflow.

### When HotA or HD Mod updates

The patch works only with the HotA and HD Mod builds in `native/supported_build.hpp`; with any other it stays off, and the installer says which files differ. HD Mod updates itself often, and every update moves code and data inside `HD_HOTA.dll`, where the patch hooks and reads. The code names each of those addresses as it is in HD 5.8 R22 and passes it through `hp_build::Rva()`, which translates it for the running build. To support a new HD build:

1. Keep the previous build's `HD_HOTA.dll` and get the new one.
2. Run `python native/tools/hd_port.py OLD/HD_HOTA.dll NEW/HD_HOTA.dll <addresses>`. It finds each address in the new build: code by its bytes (with relocated words and call offsets masked) or through all its callers, data through every instruction that refers to it. It marks anything without a unique answer for review.
3. Add a `Move` table and the build's file hashes to `native/supported_build.hpp`, and its hashes to `NativeBridge.HdBuilds`.
4. Play a game with Continue on the new build before releasing; byte checks and data layouts can change as well as addresses.

Checked 2026-10-26 for HD 5.8 R24, which moved 18 functions and 10 data areas, including the whole simultaneous-turn block by 0x24 bytes:
- Two R24 HotAs played a game with simultaneous turns and a day change.
- One left and came back with Continue, and both reached day 3 with identical games.
- R22 still works.
- Two things besides addresses changed in R24. HD's operator new and delete now go through pointers HD sets at start. Its memory files no longer carry R22's allocator tag. Both are per-build entries now.

Players with a fresh HotA 1.8.1 from the official installer have the August `HotA.dll` and HD 5.8 R7. The installer tells them to update through HotA's launcher. That is how the test machine got its builds; it has not been checked on a fresh install.

## Build desktop packages

For a regular self-contained package, build on the target platform and choose its runtime identifier (`win-x64`, `linux-x64`, `osx-arm64`, etc.):

```sh
dotnet publish src/HeroesLobby.Desktop -c Release -r win-x64 --self-contained true -o artifacts/desktop-win-x64
```

For a smaller native executable, install the platform's [.NET Native AOT prerequisites](https://learn.microsoft.com/dotnet/core/deploying/native-aot/) and publish with:

```sh
dotnet publish src/HeroesLobby.Desktop -c Release -r win-x64 --self-contained true -p:PublishAot=true -p:OptimizationPreference=Size -p:DebugType=None -p:DebugSymbols=false -o artifacts/desktop-native-win-x64
```

Package the executable and native libraries, excluding `.pdb` debugging symbols. Native AOT builds should run on their target OS. Windows desktop and the Linux Docker server have been checked locally; Linux/macOS desktop packaging still needs validation on those systems. Avalonia draws its own controls without an embedded browser.

Build the x86 adapter with `native/build.ps1 -OutputDirectory artifacts/native/lobby-x86 -Test` using MSVC. The desktop includes that directory by default; override it with `-p:NativeAdapterDirectory=C:/absolute/path/to/native/build`. Only `HeroesPost.Native.dll` and `HeroesPost.Injector.exe` are packaged, never game files or local captures.

## License

The lobby source is MIT licensed; see [LICENSE](LICENSE). Dependencies retain their respective licenses. Heroes III and HotA are third-party projects and are not included. The local `re/` research directory, tools, game binaries, credentials, and build outputs are excluded from the source archive.
