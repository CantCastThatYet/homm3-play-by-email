using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
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

    private static async Task<HttpClient> Player(LobbyFactory app, string name)
    {
        var http = app.CreateClient();
        var response = await http.PostAsJsonAsync("/api/players", new RegisterPlayer(name), cancellationToken: TestContext.Current.CancellationToken);
        response.EnsureSuccessStatusCode();
        var identity = (await response.Content.ReadFromJsonAsync<PlayerCredentials>(cancellationToken: TestContext.Current.CancellationToken))!;
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", identity.Token);
        return http;
    }

    [Fact]
    public async Task Lobby_lists_relay_sessions_and_carries_chat()
    {
        await using var app = new LobbyFactory();
        var ann = await Player(app, "Ann"); var bob = await Player(app, "Bob");
        using var anonymous = app.CreateClient();
        Assert.Equal(HttpStatusCode.Unauthorized, (await anonymous.GetAsync("/api/relay/lobby", TestContext.Current.CancellationToken)).StatusCode);

        // A HotA host opens "Duel" through the relay: the session name is "<game>ú<host>".
        var hub = app.Services.GetRequiredService<RelayHub>();
        hub.Handle(new Peer(), new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate)
            .Guid(Guid.NewGuid()).Guid(Guid.Empty).U32(0).U32(4).U32(0).Str("DuelúRed").Str("secret").U32(0).U32(0).U32(0).U32(0).Finish()[4..]);

        var first = (await ann.GetFromJsonAsync<RelayLobby>("/api/relay/lobby", TestContext.Current.CancellationToken))!;
        Assert.Equal("Ann", first.Player);
        var game = Assert.Single(first.Games);
        Assert.Equal(("Duel", "Red", false, 4, true), (game.Name, game.Host, game.Started, game.MaxPlayers, game.Password));
        Assert.Empty(first.Mine);
        Assert.Empty(first.Chat);

        (await ann.PostAsJsonAsync("/api/relay/chat", new RelayChatPost("  Anyone for a duel?  "), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        (await bob.PostAsJsonAsync("/api/relay/chat", new RelayChatPost("Me"), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        Assert.Equal(HttpStatusCode.BadRequest, (await bob.PostAsJsonAsync("/api/relay/chat", new RelayChatPost(" "), TestContext.Current.CancellationToken)).StatusCode);

        var chat = (await bob.GetFromJsonAsync<RelayLobby>("/api/relay/lobby", TestContext.Current.CancellationToken))!.Chat;
        Assert.Equal([("Ann", "Anyone for a duel?"), ("Bob", "Me")], chat.Select(l => (l.Name, l.Text)));
        var after = (await bob.GetFromJsonAsync<RelayLobby>($"/api/relay/lobby?after={chat[0].Id}", TestContext.Current.CancellationToken))!.Chat;
        Assert.Equal("Me", Assert.Single(after).Text);
    }
}
