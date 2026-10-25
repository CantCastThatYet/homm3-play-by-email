using System.Text;
using System.Text.Json;
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

    /// <summary>The last <paramref name="count"/> lines, oldest first.</summary>
    public List<RelayChatLine> Recent(int count = 30)
    {
        lock (gate) return lines.TakeLast(count).ToList();
    }
}

/// <summary>
/// The in-game lobby, pushed over a relay connection. After Hello with its lobby token a
/// client sends LobbyWatch; it then gets its lobby state, the recent chat, a new state
/// whenever its contents change and every new chat line. Chat and turn limits come back
/// on the same connection.
/// </summary>
public sealed class RelayLobbyFeed
{
    // HotA names a session "<game>ú<host>"; the relay keeps the bytes as Latin-1.
    private const char SessionSeparator = 'ú';
    // Changes arrive in bursts (a join is several frames); one push covers the burst.
    private static readonly TimeSpan Settle = TimeSpan.FromMilliseconds(100);
    private const int ChatBurst = 10;
    private static readonly TimeSpan ChatWindow = TimeSpan.FromSeconds(20);

    private sealed class Watcher(RelayAccount account)
    {
        public RelayAccount Account { get; } = account;
        public byte[] Last { get; set; } = [];
        public Queue<DateTimeOffset> Posts { get; } = new();
    }

    private readonly RelayHub hub;
    private readonly RelayCaptures captures;
    private readonly RelayChat chat;
    private readonly ILogger<RelayLobbyFeed> logger;
    private readonly int defaultTurnSeconds;
    private readonly object gate = new();
    private readonly Dictionary<IRelayPeer, Watcher> watchers = [];
    private readonly Timer timer;

    public RelayLobbyFeed(RelayHub hub, RelayCaptures captures, RelayChat chat, IConfiguration config, ILogger<RelayLobbyFeed> logger)
    {
        (this.hub, this.captures, this.chat, this.logger) = (hub, captures, chat, logger);
        defaultTurnSeconds = StandInManager.TurnSecondsSetting(config);
        timer = new Timer(_ => Publish());
        hub.Changed += Changed;
        captures.Stored += Changed;
    }

    public static bool Handles(ushort type) => type is RelayProtocol.LobbyWatch or RelayProtocol.LobbyChat or RelayProtocol.LobbyTurnLimit;

    public static RelayOpenGame Open(RelayGameView game)
    {
        var at = game.Name.IndexOf(SessionSeparator);
        var name = at < 0 ? game.Name : game.Name[..at];
        var host = at < 0 ? "" : game.Name[(at + 1)..];
        return new(game.Instance, name, host, game.Started, game.Seats.Count, (int)game.MaxPlayers, game.Password);
    }

    public void Changed() => timer.Change(Settle, Timeout.InfiniteTimeSpan);

    public void Disconnected(IRelayPeer peer) { lock (gate) watchers.Remove(peer); }

    public void Handle(IRelayPeer peer, byte[] frame)
    {
        var type = (ushort)(frame[0] | frame[1] << 8);
        var r = new FrameReader(frame, 2);
        var account = hub.Account(peer);
        if (account is null || account.StandIn)
        {
            peer.Post(Notice(RelayProtocol.NoticeSignIn, "The server does not know this identity."));
            return;
        }
        lock (gate)
        {
            switch (type)
            {
                case RelayProtocol.LobbyWatch:
                    var watcher = watchers[peer] = new Watcher(account);
                    Push(peer, watcher, hub.Games());
                    peer.Post(ChatFrame(chat.Recent()));
                    logger.LogInformation("Lobby watched by {Name} from {Peer}", account.Name, peer.Label);
                    return;
                case RelayProtocol.LobbyChat:
                    Say(peer, account, Encoding.UTF8.GetString(r.Blob()));
                    return;
                case RelayProtocol.LobbyTurnLimit:
                    var id = r.Guid(); var seconds = r.U32();
                    if (!r.Ok) return;
                    if (seconds is < 30 or > 7 * 24 * 3600) { peer.Post(Notice(RelayProtocol.NoticeText, "Choose a turn limit between 30 seconds and 7 days.")); return; }
                    if (hub.Games().FirstOrDefault(g => g.Instance == id) is not { } game || game.Seats.All(s => s.Account != account.Id))
                    { peer.Post(Notice(RelayProtocol.NoticeText, "You have no seat in this game.")); return; }
                    hub.SetTurnSeconds(id, (int)seconds);
                    return;
            }
        }
    }

    private void Say(IRelayPeer peer, RelayAccount account, string text)
    {
        if (!watchers.TryGetValue(peer, out var watcher)) return;
        var now = DateTimeOffset.UtcNow;
        while (watcher.Posts.Count > 0 && now - watcher.Posts.Peek() > ChatWindow) watcher.Posts.Dequeue();
        if (watcher.Posts.Count >= ChatBurst) { peer.Post(Notice(RelayProtocol.NoticeText, "Slow down: too many messages.")); return; }
        string clean;
        try { clean = LobbyStore.Clean(text, "Message", 200); }
        catch (LobbyError error) { peer.Post(Notice(RelayProtocol.NoticeText, error.Message)); return; }
        watcher.Posts.Enqueue(now);
        var frame = ChatFrame([chat.Post(account.Name, clean)]);
        foreach (var other in watchers.Keys) other.Post(frame);
    }

    private void Publish()
    {
        try
        {
            lock (gate)
            {
                if (watchers.Count == 0) return;
                var games = hub.Games();
                foreach (var (peer, watcher) in watchers) Push(peer, watcher, games);
            }
        }
        catch (Exception error) { logger.LogError(error, "Lobby push failed"); }
    }

    // Sends the watcher's state if it differs from the last one it got.
    private void Push(IRelayPeer peer, Watcher watcher, IReadOnlyList<RelayGameView> games)
    {
        var state = new RelayLobby(watcher.Account.Name, defaultTurnSeconds,
            RelayEndpoints.MyGames(watcher.Account.Id, games, captures, defaultTurnSeconds),
            games.Select(Open).OrderBy(g => g.Started).ThenBy(g => g.Name, StringComparer.OrdinalIgnoreCase).ToList());
        var json = JsonSerializer.SerializeToUtf8Bytes(state, LobbyJsonContext.Default.RelayLobby);
        if (json.AsSpan().SequenceEqual(watcher.Last)) return;
        watcher.Last = json;
        peer.Post(new FrameWriter(RelayProtocol.LobbyState).Blob(json).Finish());
    }

    private static byte[] ChatFrame(List<RelayChatLine> lines) =>
        new FrameWriter(RelayProtocol.LobbyChatLines).Blob(JsonSerializer.SerializeToUtf8Bytes(lines, LobbyJsonContext.Default.ListRelayChatLine)).Finish();

    private static byte[] Notice(uint code, string text) =>
        new FrameWriter(RelayProtocol.LobbyNotice).U32(code).Blob(Encoding.UTF8.GetBytes(text)).Finish();
}
