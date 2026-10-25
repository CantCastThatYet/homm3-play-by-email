using System.Security.Cryptography;

namespace HeroesLobby.Server.Relay;

/// <summary>One connected game client. Frames are queued and written by the transport.</summary>
public interface IRelayPeer
{
    string Label { get; }
    void Post(byte[] frame);
}

public sealed class RelayEntity
{
    public required uint Id { get; init; }
    public required bool Group { get; init; }
    public required IRelayPeer Owner { get; set; }
    public uint Flags { get; set; }
    public uint Parent { get; set; }
    public string Short { get; set; } = "";
    public string Long { get; set; } = "";
    public byte[] Data { get; set; } = [];
    public List<uint> Members { get; } = [];
    public List<uint> Subgroups { get; } = [];
    /// <summary>Game messages delivered to this player since it joined, in order. A resumed client replays from its captured count.</summary>
    public List<byte[]> Delivered { get; } = [];
    /// <summary>Game messages this player has sent. A capture that sent fewer is missing later actions.</summary>
    public int Sent { get; set; }
    /// <summary>Lobby player who created this seat; null for clients without a ticket.</summary>
    public string? Account { get; set; }
    public DateTimeOffset? AwaySince { get; set; }
    public const int DeliveredLimit = 200_000;
}

public sealed class RelaySession
{
    public required RelaySessionInfo Info { get; set; }
    public required IRelayPeer Host { get; set; }
    public List<IRelayPeer> Peers { get; } = [];
    public Dictionary<uint, RelayEntity> Entities { get; } = [];
    /// <summary>Set once the host transmits the started world; from then on departures hold their seats.</summary>
    public bool Started { get; set; }
    public RelayJournal? Journal { get; set; }
    /// <summary>How long a stand-in waits for an away seat's player before ending that seat's turn; null uses the server default.</summary>
    public int? TurnSeconds { get; set; }
    public uint PlayerCount => (uint)Entities.Values.Count(e => !e.Group);
    public RelaySessionInfo Current => Info with { CurrentPlayers = PlayerCount };
}

/// <summary>
/// Authoritative DirectPlay session state. Each game client mirrors it and turns the
/// relay's change notifications into the DirectPlay system messages the engine expects.
/// All state is guarded by one lock; frames are only queued while holding it.
/// </summary>
/// <summary>Who a relay client is, from its Hello ticket. Stand-ins are the server's own HotA clients.</summary>
public sealed record RelayAccount(string Id, string Name, bool StandIn = false);

public sealed record RelaySeatView(uint Id, string Name, string? Account, bool Away, bool StandIn, DateTimeOffset? AwaySince, int Delivered, int Sent);
public sealed record RelayGameView(Guid Instance, string Name, bool Started, int? TurnSeconds, int LiveClients, IReadOnlyList<RelaySeatView> Seats,
    uint MaxPlayers = 0, bool Password = false);

public sealed class RelayHub(ILogger<RelayHub> logger, IConfiguration config)
{
    /// <summary>Resolves a Hello ticket; null for an unknown or missing ticket.</summary>
    public Func<string, RelayAccount?> Authenticate { get; set; } = _ => null;
    private readonly Dictionary<IRelayPeer, RelayAccount> accounts = [];
    private readonly string? journalDirectory = config["Relay:JournalDirectory"];
    private readonly object gate = new();
    private readonly List<RelaySession> sessions = [];
    private readonly Dictionary<IRelayPeer, RelaySession> membership = [];
    private readonly Dictionary<IRelayPeer, (RelayEntity Seat, int From)> claims = [];

    public IReadOnlyList<RelaySessionInfo> Sessions() { lock (gate) return sessions.Select(s => s.Current).ToList(); }

    public void Disconnected(IRelayPeer peer) { lock (gate) { Leave(peer); accounts.Remove(peer); } }

    public IReadOnlyList<RelayGameView> Games()
    {
        lock (gate)
            return sessions.Select(s => new RelayGameView(s.Info.Instance, s.Info.Name, s.Started, s.TurnSeconds, s.Peers.Count,
                s.Entities.Values.Where(e => !e.Group).Select(e => new RelaySeatView(e.Id, e.Short, e.Account, e.Owner is AwaySeat,
                    accounts.GetValueOrDefault(e.Owner)?.StandIn == true, e.AwaySince, e.Delivered.Count, e.Sent)).ToList(),
                s.Info.MaxPlayers, s.Info.Password.Length > 0)).ToList();
    }

    public bool SetTurnSeconds(Guid instance, int? seconds)
    {
        lock (gate)
        {
            var session = sessions.FirstOrDefault(s => s.Info.Instance == instance);
            if (session is null) return false;
            session.TurnSeconds = seconds; return true;
        }
    }

    public void Handle(IRelayPeer peer, byte[] frame)
    {
        var type = (ushort)(frame[0] | frame[1] << 8);
        var r = new FrameReader(frame, 2);
        lock (gate)
        {
            switch (type)
            {
                case RelayProtocol.Hello:
                    var version = r.U32(); var ticket = r.Str(); var name = r.Str();
                    var account = ticket.Length == 0 ? null : Authenticate(ticket);
                    if (account is not null) accounts[peer] = account;
                    else if (ticket.Length != 0) logger.LogWarning("Relay client {Peer} presented an unknown ticket", peer.Label);
                    logger.LogInformation("Relay client {Peer} connected as {Name} ({Account}), protocol {Version}", peer.Label, name, account?.Name ?? "no account", version);
                    return;
                case RelayProtocol.Enum: EnumSessions(peer, r); return;
                case RelayProtocol.Open: Open(peer, r); return;
                case RelayProtocol.Rejoin: Rejoin(peer, r); return;
                case RelayProtocol.CreatePlayer: CreateEntity(peer, r, group: false); return;
                case RelayProtocol.CreateGroup: CreateEntity(peer, r, group: true); return;
                case RelayProtocol.DestroyPlayer:
                case RelayProtocol.DestroyGroup: Destroy(peer, r); return;
                case RelayProtocol.Send: Send(peer, r); return;
                case RelayProtocol.Chat: Chat(peer, r); return;
                case RelayProtocol.SetName: SetName(peer, r); return;
                case RelayProtocol.SetData: SetData(peer, r); return;
                case RelayProtocol.SetSession: SetSession(peer, r); return;
                case RelayProtocol.GroupMember: GroupMember(peer, r); return;
                case RelayProtocol.GroupInGroup: GroupInGroup(peer, r); return;
                case RelayProtocol.Close:
                    var request = r.U32(); Leave(peer); Reply(peer, request, RelayProtocol.Ok); return;
                default:
                    logger.LogWarning("Relay client {Peer} sent unknown frame {Type}", peer.Label, type); return;
            }
        }
    }

    private static void Reply(IRelayPeer peer, uint request, uint result, Action<FrameWriter>? body = null)
    {
        var w = new FrameWriter(RelayProtocol.Reply).U32(request).U32(result);
        body?.Invoke(w);
        peer.Post(w.Finish());
    }
    private static void Others(RelaySession session, IRelayPeer except, byte[] frame)
    {
        foreach (var other in session.Peers) if (other != except) other.Post(frame);
    }

    private void EnumSessions(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var application = r.Guid();
        var visible = sessions.Where(s => application == Guid.Empty || s.Info.Application == Guid.Empty || s.Info.Application == application).ToList();
        Reply(peer, request, RelayProtocol.Ok, w =>
        {
            w.U32((uint)visible.Count);
            foreach (var s in visible)
            {
                var info = s.Current;
                if (info.Password.Length != 0) info = info with { Flags = info.Flags | RelayProtocol.SessionPasswordRequired };
                info.Write(w);
            }
        });
    }

    private void Open(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var mode = r.U32(); var info = RelaySessionInfo.Read(r);
        if (!r.Ok) { Reply(peer, request, RelayProtocol.ErrInvalidParams); return; }
        if (membership.ContainsKey(peer)) { Reply(peer, request, RelayProtocol.ErrAccessDenied); return; }
        RelaySession session;
        if (mode == RelayProtocol.OpenCreate)
        {
            session = new() { Info = info with { Instance = info.Instance == Guid.Empty ? Guid.NewGuid() : info.Instance }, Host = peer };
            if (sessions.Any(s => s.Info.Instance == session.Info.Instance)) { Reply(peer, request, RelayProtocol.ErrAccessDenied); return; }
            sessions.Add(session);
            if (journalDirectory is not null) session.Journal = RelayJournal.Open(journalDirectory, session.Info);
            logger.LogInformation("Relay session {Name} created by {Peer}", info.Name, peer.Label);
        }
        else
        {
            session = sessions.FirstOrDefault(s => s.Info.Instance == info.Instance)!;
            if (session is null) { Reply(peer, request, RelayProtocol.ErrNoSessions); return; }
            if (session.Info.Password.Length != 0 && session.Info.Password != info.Password) { Reply(peer, request, RelayProtocol.ErrInvalidPassword); return; }
            if ((session.Info.Flags & (RelayProtocol.SessionJoinDisabled | RelayProtocol.SessionNewPlayersDisabled)) != 0 ||
                session.Info.MaxPlayers != 0 && session.PlayerCount >= session.Info.MaxPlayers)
            { Reply(peer, request, RelayProtocol.ErrCantCreatePlayer); return; }
            logger.LogInformation("Relay client {Peer} joined session {Name}", peer.Label, session.Info.Name);
        }
        session.Peers.Add(peer); membership[peer] = session;
        Reply(peer, request, RelayProtocol.Ok, w => Snapshot(session, w));
    }

    private static void Snapshot(RelaySession session, FrameWriter w)
    {
        session.Current.Write(w);
        w.U32((uint)session.Entities.Count);
        foreach (var e in session.Entities.Values)
        {
            w.U32(e.Id).U32(e.Group ? 1u : 0u).U32(e.Flags).U32(e.Parent).Str(e.Short).Str(e.Long).Blob(e.Data).U32((uint)e.Members.Count);
            foreach (var m in e.Members) w.U32(m);
        }
    }

    /// <summary>
    /// A resumed client joins a started game in place of a held seat. Its next CreatePlayer
    /// takes that seat's identity, so the other clients' engines see the same player return.
    /// </summary>
    private void Rejoin(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var name = r.Str(); var seat = r.Str(); var from = r.U32();
        if (!r.Ok || name.Length == 0 || seat.Length == 0) { Reply(peer, request, RelayProtocol.ErrInvalidParams); return; }
        int? sent = r.Remaining >= 4 ? (int)r.U32() : null;
        if (membership.ContainsKey(peer)) { Reply(peer, request, RelayProtocol.ErrAccessDenied); return; }
        var held = sessions.Where(s => s.Started && s.Info.Name.StartsWith(name, StringComparison.Ordinal))
            .SelectMany(s => s.Entities.Values.Where(e => !e.Group && e.Owner is AwaySeat && string.Equals(e.Short, seat, StringComparison.OrdinalIgnoreCase)).Select(e => (s, e)))
            .ToList();
        if (held.Count != 1) { logger.LogWarning("Relay client {Peer} cannot rejoin {Name} as {Seat}: {Count} held seats match", peer.Label, name, seat, held.Count); Reply(peer, request, RelayProtocol.ErrNoSessions); return; }
        var (session, entity) = held[0];
        var account = accounts.GetValueOrDefault(peer);
        if (entity.Account is not null && account?.StandIn != true && account?.Id != entity.Account)
        { logger.LogWarning("Relay client {Peer} ({Account}) may not take seat {Seat}", peer.Label, account?.Name ?? "no account", seat); Reply(peer, request, RelayProtocol.ErrAccessDenied); return; }
        if (sent < entity.Sent)
            logger.LogWarning("Relay client {Peer} resumes seat {Seat} from a capture that had sent {Sent} of the seat's {Total} messages; its later actions are lost", peer.Label, seat, sent, entity.Sent);
        if (from > entity.Delivered.Count) { logger.LogWarning("Relay client {Peer} resumes seat {Seat} at message {From} but only {Count} were delivered", peer.Label, seat, from, entity.Delivered.Count); Reply(peer, request, RelayProtocol.ErrInvalidParams); return; }
        session.Peers.Add(peer); membership[peer] = session; claims[peer] = (entity, (int)from);
        logger.LogInformation("Relay client {Peer} rejoins {Session} for seat {Id:x8} '{Seat}'", peer.Label, session.Info.Name, entity.Id, entity.Short);
        Reply(peer, request, RelayProtocol.Ok, w => Snapshot(session, w));
    }

    private bool Member(IRelayPeer peer, out RelaySession session) => membership.TryGetValue(peer, out session!);

    private static uint NewId(RelaySession session)
    {
        while (true)
        {
            var id = BitConverter.ToUInt32(RandomNumberGenerator.GetBytes(4)) | 0x100;
            if (!session.Entities.ContainsKey(id)) return id;
        }
    }

    private void CreateEntity(IRelayPeer peer, FrameReader r, bool group)
    {
        var request = r.U32();
        var parent = group ? r.U32() : 0;
        var flags = r.U32(); var shortName = r.Str(); var longName = r.Str(); var data = r.Blob();
        if (!r.Ok) { Reply(peer, request, RelayProtocol.ErrInvalidParams); return; }
        if (!Member(peer, out var session)) { Reply(peer, request, RelayProtocol.ErrAccessDenied); return; }
        if (parent != 0 && !(session.Entities.TryGetValue(parent, out var p) && p.Group)) { Reply(peer, request, RelayProtocol.ErrInvalidGroup); return; }
        if (!group && claims.Remove(peer, out var claim))
        {
            var seat = claim.Seat;
            if (seat.Owner is not AwaySeat || !session.Entities.ContainsKey(seat.Id)) { Reply(peer, request, RelayProtocol.ErrCantCreatePlayer); return; }
            if (session.Host == seat.Owner) session.Host = peer;
            seat.Owner = peer; seat.AwaySince = null;
            Reply(peer, request, RelayProtocol.Ok, w => w.U32(seat.Id));
            logger.LogInformation("Relay seat {Id:x8} '{Name}' in {Session} reclaimed by {Peer}; replaying messages {From}..{Count}", seat.Id, seat.Short, session.Info.Name, peer.Label, claim.From, seat.Delivered.Count);
            foreach (var frame in seat.Delivered.Skip(claim.From)) peer.Post(frame);
            return;
        }
        uint id;
        if (!group && (flags & RelayProtocol.PlayerServer) != 0)
        {
            if (session.Host != peer || session.Entities.ContainsKey(RelayProtocol.ServerPlayerId)) { Reply(peer, request, RelayProtocol.ErrCantCreatePlayer); return; }
            id = RelayProtocol.ServerPlayerId;
        }
        else
        {
            if (!group && session.Info.MaxPlayers != 0 && session.PlayerCount >= session.Info.MaxPlayers) { Reply(peer, request, RelayProtocol.ErrCantCreatePlayer); return; }
            id = NewId(session);
        }
        var entity = new RelayEntity { Id = id, Group = group, Owner = peer, Flags = flags, Parent = parent, Short = shortName, Long = longName, Data = data,
            Account = group || accounts.GetValueOrDefault(peer) is not { StandIn: false } owner ? null : owner.Id };
        session.Entities[id] = entity;
        if (parent != 0) session.Entities[parent].Subgroups.Add(id);
        Reply(peer, request, RelayProtocol.Ok, w => w.U32(id));
        Others(session, peer, new FrameWriter(RelayProtocol.Created).U32(id).U32(group ? 1u : 0u).U32(flags).U32(parent)
            .Str(shortName).Str(longName).Blob(data).U32(session.PlayerCount).Finish());
        logger.LogInformation("Relay {Kind} {Id:x8} '{Name}' created in {Session} by {Peer}", group ? "group" : "player", id, shortName, session.Info.Name, peer.Label);
    }

    private void Destroy(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var id = r.U32();
        if (!Member(peer, out var session) || !session.Entities.TryGetValue(id, out var entity)) { Reply(peer, request, RelayProtocol.ErrInvalidPlayer); return; }
        if (entity.Owner != peer && !entity.Group) { Reply(peer, request, RelayProtocol.ErrAccessDenied); return; }
        // A started game keeps the seat: the departing client becomes away instead of a removed player.
        if (session.Started && !entity.Group) { Hold(session, peer, entity); Reply(peer, request, RelayProtocol.Ok); return; }
        Remove(session, entity, peer);
        Reply(peer, request, RelayProtocol.Ok);
    }

    private void Remove(RelaySession session, RelayEntity entity, IRelayPeer? origin)
    {
        foreach (var child in entity.Subgroups.ToList()) if (session.Entities.TryGetValue(child, out var c)) Remove(session, c, origin);
        session.Entities.Remove(entity.Id);
        foreach (var other in session.Entities.Values) { other.Members.Remove(entity.Id); other.Subgroups.Remove(entity.Id); }
        var frame = new FrameWriter(RelayProtocol.Destroyed).U32(entity.Id).U32(session.PlayerCount).Finish();
        foreach (var other in session.Peers) if (other != origin) other.Post(frame);
        logger.LogInformation("Relay {Kind} {Id:x8} removed from {Session}", entity.Group ? "group" : "player", entity.Id, session.Info.Name);
    }

    /// <summary>DirectPlay delivery: to all players except the sender, one player, or a group's member players.</summary>
    private List<RelayEntity> Recipients(RelaySession session, uint from, uint to)
    {
        if (to == 0) return session.Entities.Values.Where(e => !e.Group && e.Id != from).ToList();
        if (!session.Entities.TryGetValue(to, out var target)) return [];
        if (!target.Group) return [target];
        return target.Members.Where(m => m != from).Select(m => session.Entities.GetValueOrDefault(m)).OfType<RelayEntity>().Where(e => !e.Group).ToList();
    }

    private void Send(IRelayPeer peer, FrameReader r)
    {
        var from = r.U32(); var to = r.U32(); r.U32(); var data = r.Blob();
        if (!r.Ok || !Member(peer, out var session) || !session.Entities.TryGetValue(from, out var sender) || sender.Owner != peer) return;
        sender.Sent++;
        var message = GameMessage.Parse(data);
        if (!session.Started && message.Id == GameMessage.LoadGame)
        {
            session.Started = true;
            logger.LogInformation("Relay session {Name} started; departures now hold seats", session.Info.Name);
        }
        var recipients = Recipients(session, from, to);
        session.Journal?.Record(from, to, data);
        if (message.Traced) logger.LogInformation("Game {Session} {From:x8}->{To:x8} {Message} to {Count}", session.Info.Name, from, to, message, recipients.Count);
        else logger.LogDebug("Game {Session} {From:x8}->{To:x8} {Message}", session.Info.Name, from, to, message);
        foreach (var recipient in recipients)
        {
            var frame = new FrameWriter(RelayProtocol.Deliver).U32(from).U32(recipient.Id).Blob(data).Finish();
            if (recipient.Delivered.Count < RelayEntity.DeliveredLimit) recipient.Delivered.Add(frame);
            recipient.Owner.Post(frame);
        }
    }

    private void Chat(IRelayPeer peer, FrameReader r)
    {
        var from = r.U32(); var to = r.U32(); var flags = r.U32(); var text = r.Str();
        if (!r.Ok || !Member(peer, out var session) || !session.Entities.TryGetValue(from, out var sender) || sender.Owner != peer) return;
        var group = to != 0 && session.Entities.TryGetValue(to, out var target) && target.Group ? to : 0;
        foreach (var recipient in Recipients(session, from, to))
            recipient.Owner.Post(new FrameWriter(RelayProtocol.ChatMessage).U32(from).U32(recipient.Id).U32(group).U32(flags).Str(text).Finish());
        logger.LogInformation("Chat in {Session} from {From:x8}: {Text}", session.Info.Name, from, text);
    }

    private void SetName(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var id = r.U32(); r.U32(); var shortName = r.Str(); var longName = r.Str();
        if (!Member(peer, out var session) || !session.Entities.TryGetValue(id, out var e)) { Reply(peer, request, RelayProtocol.ErrInvalidPlayer); return; }
        e.Short = shortName; e.Long = longName;
        Reply(peer, request, RelayProtocol.Ok);
        Others(session, peer, new FrameWriter(RelayProtocol.Name).U32(id).Str(shortName).Str(longName).Finish());
    }

    private void SetData(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var id = r.U32(); r.U32(); var data = r.Blob();
        if (!Member(peer, out var session) || !session.Entities.TryGetValue(id, out var e)) { Reply(peer, request, RelayProtocol.ErrInvalidPlayer); return; }
        e.Data = data;
        Reply(peer, request, RelayProtocol.Ok);
        Others(session, peer, new FrameWriter(RelayProtocol.Data).U32(id).Blob(data).Finish());
    }

    private void SetSession(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var info = RelaySessionInfo.Read(r);
        if (!r.Ok || !Member(peer, out var session)) { Reply(peer, request, RelayProtocol.ErrInvalidParams); return; }
        session.Info = info with { Instance = session.Info.Instance };
        Reply(peer, request, RelayProtocol.Ok);
        var w = new FrameWriter(RelayProtocol.Session); session.Current.Write(w);
        Others(session, peer, w.Finish());
    }

    private void GroupMember(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var group = r.U32(); var player = r.U32(); var add = r.U32() != 0;
        if (!Member(peer, out var session) || !session.Entities.TryGetValue(group, out var g) || !g.Group) { Reply(peer, request, RelayProtocol.ErrInvalidGroup); return; }
        if (!session.Entities.TryGetValue(player, out var p) || p.Group) { Reply(peer, request, RelayProtocol.ErrInvalidPlayer); return; }
        if (add) { if (!g.Members.Contains(player)) g.Members.Add(player); } else g.Members.Remove(player);
        Reply(peer, request, RelayProtocol.Ok);
        Others(session, peer, new FrameWriter(RelayProtocol.Member).U32(group).U32(player).U32(add ? 1u : 0u).Finish());
    }

    private void GroupInGroup(IRelayPeer peer, FrameReader r)
    {
        var request = r.U32(); var parent = r.U32(); var group = r.U32(); var add = r.U32() != 0;
        if (!Member(peer, out var session) || !session.Entities.TryGetValue(parent, out var p) || !p.Group ||
            !session.Entities.TryGetValue(group, out var g) || !g.Group) { Reply(peer, request, RelayProtocol.ErrInvalidGroup); return; }
        if (add) { if (!p.Subgroups.Contains(group)) p.Subgroups.Add(group); } else p.Subgroups.Remove(group);
        Reply(peer, request, RelayProtocol.Ok);
        Others(session, peer, new FrameWriter(RelayProtocol.GroupInGroupChanged).U32(parent).U32(group).U32(add ? 1u : 0u).Finish());
    }

    private void Leave(IRelayPeer peer)
    {
        claims.Remove(peer);
        if (!membership.Remove(peer, out var session)) return;
        session.Peers.Remove(peer);
        if (session.Started)
        {
            foreach (var player in session.Entities.Values.Where(e => e.Owner == peer && !e.Group).ToList()) Hold(session, peer, player);
            // The host role stays with the host's seat (the server player) until someone reclaims it.
            if (session.Host == peer && (session.Entities.GetValueOrDefault(RelayProtocol.ServerPlayerId) ?? session.Entities.Values.FirstOrDefault(e => e.Owner is AwaySeat)) is { Owner: AwaySeat } away)
                session.Host = away.Owner;
            logger.LogInformation("Relay client {Peer} left started session {Name}; {Live} live clients remain", peer.Label, session.Info.Name, session.Peers.Count);
            return;
        }
        foreach (var player in session.Entities.Values.Where(e => e.Owner == peer && !e.Group).ToList()) Remove(session, player, peer);
        logger.LogInformation("Relay client {Peer} left session {Name}", peer.Label, session.Info.Name);
        if (session.Peers.Count == 0) { sessions.Remove(session); logger.LogInformation("Relay session {Name} closed", session.Info.Name); return; }
        if (session.Host == peer)
        {
            if ((session.Info.Flags & RelayProtocol.SessionMigrateHost) != 0)
            {
                session.Host = session.Peers[0];
                session.Host.Post(new FrameWriter(RelayProtocol.Host).Finish());
                logger.LogInformation("Relay session {Name} host migrated to {Peer}", session.Info.Name, session.Host.Label);
            }
            else
            {
                var lost = new FrameWriter(RelayProtocol.SessionLost).Finish();
                foreach (var other in session.Peers.ToList()) { other.Post(lost); membership.Remove(other); }
                sessions.Remove(session);
            }
        }
    }

    /// <summary>
    /// Keeps a departed player's seat in a started game. Other clients are not told: their
    /// engines still see the player, and messages addressed to it are retained for the seat.
    /// </summary>
    private void Hold(RelaySession session, IRelayPeer peer, RelayEntity player)
    {
        if (player.Owner is AwaySeat) return;
        player.Owner = new AwaySeat($"away:{player.Short}"); player.AwaySince = DateTimeOffset.UtcNow;
        logger.LogInformation("Relay seat {Id:x8} '{Name}' in {Session} is away (was {Peer})", player.Id, player.Short, session.Info.Name, peer.Label);
    }
}

/// <summary>Placeholder owner of a held seat. Its messages stay in the seat's delivery log.</summary>
public sealed class AwaySeat(string label) : IRelayPeer
{
    public string Label { get; } = label;
    public void Post(byte[] frame) { }
}

/// <summary>Engine message header (sender, ?, id, size), followed by the payload at +0x14.</summary>
public readonly record struct GameMessage(uint Id, uint Size, uint Sub, int Length)
{
    public const uint LoadGame = 0x3E8, LoadChunk = 0x3E9, LoadResend = 0x3EA, LoadEnd = 0x3EB, Departure = 0x3F6, CurrentPlayer = 0x3F8,
        LoadReceipt = 0x439, Simultaneous = 0x8A2;
    public static GameMessage Parse(byte[] data)
    {
        uint At(int offset) => data.Length >= offset + 4 ? BitConverter.ToUInt32(data, offset) : 0;
        return new(At(8), At(12), At(0x14), data.Length);
    }
    public bool Traced => Id is LoadGame or LoadEnd or LoadResend or Departure or CurrentPlayer or LoadReceipt or Simultaneous;
    public override string ToString() => Id == Simultaneous ? $"0x{Id:x}/{Sub:x} ({Length} B)" : $"0x{Id:x} ({Length} B)";
}

/// <summary>Development journal of every game message in a session, for offline protocol analysis.</summary>
public sealed class RelayJournal
{
    private readonly FileStream file;
    private RelayJournal(FileStream file) => this.file = file;
    public static RelayJournal Open(string directory, RelaySessionInfo info)
    {
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"{DateTime.UtcNow:yyyyMMdd-HHmmss}-{info.Instance:N}.bin");
        return new(new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read));
    }
    // Record: i64 unix ms, u32 from, u32 to, u32 length, bytes.
    public void Record(uint from, uint to, byte[] data)
    {
        Span<byte> header = stackalloc byte[20];
        BitConverter.TryWriteBytes(header, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        BitConverter.TryWriteBytes(header[8..], from); BitConverter.TryWriteBytes(header[12..], to); BitConverter.TryWriteBytes(header[16..], data.Length);
        file.Write(header); file.Write(data); file.Flush();
    }
}
