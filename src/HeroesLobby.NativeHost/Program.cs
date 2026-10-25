using HeroesLobby.NativeHost;

namespace HeroesLobby.NativeHost;

internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        using var cancellation = new CancellationTokenSource(TimeSpan.FromMinutes(5));
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };
        try
        {
            if (args.Length is 5 or 6 && args[0] is "play" or "continue")
            {
                // Development stand-in for the app: start or continue a relay game, then keep the seat saved until HotA closes.
                var token = Environment.GetEnvironmentVariable("HEROES_POST_TOKEN") ?? throw new InvalidOperationException("Set HEROES_POST_TOKEN to the player's token.");
                var server = new Uri(args[3]);
                using var http = new HttpClient { BaseAddress = server, Timeout = TimeSpan.FromSeconds(90) };
                var api = new RelayApi(http, token);
                var info = await api.Info(cancellation.Token);
                var relay = new RelayEndpoint(server.Host, info.Port, token, args[4]);
                var pid = args[0] == "play" ? await RelayPlay.Play(args[1], args[2], relay, cancellation.Token)
                    : await RelayPlay.Continue(args[1], args[2], relay, api, Guid.Parse(args[5]), Path.Combine(args[1], "HeroesPost.downloads"), cancellation.Token);
                Console.WriteLine($"HotA PID: {pid}");
                using var forever = new CancellationTokenSource();
                Console.CancelKeyPress += (_, _) => forever.Cancel();
                await new SeatKeeper(pid, api, text => Console.WriteLine($"{DateTime.Now:HH:mm:ss} {text}")).Run(forever.Token);
                Console.WriteLine("HotA closed.");
            }
            else if (args.Length == 2 && args[0] == "games")
            {
                var token = Environment.GetEnvironmentVariable("HEROES_POST_TOKEN") ?? throw new InvalidOperationException("Set HEROES_POST_TOKEN to the player's token.");
                using var http = new HttpClient { BaseAddress = new Uri(args[1]) };
                foreach (var game in await new RelayApi(http, token).Games(cancellation.Token)) Console.WriteLine(game);
            }
            else if (args.Length == 3 && args[0] == "prepare") Console.WriteLine(NativeBridge.Prepare(args[1], args[2]));
            else if (args.Length == 3 && args[0] == "launch") await NativeBridge.Launch(args[1], args[2], cancellation.Token);
            else if(args.Length==5&&args[0]=="resume"&&int.TryParse(args[4],out var resumeColor))
                Console.WriteLine(await NativeBridge.Resume(args[1],args[2],args[3],resumeColor,cancellation.Token));
            else if(args.Length==2&&args[0]=="capture-current"&&int.TryParse(args[1],out var capturePid))
                Console.WriteLine(await NativeBridge.CaptureCurrentBranch(capturePid,cancellation.Token));
            else if(args.Length==3&&args[0]=="bundle") CaptureBundle.Write(args[1],args[2]);
            else if (args.Length is 7 or 8 or 9 && args[0] == "stage-session")
            {
                if (!int.TryParse(args[6], out var bootstrapPid)) throw new ArgumentException("A running owned client PID is required.");
                var port = 0;
                if (args.Length == 9 && !int.TryParse(args[8], out port)) throw new ArgumentException("The optional native port must be numeric.");
                Console.WriteLine(await NativeBridge.BootstrapNetwork(bootstrapPid, args[1],
                    new BootstrapRequest(args[2], args[3], args[4], args[5], args.Length >= 8 ? args[7] : "", port), cancellation.Token));
            }
            else if (args.Length == 8 && args[0] == "start-session")
            {
                if (!int.TryParse(args[2], out var startPid) || !uint.TryParse(args[4], out var firstPeer) || !uint.TryParse(args[5], out var secondPeer) || !int.TryParse(args[7], out var date))
                    throw new ArgumentException("Specify a PID, two native player IDs and a numeric month/week/day date.");
                Console.WriteLine(await NativeBridge.StartNetwork(startPid, args[1], new StartRequest(args[3], firstPeer, secondPeer, args[6], date), cancellation.Token));
            }
            else if (args.Length == 3 && args[0] == "watch-save" && int.TryParse(args[1], out var watchPid))
                await NativeBridge.WatchSave(watchPid, args[2], cancellation.Token);
            else if (args.Length is 2 or 3 && args[0] == "watch-completion" && int.TryParse(args[1], out var completionPid))
            {
                if (args.Length == 3 && args[2] != "snapshot") throw new ArgumentException("Optional completion mode must be 'snapshot'.");
                Console.WriteLine(await NativeBridge.WatchCompletion(completionPid, args.Length == 3, cancellation.Token));
            }
            else if (args.Length == 7 && args[0] == "queue-capture" && long.TryParse(args[5], out var revision))
            {
                var pending = new CheckpointOutbox(args[6]).EnqueueCapture(args[1], new Uri(args[2]), args[3], args[4], revision);
                Console.WriteLine($"Queued capture {pending.Key}; {pending.Length} bytes. This stores checkpoint material, not a completed playable turn.");
            }
            else if (args.Length == 4 && args[0] == "send-pending")
            {
                var token = Environment.GetEnvironmentVariable("HEROES_POST_TOKEN") ?? throw new InvalidOperationException("Set HEROES_POST_TOKEN to the authenticated player token. Do not put it in command-line arguments.");
                var outbox = new CheckpointOutbox(args[1]); var server = new Uri(args[2]);
                using var http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromSeconds(90) };
                foreach (var item in outbox.Pending(server, args[3]))
                {
                    var receipt = await outbox.Send(item, http, server, args[3], token, cancellation.Token);
                    Console.WriteLine($"Stored capture {item.Key}: receipt {receipt.Id}, SHA-256 {receipt.Sha256}.");
                }
            }
            else if (args.Length is 2 or 3 && args[0] == "probe" && int.TryParse(args[1], out var pid))
            {
                cancellation.CancelAfter(TimeSpan.FromSeconds(10));
                Console.WriteLine(await NativeBridge.Request(pid, args.Length == 3 ? args[2] : "probe", cancellation.Token));
            }
            else
            {
                Console.Error.WriteLine("Native integration experiment (Windows, pinned build):\n  prepare <installed-game-directory> <new-isolated-directory>\n  launch <isolated-directory> <native-build-directory>\n  stage-session <isolated-directory> Host|Join <player-name> <session-name> <IPv4> <pid> [map.h3m] [local-port]\n  start-session <isolated-directory> <pid> HostStart|PeerAccept <peer-id-1> <peer-id-2> <map.h3m> <date-MWD>\n  probe <pid> [command]\n  play <isolated-directory> <native-build-directory> <server-url> <relay-name>\n  continue <isolated-directory> <native-build-directory> <server-url> <relay-name> <game-id>\n  games <server-url>   (play, continue, games: token in HEROES_POST_TOKEN)\n  watch-completion <pid> [snapshot]\n  watch-save <pid> <save-basename>\n  queue-capture <manifest-path> <server> <game-id> <player-id> <base-revision> <outbox-directory>\n  send-pending <outbox-directory> <server> <player-id> (token: HEROES_POST_TOKEN)\nThis tool does not yet implement playable asynchronous turns.");
                return 2;
            }
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex.Message); return 1; }
    }
}
