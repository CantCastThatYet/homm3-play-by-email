using System.Buffers.Binary;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using HeroesLobby.Contracts;
using HeroesLobby.Server.Relay;
using Microsoft.Extensions.DependencyInjection;
using Xunit;

namespace HeroesLobby.Tests;

public class RelayLobbyTests
{
    private sealed class Peer : IRelayPeer
    {
        public string Label => "host";
        public void Post(byte[] frame) { }
    }

    /// <summary>A lobby connection to the test server's relay port.</summary>
    private sealed class Client : IAsyncDisposable
    {
        private readonly TcpClient tcp = new();
        private NetworkStream stream = null!;

        public static async Task<Client> Connect(LobbyFactory app, string ticket, string name)
        {
            var relay = app.Services.GetRequiredService<RelayServer>();
            await relay.Started;
            var client = new Client();
            await client.tcp.ConnectAsync("127.0.0.1", relay.Port, TestContext.Current.CancellationToken);
            client.stream = client.tcp.GetStream();
            await client.Send(new FrameWriter(RelayProtocol.Hello).U32(RelayProtocol.Version).Str(ticket).Str(name));
            return client;
        }

        public async Task Send(FrameWriter frame) => await stream.WriteAsync(frame.Finish(), TestContext.Current.CancellationToken);

        /// <summary>The next frame of this type, skipping others.</summary>
        public async Task<FrameReader> Next(ushort type)
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(5));
            while (true)
            {
                var header = new byte[4];
                await stream.ReadExactlyAsync(header, timeout.Token);
                var frame = new byte[BinaryPrimitives.ReadUInt32LittleEndian(header)];
                await stream.ReadExactlyAsync(frame, timeout.Token);
                if (BinaryPrimitives.ReadUInt16LittleEndian(frame) == type) return new FrameReader(frame, 2);
            }
        }

        public async Task<RelayLobby> State() =>
            JsonSerializer.Deserialize((await Next(RelayProtocol.LobbyState)).Blob(), LobbyJsonContext.Default.RelayLobby)!;
        public async Task<List<RelayChatLine>> Chat() =>
            JsonSerializer.Deserialize((await Next(RelayProtocol.LobbyChatLines)).Blob(), LobbyJsonContext.Default.ListRelayChatLine)!;
        public async Task<(uint Code, string Text)> Notice()
        {
            var r = await Next(RelayProtocol.LobbyNotice);
            return (r.U32(), Encoding.UTF8.GetString(r.Blob()));
        }
        public Task Say(string text) => Send(new FrameWriter(RelayProtocol.LobbyChat).Blob(Encoding.UTF8.GetBytes(text)));

        public async ValueTask DisposeAsync() { tcp.Dispose(); await Task.CompletedTask; }
    }

    private static async Task<string> Token(LobbyFactory app, string name)
    {
        using var http = app.CreateClient();
        var response = await http.PostAsJsonAsync("/api/players", new RegisterPlayer(name), cancellationToken: TestContext.Current.CancellationToken);
        response.EnsureSuccessStatusCode();
        return (await response.Content.ReadFromJsonAsync<PlayerCredentials>(cancellationToken: TestContext.Current.CancellationToken))!.Token;
    }

    private static FrameWriter Session(FrameWriter w, Guid instance, string name, string password) =>
        w.Guid(instance).Guid(Guid.Empty).U32(0).U32(4).U32(0).Str(name).Str(password).U32(0).U32(0).U32(0).U32(0);

    [Fact]
    public async Task Lobby_is_pushed_over_the_relay_connection()
    {
        await using var app = new LobbyFactory();
        using var http = app.CreateClient();
        Assert.Equal(System.Net.HttpStatusCode.NotFound, (await http.GetAsync("/api/relay/lobby", TestContext.Current.CancellationToken)).StatusCode);

        // Without a known token the lobby asks the client to sign in again.
        await using (var stranger = await Client.Connect(app, "not-a-token", "Eve"))
        {
            await stranger.Send(new FrameWriter(RelayProtocol.LobbyWatch));
            Assert.Equal(RelayProtocol.NoticeSignIn, (await stranger.Notice()).Code);
        }

        await using var ann = await Client.Connect(app, await Token(app, "Ann"), "Ann");
        await ann.Send(new FrameWriter(RelayProtocol.LobbyWatch));
        var first = await ann.State();
        Assert.Equal(("Ann", 600), (first.Player, first.DefaultTurnSeconds));
        Assert.Empty(first.Games); Assert.Empty(first.Mine);
        Assert.Empty(await ann.Chat());

        // A HotA host opens "Duel" through the relay: the session name is "<game>ú<host>". Ann is told without asking.
        var hub = app.Services.GetRequiredService<RelayHub>();
        hub.Handle(new Peer(), Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate), Guid.NewGuid(), "DuelúRed", "secret").Finish()[4..]);
        var game = Assert.Single((await ann.State()).Games);
        Assert.Equal(("Duel", "Red", false, 4, true), (game.Name, game.Host, game.Started, game.MaxPlayers, game.Password));

        // Chat reaches every watcher; a new watcher gets the recent lines.
        await ann.Say("  Anyone for a duel?  ");
        Assert.Equal(("Ann", "Anyone for a duel?"), (await ann.Chat()).Select(l => (l.Name, l.Text)).Single());
        await using var bob = await Client.Connect(app, await Token(app, "Bob"), "Bob");
        await bob.Send(new FrameWriter(RelayProtocol.LobbyWatch));
        Assert.Equal("Bob", (await bob.State()).Player);
        Assert.Equal("Anyone for a duel?", Assert.Single(await bob.Chat()).Text);
        await bob.Say("Мне");
        Assert.Equal(("Bob", "Мне"), (await ann.Chat()).Select(l => (l.Name, l.Text)).Single());
        await bob.Say(" ");
        Assert.Equal(RelayProtocol.NoticeText, (await bob.Notice()).Code);

        // A seat Ann creates on the same connection is hers: it shows under her games and she may set its turn limit.
        await ann.Send(new FrameWriter(RelayProtocol.LobbyTurnLimit).Guid(game.Id).U32(1800));
        Assert.Equal("You have no seat in this game.", (await ann.Notice()).Text);
        var mine = Guid.NewGuid();
        await ann.Send(Session(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate), mine, "MineúAnn", ""));
        await ann.Send(new FrameWriter(RelayProtocol.CreatePlayer).U32(2).U32(RelayProtocol.PlayerServer).Str("Ann").Str("Ann").Blob([]));
        RelayLobby state;
        do state = await ann.State(); while (state.Mine.Count == 0);
        Assert.Equal((mine, 600), (state.Mine[0].Id, state.Mine[0].TurnSeconds));
        await ann.Send(new FrameWriter(RelayProtocol.LobbyTurnLimit).Guid(mine).U32(1800));
        Assert.Equal(1800, (await ann.State()).Mine[0].TurnSeconds);
    }
}
