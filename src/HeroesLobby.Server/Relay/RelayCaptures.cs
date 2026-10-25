using System.Collections.Concurrent;
using System.IO.Compression;
using System.Text.Json;
using HeroesLobby.Contracts;

namespace HeroesLobby.Server.Relay;

/// <summary>A stored capture of one relay seat, the state a stand-in or returning player resumes from.</summary>
public sealed record RelayCapture(string Path, string Kind, int Color, int Day, int Week, int Month,
    long Consumed, long Sent, string MapFile, string Source, DateTimeOffset Stored);

/// <summary>
/// Latest captures per relay seat, uploaded by players' apps and by stand-ins. Files live under
/// Relay:CaptureDirectory/&lt;game&gt;/&lt;seat&gt;/; the newest few are kept.
/// </summary>
public sealed class RelayCaptures(IConfiguration config, ILogger<RelayCaptures> logger)
{
    private const int Kept = 4;
    private readonly string root = Path.GetFullPath(config["Relay:CaptureDirectory"] ?? "data/relay-captures");
    private readonly ConcurrentDictionary<(Guid, uint), RelayCapture> latest = new();
    private readonly object gate = new();

    public RelayCapture? Latest(Guid game, uint seat) => latest.GetValueOrDefault((game, seat));

    /// <summary>Validates and stores a capture bundle. Refuses one older than the seat's latest.</summary>
    public RelayCapture Store(Guid game, uint seat, byte[] bundle, string source)
    {
        var capture = Inspect(bundle);
        lock (gate)
        {
            if (Latest(game, seat) is { } current && capture.Consumed < current.Consumed)
                throw new LobbyError(409, $"This capture is older than the stored one ({capture.Consumed} < {current.Consumed} relay messages).");
            var folder = System.IO.Path.Combine(root, game.ToString("N"), seat.ToString("x8"));
            Directory.CreateDirectory(folder);
            var path = System.IO.Path.Combine(folder, $"{DateTime.UtcNow:yyyyMMdd-HHmmss-fff}-{source}.hpcap");
            var temporary = path + ".pending";
            File.WriteAllBytes(temporary, bundle);
            File.Move(temporary, path);
            capture = capture with { Path = path, Source = source, Stored = DateTimeOffset.UtcNow };
            latest[(game, seat)] = capture;
            foreach (var old in Directory.GetFiles(folder, "*.hpcap").OrderDescending().Skip(Kept))
                try { File.Delete(old); } catch (IOException) { }
            logger.LogInformation("Stored {Kind} capture of seat {Seat:x8} in {Game} from {Source}: day {Month}/{Week}/{Day}, relay {Consumed} in / {Sent} out",
                capture.Kind, seat, game, source, capture.Month, capture.Week, capture.Day, capture.Consumed, capture.Sent);
            return capture;
        }
    }

    public static RelayCapture Inspect(byte[] bundle)
    {
        int color;
        using (var zip = new ZipArchive(new MemoryStream(bundle), ZipArchiveMode.Read))
        {
            var entry = zip.GetEntry("manifest.json") ?? throw new InvalidDataException("The capture has no manifest.");
            if (entry.Length > 1024 * 1024) throw new InvalidDataException("The capture manifest is too large.");
            using var stream = entry.Open();
            using var manifest = JsonDocument.Parse(stream);
            color = manifest.RootElement.GetProperty("local_player").GetInt32();
        }
        var data = NativeCaptureArchive.Read(new MemoryStream(bundle), color);
        using var document = JsonDocument.Parse(data.ManifestBytes);
        var m = document.RootElement;
        long Number(string name) => m.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number ? value.GetInt64() : -1;
        var map = m.TryGetProperty("map_file", out var file) && file.ValueKind == JsonValueKind.String ? file.GetString()! : "";
        return new("", data.Kind == "completion" ? "completion" : "current", color, data.Day, data.Week, data.Month,
            Number("relay_consumed"), Number("relay_sent"), map, "", default);
    }
}

public static class RelayEndpoints
{
    public static void MapRelayEndpoints(this WebApplication app)
    {
        static PlayerInfo Who(HttpContext context, LobbyStore lobby) => lobby.Authenticate(context.Request.Headers.Authorization.ToString());
        static (RelayGameView Game, RelaySeatView Seat) Mine(RelayHub hub, Guid id, PlayerInfo player)
        {
            var game = hub.Games().FirstOrDefault(g => g.Instance == id) ?? throw new LobbyError(404, "This game is no longer on the relay.");
            var seat = game.Seats.FirstOrDefault(s => s.Account == player.Id) ?? throw new LobbyError(403, "You have no seat in this game.");
            return (game, seat);
        }

        app.MapGet("/api/relay", (RelayServer relay, StandInManager standIns) =>
            new RelayServerInfo(relay.Port, standIns.Enabled, standIns.DefaultTurnSeconds));

        app.MapGet("/api/relay/games", (HttpContext context, LobbyStore lobby, RelayHub hub, RelayCaptures captures, StandInManager standIns) =>
        {
            var player = Who(context, lobby);
            return hub.Games().SelectMany(g => g.Seats.Where(s => s.Account == player.Id).Select(s =>
            {
                var capture = captures.Latest(g.Instance, s.Id);
                var state = !g.Started ? "Lobby" : !s.Away ? (s.StandIn ? "StandIn" : "Playing") : "Away";
                return new RelayGameInfo(g.Instance, g.Name, g.Started, s.Name, s.Id, state, g.TurnSeconds ?? standIns.DefaultTurnSeconds,
                    g.LiveClients, capture?.Kind, capture is null ? null : $"{capture.Month}/{capture.Week}/{capture.Day}",
                    g.Seats.Select(x => x.Name).ToArray());
            })).ToList();
        });

        app.MapPost("/api/relay/games/{id:guid}/captures", async (Guid id, HttpContext context, LobbyStore lobby, RelayHub hub, RelayCaptures captures) =>
        {
            var player = Who(context, lobby);
            var limit = context.Features.Get<Microsoft.AspNetCore.Http.Features.IHttpMaxRequestBodySizeFeature>();
            if (limit is { IsReadOnly: false }) limit.MaxRequestBodySize = NativeCaptureArchive.MaxBytes;
            var (_, seat) = Mine(hub, id, player);
            // A stand-in holds the seat: its captures are newer than anything the player's app still has.
            if (seat.StandIn) throw new LobbyError(409, "A stand-in is playing this seat.");
            using var body = new MemoryStream();
            await context.Request.Body.CopyToAsync(body, context.RequestAborted);
            try { var capture = captures.Store(id, seat.Id, body.ToArray(), "player"); return Results.Ok(new { capture.Kind, capture.Consumed }); }
            catch (InvalidDataException error) { throw new LobbyError(400, "Invalid capture: " + error.Message); }
        });

        // Hands a seat back to its player: stops the stand-in after capturing it, then serves the latest capture.
        app.MapPost("/api/relay/games/{id:guid}/claim", async (Guid id, HttpContext context, LobbyStore lobby, RelayHub hub, RelayCaptures captures, StandInManager standIns) =>
        {
            var player = Who(context, lobby);
            var (game, seat) = Mine(hub, id, player);
            if (!game.Started) throw new LobbyError(409, "This game has not started.");
            if (!seat.Away && !seat.StandIn) throw new LobbyError(409, "Your HotA is already connected to this game.");
            await standIns.Release(id, seat.Id, context.RequestAborted);
            var capture = captures.Latest(id, seat.Id) ?? throw new LobbyError(404, "There is no saved position for your seat yet.");
            return new RelayClaim(id, game.Name, seat.Name, capture.Color, capture.Kind, capture.MapFile);
        });

        app.MapGet("/api/relay/games/{id:guid}/capture", (Guid id, HttpContext context, LobbyStore lobby, RelayHub hub, RelayCaptures captures) =>
        {
            var (_, seat) = Mine(hub, id, Who(context, lobby));
            var capture = captures.Latest(id, seat.Id) ?? throw new LobbyError(404, "There is no saved position for your seat yet.");
            context.Response.Headers.CacheControl = "no-store";
            return Results.File(capture.Path, "application/octet-stream", "seat.hpcap");
        });

        app.MapPut("/api/relay/games/{id:guid}/turn-limit", (Guid id, RelayTurnLimit request, HttpContext context, LobbyStore lobby, RelayHub hub) =>
        {
            Mine(hub, id, Who(context, lobby));
            if (request.TurnSeconds is < 30 or > 7 * 24 * 3600) throw new LobbyError(400, "Choose a turn limit between 30 seconds and 7 days.");
            hub.SetTurnSeconds(id, request.TurnSeconds);
            return Results.Ok(request);
        }).RequireRateLimiting("writes");
    }
}
