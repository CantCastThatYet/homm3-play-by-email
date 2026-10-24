using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text.Json;
using HeroesLobby.Contracts;
using Xunit;

namespace HeroesLobby.Tests;

public sealed class StartedGameFixture : IAsyncDisposable
{
    public LobbyFactory Server { get; }
    public HttpClient Host { get; }
    public HttpClient Peer { get; }
    public PlayerCredentials HostIdentity { get; private set; } = null!;
    public PlayerCredentials PeerIdentity { get; private set; } = null!;
    public GameInfo Game { get; private set; } = null!;
    public NativeLaunch Run { get; private set; } = null!;
    public string Prefix => $"api/games/{Game.Id}";
    public const string Build = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    public StartedGameFixture(string? database = null, bool kestrel = false)
    {
        Server = database is null ? new LobbyFactory() : new LobbyFactory(database);
        if (kestrel) Server.UseKestrel(0);
        Host = Server.CreateClient(); Peer = Server.CreateClient();
    }
    public async Task Initialize()
    {
        async Task<PlayerCredentials> Register(HttpClient http, string name)
        {
            using var reply = await http.PostAsJsonAsync("api/players", new RegisterPlayer(name), TestContext.Current.CancellationToken); reply.EnsureSuccessStatusCode();
            var who = (await reply.Content.ReadFromJsonAsync<PlayerCredentials>(TestContext.Current.CancellationToken))!;
            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", who.Token); return who;
        }
        HostIdentity = await Register(Host, "Red"); PeerIdentity = await Register(Peer, "Blue");
        using var create = await Host.PostAsJsonAsync("api/games", new CreateGame("Resume test", "Judgement Day", 2, null), TestContext.Current.CancellationToken);
        Game = (await create.Content.ReadFromJsonAsync<GameInfo>(TestContext.Current.CancellationToken))!;
        (await Peer.PostAsJsonAsync(Prefix + "/join", new JoinGame(null), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        foreach (var http in new[] { Host, Peer })
        {
            Game = (await Host.GetFromJsonAsync<GameInfo>(Prefix, TestContext.Current.CancellationToken))!;
            (await http.PutAsJsonAsync(Prefix + "/ready", new ReadyRequest(Game.Revision, true, true), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        }
        Game = (await Host.GetFromJsonAsync<GameInfo>(Prefix, TestContext.Current.CancellationToken))!;
        using var launch = await Host.PostAsJsonAsync(Prefix + "/native", new BeginNativeLaunch(Game.Revision, "127.0.0.1"), TestContext.Current.CancellationToken);
        Run = (await launch.Content.ReadFromJsonAsync<NativeLaunch>(TestContext.Current.CancellationToken))!;
        foreach (var (http, phase, id) in new[] { (Host,"Hosted",1u), (Peer,"Joined",22u), (Peer,"Armed",22u), (Host,"Started",1u), (Peer,"Started",22u) })
            (await http.PutAsJsonAsync(Prefix + "/native", new NativeLaunchReport(Run.Id, phase, id), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        Game = (await Host.GetFromJsonAsync<GameInfo>(Prefix, TestContext.Current.CancellationToken))!;
    }
    public async Task<ResumeLease> Acquire(HttpClient http, long revision, bool initialize = false)
    {
        using var result = await http.PostAsJsonAsync(Prefix + "/resume/acquire", new AcquireResumeLease(revision, initialize), TestContext.Current.CancellationToken);
        result.EnsureSuccessStatusCode(); return (await result.Content.ReadFromJsonAsync<ResumeLease>(TestContext.Current.CancellationToken))!;
    }
    public async Task<CheckpointInfo> Upload(HttpClient http, byte[] bytes)
    {
        var current = (await Host.GetFromJsonAsync<GameInfo>(Prefix, TestContext.Current.CancellationToken))!;
        using var request = new HttpRequestMessage(HttpMethod.Post, Prefix + $"/checkpoints?baseRevision={current.Revision}") { Content = new ByteArrayContent(bytes) };
        request.Headers.Add("Idempotency-Key", Guid.NewGuid().ToString("N"));
        using var result = await http.SendAsync(request, TestContext.Current.CancellationToken); result.EnsureSuccessStatusCode();
        return (await result.Content.ReadFromJsonAsync<CheckpointInfo>(TestContext.Current.CancellationToken))!;
    }
    public PublishNativeBranch Publication(ResumeLease lease, CheckpointInfo checkpoint, long revision = 0) =>
        new(revision, lease.Token, checkpoint.Id, Run.Id, Run.SessionName, Build);
    public async Task<NativeBranchInfo> Publish(HttpClient http, PublishNativeBranch request)
    {
        using var result = await http.PostAsJsonAsync(Prefix + "/branch", request, TestContext.Current.CancellationToken);
        Assert.True(result.IsSuccessStatusCode, await result.Content.ReadAsStringAsync(TestContext.Current.CancellationToken));
        return (await result.Content.ReadFromJsonAsync<NativeBranchInfo>(TestContext.Current.CancellationToken))!;
    }
    public static byte[] Capture(int color = 0, int day = 1, bool completed = false)
    {
        byte[] world = [1, 2, 3, (byte)day, (byte)color]; byte[] baseline = [4, 5, 6];
        string Hash(byte[] b) => Convert.ToHexString(SHA256.HashData(b)).ToLowerInvariant();
        object Stream(string role, string file, byte[] bytes) => new { role, file, status = "present", open = 0, length = bytes.Length, captured_bytes = bytes.Length, sha256 = Hash(bytes) };
        var manifest = new {
            schema = "hota-native-capture-v1", complete_restore = false, status = "captured", metadata_valid = true,
            local_player = color, current_player = color, world_date_valid = true, world_date_words = new[] { day, 1, 1 },
            native_save = new { observed = true, name_complete = true, return_byte = 1, name = "RMTGEXBS.BIN" },
            native_completion = new { observed = completed, marker_confirmed = completed, fresh_world_serialized = completed, player = color },
            participants = Enumerable.Range(0, 8).Select(i => new { player = i, ack = 0, done = i == color && completed ? 1 : 0, role = i == 1 ? 1 : 0, participates = i < 2 ? 1 : 0, predecessor = i == 1 ? 0 : -1, successor = i == 0 ? 1 : -1 }).ToArray(),
            raw_st_metadata = (string?)null, raw_st_metadata_sha256 = "",
            streams = new[] { Stream("local_snapshot", "world.bin", world), Stream("baseline_orig", "baseline.bin", baseline) },
            deferred = new { status = "empty", declared_count = 0, copied_count = 0, packets = Array.Empty<object>() }
        };
        using var output = new MemoryStream();
        using (var zip = new ZipArchive(output, ZipArchiveMode.Create, true))
        {
            using (var entry = zip.CreateEntry("manifest.json").Open()) entry.Write(JsonSerializer.SerializeToUtf8Bytes(manifest));
            using (var entry = zip.CreateEntry("world.bin").Open()) entry.Write(world);
            using (var entry = zip.CreateEntry("baseline.bin").Open()) entry.Write(baseline);
        }
        return output.ToArray();
    }
    public async ValueTask DisposeAsync() { Host.Dispose(); Peer.Dispose(); await Server.DisposeAsync(); }
}

public sealed class NativeBranchTests
{
    [Fact]
    public async Task Persistent_resume_needs_no_peer_presence_and_duplicate_leases_fail()
    {
        var database = Path.Combine(Path.GetTempPath(), "heroes-branch", Guid.NewGuid().ToString("N"), "lobby.db");
        NativeBranchInfo branch; PlayerCredentials owner; string prefix; ResumeLease lease;
        await using (var f = new StartedGameFixture(database))
        {
            await f.Initialize(); owner = f.HostIdentity; prefix = f.Prefix;
            lease = await f.Acquire(f.Host, 0, true);
            var checkpoint = await f.Upload(f.Host, StartedGameFixture.Capture());
            branch = await f.Publish(f.Host, f.Publication(lease, checkpoint));
            Assert.Equal("unverified", branch.RestoreCapability); Assert.Equal(1, branch.Revision);
            Assert.Equal(branch, await f.Publish(f.Host, f.Publication(lease, checkpoint))); // Lost response replay.
            Assert.Equal(HttpStatusCode.Conflict, (await f.Host.PostAsJsonAsync(prefix + "/resume/acquire", new AcquireResumeLease(1), TestContext.Current.CancellationToken)).StatusCode);
        }
        // Both clients and the whole server were closed. Only this member reconnects, without ready/presence writes.
        await using var restarted = new LobbyFactory(database); using var api = restarted.CreateClient();
        api.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", owner.Token);
        Assert.Equal(branch, await api.GetFromJsonAsync<NativeBranchInfo>(prefix + "/branch", TestContext.Current.CancellationToken));
        Assert.Equal(HttpStatusCode.Conflict, (await api.PostAsJsonAsync(prefix + "/resume/acquire", new AcquireResumeLease(1), TestContext.Current.CancellationToken)).StatusCode);
        (await api.PostAsJsonAsync(prefix + "/resume/release", new ResumeLeaseToken(lease.Token), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        var races = await Task.WhenAll(Enumerable.Range(0, 2).Select(_ => api.PostAsJsonAsync(prefix + "/resume/acquire", new AcquireResumeLease(1), TestContext.Current.CancellationToken)));
        Assert.Single(races, r => r.StatusCode == HttpStatusCode.Conflict);
        using var acquired = Assert.Single(races, r => r.IsSuccessStatusCode);
        var current = (await acquired.Content.ReadFromJsonAsync<ResumeLease>(TestContext.Current.CancellationToken))!;
        Assert.Equal(branch, current.Branch); Assert.NotEqual(lease.Token, current.Token);
        Assert.Equal(HttpStatusCode.Conflict, (await api.PostAsJsonAsync(prefix + "/resume/release", new ResumeLeaseToken(lease.Token), TestContext.Current.CancellationToken)).StatusCode);
        Assert.Equal(HttpStatusCode.Conflict, (await api.PostAsJsonAsync(prefix + "/resume/acquire", new AcquireResumeLease(0), TestContext.Current.CancellationToken)).StatusCode);
        (await api.PostAsJsonAsync(prefix + "/resume/renew", new ResumeLeaseToken(current.Token), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
    }

    [Fact]
    public async Task Publication_rejects_foreign_owner_color_session_opaque_and_stale_branches()
    {
        await using var f = new StartedGameFixture(); await f.Initialize();
        using var stranger = f.Server.CreateClient();
        using var registration = await stranger.PostAsJsonAsync("api/players", new RegisterPlayer("Outside"), TestContext.Current.CancellationToken);
        var outsider = (await registration.Content.ReadFromJsonAsync<PlayerCredentials>(TestContext.Current.CancellationToken))!;
        stranger.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", outsider.Token);
        Assert.Equal(HttpStatusCode.Forbidden, (await stranger.GetAsync(f.Prefix + "/branch", TestContext.Current.CancellationToken)).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, (await stranger.PostAsJsonAsync(f.Prefix + "/resume/acquire", new AcquireResumeLease(0, true), TestContext.Current.CancellationToken)).StatusCode);
        var lease = await f.Acquire(f.Host, 0, true);
        async Task<HttpStatusCode> PublishStatus(CheckpointInfo c, string? launch = null, long revision = 0) =>
            (await f.Host.PostAsJsonAsync(f.Prefix + "/branch", f.Publication(lease, c, revision) with { LaunchId = launch ?? f.Run.Id }, TestContext.Current.CancellationToken)).StatusCode;
        var peer = await f.Upload(f.Peer, StartedGameFixture.Capture(1)); Assert.Equal(HttpStatusCode.Forbidden, await PublishStatus(peer));
        var wrong = await f.Upload(f.Host, StartedGameFixture.Capture(1)); Assert.Equal(HttpStatusCode.BadRequest, await PublishStatus(wrong));
        var opaque = await f.Upload(f.Host, [1, 2, 3]); Assert.Equal(HttpStatusCode.BadRequest, await PublishStatus(opaque));
        var valid = await f.Upload(f.Host, StartedGameFixture.Capture()); Assert.Equal(HttpStatusCode.Conflict, await PublishStatus(valid, "old-session"));
        await f.Publish(f.Host, f.Publication(lease, valid));
        var newer = await f.Upload(f.Host, StartedGameFixture.Capture(day: 2, completed: true));
        Assert.Equal(HttpStatusCode.Conflict, await PublishStatus(newer));
        var second = await f.Publish(f.Host, f.Publication(lease, newer, 1)); Assert.Equal(2, second.Revision);
        var oldDate = await f.Upload(f.Host, StartedGameFixture.Capture(day: 1, completed: true));
        Assert.Equal(HttpStatusCode.Conflict, await PublishStatus(oldDate, revision: 2));
        Assert.Equal(HttpStatusCode.Conflict, (await f.Peer.GetAsync(f.Prefix + "/branch", TestContext.Current.CancellationToken)).StatusCode);
        Assert.Equal(second, await f.Host.GetFromJsonAsync<NativeBranchInfo>(f.Prefix + "/branch", TestContext.Current.CancellationToken));
    }
}
