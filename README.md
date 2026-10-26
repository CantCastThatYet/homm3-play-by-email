# Heroes Post

A native desktop companion for self-hosted Heroes III communities. Written in C# with Avalonia, ASP.NET Core, SignalR, and SQLite on .NET 10.

**The Windows prototype plays HotA multiplayer through this server.** HotA's DirectPlay transport is replaced by a relay in the server, so players host and join in HotA's own multiplayer screens and chat in HotA. A player can leave after ending a turn, or mid-turn, and come back later. Meanwhile the server plays their seat with its own HotA copy. It finishes a turn they ended, and ends any other turn after the game's turn limit. See [the relay and its verification](docs/relay-transport.md).

## Try it

Start the server from this directory. Stand-ins need Windows and prepared HotA copies; see [Playing by hand](docs/relay-transport.md#playing-by-hand):

```sh
dotnet run --project src/HeroesLobby.Server -- --urls http://0.0.0.0:5080
```

Each player opens the desktop app, enters the server address and a name, connects, and clicks **Set up HotA** once. **Play HotA** opens HotA connected to the server. In HotA choose New Game, Multiplayer, TCP/IP, then Host or Join, and turn on simultaneous turns in the game options. Keep the app open while playing: it saves your position on the server. After you leave, your game appears under **Your games** with **Continue**; the turn limit for away seats is chosen there too.

**In-game lobby (no app).** Copy `version.dll` and `HeroesPost.Native.dll` from `artifacts/native/patch-x86` into the HotA folder and put `server=http://host:5080` in `HeroesPost.ini` there. HotA's multiplayer screen then has a Heroes Post button. It asks you to sign in or register an account (a name and a password that work from any computer), then opens a lobby with your games, the server's games, chat, Create, Join and Continue; see [In-game lobby](docs/relay-transport.md#in-game-lobby-game-patch) and [Accounts](docs/relay-transport.md#accounts).

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

There is no central service or account provider. `compose.yaml` publishes port 5080, stores state in the named `lobby-data` volume, and restarts the container automatically. Change `Lobby__Name` to name your community. `docker compose down` retains the volume; adding `-v` deletes it.

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
