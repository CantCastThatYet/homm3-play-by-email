using System.IO.Compression;
using System.Net;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text.Json;
using HeroesLobby.Contracts;
using HeroesLobby.NativeHost;
using Xunit;

namespace HeroesLobby.Tests;

public class CheckpointOutboxTests
{
    private static CancellationToken Cancellation => TestContext.Current.CancellationToken;
    private static string Temporary() { var path = Path.Combine(Path.GetTempPath(), "heroes-post-outbox-tests", Guid.NewGuid().ToString("N")); Directory.CreateDirectory(path); return path; }
    private static string Capture(string root)
    {
        var bytes = new byte[] { 7, 8, 9, 10 };
        File.WriteAllBytes(Path.Combine(root, "stream-0.bin"), bytes);
        var manifest = new
        {
            schema = "hota-native-capture-v1", complete_restore = false,
            raw_st_metadata = (string?)null, raw_st_metadata_sha256 = "",
            streams = new[] { new { file = "stream-0.bin", sha256 = Convert.ToHexString(SHA256.HashData(bytes)), captured_bytes = bytes.Length } },
            deferred = new { packets = Array.Empty<object>() }
        };
        var path = Path.Combine(root, "manifest.json"); File.WriteAllText(path, JsonSerializer.Serialize(manifest)); return path;
    }
    private static async Task<(HttpClient Http, PlayerCredentials Identity, GameInfo Game)> Session(CheckpointFactory app)
    {
        var http = app.CreateClient(new() { AllowAutoRedirect = false });
        using var registration = await http.PostAsJsonAsync("/api/players", new RegisterPlayer("Outbox"), Cancellation);
        registration.EnsureSuccessStatusCode();
        var identity = (await registration.Content.ReadFromJsonAsync<PlayerCredentials>(Cancellation))!;
        http.DefaultRequestHeaders.Authorization = new("Bearer", identity.Token);
        using var creation = await http.PostAsJsonAsync("/api/games", new CreateGame("Outbox", "Small", 2, null), Cancellation);
        creation.EnsureSuccessStatusCode();
        return (http, identity, (await creation.Content.ReadFromJsonAsync<GameInfo>(Cancellation))!);
    }

    [Fact]
    public async Task Lost_receipt_retries_same_bytes_and_key_after_client_restart()
    {
        await using var app = new CheckpointFactory(); var session = await Session(app);
        using var http = session.Http;
        var directory = Temporary(); var outbox = new CheckpointOutbox(Path.Combine(directory, "outbox"));
        var item = outbox.EnqueueCapture(Capture(directory), http.BaseAddress!, session.Game.Id, session.Identity.Player.Id, session.Game.Revision);
        Assert.Equal(item, outbox.EnqueueCapture(Path.Combine(directory, "manifest.json"), http.BaseAddress!, session.Game.Id, session.Identity.Player.Id, session.Game.Revision));
        // Server commits, but the first response is lost before the client can persist a receipt.
        using var unreliable = new HttpClient(new LoseFirstResponse(app.Server.CreateHandler()));
        await Assert.ThrowsAsync<IOException>(() => outbox.Send(item, unreliable, http.BaseAddress!, session.Identity.Player.Id, session.Identity.Token, Cancellation));
        var restarted = new CheckpointOutbox(outbox.Root);
        Assert.Equal(item, Assert.Single(restarted.Pending(http.BaseAddress!, session.Identity.Player.Id)));
        var receipt = await restarted.Send(item, http, http.BaseAddress!, session.Identity.Player.Id, session.Identity.Token, Cancellation);
        Assert.Empty(new CheckpointOutbox(outbox.Root).Pending(http.BaseAddress!, session.Identity.Player.Id));
        var list = await http.GetFromJsonAsync<List<CheckpointInfo>>($"/api/games/{session.Game.Id}/checkpoints", Cancellation);
        Assert.Equal(receipt, Assert.Single(list!));
        var received = await http.GetByteArrayAsync($"/api/games/{session.Game.Id}/checkpoints/{receipt.Id}/content", Cancellation);
        Assert.Equal(File.ReadAllBytes(Path.Combine(outbox.Root, item.Key, "checkpoint.zip")), received);
        using var archive = new ZipArchive(new MemoryStream(received));
        Assert.Equal(2, archive.Entries.Count);
        using var manifest = JsonDocument.Parse(archive.GetEntry("manifest.json")!.Open());
        Assert.False(manifest.RootElement.GetProperty("complete_restore").GetBoolean());
        var after = (await http.GetFromJsonAsync<GameInfo>($"/api/games/{session.Game.Id}", Cancellation))!;
        Assert.Equal(session.Game.Revision, after.Revision); Assert.Equal(session.Game.Status, after.Status);
        Assert.Equal(session.Game.Members, after.Members);
    }

    [Fact]
    public async Task Changed_payload_and_wrong_identity_are_rejected_before_http()
    {
        var directory = Temporary(); var outbox = new CheckpointOutbox(Path.Combine(directory, "outbox"));
        var address = new Uri("http://localhost:5081/");
        var item = outbox.EnqueueCapture(Capture(directory), address, "game", "player", 1);
        using var http = new HttpClient(new NeverSend());
        await Assert.ThrowsAsync<InvalidOperationException>(() => outbox.Send(item, http, address, "other", "token", Cancellation));
        await Assert.ThrowsAsync<InvalidOperationException>(() => outbox.Send(item, http, new Uri("http://localhost:5080/"), "player", "token", Cancellation));
        Assert.Empty(outbox.Pending(address, "other"));
        File.WriteAllBytes(Path.Combine(outbox.Root, item.Key, "checkpoint.zip"), [1, 2, 3]);
        await Assert.ThrowsAsync<InvalidDataException>(() => outbox.Send(item, http, address, "player", "token", Cancellation));
        Assert.Single(outbox.Pending(address, "player"));
    }

    [Theory]
    [InlineData("../outside.bin")]
    [InlineData("C:\\outside.bin")]
    [InlineData("stream-0.bin")]
    public void Invalid_or_changed_capture_is_not_published(string reference)
    {
        var directory = Temporary(); var path = Capture(directory);
        var text = File.ReadAllText(path).Replace("stream-0.bin", reference.Replace("\\", "\\\\"), StringComparison.Ordinal);
        File.WriteAllText(path, text);
        if (reference == "stream-0.bin") File.WriteAllBytes(Path.Combine(directory, reference), [10, 9, 8, 7]);
        var outbox = new CheckpointOutbox(Path.Combine(directory, "outbox"));
        Assert.Throws<InvalidDataException>(() => outbox.EnqueueCapture(path, new Uri("http://localhost/"), "game", "player", 1));
        Assert.Empty(outbox.Pending(new Uri("http://localhost/"), "player"));
    }

    [Fact]
    public async Task Rejected_upload_retains_original_revision_and_remains_pending()
    {
        await using var app = new CheckpointFactory(); var session = await Session(app);
        using var http = session.Http;
        var directory = Temporary(); var outbox = new CheckpointOutbox(Path.Combine(directory, "outbox"));
        var item = outbox.EnqueueCapture(Capture(directory), http.BaseAddress!, session.Game.Id, session.Identity.Player.Id, session.Game.Revision);
        using var changed = await http.PutAsJsonAsync($"/api/games/{session.Game.Id}/ready", new ReadyRequest(session.Game.Revision, true), Cancellation);
        changed.EnsureSuccessStatusCode();
        var error = await Assert.ThrowsAsync<HttpRequestException>(() => outbox.Send(item, http, http.BaseAddress!, session.Identity.Player.Id, session.Identity.Token, Cancellation));
        Assert.Equal(HttpStatusCode.Conflict, error.StatusCode);
        Assert.Equal(item, Assert.Single(new CheckpointOutbox(outbox.Root).Pending(http.BaseAddress!, session.Identity.Player.Id)));
    }

    private sealed class LoseFirstResponse(HttpMessageHandler inner) : DelegatingHandler(inner)
    {
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var response = await base.SendAsync(request, cancellationToken);
            response.EnsureSuccessStatusCode(); response.Dispose();
            throw new IOException("Simulated connection loss after server commit.");
        }
    }
    private sealed class NeverSend : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            throw new InvalidOperationException("No HTTP request was expected.");
    }
}
