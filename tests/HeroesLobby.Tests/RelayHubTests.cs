using HeroesLobby.Server.Relay;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace HeroesLobby.Tests;

public class RelayHubTests
{
    private sealed class Peer(string label) : IRelayPeer
    {
        public string Label { get; } = label;
        public List<byte[]> Frames { get; } = [];
        public void Post(byte[] frame) => Frames.Add(frame);
        public IEnumerable<(ushort Type, FrameReader Reader)> Of(ushort type) =>
            Frames.Where(f => BitConverter.ToUInt16(f, 4) == type).Select(f => (type, new FrameReader(f, 6)));
        public uint ReplyResult(uint request) =>
            Of(RelayProtocol.Reply).Select(x => (Request: x.Reader.U32(), Result: x.Reader.U32())).Last(x => x.Request == request).Result;
    }

    private static readonly Guid Instance = Guid.NewGuid();
    private static RelayHub Hub() => new(NullLogger<RelayHub>.Instance, new ConfigurationBuilder().Build());
    private static byte[] Body(FrameWriter w) => w.Finish()[4..];

    private static FrameWriter Session(FrameWriter w) =>
        w.Guid(Instance).Guid(Guid.Empty).U32(RelayProtocol.SessionMigrateHost).U32(8).U32(0).Str("Game").Str("").U32(0).U32(0).U32(0).U32(0);

    private static uint CreatePlayer(RelayHub hub, Peer peer, uint request, uint flags, string name)
    {
        hub.Handle(peer, Body(new FrameWriter(RelayProtocol.CreatePlayer).U32(request).U32(flags).Str(name).Str(name).Blob([])));
        var reply = peer.Of(RelayProtocol.Reply).Last().Reader;
        Assert.Equal(request, reply.U32()); Assert.Equal(RelayProtocol.Ok, reply.U32());
        return reply.U32();
    }

    // An engine message: sender colour, sender id, message id, size, then payload.
    private static byte[] Message(uint id, uint sender) =>
        [.. BitConverter.GetBytes(0u), .. BitConverter.GetBytes(sender), .. BitConverter.GetBytes(id), .. BitConverter.GetBytes(24u), .. new byte[8]];

    private static void Send(RelayHub hub, Peer peer, uint from, uint to, byte[] data) =>
        hub.Handle(peer, Body(new FrameWriter(RelayProtocol.Send).U32(from).U32(to).U32(0).Blob(data)));

    private static (RelayHub Hub, Peer Red, Peer Blue, uint RedId, uint BlueId) StartedGame()
    {
        var hub = Hub(); var red = new Peer("red"); var blue = new Peer("blue");
        hub.Handle(red, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate))));
        var redId = CreatePlayer(hub, red, 2, RelayProtocol.PlayerServer, "Red");
        hub.Handle(blue, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenJoin))));
        Assert.Equal(RelayProtocol.Ok, blue.ReplyResult(1));
        var blueId = CreatePlayer(hub, blue, 2, 0, "Blue");
        Send(hub, red, redId, 0, Message(GameMessage.LoadGame, redId));
        return (hub, red, blue, redId, blueId);
    }

    [Fact]
    public void StartedGameHoldsDepartedSeatWithoutTellingOthers()
    {
        var (hub, red, blue, _, _) = StartedGame();
        hub.Disconnected(blue);
        Assert.Empty(red.Of(RelayProtocol.Destroyed));
        Assert.Empty(red.Of(RelayProtocol.SessionLost));
        Assert.Single(hub.Sessions());
    }

    [Fact]
    public void PreGameDepartureStillRemovesThePlayer()
    {
        var hub = Hub(); var red = new Peer("red"); var blue = new Peer("blue");
        hub.Handle(red, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate))));
        CreatePlayer(hub, red, 2, RelayProtocol.PlayerServer, "Red");
        hub.Handle(blue, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenJoin))));
        var blueId = CreatePlayer(hub, blue, 2, 0, "Blue");
        hub.Disconnected(blue);
        Assert.Equal(blueId, red.Of(RelayProtocol.Destroyed).Single().Reader.U32());
    }

    [Fact]
    public void RejoinClaimsHeldSeatAndReplaysFromCapturedCount()
    {
        var (hub, red, blue, redId, blueId) = StartedGame();
        // Blue's engine had consumed the start message when its seat was captured.
        var consumed = (uint)blue.Of(RelayProtocol.Deliver).Count();
        hub.Disconnected(blue);
        Send(hub, red, redId, blueId, Message(GameMessage.Simultaneous, redId));
        Send(hub, red, redId, 0, Message(GameMessage.LoadEnd, redId));

        var back = new Peer("blue-back");
        hub.Handle(back, Body(new FrameWriter(RelayProtocol.Rejoin).U32(7).Str("Ga").Str("blue").U32(consumed)));
        Assert.Equal(RelayProtocol.Ok, back.ReplyResult(7));
        // The resumed client's own session setup creates a player; it gets the held seat's identity.
        Assert.Equal(blueId, CreatePlayer(hub, back, 8, RelayProtocol.PlayerServer, "HeroesPost"));
        var replayed = back.Of(RelayProtocol.Deliver).Select(x => { var r = x.Reader; r.U32(); r.U32(); return GameMessage.Parse(r.Blob()).Id; }).ToList();
        Assert.Equal([GameMessage.Simultaneous, GameMessage.LoadEnd], replayed);
        Assert.Single(red.Of(RelayProtocol.Created), x => x.Reader.U32() == blueId); // no second arrival announced

        // The seat is live again: Red's next message reaches the rejoined client.
        Send(hub, red, redId, blueId, Message(GameMessage.LoadReceipt, redId));
        Assert.Equal(3, back.Of(RelayProtocol.Deliver).Count());
    }

    [Fact]
    public void HostSeatCanBeHeldAndReclaimed()
    {
        var (hub, red, blue, redId, blueId) = StartedGame();
        var consumed = (uint)red.Of(RelayProtocol.Deliver).Count();
        hub.Disconnected(red);
        Assert.Empty(blue.Of(RelayProtocol.SessionLost));
        Assert.Empty(blue.Of(RelayProtocol.Host));
        Send(hub, blue, blueId, redId, Message(GameMessage.Simultaneous, blueId));

        var back = new Peer("red-back");
        hub.Handle(back, Body(new FrameWriter(RelayProtocol.Rejoin).U32(3).Str("Game").Str("red").U32(consumed)));
        Assert.Equal(RelayProtocol.Ok, back.ReplyResult(3));
        Assert.Equal(redId, CreatePlayer(hub, back, 4, RelayProtocol.PlayerServer, "HeroesPost"));
        Assert.Single(back.Of(RelayProtocol.Deliver));
        Send(hub, blue, blueId, redId, Message(GameMessage.LoadReceipt, blueId));
        Assert.Equal(2, back.Of(RelayProtocol.Deliver).Count());
    }

    [Fact]
    public void RejoinRequiresAHeldSeatAndAPlausibleCount()
    {
        var (hub, _, blue, _, _) = StartedGame();
        var early = new Peer("early");
        hub.Handle(early, Body(new FrameWriter(RelayProtocol.Rejoin).U32(1).Str("Game").Str("Blue").U32(0)));
        Assert.Equal(RelayProtocol.ErrNoSessions, early.ReplyResult(1));

        hub.Disconnected(blue);
        var ahead = new Peer("ahead");
        hub.Handle(ahead, Body(new FrameWriter(RelayProtocol.Rejoin).U32(2).Str("Game").Str("Blue").U32(1000)));
        Assert.Equal(RelayProtocol.ErrInvalidParams, ahead.ReplyResult(2));
    }
}
