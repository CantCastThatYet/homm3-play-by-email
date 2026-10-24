using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using HeroesLobby.Contracts;
using Xunit;

namespace HeroesLobby.Tests;

public class NativeLaunchTests
{
    [Fact]
    public async Task Startup_is_authorized_idempotent_persistent_and_requires_real_peer_progress()
    {
        var database = Path.Combine(Path.GetTempPath(), "heroes-native-test", Guid.NewGuid().ToString("N"), "lobby.db");
        string gameId; NativeLaunch run;
        await using (var server = new LobbyFactory(database))
        {
            var host = server.CreateClient(); var peer = server.CreateClient(); var stranger = server.CreateClient();
            foreach (var (http, name) in new[] { (host, "Red"), (peer, "Blue"), (stranger, "Outside") })
            {
                var registered = await http.PostAsJsonAsync("api/players", new RegisterPlayer(name), TestContext.Current.CancellationToken);
                var credential = (await registered.Content.ReadFromJsonAsync<PlayerCredentials>(TestContext.Current.CancellationToken))!;
                http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", credential.Token);
            }
            var created = await host.PostAsJsonAsync("api/games", new CreateGame("Native test", "Judgement Day", 2, "friends"), TestContext.Current.CancellationToken);
            var game = (await created.Content.ReadFromJsonAsync<GameInfo>(TestContext.Current.CancellationToken))!; gameId = game.Id;
            var prefix = $"api/games/{gameId}";
            (await peer.PostAsJsonAsync(prefix + "/join", new JoinGame("friends"), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
            async Task<GameInfo> Current() => (await host.GetFromJsonAsync<GameInfo>(prefix, TestContext.Current.CancellationToken))!;
            async Task Ready(HttpClient http) => (await http.PutAsJsonAsync(prefix + "/ready", new ReadyRequest((await Current()).Revision, true, true), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
            Assert.Equal(HttpStatusCode.Conflict, (await host.PostAsJsonAsync(prefix + "/native", new BeginNativeLaunch((await Current()).Revision, "127.0.0.1"), TestContext.Current.CancellationToken)).StatusCode);
            await Ready(host); await Ready(peer); game = await Current();
            Assert.Equal(HttpStatusCode.Forbidden, (await peer.PostAsJsonAsync(prefix + "/native", new BeginNativeLaunch(game.Revision, "127.0.0.1"), TestContext.Current.CancellationToken)).StatusCode);
            var begun = await host.PostAsJsonAsync(prefix + "/native", new BeginNativeLaunch(game.Revision, "127.0.0.1"), TestContext.Current.CancellationToken); begun.EnsureSuccessStatusCode();
            run = (await begun.Content.ReadFromJsonAsync<NativeLaunch>(TestContext.Current.CancellationToken))!;
            var again = await host.PostAsJsonAsync(prefix + "/native", new BeginNativeLaunch(game.Revision, "127.0.0.1"), TestContext.Current.CancellationToken);
            Assert.Equal(run.Id, (await again.Content.ReadFromJsonAsync<NativeLaunch>(TestContext.Current.CancellationToken))!.Id);
            Assert.Equal(HttpStatusCode.Forbidden, (await stranger.GetAsync(prefix + "/native", TestContext.Current.CancellationToken)).StatusCode);
            async Task<HttpResponseMessage> Report(HttpClient http, string phase, uint id, string? attempt = null) =>
                await http.PutAsJsonAsync(prefix + "/native", new NativeLaunchReport(attempt ?? run.Id, phase, id), TestContext.Current.CancellationToken);
            Assert.Equal(HttpStatusCode.Conflict, (await Report(host, "Started", 1)).StatusCode);
            Assert.Equal(HttpStatusCode.Conflict, (await Report(host, "Hosted", 1, "old-attempt")).StatusCode);
            (await Report(host, "Hosted", 1)).EnsureSuccessStatusCode();
            Assert.Equal(HttpStatusCode.BadRequest, (await Report(peer, "Joined", 1)).StatusCode);
            (await Report(peer, "Joined", 22)).EnsureSuccessStatusCode();
            Assert.Equal(HttpStatusCode.Conflict, (await Report(host, "Started", 1)).StatusCode);
            (await Report(peer, "Armed", 22)).EnsureSuccessStatusCode();
            (await Report(host, "Started", 1)).EnsureSuccessStatusCode();
            (await Report(peer, "Started", 22)).EnsureSuccessStatusCode();
            Assert.Equal("Playing", (await Current()).Status);
            Assert.Equal(HttpStatusCode.Conflict, (await host.PostAsJsonAsync(prefix + "/reopen", new RevisionRequest((await Current()).Revision), TestContext.Current.CancellationToken)).StatusCode);
        }
        await using var restarted = new LobbyFactory(database); using var api = restarted.CreateClient();
        Assert.Equal("Playing", (await api.GetFromJsonAsync<GameInfo>($"api/games/{gameId}", TestContext.Current.CancellationToken))!.Status);
    }
}
