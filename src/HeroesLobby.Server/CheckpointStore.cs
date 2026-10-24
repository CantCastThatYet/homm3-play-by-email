using System.Buffers;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using HeroesLobby.Contracts;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.Data.Sqlite;

namespace HeroesLobby.Server;

/// <summary>Opaque native checkpoint storage. Never runs the engine or advances a game.</summary>
public sealed class CheckpointStore
{
    private readonly string connectionString;
    public long MaxBytes { get; }

    public CheckpointStore(IConfiguration configuration, LobbyStore lobby)
    {
        // LobbyStore creates the shared database first. Keeping content and metadata in SQLite
        // avoids a file/metadata dual-write: a crash exposes the entire checkpoint or none of it.
        _ = lobby;
        var path = Path.GetFullPath(configuration["Lobby:Database"] ?? "data/lobby.db");
        connectionString = new SqliteConnectionStringBuilder { DataSource = path, DefaultTimeout = 5 }.ToString();
        MaxBytes = configuration.GetValue<long?>("Checkpoints:MaxBytes") ?? 64 * 1024 * 1024;
        if (MaxBytes is < 1 or > 256 * 1024 * 1024)
            throw new InvalidOperationException("Checkpoints:MaxBytes must be between 1 and 268435456.");
        using var db = Open();
        using var command = db.CreateCommand();
        command.CommandText = """
            CREATE TABLE IF NOT EXISTS checkpoints (
                id TEXT PRIMARY KEY, game_id TEXT NOT NULL, owner_id TEXT NOT NULL,
                base_revision INTEGER NOT NULL, idempotency_key TEXT NOT NULL,
                sha256 TEXT NOT NULL, length INTEGER NOT NULL, created_at TEXT NOT NULL,
                content BLOB NOT NULL,
                UNIQUE(game_id, owner_id, idempotency_key),
                CHECK(length > 0 AND length = length(content))
            );
            CREATE INDEX IF NOT EXISTS checkpoints_game ON checkpoints(game_id, created_at);
            """;
        command.ExecuteNonQuery();
    }

    private SqliteConnection Open()
    {
        var db = new SqliteConnection(connectionString);
        db.Open();
        using var command = db.CreateCommand();
        command.CommandText = "PRAGMA synchronous=FULL";
        command.ExecuteNonQuery();
        return db;
    }

    public async Task<CheckpointInfo> Upload(string gameId, PlayerInfo player, long baseRevision,
        string idempotencyKey, Stream source, long? contentLength, CancellationToken cancellationToken)
    {
        idempotencyKey = LobbyStore.Clean(idempotencyKey, "Idempotency-Key", 128);
        if (baseRevision < 1) throw new LobbyError(400, "A positive baseRevision is required.");
        if (contentLength > MaxBytes) throw new LobbyError(413, "Checkpoint exceeds this server's upload limit.");
        // Reject nonmembers before buffering their upload; recheck inside the commit transaction.
        using (var db = Open()) RequireMember(db, null, gameId, player);
        using var buffer = new MemoryStream();
        var chunk = ArrayPool<byte>.Shared.Rent(81920);
        try
        {
            while (true)
            {
                var count = await source.ReadAsync(chunk.AsMemory(), cancellationToken);
                if (count == 0) break;
                if (buffer.Length + count > MaxBytes)
                    throw new LobbyError(413, "Checkpoint exceeds this server's upload limit.");
                await buffer.WriteAsync(chunk.AsMemory(0, count), cancellationToken);
            }
        }
        finally { ArrayPool<byte>.Shared.Return(chunk); }
        if (buffer.Length == 0) throw new LobbyError(400, "Checkpoint content must not be empty.");
        if (contentLength is not null && contentLength != buffer.Length)
            throw new LobbyError(400, "Checkpoint content length does not match the request.");
        var bytes = buffer.ToArray();
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        cancellationToken.ThrowIfCancellationRequested();
        using var connection = Open();
        // Immediate SQLite transaction serializes with lobby changes and other uploads, including
        // concurrent retries. Membership and revision cannot change between validation and commit.
        using var transaction = connection.BeginTransaction(deferred: false);
        var game = RequireMember(connection, transaction, gameId, player);
        using var existing = connection.CreateCommand();
        existing.Transaction = transaction;
        existing.CommandText = """
            SELECT id, game_id, owner_id, base_revision, sha256, length, created_at
            FROM checkpoints WHERE game_id=$game AND owner_id=$owner AND idempotency_key=$key
            """;
        existing.Parameters.AddWithValue("$game", gameId);
        existing.Parameters.AddWithValue("$owner", player.Id);
        existing.Parameters.AddWithValue("$key", idempotencyKey);
        using (var reader = existing.ExecuteReader())
        {
            if (reader.Read())
            {
                var receipt = ReadInfo(reader);
                if (receipt.BaseRevision != baseRevision || receipt.Sha256 != hash || receipt.Length != bytes.LongLength)
                    throw new LobbyError(409, "That Idempotency-Key already identifies a different checkpoint request.");
                return receipt;
            }
        }
        if (game.Revision != baseRevision)
            throw new LobbyError(409, "The game changed. Refresh its revision before uploading a new checkpoint.");
        var info = new CheckpointInfo(Guid.NewGuid().ToString("N"), gameId, player.Id, baseRevision,
            hash, bytes.LongLength, DateTimeOffset.UtcNow);
        using var insert = connection.CreateCommand();
        insert.Transaction = transaction;
        insert.CommandText = """
            INSERT INTO checkpoints(id, game_id, owner_id, base_revision, idempotency_key, sha256, length, created_at, content)
            VALUES($id, $game, $owner, $revision, $key, $hash, $length, $created, $content)
            """;
        insert.Parameters.AddWithValue("$id", info.Id);
        insert.Parameters.AddWithValue("$game", info.GameId);
        insert.Parameters.AddWithValue("$owner", info.OwnerPlayerId);
        insert.Parameters.AddWithValue("$revision", info.BaseRevision);
        insert.Parameters.AddWithValue("$key", idempotencyKey);
        insert.Parameters.AddWithValue("$hash", info.Sha256);
        insert.Parameters.AddWithValue("$length", info.Length);
        insert.Parameters.AddWithValue("$created", info.CreatedAt.ToString("O", CultureInfo.InvariantCulture));
        insert.Parameters.Add("$content", SqliteType.Blob).Value = bytes;
        insert.ExecuteNonQuery();
        cancellationToken.ThrowIfCancellationRequested();
        transaction.Commit();
        return info;
    }

    public List<CheckpointInfo> List(string gameId, PlayerInfo player)
    {
        using var db = Open();
        using var transaction = db.BeginTransaction(deferred: true);
        RequireMember(db, transaction, gameId, player);
        using var command = db.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            SELECT id, game_id, owner_id, base_revision, sha256, length, created_at
            FROM checkpoints WHERE game_id=$game ORDER BY created_at, id
            """;
        command.Parameters.AddWithValue("$game", gameId);
        using var reader = command.ExecuteReader();
        var result = new List<CheckpointInfo>();
        while (reader.Read()) result.Add(ReadInfo(reader));
        return result;
    }

    public (CheckpointInfo Info, byte[] Content) Download(string gameId, string checkpointId, PlayerInfo player)
    {
        using var db = Open();
        using var transaction = db.BeginTransaction(deferred: true);
        RequireMember(db, transaction, gameId, player);
        using var command = db.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            SELECT id, game_id, owner_id, base_revision, sha256, length, created_at, content
            FROM checkpoints WHERE game_id=$game AND id=$id
            """;
        command.Parameters.AddWithValue("$game", gameId);
        command.Parameters.AddWithValue("$id", checkpointId);
        using var reader = command.ExecuteReader();
        if (!reader.Read()) throw new LobbyError(404, "This checkpoint does not exist in this game.");
        var info = ReadInfo(reader);
        var bytes = (byte[])reader[7];
        if (bytes.LongLength != info.Length || Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant() != info.Sha256)
            throw new LobbyError(500, "Stored checkpoint failed its integrity check.");
        return (info, bytes);
    }

    private static CheckpointInfo ReadInfo(SqliteDataReader reader) => new(reader.GetString(0), reader.GetString(1),
        reader.GetString(2), reader.GetInt64(3), reader.GetString(4), reader.GetInt64(5),
        DateTimeOffset.Parse(reader.GetString(6), CultureInfo.InvariantCulture));

    private static GameInfo RequireMember(SqliteConnection db, SqliteTransaction? transaction, string gameId, PlayerInfo player)
    {
        using var command = db.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = "SELECT document FROM games WHERE id=$id";
        command.Parameters.AddWithValue("$id", gameId);
        if (command.ExecuteScalar() is not string json) throw new LobbyError(404, "This game no longer exists.");
        var game = JsonSerializer.Deserialize<StoredGame>(json)!.Game;
        if (!game.Members.Any(m => m.PlayerId == player.Id)) throw new LobbyError(403, "Join this game first.");
        return game;
    }
}

public static class CheckpointEndpoints
{
    public static void MapCheckpointEndpoints(this WebApplication app)
    {
        app.MapPost("/api/games/{id}/checkpoints", async (string id, long baseRevision, HttpContext context,
            LobbyStore lobby, CheckpointStore checkpoints) =>
        {
            var player = lobby.Authenticate(context.Request.Headers.Authorization.ToString());
            var limit = context.Features.Get<IHttpMaxRequestBodySizeFeature>();
            if (limit is { IsReadOnly: false }) limit.MaxRequestBodySize = checkpoints.MaxBytes;
            return await checkpoints.Upload(id, player, baseRevision, context.Request.Headers["Idempotency-Key"].ToString(),
                context.Request.Body, context.Request.ContentLength, context.RequestAborted);
        }).RequireRateLimiting("writes");
        app.MapGet("/api/games/{id}/checkpoints", (string id, HttpContext context, LobbyStore lobby, CheckpointStore checkpoints) =>
            checkpoints.List(id, lobby.Authenticate(context.Request.Headers.Authorization.ToString())));
        app.MapGet("/api/games/{id}/checkpoints/{checkpointId}/content", (string id, string checkpointId,
            HttpContext context, LobbyStore lobby, CheckpointStore checkpoints) =>
        {
            var item = checkpoints.Download(id, checkpointId, lobby.Authenticate(context.Request.Headers.Authorization.ToString()));
            context.Response.Headers.CacheControl = "no-store";
            context.Response.Headers.ETag = $"\"{item.Info.Sha256}\"";
            return Results.File(item.Content, "application/octet-stream", $"{item.Info.Id}.bin");
        });
    }
}
