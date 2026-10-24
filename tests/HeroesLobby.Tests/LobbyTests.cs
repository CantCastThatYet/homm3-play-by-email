using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using HeroesLobby.Contracts;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http.Connections;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.SignalR.Client;
using Microsoft.Extensions.Configuration;
using Microsoft.Data.Sqlite;
using Xunit;

namespace HeroesLobby.Tests;

public sealed class LobbyFactory(string? database = null) : WebApplicationFactory<Program>
{
    public string Database { get; } = database ?? Path.Combine(Path.GetTempPath(), "heroes-post-tests", Guid.NewGuid().ToString("N"), "lobby.db");
    protected override void ConfigureWebHost(IWebHostBuilder builder) => builder.ConfigureAppConfiguration((_, config) =>
        config.AddInMemoryCollection(new Dictionary<string, string?> { ["Lobby:Database"] = Database, ["Lobby:Name"] = "Test server" }));
}

public class LobbyTests
{
    private static async Task<(HttpClient Http, PlayerCredentials Identity)> Player(LobbyFactory app, string name)
    {
        var http = app.CreateClient();
        var response = await http.PostAsJsonAsync("/api/players", new RegisterPlayer(name), cancellationToken: TestContext.Current.CancellationToken); response.EnsureSuccessStatusCode();
        var identity = (await response.Content.ReadFromJsonAsync<PlayerCredentials>(cancellationToken: TestContext.Current.CancellationToken))!;
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", identity.Token);
        return (http, identity);
    }
    private static async Task<GameInfo> Game(HttpClient http, int capacity = 4, string? password = "friends")
    {
        var response = await http.PostAsJsonAsync("/api/games", new CreateGame("The long campaign", "Giant map", capacity, password), cancellationToken: TestContext.Current.CancellationToken);
        response.EnsureSuccessStatusCode(); return (await response.Content.ReadFromJsonAsync<GameInfo>(cancellationToken: TestContext.Current.CancellationToken))!;
    }
    private static async Task<GameInfo> Read(HttpClient http, string id) => (await http.GetFromJsonAsync<GameInfo>($"/api/games/{id}", cancellationToken: TestContext.Current.CancellationToken))!;

    [Fact]
    public async Task Passwords_authentication_and_public_responses_are_separated()
    {
        await using var app = new LobbyFactory();
        var host = await Player(app, "Host"); var friend = await Player(app, "Friend"); var game = await Game(host.Http);
        using var anonymous = app.CreateClient();
        Assert.Equal(HttpStatusCode.Unauthorized, (await anonymous.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame("friends"), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame("wrong"), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
        var response = await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame("friends"), cancellationToken: TestContext.Current.CancellationToken); response.EnsureSuccessStatusCode();
        var json = await anonymous.GetStringAsync("/api/games", cancellationToken: TestContext.Current.CancellationToken);
        Assert.DoesNotContain("friends", json); Assert.DoesNotContain("passwordHash", json); Assert.DoesNotContain(host.Identity.Token, json);
        var rejoin = await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame(null), cancellationToken: TestContext.Current.CancellationToken); rejoin.EnsureSuccessStatusCode();
        Assert.Equal(2, (await Read(host.Http, game.Id)).Members.Count);
        Assert.Equal(HttpStatusCode.Forbidden, (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/lock", new RevisionRequest(2), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
    }

    [Fact]
    public async Task Concurrent_joins_cannot_overfill_or_duplicate_colors()
    {
        await using var app = new LobbyFactory(); var host = await Player(app, "Host"); var game = await Game(host.Http, 2, null);
        var a = await Player(app, "A"); var b = await Player(app, "B");
        var results = await Task.WhenAll(a.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame(null), cancellationToken: TestContext.Current.CancellationToken), b.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame(null), cancellationToken: TestContext.Current.CancellationToken));
        Assert.Single(results, r => r.IsSuccessStatusCode); Assert.Single(results, r => r.StatusCode == HttpStatusCode.Conflict);
        var saved = await Read(host.Http, game.Id); Assert.Equal(2, saved.Members.Count); Assert.Equal(2, saved.Members.Select(m => m.Color).Distinct().Count());
    }

    [Fact]
    public async Task Readiness_requires_current_state_and_lock_freezes_the_roster()
    {
        await using var app = new LobbyFactory(); var host = await Player(app, "Host"); var friend = await Player(app, "Friend"); var outsider = await Player(app, "Outsider");
        var game = await Game(host.Http);
        (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame("friends"), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        Assert.Equal(HttpStatusCode.Conflict, (await host.Http.PutAsJsonAsync($"/api/games/{game.Id}/ready", new ReadyRequest(1, true), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
        game = await Read(host.Http, game.Id);
        Assert.Equal(HttpStatusCode.Conflict, (await host.Http.PostAsJsonAsync($"/api/games/{game.Id}/lock", new RevisionRequest(game.Revision), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
        (await host.Http.PutAsJsonAsync($"/api/games/{game.Id}/ready", new ReadyRequest(game.Revision, true), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        game = await Read(host.Http, game.Id);
        (await friend.Http.PutAsJsonAsync($"/api/games/{game.Id}/settings", new PlayerSettings(game.Revision, 1, "Castle", "Random", "Gold"), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        game = await Read(host.Http, game.Id); Assert.All(game.Members, m => Assert.False(m.Ready));
        foreach (var player in new[] { host.Http, friend.Http })
        {
            game = await Read(player, game.Id);
            (await player.PutAsJsonAsync($"/api/games/{game.Id}/ready", new ReadyRequest(game.Revision, true), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        }
        game = await Read(host.Http, game.Id);
        (await host.Http.PostAsJsonAsync($"/api/games/{game.Id}/lock", new RevisionRequest(game.Revision), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        game = await Read(host.Http, game.Id); Assert.Equal("Prepared", game.Status);
        Assert.Equal(HttpStatusCode.Conflict, (await outsider.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame("friends"), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
        Assert.Equal(HttpStatusCode.Conflict, (await friend.Http.PutAsJsonAsync($"/api/games/{game.Id}/ready", new ReadyRequest(game.Revision, false), cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
        (await host.Http.PostAsJsonAsync($"/api/games/{game.Id}/reopen", new RevisionRequest(game.Revision), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        game = await Read(host.Http, game.Id); Assert.Equal("Lobby", game.Status); Assert.All(game.Members, m => Assert.False(m.Ready));
    }

    [Fact]
    public async Task Server_restart_keeps_identity_membership_and_readiness()
    {
        var app = new LobbyFactory(); var database = app.Database;
        var host = await Player(app, "Persistent host"); var game = await Game(host.Http);
        (await host.Http.PutAsJsonAsync($"/api/games/{game.Id}/ready", new ReadyRequest(game.Revision, true), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        host.Http.Dispose(); await app.DisposeAsync();
        await using var restarted = new LobbyFactory(database); using var http = restarted.CreateClient();
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", host.Identity.Token);
        Assert.Equal(host.Identity.Player.Id, (await http.GetFromJsonAsync<PlayerInfo>("/api/me", cancellationToken: TestContext.Current.CancellationToken))!.Id);
        var restored = await Read(http, game.Id); Assert.True(restored.Members.Single().Ready); Assert.Equal(game.HostId, restored.HostId);
        using var db = new SqliteConnection($"Data Source={database}"); db.Open();
        using var cmd = db.CreateCommand(); cmd.CommandText = "SELECT token_hash FROM players WHERE id=$id"; cmd.Parameters.AddWithValue("$id", host.Identity.Player.Id);
        Assert.NotEqual(host.Identity.Token, (string)cmd.ExecuteScalar()!);
    }

    [Fact]
    public async Task Explicit_leave_transfers_host_then_removes_empty_game()
    {
        await using var app = new LobbyFactory(); var host = await Player(app, "Host"); var friend = await Player(app, "Friend");
        var game = await Game(host.Http); (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame("friends"), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        game = await Read(host.Http, game.Id);
        (await host.Http.PostAsJsonAsync($"/api/games/{game.Id}/leave", new RevisionRequest(game.Revision), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        game = await Read(friend.Http, game.Id); Assert.Equal(friend.Identity.Player.Id, game.HostId);
        (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/leave", new RevisionRequest(game.Revision), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        Assert.Equal(HttpStatusCode.NotFound, (await friend.Http.GetAsync($"/api/games/{game.Id}", cancellationToken: TestContext.Current.CancellationToken)).StatusCode);
    }

    [Fact]
    public async Task Connected_clients_receive_committed_lobby_change_hints()
    {
        await using var app = new LobbyFactory(); var player = await Player(app, "Host");
        await using var connection = new HubConnectionBuilder().WithUrl("http://localhost/hubs/lobby", options =>
        {
            options.Transports = HttpTransportType.LongPolling; options.HttpMessageHandlerFactory = _ => app.Server.CreateHandler();
        }).Build();
        var changed = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        connection.On<string>("Changed", id => changed.TrySetResult(id)); await connection.StartAsync(cancellationToken: TestContext.Current.CancellationToken);
        var game = await Game(player.Http);
        Assert.Equal(game.Id, await changed.Task.WaitAsync(TimeSpan.FromSeconds(10), cancellationToken: TestContext.Current.CancellationToken));
        Assert.Equal(game.Name, (await Read(player.Http, game.Id)).Name);
    }
}
