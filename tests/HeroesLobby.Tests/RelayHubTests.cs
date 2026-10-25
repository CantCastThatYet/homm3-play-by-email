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

    private static void Hello(RelayHub hub, Peer peer, string ticket) =>
        hub.Handle(peer, Body(new FrameWriter(RelayProtocol.Hello).U32(RelayProtocol.Version).Str(ticket).Str(peer.Label)));

    private static RelayHub AccountHub()
    {
        var hub = Hub();
        hub.Authenticate = ticket => ticket switch
        {
            "red-token" => new RelayAccount("red-id", "Red player"),
            "blue-token" => new RelayAccount("blue-id", "Blue player"),
            "standin-token" => new RelayAccount("standin", "Stand-in", StandIn: true),
            _ => null
        };
        return hub;
    }

    [Fact]
    public void SeatsBelongToTheAccountThatCreatedThem()
    {
        var hub = AccountHub(); var red = new Peer("red"); var blue = new Peer("blue");
        Hello(hub, red, "red-token"); Hello(hub, blue, "blue-token");
        hub.Handle(red, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate))));
        var redId = CreatePlayer(hub, red, 2, RelayProtocol.PlayerServer, "Red");
        hub.Handle(blue, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenJoin))));
        var blueId = CreatePlayer(hub, blue, 2, 0, "Blue");
        Send(hub, red, redId, 0, Message(GameMessage.LoadGame, redId));
        Send(hub, blue, blueId, 0, Message(GameMessage.LoadReceipt, blueId));
        Send(hub, blue, blueId, 0, Message(GameMessage.LoadReceipt, blueId));

        var game = hub.Games().Single();
        Assert.Equal("red-id", game.Seats.Single(s => s.Id == redId).Account);
        var blueSeat = game.Seats.Single(s => s.Id == blueId);
        Assert.Equal(("blue-id", 2, false), (blueSeat.Account, blueSeat.Sent, blueSeat.Away));

        hub.Disconnected(blue);
        Assert.True(hub.Games().Single().Seats.Single(s => s.Id == blueId) is { Away: true, AwaySince: not null });
        // Another player cannot take Blue's seat; Blue's own account and the server's stand-in can.
        var intruder = new Peer("intruder"); Hello(hub, intruder, "red-token");
        hub.Handle(intruder, Body(new FrameWriter(RelayProtocol.Rejoin).U32(5).Str("Game").Str("Blue").U32(0).U32(2)));
        Assert.Equal(RelayProtocol.ErrAccessDenied, intruder.ReplyResult(5));
        var standIn = new Peer("standin"); Hello(hub, standIn, "standin-token");
        hub.Handle(standIn, Body(new FrameWriter(RelayProtocol.Rejoin).U32(6).Str("Game").Str("Blue").U32(0).U32(2)));
        Assert.Equal(RelayProtocol.Ok, standIn.ReplyResult(6));
        Assert.Equal(blueId, CreatePlayer(hub, standIn, 7, RelayProtocol.PlayerServer, "HeroesPost"));
        Assert.True(hub.Games().Single().Seats.Single(s => s.Id == blueId) is { Away: false, StandIn: true, Account: "blue-id" });
    }

    private static RelayState State(string directory) =>
        new(new ConfigurationBuilder().AddInMemoryCollection(new Dictionary<string, string?> { ["Relay:StateDirectory"] = directory }).Build(), NullLogger<RelayState>.Instance);
    private static string TemporaryDirectory() => Path.Combine(Path.GetTempPath(), "heroes-post-tests", Guid.NewGuid().ToString("N"));

    private static RelayHub AccountHub(RelayState? state)
    {
        var hub = new RelayHub(NullLogger<RelayHub>.Instance, new ConfigurationBuilder().Build(), state);
        hub.Authenticate = ticket => ticket switch
        {
            "red-token" => new RelayAccount("red-id", "Red player"),
            "blue-token" => new RelayAccount("blue-id", "Blue player"),
            _ => null
        };
        return hub;
    }

    private static (RelayHub Hub, Peer Red, Peer Blue, uint RedId, uint BlueId) StartedAccountGame(RelayState? state = null)
    {
        var hub = AccountHub(state);
        var red = new Peer("red"); var blue = new Peer("blue");
        Hello(hub, red, "red-token"); Hello(hub, blue, "blue-token");
        hub.Handle(red, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate))));
        var redId = CreatePlayer(hub, red, 2, RelayProtocol.PlayerServer, "Red");
        hub.Handle(blue, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenJoin))));
        var blueId = CreatePlayer(hub, blue, 2, 0, "Blue");
        Send(hub, blue, blueId, 0, Message(GameMessage.LoadReceipt, blueId));
        Send(hub, red, redId, 0, Message(GameMessage.LoadGame, redId));
        return (hub, red, blue, redId, blueId);
    }

    private static FrameWriter Resume(uint request, Guid game, uint seat, uint generation, uint received, uint sent) =>
        new FrameWriter(RelayProtocol.Resume).U32(request).Guid(game).U32(seat).U32(generation).U32(received).U32(sent);

    private static List<uint> Delivered(Peer peer) =>
        peer.Of(RelayProtocol.Deliver).Select(x => { var r = x.Reader; r.U32(); r.U32(); return GameMessage.Parse(r.Blob()).Id; }).ToList();

    [Fact]
    public void LiveClientResumesItsSeatAfterItsConnectionDrops()
    {
        var (hub, red, blue, redId, blueId) = StartedAccountGame();
        var received = (uint)blue.Of(RelayProtocol.Deliver).Count();
        hub.Disconnected(blue);
        Send(hub, red, redId, blueId, Message(GameMessage.Simultaneous, redId));
        Send(hub, red, redId, 0, Message(GameMessage.CurrentPlayer, redId));

        var back = new Peer("blue-again"); Hello(hub, back, "blue-token");
        hub.Handle(back, Body(Resume(9, Instance, blueId, 0, received, 1)));
        Assert.Equal(RelayProtocol.Ok, back.ReplyResult(9));
        // The relay holds one of Blue's messages, and replays the two Blue missed.
        var reply = back.Of(RelayProtocol.Reply).Single().Reader; reply.U32(); reply.U32();
        Assert.Equal(1u, reply.U32());
        Assert.Equal([GameMessage.Simultaneous, GameMessage.CurrentPlayer], Delivered(back));
        Assert.True(hub.Games().Single().Seats.Single(s => s.Id == blueId) is { Away: false });
        // Blue's messages reach Red again, and Red's reach the resumed connection.
        Send(hub, back, blueId, redId, Message(GameMessage.LoadReceipt, blueId));
        Assert.Equal(GameMessage.LoadReceipt, Delivered(red).Last());
        Send(hub, red, redId, blueId, Message(GameMessage.LoadEnd, redId));
        Assert.Equal(GameMessage.LoadEnd, Delivered(back).Last());
    }

    [Fact]
    public void ResumeTakesTheSeatFromAConnectionTheRelayStillThinksIsOpen()
    {
        var (hub, red, blue, redId, blueId) = StartedAccountGame();
        var received = (uint)blue.Of(RelayProtocol.Deliver).Count();
        var back = new Peer("blue-again"); Hello(hub, back, "blue-token");
        hub.Handle(back, Body(Resume(1, Instance, blueId, 0, received, 1)));
        Assert.Equal(RelayProtocol.Ok, back.ReplyResult(1));
        var before = blue.Frames.Count;
        Send(hub, red, redId, blueId, Message(GameMessage.Simultaneous, redId));
        Assert.Equal(before, blue.Frames.Count);
        Assert.Equal([GameMessage.Simultaneous], Delivered(back));
        // The old connection closing later does not take the seat away.
        hub.Disconnected(blue);
        Assert.True(hub.Games().Single().Seats.Single(s => s.Id == blueId) is { Away: false });
    }

    [Fact]
    public void ResumeIsRefusedToOtherAccountsAndOlderProcesses()
    {
        var (hub, _, blue, _, blueId) = StartedAccountGame();
        hub.Disconnected(blue);
        var intruder = new Peer("intruder"); Hello(hub, intruder, "red-token");
        hub.Handle(intruder, Body(Resume(1, Instance, blueId, 0, 0, 0)));
        Assert.Equal(RelayProtocol.ErrAccessDenied, intruder.ReplyResult(1));
        var unknown = new Peer("unknown"); Hello(hub, unknown, "blue-token");
        hub.Handle(unknown, Body(Resume(2, Guid.NewGuid(), blueId, 0, 0, 0)));
        Assert.Equal(RelayProtocol.ErrNoSessions, unknown.ReplyResult(2));

        // A new process resumes Blue's capture: the seat's generation rises, so the old process cannot take it back.
        var resumed = new Peer("resumed"); Hello(hub, resumed, "blue-token");
        hub.Handle(resumed, Body(new FrameWriter(RelayProtocol.Rejoin).U32(3).Str("Game").Str("Blue").U32(0).U32(1)));
        hub.Handle(resumed, Body(new FrameWriter(RelayProtocol.CreatePlayer).U32(4).U32(RelayProtocol.PlayerServer).Str("HeroesPost").Str("HeroesPost").Blob([])));
        var created = resumed.Of(RelayProtocol.Reply).Last().Reader; created.U32(); created.U32();
        Assert.Equal((blueId, 1u), (created.U32(), created.U32()));
        var stale = new Peer("stale"); Hello(hub, stale, "blue-token");
        hub.Handle(stale, Body(Resume(5, Instance, blueId, 0, 0, 1)));
        Assert.Equal(RelayProtocol.ErrAccessDenied, stale.ReplyResult(5));
    }

    [Fact]
    public void StartedGamesSurviveARestartOfTheServer()
    {
        var directory = TemporaryDirectory();
        try
        {
            var state = State(directory);
            var (hub, red, blue, redId, blueId) = StartedAccountGame(state);
            Send(hub, red, redId, blueId, Message(GameMessage.Simultaneous, redId));
            Send(hub, blue, blueId, 0, Message(GameMessage.CurrentPlayer, blueId));
            Assert.True(hub.SetTurnSeconds(Instance, 1200));
            var redReceived = (uint)red.Of(RelayProtocol.Deliver).Count();
            var blueReceived = (uint)blue.Of(RelayProtocol.Deliver).Count();
            hub.Disconnected(blue);
            // The server stops while Red is still connected, after one more message for Blue.
            Send(hub, red, redId, blueId, Message(GameMessage.LoadEnd, redId));
            state.Close();

            var restarted = AccountHub(State(directory));
            var game = Assert.Single(restarted.Games());
            Assert.Equal((Instance, true, (int?)1200, 0), (game.Instance, game.Started, game.TurnSeconds, game.LiveClients));
            Assert.All(game.Seats, s => Assert.True(s.Away));
            var blueSeat = game.Seats.Single(s => s.Id == blueId);
            Assert.Equal((2, 3, "blue-id"), (blueSeat.Sent, blueSeat.Delivered, blueSeat.Account));
            Assert.Equal("red-id", game.Seats.Single(s => s.Id == redId).Account);

            // Both live clients take their seats back; Blue gets the message it missed.
            var red2 = new Peer("red2"); Hello(restarted, red2, "red-token");
            restarted.Handle(red2, Body(Resume(1, Instance, redId, 0, redReceived, 2)));
            Assert.Equal(RelayProtocol.Ok, red2.ReplyResult(1));
            Assert.Empty(Delivered(red2));
            var blue2 = new Peer("blue2"); Hello(restarted, blue2, "blue-token");
            restarted.Handle(blue2, Body(Resume(1, Instance, blueId, 0, blueReceived, 2)));
            Assert.Equal(RelayProtocol.Ok, blue2.ReplyResult(1));
            Assert.Equal([GameMessage.LoadEnd], Delivered(blue2));
            Send(restarted, red2, redId, blueId, Message(GameMessage.LoadReceipt, redId));
            Assert.Equal(GameMessage.LoadReceipt, Delivered(blue2).Last());

            // Red is still the host: its departure keeps the game, as before the restart.
            restarted.Disconnected(red2);
            Assert.Empty(blue2.Of(RelayProtocol.SessionLost));
            Assert.Single(restarted.Games());
        }
        finally { try { Directory.Delete(directory, true); } catch (IOException) { } }
    }

    [Fact]
    public void RejoinCanNameTheSeatByIdWhenPlayersShareAName()
    {
        var hub = AccountHub(null); var red = new Peer("red"); var blue = new Peer("blue");
        Hello(hub, red, "red-token"); Hello(hub, blue, "blue-token");
        hub.Handle(red, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate))));
        CreatePlayer(hub, red, 2, RelayProtocol.PlayerServer, "Player");
        hub.Handle(blue, Body(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenJoin))));
        var blueId = CreatePlayer(hub, blue, 2, 0, "Player");
        Send(hub, red, 1, 0, Message(GameMessage.LoadGame, 1));
        hub.Disconnected(red); hub.Disconnected(blue);

        var byName = new Peer("by-name"); Hello(hub, byName, "blue-token");
        hub.Handle(byName, Body(new FrameWriter(RelayProtocol.Rejoin).U32(1).Str("Game").Str("Player").U32(0).U32(0)));
        Assert.Equal(RelayProtocol.ErrNoSessions, byName.ReplyResult(1));
        var byId = new Peer("by-id"); Hello(hub, byId, "blue-token");
        hub.Handle(byId, Body(new FrameWriter(RelayProtocol.Rejoin).U32(1).Str("Game").Str("Player").U32(1).U32(0).U32(blueId)));
        Assert.Equal(RelayProtocol.Ok, byId.ReplyResult(1));
        Assert.Equal(blueId, CreatePlayer(hub, byId, 2, RelayProtocol.PlayerServer, "Player"));
    }

    [Fact]
    public void AResumedSeatIsHeardOnlyOnceItHasCaughtUp()
    {
        var (hub, red, blue, redId, blueId) = StartedAccountGame();
        Send(hub, blue, blueId, 0, Message(GameMessage.Simultaneous, blueId));
        Send(hub, blue, blueId, 0, Message(GameMessage.LoadGame, blueId));
        hub.Disconnected(blue);
        Send(hub, red, redId, blueId, Message(GameMessage.CurrentPlayer, redId));
        var before = Delivered(red).Count;

        var back = new Peer("blue-back"); Hello(hub, back, "blue-token");
        hub.Handle(back, Body(new FrameWriter(RelayProtocol.Rejoin).U32(1).Str("Game").Str("Blue").U32(1).U32(1)));
        hub.Handle(back, Body(new FrameWriter(RelayProtocol.CreatePlayer).U32(2).U32(RelayProtocol.PlayerServer).Str("HeroesPost").Str("HeroesPost").Blob([])));
        var reply = back.Of(RelayProtocol.Reply).Last().Reader; reply.U32(); reply.U32();
        Assert.Equal((blueId, 1u, 1u), (reply.U32(), reply.U32(), reply.U32())); // seat, generation, frames replayed
        // Working through the replay, the engine repeats the day's end its own way and announces itself.
        Send(hub, back, blueId, 0, Message(GameMessage.Simultaneous, blueId));
        Send(hub, back, blueId, 0, Message(GameMessage.LoadGame, blueId));
        Send(hub, back, blueId, 0, Message(GameMessage.CurrentPlayer, blueId));
        Assert.Equal(before, Delivered(red).Count);
        // Caught up: what it sends now is the player's.
        hub.Handle(back, Body(new FrameWriter(RelayProtocol.CaughtUp)));
        Send(hub, back, blueId, redId, Message(GameMessage.LoadReceipt, blueId));
        Assert.Equal([GameMessage.LoadReceipt], Delivered(red).Skip(before));
        Assert.Equal(5, hub.Games().Single().Seats.Single(s => s.Id == blueId).Sent);
    }

    [Fact]
    public void TurnLimitIsKeptPerGame()
    {
        var (hub, _, _, _, _) = StartedGame();
        var id = hub.Games().Single().Instance;
        Assert.Null(hub.Games().Single().TurnSeconds);
        Assert.True(hub.SetTurnSeconds(id, 900));
        Assert.Equal(900, hub.Games().Single().TurnSeconds);
        Assert.False(hub.SetTurnSeconds(Guid.NewGuid(), 60));
    }
}
