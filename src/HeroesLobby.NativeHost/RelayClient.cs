using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;
using HeroesLobby.Contracts;

namespace HeroesLobby.NativeHost;

/// <summary>Where an owned HotA copy's transport connects, and as whom. The ticket is the player's lobby token.</summary>
public sealed record RelayEndpoint(string Host, int Port, string Ticket, string Name)
{
    public const string ConfigFile = "HeroesPost.relay.ini";
    // Relay strings are bytes on the wire; HotA's session names contain non-ASCII separators.
    public static readonly System.Text.Encoding Encoding = System.Text.Encoding.Latin1;

    /// <summary>A normal client: HotA's own multiplayer screens host and join games on the relay.</summary>
    public void WritePlay(string root) => Write(root, []);

    /// <summary>A resumed client that takes back its held seat in a running game.</summary>
    public void WriteRejoin(string root, string session, string seat) => Write(root, [$"rejoin={session}", $"seat={seat}"]);

    private void Write(string root, string[] extra)
    {
        foreach (var value in new[] { Host, Ticket, Name }.Concat(extra))
            if (value.Any(c => c is '\r' or '\n')) throw new ArgumentException("Relay settings cannot contain line breaks.");
        File.Delete(Path.Combine(root, "HeroesPost.deferred.bin"));
        File.WriteAllLines(Path.Combine(root, ConfigFile), [$"host={Host}", $"port={Port}", $"name={Name}", $"ticket={Ticket}", .. extra], Encoding);
    }
}

/// <summary>The relay part of the lobby server's HTTP API, authenticated with the player's token.</summary>
public sealed class RelayApi(HttpClient http, string token)
{
    private HttpRequestMessage Message(HttpMethod method, string path, HttpContent? content = null)
    {
        var message = new HttpRequestMessage(method, path) { Content = content };
        message.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        return message;
    }

    private static async Task<T> Read<T>(HttpResponseMessage response, JsonTypeInfo<T> type, CancellationToken cancellation)
    {
        await Check(response, cancellation);
        return await response.Content.ReadFromJsonAsync(type, cancellation) ?? throw new InvalidOperationException("The server returned an empty response.");
    }

    private static async Task Check(HttpResponseMessage response, CancellationToken cancellation)
    {
        if (response.IsSuccessStatusCode) return;
        var detail = "";
        try { using var problem = JsonDocument.Parse(await response.Content.ReadAsStringAsync(cancellation)); detail = problem.RootElement.GetProperty("detail").GetString() ?? ""; }
        catch (Exception) { }
        throw new InvalidOperationException(detail.Length != 0 ? detail : $"The server answered {(int)response.StatusCode} {response.ReasonPhrase}.");
    }

    public async Task<RelayServerInfo> Info(CancellationToken cancellation)
    {
        using var response = await http.SendAsync(Message(HttpMethod.Get, "/api/relay"), cancellation);
        return await Read(response, LobbyJsonContext.Default.RelayServerInfo, cancellation);
    }

    public async Task<List<RelayGameInfo>> Games(CancellationToken cancellation)
    {
        using var response = await http.SendAsync(Message(HttpMethod.Get, "/api/relay/games"), cancellation);
        return await Read(response, LobbyJsonContext.Default.ListRelayGameInfo, cancellation);
    }

    public async Task UploadCapture(Guid game, string manifest, CancellationToken cancellation)
    {
        var bundle = Path.Combine(Path.GetTempPath(), $"heroespost-{Guid.NewGuid():N}.hpcap");
        try
        {
            CaptureBundle.Write(manifest, bundle);
            using var content = new ByteArrayContent(await File.ReadAllBytesAsync(bundle, cancellation));
            content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
            using var response = await http.SendAsync(Message(HttpMethod.Post, $"/api/relay/games/{game}/captures", content), cancellation);
            await Check(response, cancellation);
        }
        finally { File.Delete(bundle); }
    }

    public async Task<RelayClaim> Claim(Guid game, CancellationToken cancellation)
    {
        using var response = await http.SendAsync(Message(HttpMethod.Post, $"/api/relay/games/{game}/claim"), cancellation);
        return await Read(response, LobbyJsonContext.Default.RelayClaim, cancellation);
    }

    public async Task DownloadCapture(Guid game, string destination, CancellationToken cancellation)
    {
        using var response = await http.SendAsync(Message(HttpMethod.Get, $"/api/relay/games/{game}/capture"), HttpCompletionOption.ResponseHeadersRead, cancellation);
        await Check(response, cancellation);
        await using var file = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None);
        await response.Content.CopyToAsync(file, cancellation);
    }

    public async Task SetTurnLimit(Guid game, int seconds, CancellationToken cancellation)
    {
        using var response = await http.SendAsync(Message(HttpMethod.Put, $"/api/relay/games/{game}/turn-limit", JsonContent.Create(new RelayTurnLimit(seconds), LobbyJsonContext.Default.RelayTurnLimit)), cancellation);
        await Check(response, cancellation);
    }
}
