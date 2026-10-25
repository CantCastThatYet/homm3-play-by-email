using System.Buffers.Binary;
using System.Text;

namespace HeroesLobby.Server.Relay;

/// <summary>
/// Wire format shared with native/dplay_relay.cpp. Every frame is a u32 length of the
/// remainder, a u16 type, then little-endian fields; strings and blobs are u32 length + bytes.
/// Requests from the game carry a request id as their first field and receive a Reply.
/// </summary>
public static class RelayProtocol
{
    public const uint Version = 1;
    public const int MaxFrame = 4 * 1024 * 1024;

    public const ushort Hello = 1, Enum = 2, Open = 3, CreatePlayer = 4, DestroyPlayer = 5, Send = 6, SetName = 7, SetData = 8,
        SetSession = 9, CreateGroup = 10, DestroyGroup = 11, GroupMember = 12, Chat = 13, Close = 14, GroupInGroup = 15,
        Rejoin = 16, Resume = 20, CaughtUp = 21;
    public const ushort Reply = 100, Deliver = 101, Created = 102, Destroyed = 103, Name = 104, Data = 105, Session = 106,
        Member = 107, Host = 108, ChatMessage = 109, SessionLost = 110, GroupInGroupChanged = 111;

    // Rejoin: request, session name prefix, seat name, frames consumed, messages sent, seat id (the
    // last two optional).
    // Resume: a live client whose connection dropped takes its seat back (request, game, seat, seat
    // generation, frames received, messages sent). The reply carries the relay's sent count for the
    // seat; the relay then replays what the client missed. CreatePlayer's reply carries the seat id
    // and its generation, which rises whenever a new process claims the seat; for a claimed seat
    // also the number of frames the relay replays to it. CaughtUp (no fields): the claiming
    // process has worked through the replay, so its messages from here on are new.

    // The in-game lobby on its own connection, after Hello. LobbyWatch subscribes; LobbyChat
    // carries UTF-8 text; LobbyTurnLimit is a game id and seconds. The server pushes
    // LobbyState (UTF-8 JSON RelayLobby) when it changes, LobbyChatLines (UTF-8 JSON array of
    // RelayChatLine) and LobbyNotice (u32 code, UTF-8 text).
    public const ushort LobbyWatch = 17, LobbyChat = 18, LobbyTurnLimit = 19;
    public const ushort LobbyState = 112, LobbyChatLines = 113, LobbyNotice = 114;
    public const uint NoticeText = 0, NoticeSignIn = 1;

    // DirectPlay HRESULTs reported back to the engine.
    public const uint Ok = 0, ErrGeneric = 0x80004005, ErrInvalidParams = 0x80070057, ErrAccessDenied = 0x8877000A,
        ErrCantCreatePlayer = 0x88770046, ErrInvalidPlayer = 0x88770096, ErrInvalidGroup = 0x8877009B,
        ErrNoSessions = 0x887700C8, ErrInvalidPassword = 0x8877014A;

    // DirectPlay flags interpreted by the relay.
    public const uint OpenJoin = 1, OpenCreate = 2;
    public const uint SessionNewPlayersDisabled = 0x1, SessionMigrateHost = 0x4, SessionJoinDisabled = 0x20, SessionPasswordRequired = 0x400;
    public const uint PlayerServer = 0x100;
    public const uint ServerPlayerId = 1;
}

public sealed class FrameWriter
{
    private readonly MemoryStream stream = new();
    public FrameWriter(ushort type) { U32(0); Span<byte> t = stackalloc byte[2]; BinaryPrimitives.WriteUInt16LittleEndian(t, type); stream.Write(t); }
    public FrameWriter U32(uint value) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, value); stream.Write(b); return this; }
    public FrameWriter Guid(Guid value) { stream.Write(value.ToByteArray()); return this; }
    public FrameWriter Blob(ReadOnlySpan<byte> value) { U32((uint)value.Length); stream.Write(value); return this; }
    public FrameWriter Str(string value) => Blob(Encoding.Latin1.GetBytes(value));
    public byte[] Finish()
    {
        var bytes = stream.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, (uint)(bytes.Length - 4));
        return bytes;
    }
}

public sealed class FrameReader(byte[] frame, int offset)
{
    private int position = offset;
    public bool Ok { get; private set; } = true;
    public int Position => position;
    public int Remaining => Ok ? frame.Length - position : 0;
    private bool Need(int count) { if (!Ok || frame.Length - position < count) Ok = false; return Ok; }
    public uint U32() { if (!Need(4)) return 0; var v = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(position)); position += 4; return v; }
    public Guid Guid() { if (!Need(16)) return System.Guid.Empty; var v = new Guid(frame.AsSpan(position, 16)); position += 16; return v; }
    public byte[] Blob()
    {
        var length = U32();
        if (length > RelayProtocol.MaxFrame || !Need((int)length)) { Ok = false; return []; }
        var value = frame.AsSpan(position, (int)length).ToArray(); position += (int)length; return value;
    }
    public string Str() => Encoding.Latin1.GetString(Blob());
}

public sealed record RelaySessionInfo(Guid Instance, Guid Application, uint Flags, uint MaxPlayers, uint CurrentPlayers,
    string Name, string Password, uint[] User)
{
    public static RelaySessionInfo Read(FrameReader r)
    {
        var instance = r.Guid(); var app = r.Guid(); var flags = r.U32(); var max = r.U32(); var current = r.U32();
        var name = r.Str(); var password = r.Str();
        var user = new[] { r.U32(), r.U32(), r.U32(), r.U32() };
        return new(instance, app, flags, max, current, name, password, user);
    }
    // Passwords never leave the relay.
    public void Write(FrameWriter w)
    {
        w.Guid(Instance).Guid(Application).U32(Flags).U32(MaxPlayers).U32(CurrentPlayers).Str(Name).Str("");
        foreach (var value in User) w.U32(value);
    }
}
