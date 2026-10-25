using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using HeroesLobby.Contracts;
using HeroesLobby.Server;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.Data.Sqlite;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Xunit;

namespace HeroesLobby.Tests;

public sealed class CheckpointFactory(string? database = null, long maxBytes = 64 * 1024 * 1024) : WebApplicationFactory<Program>
{
    public string Database { get; } = database ?? Path.Combine(Path.GetTempPath(), "heroes-post-checkpoint-tests", Guid.NewGuid().ToString("N"), "lobby.db");
    protected override void ConfigureWebHost(IWebHostBuilder builder) => builder.ConfigureAppConfiguration((_, config) =>
        config.AddInMemoryCollection(new Dictionary<string, string?>
        {
            ["Lobby:Database"] = Database,
            ["Relay:Port"] = "0", ["Checkpoints:MaxBytes"] = maxBytes.ToString(System.Globalization.CultureInfo.InvariantCulture)
        }));
}

public class CheckpointTests
{
    private static CancellationToken Cancellation => TestContext.Current.CancellationToken;
    private static async Task<(HttpClient Http, PlayerCredentials Identity)> Player(CheckpointFactory app, string name)
    {
        var http = app.CreateClient();
        var result = await http.PostAsJsonAsync("/api/players", new RegisterPlayer(name), Cancellation);
        result.EnsureSuccessStatusCode();
        var identity = (await result.Content.ReadFromJsonAsync<PlayerCredentials>(Cancellation))!;
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", identity.Token);
        return (http, identity);
    }
    private static async Task<GameInfo> Game(HttpClient http)
    {
        var result = await http.PostAsJsonAsync("/api/games", new CreateGame("Checkpoint test", "Small", 3, null), Cancellation);
        result.EnsureSuccessStatusCode();
        return (await result.Content.ReadFromJsonAsync<GameInfo>(Cancellation))!;
    }
    private static async Task<HttpResponseMessage> Upload(HttpClient http, GameInfo game, byte[] bytes, string key, long? revision = null)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, $"/api/games/{game.Id}/checkpoints?baseRevision={revision ?? game.Revision}");
        request.Headers.Add("Idempotency-Key", key);
        request.Content = new ByteArrayContent(bytes);
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        return await http.SendAsync(request, Cancellation);
    }
    private static string ContentUrl(GameInfo game, CheckpointInfo info) => $"/api/games/{game.Id}/checkpoints/{info.Id}/content";

    [Fact]
    public async Task Committed_checkpoint_survives_restart_without_advancing_game()
    {
        var app = new CheckpointFactory();
        var host = await Player(app, "Host"); var game = await Game(host.Http);
        var bytes = RandomNumberGenerator.GetBytes(32 * 1024); // Exceeds the ordinary lobby request cap.
        using var response = await Upload(host.Http, game, bytes, "first"); response.EnsureSuccessStatusCode();
        var info = (await response.Content.ReadFromJsonAsync<CheckpointInfo>(Cancellation))!;
        Assert.Equal(host.Identity.Player.Id, info.OwnerPlayerId);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(), info.Sha256);
        Assert.Equal(bytes.Length, info.Length);
        var after = (await host.Http.GetFromJsonAsync<GameInfo>($"/api/games/{game.Id}", Cancellation))!;
        Assert.Equal(game.Revision, after.Revision); Assert.Equal(game.Status, after.Status);
        var database = app.Database;
        host.Http.Dispose(); await app.DisposeAsync();
        await using var restarted = new CheckpointFactory(database); using var client = restarted.CreateClient();
        client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", host.Identity.Token);
        var listed = await client.GetFromJsonAsync<List<CheckpointInfo>>($"/api/games/{game.Id}/checkpoints", Cancellation);
        Assert.Equal(info, Assert.Single(listed!));
        Assert.Equal(bytes, await client.GetByteArrayAsync(ContentUrl(game, info), Cancellation));
    }

    [Fact]
    public async Task Idempotency_is_scoped_to_owner_and_rejects_different_requests()
    {
        await using var app = new CheckpointFactory(); var host = await Player(app, "Host"); var game = await Game(host.Http);
        byte[] bytes = [1, 2, 3];
        var requests = await Task.WhenAll(Upload(host.Http, game, bytes, "retry"), Upload(host.Http, game, bytes, "retry"));
        var receipts = new List<CheckpointInfo>();
        foreach (var response in requests)
        {
            using (response) { response.EnsureSuccessStatusCode(); receipts.Add((await response.Content.ReadFromJsonAsync<CheckpointInfo>(Cancellation))!); }
        }
        Assert.Equal(receipts[0], receipts[1]);
        using var different = await Upload(host.Http, game, [1, 2, 4], "retry");
        Assert.Equal(HttpStatusCode.Conflict, different.StatusCode);
        (await host.Http.PutAsJsonAsync($"/api/games/{game.Id}/ready", new ReadyRequest(game.Revision, true), Cancellation)).EnsureSuccessStatusCode();
        using var retryAfterRevision = await Upload(host.Http, game, bytes, "retry");
        retryAfterRevision.EnsureSuccessStatusCode();
        Assert.Equal(receipts[0], await retryAfterRevision.Content.ReadFromJsonAsync<CheckpointInfo>(Cancellation));
        using var staleNew = await Upload(host.Http, game, bytes, "new");
        Assert.Equal(HttpStatusCode.Conflict, staleNew.StatusCode);
        using var changedRevision = await Upload(host.Http, game, bytes, "retry", game.Revision + 1);
        Assert.Equal(HttpStatusCode.Conflict, changedRevision.StatusCode);
        Assert.Single((await host.Http.GetFromJsonAsync<List<CheckpointInfo>>($"/api/games/{game.Id}/checkpoints", Cancellation))!);
    }

    [Fact]
    public async Task Only_current_members_can_upload_list_or_download_and_games_are_isolated()
    {
        await using var app = new CheckpointFactory(); var host = await Player(app, "Host"); var friend = await Player(app, "Friend");
        var game = await Game(host.Http); var other = await Game(friend.Http);
        using var uploaded = await Upload(host.Http, game, [7, 8], "one"); uploaded.EnsureSuccessStatusCode();
        var info = (await uploaded.Content.ReadFromJsonAsync<CheckpointInfo>(Cancellation))!;
        using var anonymous = app.CreateClient();
        Assert.Equal(HttpStatusCode.Unauthorized, (await anonymous.GetAsync(ContentUrl(game, info), Cancellation)).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, (await friend.Http.GetAsync(ContentUrl(game, info), Cancellation)).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, (await friend.Http.GetAsync($"/api/games/{game.Id}/checkpoints", Cancellation)).StatusCode);
        using var forbidden = await Upload(friend.Http, game, [1], "blocked"); Assert.Equal(HttpStatusCode.Forbidden, forbidden.StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await friend.Http.GetAsync(ContentUrl(other, info), Cancellation)).StatusCode);
        (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame(null), Cancellation)).EnsureSuccessStatusCode();
        Assert.Equal(new byte[] { 7, 8 }, await friend.Http.GetByteArrayAsync(ContentUrl(game, info), Cancellation));
        game = (await host.Http.GetFromJsonAsync<GameInfo>($"/api/games/{game.Id}", Cancellation))!;
        using var sameKeyDifferentOwner = await Upload(friend.Http, game, [9], "one"); sameKeyDifferentOwner.EnsureSuccessStatusCode();
        Assert.NotEqual(info.Id, (await sameKeyDifferentOwner.Content.ReadFromJsonAsync<CheckpointInfo>(Cancellation))!.Id);
        (await host.Http.PostAsJsonAsync($"/api/games/{game.Id}/leave", new RevisionRequest(game.Revision), Cancellation)).EnsureSuccessStatusCode();
        Assert.Equal(HttpStatusCode.Forbidden, (await host.Http.GetAsync(ContentUrl(game, info), Cancellation)).StatusCode);
    }

    [Fact]
    public async Task Oversized_empty_and_interrupted_uploads_leave_no_metadata_or_partial_content()
    {
        await using var app = new CheckpointFactory(maxBytes: 32); var host = await Player(app, "Host"); var game = await Game(host.Http);
        using var tooLarge = await Upload(host.Http, game, new byte[33], "large");
        Assert.Equal(HttpStatusCode.RequestEntityTooLarge, tooLarge.StatusCode);
        using var empty = await Upload(host.Http, game, [], "empty"); Assert.Equal(HttpStatusCode.BadRequest, empty.StatusCode);
        var store = app.Services.GetRequiredService<CheckpointStore>();
        await Assert.ThrowsAsync<LobbyError>(() => store.Upload(game.Id, host.Identity.Player, game.Revision, "streamed",
            new MemoryStream(new byte[33]), null, Cancellation));
        await Assert.ThrowsAsync<IOException>(() => store.Upload(game.Id, host.Identity.Player, game.Revision, "interrupted",
            new InterruptedStream(), null, Cancellation));
        using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => store.Upload(game.Id, host.Identity.Player, game.Revision, "cancelled",
            new MemoryStream([1, 2]), null, cancelled.Token));
        Assert.Empty(store.List(game.Id, host.Identity.Player));
        using var db = new SqliteConnection($"Data Source={app.Database}"); db.Open();
        using var command = db.CreateCommand(); command.CommandText = "SELECT count(*) FROM checkpoints";
        Assert.Equal(0L, (long)command.ExecuteScalar()!);
    }

    [Fact]
    public async Task Kestrel_allows_checkpoint_body_above_lobby_limit_and_enforces_checkpoint_limit()
    {
        await using var app = new CheckpointFactory(maxBytes: 40 * 1024);
        app.UseKestrel(0);
        var host = await Player(app, "Host"); var game = await Game(host.Http);
        using var accepted = await Upload(host.Http, game, new byte[32 * 1024], "large-valid");
        accepted.EnsureSuccessStatusCode();
        using var excessive = await Upload(host.Http, game, new byte[41 * 1024], "large-invalid");
        Assert.Equal(HttpStatusCode.RequestEntityTooLarge, excessive.StatusCode);
        (await host.Http.GetAsync("/health", Cancellation)).EnsureSuccessStatusCode();
    }

    [Fact]
    public async Task Member_who_leaves_during_upload_cannot_commit_checkpoint()
    {
        await using var app = new CheckpointFactory(); var host = await Player(app, "Host"); var friend = await Player(app, "Friend");
        var game = await Game(host.Http);
        (await friend.Http.PostAsJsonAsync($"/api/games/{game.Id}/join", new JoinGame(null), Cancellation)).EnsureSuccessStatusCode();
        game = (await host.Http.GetFromJsonAsync<GameInfo>($"/api/games/{game.Id}", Cancellation))!;
        var store = app.Services.GetRequiredService<CheckpointStore>();
        using var source = new PausedStream();
        var upload = store.Upload(game.Id, host.Identity.Player, game.Revision, "leave-race", source, null, Cancellation);
        await source.Started.Task.WaitAsync(Cancellation);
        (await host.Http.PostAsJsonAsync($"/api/games/{game.Id}/leave", new RevisionRequest(game.Revision), Cancellation)).EnsureSuccessStatusCode();
        source.Resume.TrySetResult();
        var error = await Assert.ThrowsAsync<LobbyError>(() => upload);
        Assert.Equal(403, error.Status);
        Assert.Empty(store.List(game.Id, friend.Identity.Player));
    }

    [Fact]
    public async Task Corrupt_checkpoint_is_not_returned_as_valid_bytes()
    {
        await using var app = new CheckpointFactory(); var host = await Player(app, "Host"); var game = await Game(host.Http);
        using var response = await Upload(host.Http, game, [1, 2], "corrupt"); response.EnsureSuccessStatusCode();
        var info = (await response.Content.ReadFromJsonAsync<CheckpointInfo>(Cancellation))!;
        using (var db = new SqliteConnection($"Data Source={app.Database}"))
        {
            db.Open(); using var command = db.CreateCommand();
            command.CommandText = "UPDATE checkpoints SET content=X'0304' WHERE id=$id";
            command.Parameters.AddWithValue("$id", info.Id); command.ExecuteNonQuery();
        }
        Assert.Equal(HttpStatusCode.InternalServerError, (await host.Http.GetAsync(ContentUrl(game, info), Cancellation)).StatusCode);
    }

    private sealed class InterruptedStream : MemoryStream
    {
        private bool first = true;
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            if (!first) throw new IOException("Simulated interrupted body");
            first = false; buffer.Span[0] = 42; return ValueTask.FromResult(1);
        }
    }

    private sealed class PausedStream : MemoryStream
    {
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Resume { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private bool first = true;
        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            if (!first) return 0;
            first = false; Started.TrySetResult();
            await Resume.Task.WaitAsync(cancellationToken);
            buffer.Span[0] = 42; return 1;
        }
    }
}
