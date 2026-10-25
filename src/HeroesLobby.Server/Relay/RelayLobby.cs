using HeroesLobby.Contracts;

namespace HeroesLobby.Server.Relay;

/// <summary>The server's lobby chat: the newest lines, in memory only.</summary>
public sealed class RelayChat
{
    private const int Kept = 200;
    private readonly object gate = new();
    private readonly LinkedList<RelayChatLine> lines = new();
    private long next = 1;

    public RelayChatLine Post(string name, string text)
    {
        lock (gate)
        {
            var line = new RelayChatLine(next++, name, text, DateTimeOffset.UtcNow);
            lines.AddLast(line);
            while (lines.Count > Kept) lines.RemoveFirst();
            return line;
        }
    }

    /// <summary>Lines after <paramref name="after"/>; a new client (0) gets the last <paramref name="recent"/>.</summary>
    public List<RelayChatLine> After(long after, int recent = 30)
    {
        lock (gate)
        {
            var found = lines.Where(l => l.Id > after);
            return (after == 0 ? found.TakeLast(recent) : found).ToList();
        }
    }
}

public static class RelayLobbyEndpoints
{
    // HotA names a session "<game>ú<host>"; the relay keeps the bytes as Latin-1.
    private const char SessionSeparator = 'ú';

    public static RelayOpenGame Open(RelayGameView game)
    {
        var at = game.Name.IndexOf(SessionSeparator);
        var name = at < 0 ? game.Name : game.Name[..at];
        var host = at < 0 ? "" : game.Name[(at + 1)..];
        return new(game.Instance, name, host, game.Started, game.Seats.Count, (int)game.MaxPlayers, game.Password);
    }

    public static void MapRelayLobbyEndpoints(this WebApplication app)
    {
        // The in-game lobby polls this every few seconds.
        app.MapGet("/api/relay/lobby", (long? after, HttpContext context, LobbyStore lobby, RelayHub hub, RelayCaptures captures,
            RelayServer relay, StandInManager standIns, RelayChat chat) =>
        {
            var player = lobby.Authenticate(context.Request.Headers.Authorization.ToString());
            context.Response.Headers.CacheControl = "no-store";
            return new RelayLobby(player.DisplayName, relay.Port, standIns.Enabled, standIns.DefaultTurnSeconds,
                RelayEndpoints.MyGames(player, hub, captures, standIns),
                hub.Games().Select(Open).OrderBy(g => g.Started).ThenBy(g => g.Name, StringComparer.OrdinalIgnoreCase).ToList(),
                chat.After(Math.Max(0, after ?? 0)));
        });

        app.MapPost("/api/relay/chat", (RelayChatPost request, HttpContext context, LobbyStore lobby, RelayChat chat) =>
        {
            var player = lobby.Authenticate(context.Request.Headers.Authorization.ToString());
            return chat.Post(player.DisplayName, LobbyStore.Clean(request.Text, "Message", 200));
        }).RequireRateLimiting("writes");
    }
}
