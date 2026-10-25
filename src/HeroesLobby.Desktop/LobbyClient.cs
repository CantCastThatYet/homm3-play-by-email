using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using HeroesLobby.Contracts;

namespace HeroesLobby.Desktop;

public sealed record SavedIdentity(string PlayerId, string DisplayName, string Token);
public sealed class LocalProfile
{
    public string NativeGameRoot { get; set; } = "";
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

/// <summary>The player's identity on one lobby server. Relay games use <see cref="HeroesLobby.NativeHost.RelayApi"/>.</summary>
public sealed class LobbyClient : IAsyncDisposable
{
    private readonly HttpClient http;
    public Uri Address { get; }
    public PlayerInfo? Player { get; private set; }
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
        return info;
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
    public ValueTask DisposeAsync() { http.Dispose(); return ValueTask.CompletedTask; }
}
