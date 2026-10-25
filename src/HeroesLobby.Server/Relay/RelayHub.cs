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
    public required IRelayPeer Owner { get; init; }
    public uint Flags { get; set; }
    public uint Parent { get; set; }
    public string Short { get; set; } = "";
    public string Long { get; set; } = "";
    public byte[] Data { get; set; } = [];
    public List<uint> Members { get; } = [];
    public List<uint> Subgroups { get; } = [];
}

public sealed class RelaySession
{
    public required RelaySessionInfo Info { get; set; }
    public required IRelayPeer Host { get; set; }
    public List<IRelayPeer> Peers { get; } = [];
    public Dictionary<uint, RelayEntity> Entities { get; } = [];
    public uint PlayerCount => (uint)Entities.Values.Count(e => !e.Group);
    public RelaySessionInfo Current => Info with { CurrentPlayers = PlayerCount };
}

/// <summary>
/// Authoritative DirectPlay session state. Each game client mirrors it and turns the
/// relay's change notifications into the DirectPlay system messages the engine expects.
/// All state is guarded by one lock; frames are only queued while holding it.
/// </summary>
public sealed class RelayHub(ILogger<RelayHub> logger)
{
    private readonly object gate = new();
    private readonly List<RelaySession> sessions = [];
    private readonly Dictionary<IRelayPeer, RelaySession> membership = [];

    public IReadOnlyList<RelaySessionInfo> Sessions() { lock (gate) return sessions.Select(s => s.Current).ToList(); }

    public void Disconnected(IRelayPeer peer) { lock (gate) Leave(peer); }

    public void Handle(IRelayPeer peer, byte[] frame)
    {
        var type = (ushort)(frame[0] | frame[1] << 8);
        var r = new FrameReader(frame, 2);
        lock (gate)
        {
            switch (type)
            {
                case RelayProtocol.Hello:
                    var version = r.U32(); r.Str(); var name = r.Str();
                    logger.LogInformation("Relay client {Peer} connected as {Name}, protocol {Version}", peer.Label, name, version);
                    return;
                case RelayProtocol.Enum: EnumSessions(peer, r); return;
                case RelayProtocol.Open: Open(peer, r); return;
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
        Reply(peer, request, RelayProtocol.Ok, w =>
        {
            session.Current.Write(w);
            w.U32((uint)session.Entities.Count);
            foreach (var e in session.Entities.Values)
            {
                w.U32(e.Id).U32(e.Group ? 1u : 0u).U32(e.Flags).U32(e.Parent).Str(e.Short).Str(e.Long).Blob(e.Data).U32((uint)e.Members.Count);
                foreach (var m in e.Members) w.U32(m);
            }
        });
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
        var entity = new RelayEntity { Id = id, Group = group, Owner = peer, Flags = flags, Parent = parent, Short = shortName, Long = longName, Data = data };
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
        foreach (var recipient in Recipients(session, from, to))
            recipient.Owner.Post(new FrameWriter(RelayProtocol.Deliver).U32(from).U32(recipient.Id).Blob(data).Finish());
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
        if (!membership.Remove(peer, out var session)) return;
        session.Peers.Remove(peer);
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
}
