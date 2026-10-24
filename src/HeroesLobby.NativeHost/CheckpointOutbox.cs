using System.Globalization;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using HeroesLobby.Contracts;

namespace HeroesLobby.NativeHost;

public sealed record PendingCheckpoint(int Version, string Key, string Server, string GameId,
    string PlayerId, long BaseRevision, string Sha256, long Length);

[JsonSerializable(typeof(PendingCheckpoint))]
[JsonSerializable(typeof(string[]))]
internal partial class OutboxJsonContext : JsonSerializerContext;

/// <summary>
/// Durable immutable upload requests. A server storage receipt is not a completed native turn.
/// Credentials are supplied by the currently authenticated caller, never persisted here.
/// </summary>
public sealed class CheckpointOutbox
{
    public string Root { get; }
    public CheckpointOutbox(string root)
    {
        Root = Path.GetFullPath(root);
        Directory.CreateDirectory(Root); CaptureBundle.CheckRegularPath(Root);
    }

    public PendingCheckpoint EnqueueCapture(string manifest, Uri server, string gameId, string playerId, long baseRevision)
    {
        var address = NormalizeServer(server);
        if (baseRevision < 1 || !ValidId(gameId) || !ValidId(playerId)) throw new ArgumentException("A game, player and positive revision are required.");
        // Prepared bytes become visible only when request.json is atomically published.
        // An interrupted preparation leaves an unreferenced staging directory, never a request.
        var staging = Path.Combine(Root, ".preparing-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        var payload = Path.Combine(staging, "checkpoint.zip");
        CaptureBundle.Write(manifest, payload);
        using var file = File.OpenRead(payload);
        var hash = Convert.ToHexString(SHA256.HashData(file)).ToLowerInvariant();
        var keyMaterial = JsonSerializer.Serialize(new[] { address, gameId, playerId, baseRevision.ToString(CultureInfo.InvariantCulture), hash }, OutboxJsonContext.Default.StringArray);
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(keyMaterial))).ToLowerInvariant();
        var pending = new PendingCheckpoint(1, key, address, gameId, playerId, baseRevision, hash, file.Length);
        file.Dispose();
        var directory = Path.Combine(Root, key);
        try { Directory.Move(staging, directory); }
        catch (IOException) when (Directory.Exists(directory))
        {
            // Another caller (or an earlier attempt) published the same immutable payload.
            // Keep this isolated staging directory for inspection; never delete a computed tree.
            CaptureBundle.CheckRegularPath(directory);
            VerifyPayload(directory, pending);
        }
        var requestPath = Path.Combine(directory, "request.json");
        if (File.Exists(requestPath))
        {
            if (Read(requestPath) != pending) throw new InvalidDataException("Outbox key collision or changed request.");
        }
        else Publish(requestPath, JsonSerializer.SerializeToUtf8Bytes(pending, OutboxJsonContext.Default.PendingCheckpoint));
        return pending;
    }

    public IReadOnlyList<PendingCheckpoint> Pending(Uri server, string playerId)
    {
        var address = NormalizeServer(server);
        var pending = new List<PendingCheckpoint>();
        foreach (var directory in Directory.EnumerateDirectories(Root).Order(StringComparer.Ordinal))
        {
            var key = Path.GetFileName(directory);
            if (key.Length != 64 || !key.All(char.IsAsciiHexDigit)) continue;
            CaptureBundle.CheckRegularPath(directory);
            var path = Path.Combine(directory, "request.json");
            if (!File.Exists(path)) continue;
            var item = Read(path);
            if (item.Key != key) throw new InvalidDataException("Outbox directory does not match its request.");
            if (item.Server != address || item.PlayerId != playerId) continue;
            var receiptPath = Path.Combine(directory, "receipt.json");
            if (File.Exists(receiptPath))
            {
                CaptureBundle.CheckRegularPath(receiptPath);
                using var receiptFile = File.OpenRead(receiptPath);
                if (receiptFile.Length > 16384) throw new InvalidDataException("Stored receipt is too large.");
                var receipt = JsonSerializer.Deserialize(receiptFile, LobbyJsonContext.Default.CheckpointInfo);
                ValidateReceipt(item, receipt);
            }
            else pending.Add(item);
        }
        return pending;
    }

    // The injected client must disable automatic redirects (as the desktop client
    // and CLI do), so a server cannot redirect native payloads to another origin.
    public async Task<CheckpointInfo> Send(PendingCheckpoint item, HttpClient http, Uri authenticatedServer,
        string authenticatedPlayerId, string token, CancellationToken cancellationToken)
    {
        if (NormalizeServer(authenticatedServer) != item.Server || authenticatedPlayerId != item.PlayerId)
            throw new InvalidOperationException("This capture belongs to a different server or player.");
        if (string.IsNullOrWhiteSpace(token)) throw new ArgumentException("An authenticated player token is required.");
        Validate(item);
        var directory = Path.Combine(Root, item.Key);
        if (Read(Path.Combine(directory, "request.json")) != item) throw new InvalidDataException("The queued request changed.");
        // Hold the payload open without write sharing through hashing and HTTP serialization.
        using var payload = OpenVerifiedPayload(directory, item);
        using var request = new HttpRequestMessage(HttpMethod.Post,
            new Uri(new Uri(item.Server), $"api/games/{Uri.EscapeDataString(item.GameId)}/checkpoints?baseRevision={item.BaseRevision.ToString(CultureInfo.InvariantCulture)}"));
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        request.Headers.Add("Idempotency-Key", item.Key);
        request.Content = new StreamContent(payload);
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength > 16384) throw new InvalidDataException("Server receipt is too large.");
        await response.Content.LoadIntoBufferAsync(16384, cancellationToken);
        var receipt = await response.Content.ReadFromJsonAsync(LobbyJsonContext.Default.CheckpointInfo, cancellationToken);
        ValidateReceipt(item, receipt);
        Publish(Path.Combine(directory, "receipt.json"), JsonSerializer.SerializeToUtf8Bytes(receipt!, LobbyJsonContext.Default.CheckpointInfo));
        return receipt!;
    }

    private static void ValidateReceipt(PendingCheckpoint item, CheckpointInfo? receipt)
    {
        if (receipt is null || !ValidId(receipt.Id) || receipt.GameId != item.GameId || receipt.OwnerPlayerId != item.PlayerId ||
            receipt.BaseRevision != item.BaseRevision || receipt.Length != item.Length || receipt.Sha256 != item.Sha256)
            throw new InvalidDataException("Server receipt does not identify the exact queued bytes and owner.");
    }
    private static void VerifyPayload(string directory, PendingCheckpoint item) { using var file = OpenVerifiedPayload(directory, item); }
    private static FileStream OpenVerifiedPayload(string directory, PendingCheckpoint item)
    {
        var path = Path.Combine(directory, "checkpoint.zip"); CaptureBundle.CheckRegularPath(path);
        var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            if (file.Length != item.Length || Convert.ToHexString(SHA256.HashData(file)).ToLowerInvariant() != item.Sha256)
                throw new InvalidDataException("Queued checkpoint bytes changed; upload was refused.");
            file.Position = 0; return file;
        }
        catch { file.Dispose(); throw; }
    }
    private static PendingCheckpoint Read(string path)
    {
        CaptureBundle.CheckRegularPath(path);
        using var file = File.OpenRead(path);
        if (file.Length > 16384) throw new InvalidDataException("Outbox request is too large.");
        var item = JsonSerializer.Deserialize(file, OutboxJsonContext.Default.PendingCheckpoint) ?? throw new InvalidDataException("Empty outbox request.");
        Validate(item); return item;
    }
    private static void Validate(PendingCheckpoint item)
    {
        if (item.Version != 1 || item.Key.Length != 64 || !item.Key.All(char.IsAsciiHexDigit) ||
            item.Sha256.Length != 64 || !item.Sha256.All(char.IsAsciiHexDigit) || item.Length is < 1 or > CaptureBundle.MaxBytes ||
            item.BaseRevision < 1 || !ValidId(item.GameId) || !ValidId(item.PlayerId) || NormalizeServer(new Uri(item.Server)) != item.Server)
            throw new InvalidDataException("Invalid persisted checkpoint request.");
    }
    private static bool ValidId(string id) => !string.IsNullOrEmpty(id) && id.Length <= 128 && id.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_');
    public static string NormalizeServer(Uri server)
    {
        if (!server.IsAbsoluteUri || server.Scheme is not ("http" or "https") || !string.IsNullOrEmpty(server.UserInfo) ||
            !string.IsNullOrEmpty(server.Query) || !string.IsNullOrEmpty(server.Fragment)) throw new ArgumentException("Invalid server URL.");
        return server.AbsoluteUri.TrimEnd('/') + "/";
    }
    private static void Publish(string path, byte[] bytes)
    {
        var temporary = path + ".pending-" + Guid.NewGuid().ToString("N");
        using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { file.Write(bytes); file.Flush(flushToDisk: true); }
        try { File.Move(temporary, path); }
        catch (IOException) when (File.Exists(path))
        {
            CaptureBundle.CheckRegularPath(path);
            if (!File.ReadAllBytes(path).AsSpan().SequenceEqual(bytes)) throw new InvalidDataException("Conflicting outbox publication.");
            File.Delete(temporary);
        }
    }
}
