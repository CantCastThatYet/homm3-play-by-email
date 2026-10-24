# Local validation

Current solo-resume integration: **34 C# tests pass**, covering branch authorization, revisions, leases, restart persistence, archive validation, failure fencing and the desktop Resume control. Native tests cover capture, completion, stream import, request validation, ports, departure handling, adventure input gating, known map intro, IPC, world audits, resume parsing, packet journaling and transport rejection.

The real desktop coordinator and HTTP server started two HotA clients, persisted their initial native checkpoints, closed both clients, disconnected Red and restored Blue alone. Blue's actual End Turn passed the completion audit and automatically published branch revision 2. Red's cold unfinished and completed-turn restores also passed. Date, resources, hero records and human membership were checked against the source checkpoint. Evidence is under `artifacts/solo-lobby-proof/run-v2` and `artifacts/completion-proof/solo-v11` through `solo-v13`.

This proves bounded solo turn restoration and submission. It does **not** prove shared next-day merging, inbound event replay, offline PvP, every game button or every adventure interaction. Completed turns remain held. See [solo resume instructions and limits](docs/solo-resume.md).

The following entries record earlier milestones and are not the current feature inventory.

The native setup adapter automatically hosted a TCP/IP session on port 24777, selected `Judgement Day.h3m`, and joined from a second isolated client on port 24778 using `127.0.0.1`. It then completed native Start and both clients' options approval. Read-only native inspection verified both clients on day 1 with Red/Blue local colors, allocated maps, matching participant tables and active simultaneous turns. Both clients were left open without ending either turn. See [the automatic startup proof](re/automatic-native-start-proof.md). This establishes game entry, not asynchronous logout/reconnect.

Checked on 2026-10-24:

- .NET 10.0.402 build and test: all 7 tests passed, with warnings treated as errors.
- Headless Avalonia test exercised the actual desktop controls and HTTP client against Kestrel: connect, create, configure, ready, lock, disconnect, and reconnect.
- Server integration tests covered password checks, authorization, simultaneous joins, stale revisions, persisted identity/readiness, host transfer, and SignalR change hints.
- Linux Docker image built and served HTTP. A game and bearer identity survived a container restart. The temporary verification game was removed afterward.
- Windows x64 Native AOT publication succeeded. Executable and required DLLs total 37,631,016 bytes; the ZIP is approximately 16 MB.

The initial Windows desktop build launched and exposed its accessibility tree. Windows screenshot capture and pointer input failed in the automation environment; the user then stopped Computer Use with Escape. No further desktop automation was attempted. UI previews in `artifacts/screenshots/` are Avalonia headless renders, not OS screenshots. The final Native AOT package has been compiled but has not undergone an interactive end-to-end run.

Linux/macOS desktop builds and the GitHub Actions workflow have not been run here. The updated Windows Native AOT package includes native startup and its adapter; capture/recovery development commands are not exposed as completed asynchronous features.
