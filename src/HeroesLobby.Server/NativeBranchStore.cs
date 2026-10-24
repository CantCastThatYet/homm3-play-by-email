using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using HeroesLobby.Contracts;
using Microsoft.Data.Sqlite;

namespace HeroesLobby.Server;

public sealed partial class LobbyStore
{
    private static readonly TimeSpan LeaseDuration = TimeSpan.FromMinutes(2);

    private static void InitializeBranches(SqliteConnection db)
    {
        using var command = db.CreateCommand();
        command.CommandText = """
            CREATE TABLE IF NOT EXISTS native_branches (
                game_id TEXT NOT NULL, owner_id TEXT NOT NULL, document TEXT,
                lease_hash TEXT, lease_expires TEXT, PRIMARY KEY(game_id,owner_id));
            """;
        command.ExecuteNonQuery();
    }

    private static (NativeLaunch Run, MemberInfo Member) RequireStarted(StoredGame stored, PlayerInfo player, bool initializing = false)
    {
        RequireMember(stored.Game, player);
        var run = stored.Native;
        if (run is null || run.Players.SingleOrDefault(p => p.PlayerId == player.Id)?.Phase != "Started" ||
            (!initializing && stored.Game.Status is not ("Playing" or "Started")) || run.Status == "Failed")
            throw new LobbyError(409, "This game has no completed native startup for your seat. An old lobby record cannot be resumed.");
        return (run, stored.Game.Members.Single(m => m.PlayerId == player.Id));
    }

    private sealed record BranchRow(NativeBranchInfo? Branch, string? LeaseHash, DateTimeOffset? Expires);
    private static BranchRow ReadBranch(SqliteConnection db, SqliteTransaction? tx, string game, string owner)
    {
        using var cmd = db.CreateCommand(); cmd.Transaction = tx;
        cmd.CommandText = "SELECT document,lease_hash,lease_expires FROM native_branches WHERE game_id=$g AND owner_id=$o";
        cmd.Parameters.AddWithValue("$g", game); cmd.Parameters.AddWithValue("$o", owner);
        using var reader = cmd.ExecuteReader();
        if (!reader.Read()) return new(null, null, null);
        return new(reader.IsDBNull(0) ? null : JsonSerializer.Deserialize<NativeBranchInfo>(reader.GetString(0)),
            reader.IsDBNull(1) ? null : reader.GetString(1),
            reader.IsDBNull(2) ? null : DateTimeOffset.Parse(reader.GetString(2), CultureInfo.InvariantCulture));
    }
    private static void SaveBranch(SqliteConnection db, SqliteTransaction tx, string game, string owner, BranchRow row)
    {
        using var cmd = db.CreateCommand(); cmd.Transaction = tx;
        cmd.CommandText = """
            INSERT INTO native_branches VALUES($g,$o,$d,$h,$e)
            ON CONFLICT(game_id,owner_id) DO UPDATE SET document=$d,lease_hash=$h,lease_expires=$e
            """;
        cmd.Parameters.AddWithValue("$g", game); cmd.Parameters.AddWithValue("$o", owner);
        cmd.Parameters.AddWithValue("$d", row.Branch is null ? DBNull.Value : JsonSerializer.Serialize(row.Branch));
        cmd.Parameters.AddWithValue("$h", (object?)row.LeaseHash ?? DBNull.Value);
        cmd.Parameters.AddWithValue("$e", row.Expires is null ? DBNull.Value : row.Expires.Value.ToString("O", CultureInfo.InvariantCulture));
        cmd.ExecuteNonQuery();
    }
    private static void RequireLease(BranchRow row, string token)
    {
        if (token is null || token.Length != 64 || !token.All(char.IsAsciiHexDigit) || row.Expires <= DateTimeOffset.UtcNow ||
            row.Expires is null || row.LeaseHash is null || !CryptographicOperations.FixedTimeEquals(
                Convert.FromHexString(row.LeaseHash), Convert.FromHexString(TokenHash(token))))
            throw new LobbyError(409, "The branch lease expired or belongs to another client. Acquire it again before writing.");
    }

    public NativeBranchInfo GetBranch(string id, PlayerInfo player)
    {
        using var db = Open(); using var tx = db.BeginTransaction(deferred: true);
        RequireStarted(Read(db, tx, id), player);
        return ReadBranch(db, tx, id, player.Id).Branch ?? throw new LobbyError(409,
            "No validated native branch has been uploaded for your seat. An opaque checkpoint alone cannot be resumed.");
    }

    public async Task<ResumeLease> AcquireBranch(string id, PlayerInfo player, AcquireResumeLease request)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(deferred: false);
            RequireStarted(Read(db, tx, id), player, request.Initialize);
            var row = ReadBranch(db, tx, id, player.Id);
            if (request.ExpectedRevision < 0 || request.ExpectedRevision != (row.Branch?.Revision ?? 0))
                throw new LobbyError(409, "Your branch changed. Fetch its latest descriptor before resuming.");
            if (!request.Initialize && row.Branch is null) throw new LobbyError(409, "No validated native branch exists for your seat.");
            if (request.Initialize && row.Branch is not null) throw new LobbyError(409, "Your initial branch already exists. Use Resume.");
            if (row.LeaseHash is not null && row.Expires > DateTimeOffset.UtcNow)
                throw new LobbyError(409, "This seat is already open in another client. Close it or wait for its lease to expire.");
            var token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
            var expires = DateTimeOffset.UtcNow + LeaseDuration;
            SaveBranch(db, tx, id, player.Id, row with { LeaseHash = TokenHash(token), Expires = expires }); tx.Commit();
            return new(token, expires, row.Branch);
        }
        finally { writeGate.Release(); }
    }

    public async Task<ResumeLease> RenewBranch(string id, PlayerInfo player, ResumeLeaseToken request)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(deferred: false);
            RequireStarted(Read(db, tx, id), player, true);
            var row = ReadBranch(db, tx, id, player.Id); RequireLease(row, request.Token);
            var expires = DateTimeOffset.UtcNow + LeaseDuration;
            SaveBranch(db, tx, id, player.Id, row with { Expires = expires }); tx.Commit();
            return new(request.Token, expires, row.Branch);
        }
        finally { writeGate.Release(); }
    }

    public async Task<ReleaseResumeLease> ReleaseBranch(string id, PlayerInfo player, ResumeLeaseToken request)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(deferred: false);
            RequireMember(Read(db, tx, id).Game, player);
            var row = ReadBranch(db, tx, id, player.Id);
            if (row.LeaseHash is null) return new(false);
            // Release can follow expiry, but must never revoke a newer client's lease.
            if (request.Token is null || request.Token.Length != 64 || TokenHash(request.Token) != row.LeaseHash)
                throw new LobbyError(409, "This lease belongs to another client.");
            SaveBranch(db, tx, id, player.Id, row with { LeaseHash = null, Expires = null }); tx.Commit();
            return new(true);
        }
        finally { writeGate.Release(); }
    }

    public async Task<NativeBranchInfo> PublishBranch(string id, PlayerInfo player, PublishNativeBranch request)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(deferred: false);
            var (run, member) = RequireStarted(Read(db, tx, id), player, true);
            if (request.LaunchId != run.Id || request.SessionName != run.SessionName)
                throw new LobbyError(409, "This capture belongs to a different native session.");
            if (request.AdapterBuild is null || request.AdapterBuild.Length != 64 || !request.AdapterBuild.All(char.IsAsciiHexDigit))
                throw new LobbyError(400, "Provide the native adapter SHA256 build identity.");
            var row = ReadBranch(db, tx, id, player.Id); RequireLease(row, request.LeaseToken);
            if (row.Branch?.CheckpointId == request.CheckpointId && row.Branch.Revision == request.ExpectedRevision + 1)
                return row.Branch; // Lost publication response; no second revision.
            if (request.ExpectedRevision != (row.Branch?.Revision ?? 0))
                throw new LobbyError(409, "A newer branch already exists. Stale captures cannot replace it.");
            using var cmd = db.CreateCommand(); cmd.Transaction = tx;
            cmd.CommandText = "SELECT sha256,length,content,created_at FROM checkpoints WHERE id=$c AND game_id=$g AND owner_id=$o";
            cmd.Parameters.AddWithValue("$c", request.CheckpointId); cmd.Parameters.AddWithValue("$g", id); cmd.Parameters.AddWithValue("$o", player.Id);
            byte[] bytes; string hash; long length; DateTimeOffset capturedAt;
            using (var reader = cmd.ExecuteReader())
            {
                if (!reader.Read()) throw new LobbyError(403, "Upload a checkpoint owned by your seat in this game first.");
                hash = reader.GetString(0); length = reader.GetInt64(1); bytes = (byte[])reader[2];
                capturedAt = DateTimeOffset.Parse(reader.GetString(3), CultureInfo.InvariantCulture);
            }
            if (row.Branch is not null && capturedAt < row.Branch.CreatedAt)
                throw new LobbyError(409, "This checkpoint predates your latest published branch. Upload a new capture.");
            if (bytes.LongLength != length || !Convert.ToHexString(SHA256.HashData(bytes)).Equals(hash, StringComparison.OrdinalIgnoreCase))
                throw new LobbyError(500, "Stored checkpoint failed its integrity check.");
            NativeCaptureData capture;
            try { using var stream = new MemoryStream(bytes, false); capture = NativeCaptureArchive.Read(stream, member.Color); }
            catch (Exception ex) when (ex is InvalidDataException or JsonException or KeyNotFoundException or InvalidOperationException or ArgumentException)
            { throw new LobbyError(400, "Checkpoint is not a supported native branch capture: " + ex.Message); }
            for (var color = 0; color < 8; color++)
                if ((capture.Participants[color * 6 + 3] != 0) != run.Players.Any(p => p.Color == color))
                    throw new LobbyError(400, "The capture's participating colors do not match the frozen native roster.");
            var absolute = (capture.Month - 1) * 28L + (capture.Week - 1) * 7 + capture.Day;
            if (row.Branch is { } prior && (absolute < (prior.Month - 1) * 28L + (prior.Week - 1) * 7 + prior.Day ||
                (absolute == (prior.Month - 1) * 28L + (prior.Week - 1) * 7 + prior.Day && prior.CaptureKind == "completion" && capture.Kind != "completion")))
                throw new LobbyError(409, "This capture predates your authoritative branch.");
            var branch = new NativeBranchInfo(id, player.Id, run.Id, run.SessionName, member.Color,
                request.ExpectedRevision + 1, request.CheckpointId, hash, length, "hota-native-capture-v1",
                capture.Kind, capture.Day, capture.Week, capture.Month, request.AdapterBuild.ToLowerInvariant(), "unverified", DateTimeOffset.UtcNow);
            SaveBranch(db, tx, id, player.Id, row with { Branch = branch }); tx.Commit(); return branch;
        }
        finally { writeGate.Release(); }
    }
}

public static class NativeBranchEndpoints
{
    public static void MapNativeBranchEndpoints(this WebApplication app)
    {
        static PlayerInfo Who(HttpContext c, LobbyStore s) => s.Authenticate(c.Request.Headers.Authorization.ToString());
        app.MapGet("/api/games/{id}/branch", (string id, HttpContext c, LobbyStore s) => s.GetBranch(id, Who(c, s)));
        app.MapPost("/api/games/{id}/branch", (string id, PublishNativeBranch r, HttpContext c, LobbyStore s) => s.PublishBranch(id, Who(c, s), r)).RequireRateLimiting("writes");
        app.MapPost("/api/games/{id}/resume/acquire", (string id, AcquireResumeLease r, HttpContext c, LobbyStore s) => s.AcquireBranch(id, Who(c, s), r)).RequireRateLimiting("writes");
        app.MapPost("/api/games/{id}/resume/renew", (string id, ResumeLeaseToken r, HttpContext c, LobbyStore s) => s.RenewBranch(id, Who(c, s), r)).RequireRateLimiting("writes");
        app.MapPost("/api/games/{id}/resume/release", (string id, ResumeLeaseToken r, HttpContext c, LobbyStore s) => s.ReleaseBranch(id, Who(c, s), r)).RequireRateLimiting("writes");
    }
}
