# Heroes Post

A native desktop companion for self-hosted Heroes III communities. Written in C# with Avalonia, ASP.NET Core, SignalR, and SQLite on .NET 10.

**The Windows prototype can start HotA from the lobby and restore a saved player turn while that player is alone.** It starts two original HotA clients with simultaneous turns on Judgement Day, stores each player's native checkpoint, and exposes **Resume HotA** after the clients close. A resumed player can end their turn and automatically upload the completed checkpoint. Shared next-day merging, notifications and offline PvP remain unfinished: this is not yet a complete asynchronous campaign. The server runs no game clients. See [solo resume instructions and limits](docs/solo-resume.md).

## Try it

Start the server from this directory with Docker:

```sh
docker compose up --build -d
```

Open the updated desktop app, enter `http://localhost:5080` and a player name, then connect. Click **Set up HotA** on each player's app. Create a two-player **Judgement Day** game, join it from the other app, retain Red/Blue and Random starting choices, then mark both players ready. The host launches first; the guest joins automatically. Same-PC testing needs separate profiles and isolated HotA copies. Other maps/preferences and WAN/NAT connectivity are not verified by this startup build.

Wait for each lobby to confirm its initial checkpoint is saved. After closing HotA, select the started game and choose **Resume HotA**; the other player does not need to be connected. Keep the lobby open while playing. After End Turn, wait for **Turn saved** before closing HotA. A completed turn reopens held, because shared next-day merging is not implemented. Mid-turn edits are not automatically uploaded; closing before completion can return you to the last server checkpoint.

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
| `HeroesLobby.Server` | Authoritative lobby state, SQLite persistence, validation, HTTP API, SignalR hints |
| `HeroesLobby.Desktop` | Avalonia desktop app, per-server identity, HTTP client, live updates and polling |
| `HeroesLobby.Tests` | Server integration and headless desktop tests |

Writes are transactional and carry a revision where stale changes matter. Settings or roster changes clear readiness. Disconnecting never leaves a game; leaving is an explicit action. The last player leaving removes the game. Leaving as host transfers hosting to the remaining roster. Locked lobbies must be reopened before anyone can change preferences or leave.

The native prototype validates its supported map and frozen two-player configuration. General map labels and hero preferences do not imply support for other native configurations. `GET /api/info` advertises `gameIntegrationAvailable: false` because complete asynchronous campaigns, shared-day progression and offline PvP are not implemented.

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
