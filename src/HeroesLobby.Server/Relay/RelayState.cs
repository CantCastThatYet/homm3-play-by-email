using System.Buffers.Binary;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace HeroesLobby.Server.Relay;

/// <summary>
/// Keeps started relay games on disk so a server restart does not end them. Each game is a folder
/// under Relay:StateDirectory (default: "relay" beside the lobby database) holding session.json, the
/// session and its seats, and one log per seat of the frames delivered to it and its sent count.
/// Logs are appended and flushed before a frame is sent on, so a restarted server can replay to a
/// returning client everything any client may already have received. Unstarted games are not kept.
/// </summary>
public sealed class RelayState
{
    private const byte DeliveredRecord = 1, SentRecord = 2;
    private readonly string? root;
    private readonly ILogger<RelayState> logger;
    private readonly Dictionary<(Guid, uint), FileStream> logs = [];

    public RelayState(IConfiguration config, ILogger<RelayState> logger)
    {
        this.logger = logger;
        var configured = config["Relay:StateDirectory"];
        if (configured == "") return; // explicitly disabled
        root = Path.GetFullPath(configured ?? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(config["Lobby:Database"] ?? "data/lobby.db"))!, "relay"));
    }

    public bool Enabled => root is not null;

    private string Folder(Guid instance) => Path.Combine(root!, instance.ToString("N"));

    /// <summary>Writes the session and its seats. Called under the hub lock.</summary>
    public void Save(RelaySession session)
    {
        if (root is null || !session.Started) return;
        try
        {
            var folder = Folder(session.Info.Instance);
            Directory.CreateDirectory(folder);
            var record = new SavedSession(session.Info.Instance, session.Info.Application, session.Info.Flags, session.Info.MaxPlayers,
                session.Info.Name, session.Info.Password, session.Info.User, session.TurnSeconds, HostSeat(session),
                session.Entities.Values.Select(e => new SavedEntity(e.Id, e.Group, e.Flags, e.Parent, e.Short, e.Long, e.Data, [.. e.Members], [.. e.Subgroups],
                    e.Account, e.AwaySince, e.Generation)).ToList());
            var path = Path.Combine(folder, "session.json");
            var temporary = path + ".pending";
            File.WriteAllBytes(temporary, JsonSerializer.SerializeToUtf8Bytes(record, RelayStateJson.Default.SavedSession));
            File.Move(temporary, path, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { logger.LogError(error, "Could not save relay game {Name}", session.Info.Name); }
    }

    private static uint? HostSeat(RelaySession session) =>
        session.Entities.Values.FirstOrDefault(e => !e.Group && e.Owner == session.Host)?.Id;

    /// <summary>The game just started: write it and every frame its seats were delivered so far.</summary>
    public void Begin(RelaySession session)
    {
        if (root is null) return;
        Save(session);
        foreach (var seat in session.Entities.Values.Where(e => !e.Group))
        {
            foreach (var frame in seat.Delivered) Append(session, seat, DeliveredRecord, frame);
            Sent(session, seat);
        }
    }

    public void Delivered(RelaySession session, RelayEntity seat, byte[] frame)
    {
        if (root is not null && session.Started) Append(session, seat, DeliveredRecord, frame);
    }

    /// <summary>Records the seat's current sent count.</summary>
    public void Sent(RelaySession session, RelayEntity seat)
    {
        if (root is null || !session.Started) return;
        Span<byte> count = stackalloc byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(count, seat.Sent);
        Append(session, seat, SentRecord, count);
    }

    // Record: u8 kind, u32 length, bytes.
    private void Append(RelaySession session, RelayEntity seat, byte kind, ReadOnlySpan<byte> payload)
    {
        try
        {
            var key = (session.Info.Instance, seat.Id);
            if (!logs.TryGetValue(key, out var log))
            {
                var folder = Folder(session.Info.Instance);
                Directory.CreateDirectory(folder);
                logs[key] = log = new FileStream(Path.Combine(folder, $"{seat.Id:x8}.log"), FileMode.Append, FileAccess.Write, FileShare.Read);
            }
            Span<byte> header = stackalloc byte[5];
            header[0] = kind;
            BinaryPrimitives.WriteInt32LittleEndian(header[1..], payload.Length);
            log.Write(header); log.Write(payload); log.Flush();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { logger.LogError(error, "Could not log relay seat {Seat:x8} in {Name}", seat.Id, session.Info.Name); }
    }

    /// <summary>The games saved before the server stopped, every seat away until its player returns.</summary>
    public List<RelaySession> Load()
    {
        var sessions = new List<RelaySession>();
        if (root is null || !Directory.Exists(root)) return sessions;
        foreach (var folder in Directory.GetDirectories(root))
        {
            var path = Path.Combine(folder, "session.json");
            if (!File.Exists(path)) continue;
            try
            {
                var saved = JsonSerializer.Deserialize(File.ReadAllBytes(path), RelayStateJson.Default.SavedSession)
                    ?? throw new InvalidDataException("empty session record");
                var session = new RelaySession
                {
                    Info = new RelaySessionInfo(saved.Instance, saved.Application, saved.Flags, saved.MaxPlayers, 0, saved.Name, saved.Password, saved.User),
                    Host = new AwaySeat("away:host"), Started = true, TurnSeconds = saved.TurnSeconds
                };
                var now = DateTimeOffset.UtcNow;
                foreach (var e in saved.Entities)
                {
                    var entity = new RelayEntity
                    {
                        Id = e.Id, Group = e.Group, Owner = new AwaySeat($"away:{e.Short}"), Flags = e.Flags, Parent = e.Parent,
                        Short = e.Short, Long = e.Long, Data = e.Data, Account = e.Account, AwaySince = e.Group ? null : e.AwaySince ?? now,
                        Generation = e.Generation
                    };
                    entity.Members.AddRange(e.Members); entity.Subgroups.AddRange(e.Subgroups);
                    if (!e.Group) ReadLog(Path.Combine(folder, $"{e.Id:x8}.log"), entity);
                    session.Entities[e.Id] = entity;
                }
                // The host role stays with the host's seat until someone reclaims it.
                var host = saved.HostSeat is { } id && session.Entities.TryGetValue(id, out var seat) ? seat
                    : session.Entities.GetValueOrDefault(RelayProtocol.ServerPlayerId) ?? session.Entities.Values.FirstOrDefault(x => !x.Group);
                if (host is not null) session.Host = host.Owner;
                sessions.Add(session);
                logger.LogInformation("Restored relay game {Name} with {Seats} seats", session.Info.Name, session.Entities.Values.Count(x => !x.Group));
            }
            catch (Exception error) when (error is IOException or JsonException or InvalidDataException or UnauthorizedAccessException)
            { logger.LogError(error, "Could not restore the relay game in {Folder}", folder); }
        }
        return sessions;
    }

    private static void ReadLog(string path, RelayEntity seat)
    {
        if (!File.Exists(path)) return;
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        var header = new byte[5];
        while (true)
        {
            if (file.ReadAtLeast(header, 5, throwOnEndOfStream: false) < 5) return;
            var length = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(1));
            if (length < 0 || length > RelayProtocol.MaxFrame + 16) return;
            var payload = new byte[length];
            // A record cut short by a crash ends the log.
            if (file.ReadAtLeast(payload, length, throwOnEndOfStream: false) < length) return;
            if (header[0] == DeliveredRecord && seat.Delivered.Count < RelayEntity.DeliveredLimit) seat.Delivered.Add(payload);
            else if (header[0] == SentRecord && length == 4) seat.Sent = BinaryPrimitives.ReadInt32LittleEndian(payload);
        }
    }

    public void Close()
    {
        foreach (var log in logs.Values) log.Dispose();
        logs.Clear();
    }
}

public sealed record SavedEntity(uint Id, bool Group, uint Flags, uint Parent, string Short, string Long, byte[] Data, uint[] Members, uint[] Subgroups,
    string? Account, DateTimeOffset? AwaySince, uint Generation = 0);
public sealed record SavedSession(Guid Instance, Guid Application, uint Flags, uint MaxPlayers, string Name, string Password, uint[] User,
    int? TurnSeconds, uint? HostSeat, List<SavedEntity> Entities);

[JsonSerializable(typeof(SavedSession))]
internal sealed partial class RelayStateJson : JsonSerializerContext;
