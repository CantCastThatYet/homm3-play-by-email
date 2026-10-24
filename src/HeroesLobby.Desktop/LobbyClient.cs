using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using System.Security.Cryptography;
using HeroesLobby.Contracts;
using Microsoft.AspNetCore.SignalR.Client;
using Microsoft.Extensions.DependencyInjection;

namespace HeroesLobby.Desktop;

public sealed record SavedIdentity(string PlayerId, string DisplayName, string Token);
public sealed class LocalProfile
{
    public string NativeGameRoot { get; set; } = "";
    public string HostAddress { get; set; } = "127.0.0.1";
    [JsonIgnore] public string StorageDirectory { get; set; } = DirectoryPath;
    public string ServerAddress { get; set; } = "http://localhost:5080";
    public string DisplayName { get; set; } = "";
    public Dictionary<string, SavedIdentity> Identities { get; set; } = [];
    public static string DirectoryPath => Environment.GetEnvironmentVariable("HEROES_POST_PROFILE") ??
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HeroesPost");
    public static string FilePath => Path.Combine(DirectoryPath, "profile.json");
    public static LocalProfile Load()
    {
        if (!File.Exists(FilePath)) return new();
        // Do not silently replace a corrupt profile: it contains the keys to reserved player slots.
        return JsonSerializer.Deserialize(File.ReadAllText(FilePath), ProfileJsonContext.Default.LocalProfile) ?? throw new InvalidDataException("The local profile is empty.");
    }
    public void Save()
    {
        Directory.CreateDirectory(StorageDirectory);
        if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(StorageDirectory, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var filePath = Path.Combine(StorageDirectory, "profile.json");
        Directory.CreateDirectory(StorageDirectory);
        var temp = filePath + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(this, ProfileJsonContext.Default.LocalProfile));
        if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(temp, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        File.Move(temp, filePath, true);
    }
}

[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(LocalProfile))]
internal partial class ProfileJsonContext : JsonSerializerContext;

public sealed class LobbyClient : IAsyncDisposable
{
    private readonly HttpClient http;
    private HubConnection? hub;
    public Uri Address { get; }
    public PlayerInfo? Player { get; private set; }
    public bool NativeStartupAvailable { get; private set; }
    public bool Live => hub?.State == HubConnectionState.Connected;
    public event Action? Changed;
    public LobbyClient(string address)
    {
        if (!Uri.TryCreate(address.Trim().TrimEnd('/') + "/", UriKind.Absolute, out var uri) ||
            uri.Scheme is not ("http" or "https") || !string.IsNullOrEmpty(uri.UserInfo) || !string.IsNullOrEmpty(uri.Query) || !string.IsNullOrEmpty(uri.Fragment))
            throw new InvalidOperationException("Enter an http:// or https:// server address without credentials, query, or fragment.");
        Address = uri;
        http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { BaseAddress = uri, Timeout = TimeSpan.FromSeconds(12) };
    }
    public async Task<ServerInfo> Connect(LocalProfile profile, string name)
    {
        var info = await Send<ServerInfo>(HttpMethod.Get, "api/info");
        NativeStartupAvailable = info.NativeStartupAvailable;
        if (info.ProtocolVersion != Protocol.Version) throw new InvalidOperationException("This server uses an incompatible lobby protocol. Update the app or connect to another server.");
        if (profile.Identities.TryGetValue(Address.AbsoluteUri, out var identity))
        {
            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", identity.Token);
            Player = await Send<PlayerInfo>(HttpMethod.Get, "api/me");
        }
        else
        {
            var credentials = await Send<PlayerCredentials>(HttpMethod.Post, "api/players", new RegisterPlayer(name));
            Player = credentials.Player;
            profile.Identities[Address.AbsoluteUri] = new(Player.Id, Player.DisplayName, credentials.Token);
            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", credentials.Token);
        }
        profile.ServerAddress = Address.AbsoluteUri.TrimEnd('/'); profile.DisplayName = Player.DisplayName; profile.Save();
        hub = new HubConnectionBuilder().WithUrl(new Uri(Address, "hubs/lobby")).WithAutomaticReconnect()
            .AddJsonProtocol(options => options.PayloadSerializerOptions.TypeInfoResolver = LobbyJsonContext.Default).Build();
        hub.On<string>("Changed", _ => Changed?.Invoke());
        hub.Reconnected += _ => { Changed?.Invoke(); return Task.CompletedTask; };
        try { using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(4)); await hub.StartAsync(timeout.Token); }
        catch (Exception ex) when (ex is HttpRequestException or OperationCanceledException or IOException) { /* HTTP polling remains available. */ }
        return info;
    }
    public Task<List<GameInfo>> Games() => Send<List<GameInfo>>(HttpMethod.Get, "api/games");
    public Task<GameInfo> Create(CreateGame request) => Send<GameInfo>(HttpMethod.Post, "api/games", request);
    public Task<GameInfo> Join(string id, string? password) => Send<GameInfo>(HttpMethod.Post, $"api/games/{id}/join", new JoinGame(password));
    public Task<GameInfo> Settings(string id, PlayerSettings request) => Send<GameInfo>(HttpMethod.Put, $"api/games/{id}/settings", request);
    public Task<GameInfo> Ready(string id, ReadyRequest request) => Send<GameInfo>(HttpMethod.Put, $"api/games/{id}/ready", request);
    public Task<GameInfo> Action(string id, string action, long revision) => Send<GameInfo>(HttpMethod.Post, $"api/games/{id}/{action}", new RevisionRequest(revision));
    public Task<NativeLaunch> BeginNative(string id, BeginNativeLaunch request) => Send<NativeLaunch>(HttpMethod.Post, $"api/games/{id}/native", request);
    public Task<NativeLaunch> Native(string id) => Send<NativeLaunch>(HttpMethod.Get, $"api/games/{id}/native");
    public Task<NativeLaunch> ReportNative(string id, NativeLaunchReport request) => Send<NativeLaunch>(HttpMethod.Put, $"api/games/{id}/native", request);
    public Task<NativeBranchInfo> Branch(string id) => Send<NativeBranchInfo>(HttpMethod.Get, $"api/games/{id}/branch");
    public Task<ResumeLease> AcquireResume(string id, long revision, bool initialize = false) => Send<ResumeLease>(HttpMethod.Post, $"api/games/{id}/resume/acquire", new AcquireResumeLease(revision, initialize));
    public Task<ResumeLease> RenewResume(string id, string token) => Send<ResumeLease>(HttpMethod.Post, $"api/games/{id}/resume/renew", new ResumeLeaseToken(token));
    public Task<ReleaseResumeLease> ReleaseResume(string id, string token) => Send<ReleaseResumeLease>(HttpMethod.Post, $"api/games/{id}/resume/release", new ResumeLeaseToken(token));
    public Task<NativeBranchInfo> PublishBranch(string id, PublishNativeBranch request) => Send<NativeBranchInfo>(HttpMethod.Post, $"api/games/{id}/branch", request);
    public async Task<CheckpointInfo> UploadCapture(string id, string archivePath, string key, CancellationToken cancellation)
    {
        var game = await Send<GameInfo>(HttpMethod.Get, $"api/games/{id}");
        using var file = File.OpenRead(archivePath);
        if (file.Length is < 1 or > 64 * 1024 * 1024) throw new InvalidDataException("Native capture exceeds the upload limit.");
        using var request = new HttpRequestMessage(HttpMethod.Post, $"api/games/{id}/checkpoints?baseRevision={game.Revision}") { Content = new StreamContent(file) };
        request.Headers.Add("Idempotency-Key", key);
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        using var response = await http.SendAsync(request, cancellation);
        await EnsureSuccess(response);
        return (await response.Content.ReadFromJsonAsync(LobbyJsonContext.Default.CheckpointInfo, cancellation))!;
    }
    public async Task DownloadBranch(NativeBranchInfo branch, string destination, CancellationToken cancellation)
    {
        using var response = await http.GetAsync($"api/games/{branch.GameId}/checkpoints/{branch.CheckpointId}/content", HttpCompletionOption.ResponseHeadersRead, cancellation);
        await EnsureSuccess(response);
        if (branch.Length is < 1 or > 64 * 1024 * 1024) throw new InvalidDataException("Branch archive exceeds the local limit.");
        using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        await using var source = await response.Content.ReadAsStreamAsync(cancellation);
        var buffer = new byte[81920]; long total = 0; int count;
        while ((count = await source.ReadAsync(buffer, cancellation)) != 0)
        {
            total += count; if (total > branch.Length) throw new InvalidDataException("Server sent more data than the branch descriptor permits.");
            hash.AppendData(buffer, 0, count); await output.WriteAsync(buffer.AsMemory(0, count), cancellation);
        }
        if (total != branch.Length || !Convert.ToHexString(hash.GetHashAndReset()).Equals(branch.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Downloaded branch does not match its authoritative content hash.");
        output.Flush(true);
    }
    private static async Task EnsureSuccess(HttpResponseMessage response)
    {
        if (response.IsSuccessStatusCode) return;
        var message = $"Server returned {(int)response.StatusCode}.";
        try { using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync()); if (doc.RootElement.TryGetProperty("detail", out var detail)) message = detail.GetString() ?? message; }
        catch (JsonException) { }
        throw new InvalidOperationException(message);
    }
    private async Task<T> Send<T>(HttpMethod method, string path, object? body = null)
    {
        using var request = new HttpRequestMessage(method, path);
        if (body is not null) request.Content = JsonContent.Create(body, LobbyJsonContext.Default.GetTypeInfo(body.GetType())!);
        using var response = await http.SendAsync(request);
        if (!response.IsSuccessStatusCode)
        {
            var message = response.StatusCode == System.Net.HttpStatusCode.TooManyRequests ? "Too many requests. Wait a minute and try again." : $"Server returned {(int)response.StatusCode}.";
            try
            {
                using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                if (doc.RootElement.TryGetProperty("detail", out var detail)) message = detail.GetString() ?? message;
            }
            catch (JsonException) { }
            throw new InvalidOperationException(message);
        }
        return await response.Content.ReadFromJsonAsync((JsonTypeInfo<T>)LobbyJsonContext.Default.GetTypeInfo(typeof(T))!) ?? throw new InvalidOperationException("The server returned an empty response.");
    }
    public async ValueTask DisposeAsync() { if (hub is not null) await hub.DisposeAsync(); http.Dispose(); }
}
