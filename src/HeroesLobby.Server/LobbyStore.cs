using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using HeroesLobby.Contracts;
using Microsoft.Data.Sqlite;

namespace HeroesLobby.Server;

public sealed class LobbyError(int status, string message) : Exception(message)
{
    public int Status { get; } = status;
}

public sealed record StoredGame(GameInfo Game, string? PasswordHash, NativeLaunch? Native = null);

public sealed partial class LobbyStore
{
    private readonly string connectionString;
    // One server process owns a database. Serialize read/modify/write transactions, including joins.
    private readonly SemaphoreSlim writeGate = new(1, 1);

    public LobbyStore(IConfiguration config)
    {
        var path = Path.GetFullPath(config["Lobby:Database"] ?? "data/lobby.db");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        connectionString = new SqliteConnectionStringBuilder { DataSource = path, DefaultTimeout = 5 }.ToString();
        using var db = Open();
        using var command = db.CreateCommand();
        command.CommandText = """
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS games (id TEXT PRIMARY KEY, document TEXT NOT NULL);
            """;
        command.ExecuteNonQuery();
        InitializeAccounts(db);
        InitializeBranches(db);
    }

    private SqliteConnection Open() { var db = new SqliteConnection(connectionString); db.Open(); return db; }
    public static string Clean(string? value, string field, int max)
    {
        var text = value?.Trim() ?? "";
        if (text.Length == 0 || text.Length > max || text.Any(char.IsControl))
            throw new LobbyError(400, $"{field} must contain 1–{max} characters and no control characters.");
        return text;
    }

    public async Task<PlayerCredentials> Register(RegisterPlayer request)
    {
        var name = Clean(request.DisplayName, "Player name", 32);
        var id = Guid.NewGuid().ToString("N");
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction();
            using var command = db.CreateCommand(); command.Transaction = tx;
            command.CommandText = "INSERT INTO players (id, name) VALUES ($id, $name)";
            command.Parameters.AddWithValue("$id", id);
            command.Parameters.AddWithValue("$name", name);
            command.ExecuteNonQuery();
            var token = AddToken(db, tx, id);
            tx.Commit();
            return new(new(id, name), token);
        }
        finally { writeGate.Release(); }
    }

    public PlayerInfo Authenticate(string? authorization)
    {
        if (authorization is null || !authorization.StartsWith("Bearer ", StringComparison.Ordinal) || authorization.Length != 71)
            throw new LobbyError(401, "Reconnect to this server to establish your player identity.");
        using var db = Open();
        using var command = db.CreateCommand();
        command.CommandText = "SELECT p.id, p.name FROM tokens t JOIN players p ON p.id=t.player_id WHERE t.token_hash=$hash";
        command.Parameters.AddWithValue("$hash", TokenHash(authorization[7..]));
        using var reader = command.ExecuteReader();
        if (!reader.Read()) throw new LobbyError(401, "This player identity is no longer valid on this server.");
        return new(reader.GetString(0), reader.GetString(1));
    }

    public List<GameInfo> List()
    {
        using var db = Open();
        using var cmd = db.CreateCommand(); cmd.CommandText = "SELECT document FROM games";
        using var reader = cmd.ExecuteReader(); var result = new List<GameInfo>();
        while (reader.Read()) result.Add(JsonSerializer.Deserialize<StoredGame>(reader.GetString(0))!.Game);
        return result.OrderByDescending(g => g.CreatedAt).ToList();
    }

    public GameInfo Get(string id)
    {
        using var db = Open(); return Read(db, null, id).Game;
    }

    public async Task<GameInfo> Create(PlayerInfo player, CreateGame request)
    {
        var name = Clean(request.Name, "Game name", 64);
        var map = Clean(request.MapName, "Map label", 100);
        if (request.Capacity is < 2 or > 8) throw new LobbyError(400, "Choose between 2 and 8 players.");
        if (request.Password?.Length > 128) throw new LobbyError(400, "Game passwords must be at most 128 characters.");
        var secret = string.IsNullOrEmpty(request.Password) ? null : HashPassword(request.Password);
        var game = new GameInfo(Guid.NewGuid().ToString("N"), name, map, request.Capacity, player.Id,
            secret is not null, "Lobby", 1, DateTimeOffset.UtcNow, [NewMember(player, 0)]);
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction();
            Save(db, tx, new(game, secret)); tx.Commit();
        }
        finally { writeGate.Release(); }
        return game;
    }

    public Task<GameInfo> Join(string id, PlayerInfo player, JoinGame request) => Change(id, stored =>
    {
        var game = stored.Game;
        if (game.Members.Any(m => m.PlayerId == player.Id)) return game; // Reconnect never consumes another slot.
        RequireOpen(game);
        if (game.Members.Count >= game.Capacity) throw new LobbyError(409, "This game is full.");
        if (request.Password?.Length > 128 || (stored.PasswordHash is not null && !VerifyPassword(request.Password ?? "", stored.PasswordHash)))
            throw new LobbyError(403, "Incorrect game password.");
        var color = Enumerable.Range(0, 8).First(c => game.Members.All(m => m.Color != c));
        return Next(game, game.Members.Select(m => m with { Ready = false }).Append(NewMember(player, color)).ToList());
    });

    public Task<GameInfo> Settings(string id, PlayerInfo player, PlayerSettings request) => Change(id, stored =>
    {
        var game = stored.Game; RequireOpen(game); RequireRevision(game, request.Revision); RequireMember(game, player);
        if (request.Color is < 0 or > 7 || !Protocol.Factions.Contains(request.Faction) || !Protocol.Bonuses.Contains(request.Bonus))
            throw new LobbyError(400, "Choose a valid color, faction, and starting bonus.");
        if (game.Members.Any(m => m.PlayerId != player.Id && m.Color == request.Color))
            throw new LobbyError(409, "That color is already taken.");
        var hero = Clean(request.Hero, "Hero preference", 40);
        // Every participant must acknowledge changes to the visible starting conditions.
        return Next(game, game.Members.Select(m => m.PlayerId == player.Id
            ? m with { Color = request.Color, Faction = request.Faction, Hero = hero, Bonus = request.Bonus, Ready = false }
            : m with { Ready = false }).ToList());
    });

    public Task<GameInfo> Ready(string id, PlayerInfo player, ReadyRequest request) => Change(id, stored =>
    {
        var game = stored.Game; RequireOpen(game); RequireRevision(game, request.Revision); RequireMember(game, player);
        return Next(game, game.Members.Select(m => m.PlayerId == player.Id ? m with { Ready = request.Ready, NativeReady = request.Ready && request.NativeReady } : m).ToList());
    });

    public Task<GameInfo> Lock(string id, PlayerInfo player, RevisionRequest request, bool locked) => Change(id, stored =>
    {
        var game = stored.Game; RequireRevision(game, request.Revision);
        if (stored.Native is not null) throw new LobbyError(409, "This game has a native launch attempt. Its roster cannot be reopened.");
        if (game.HostId != player.Id) throw new LobbyError(403, "Only the host can lock or reopen this lobby.");
        if (locked && (game.Members.Count < 2 || game.Members.Any(m => !m.Ready)))
            throw new LobbyError(409, "At least two players must join, and everyone must be ready.");
        return Next(game, game.Members.Select(m => m with { Ready = locked && m.Ready }).ToList()) with { Status = locked ? "Prepared" : "Lobby" };
    });

    public Task<GameInfo> Leave(string id, PlayerInfo player, RevisionRequest request) => Change(id, stored =>
    {
        var game = stored.Game; RequireOpen(game); RequireRevision(game, request.Revision); RequireMember(game, player);
        var members = game.Members.Where(m => m.PlayerId != player.Id).Select(m => m with { Ready = false }).ToList();
        return Next(game, members) with { HostId = members.Any(m => m.PlayerId == game.HostId) ? game.HostId : members.FirstOrDefault()?.PlayerId ?? "" };
    });

    private async Task<GameInfo> Change(string id, Func<StoredGame, GameInfo> change)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(); var stored = Read(db, tx, id);
            var game = change(stored);
            if (game.Members.Count == 0)
            {
                using var cmd = db.CreateCommand(); cmd.Transaction = tx;
                cmd.CommandText = "DELETE FROM games WHERE id=$id"; cmd.Parameters.AddWithValue("$id", id); cmd.ExecuteNonQuery();
            }
            else Save(db, tx, stored with { Game = game });
            tx.Commit(); return game;
        }
        finally { writeGate.Release(); }
    }
    private static StoredGame Read(SqliteConnection db, SqliteTransaction? tx, string id)
    {
        using var cmd = db.CreateCommand(); cmd.Transaction = tx;
        cmd.CommandText = "SELECT document FROM games WHERE id=$id"; cmd.Parameters.AddWithValue("$id", id);
        return cmd.ExecuteScalar() is string json ? JsonSerializer.Deserialize<StoredGame>(json)! : throw new LobbyError(404, "This game no longer exists.");
    }
    private static void Save(SqliteConnection db, SqliteTransaction tx, StoredGame stored)
    {
        using var cmd = db.CreateCommand(); cmd.Transaction = tx;
        cmd.CommandText = "INSERT INTO games VALUES ($id,$doc) ON CONFLICT(id) DO UPDATE SET document=$doc";
        cmd.Parameters.AddWithValue("$id", stored.Game.Id); cmd.Parameters.AddWithValue("$doc", JsonSerializer.Serialize(stored)); cmd.ExecuteNonQuery();
    }
    private static MemberInfo NewMember(PlayerInfo player, int color) => new(player.Id, player.DisplayName, color, "Random", "Random", "Random", false);
    private static GameInfo Next(GameInfo game, List<MemberInfo> members) => game with { Revision = game.Revision + 1, Members = members };
    private static void RequireOpen(GameInfo game) { if (game.Status != "Lobby") throw new LobbyError(409, "The host must reopen this lobby before changing it."); }
    private static void RequireRevision(GameInfo game, long revision) { if (game.Revision != revision) throw new LobbyError(409, "The lobby changed. Review the latest state and try again."); }
    private static void RequireMember(GameInfo game, PlayerInfo player) { if (!game.Members.Any(m => m.PlayerId == player.Id)) throw new LobbyError(403, "Join this game first."); }
    private static string TokenHash(string value) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value)));
    private static string HashPassword(string password)
    {
        var salt = RandomNumberGenerator.GetBytes(16);
        return Convert.ToBase64String(salt) + ":" + Convert.ToBase64String(Rfc2898DeriveBytes.Pbkdf2(password, salt, 210_000, HashAlgorithmName.SHA256, 32));
    }
    private static bool VerifyPassword(string password, string hash)
    {
        var pieces = hash.Split(':');
        return CryptographicOperations.FixedTimeEquals(Convert.FromBase64String(pieces[1]),
            Rfc2898DeriveBytes.Pbkdf2(password, Convert.FromBase64String(pieces[0]), 210_000, HashAlgorithmName.SHA256, 32));
    }
}
