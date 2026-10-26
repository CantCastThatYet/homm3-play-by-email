using System.Security.Cryptography;
using HeroesLobby.Contracts;
using Microsoft.Data.Sqlite;

namespace HeroesLobby.Server;

// Accounts: a name and a password sign a player in from any HotA, and each sign-in gets its own
// token, so signing out on one computer leaves the others signed in. Players made before accounts
// (POST /api/players, still used by the desktop app) have no name or password until they register
// with their token, which keeps their id and with it their games.
public sealed partial class LobbyStore
{
    private static void InitializeAccounts(SqliteConnection db)
    {
        using var version = db.CreateCommand(); version.CommandText = "PRAGMA user_version";
        if ((long)version.ExecuteScalar()! >= 2) return;
        using var exists = db.CreateCommand(); exists.CommandText = "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='players'";
        var upgrade = (long)exists.ExecuteScalar()! > 0;
        using var tx = db.BeginTransaction();
        using var command = db.CreateCommand(); command.Transaction = tx;
        // Version 1 kept one token per player in the players table.
        command.CommandText = """
            CREATE TABLE players_v2 (id TEXT PRIMARY KEY, name TEXT NOT NULL, account TEXT UNIQUE COLLATE NOCASE, password_hash TEXT);
            CREATE TABLE tokens (token_hash TEXT PRIMARY KEY, player_id TEXT NOT NULL, created_at TEXT NOT NULL);
            CREATE INDEX tokens_player ON tokens(player_id);
            """ + (upgrade ? """
            INSERT INTO players_v2 (id, name) SELECT id, name FROM players;
            INSERT INTO tokens SELECT token_hash, id, strftime('%Y-%m-%dT%H:%M:%SZ', 'now') FROM players;
            DROP TABLE players;
            """ : "") + """
            ALTER TABLE players_v2 RENAME TO players;
            PRAGMA user_version=2;
            """;
        command.ExecuteNonQuery();
        tx.Commit();
    }

    // Account names are what players type to sign in and what HotA shows: plain ASCII, so any code page shows them.
    private static string AccountName(string? value)
    {
        var name = value?.Trim() ?? "";
        if (name.Length is < 3 or > 20 || name.Contains("  ") || !name.All(c => char.IsAsciiLetterOrDigit(c) || c is ' ' or '-' or '_' or '.'))
            throw new LobbyError(400, "An account name is 3 to 20 Latin letters, digits, spaces, dots, dashes or underscores.");
        return name;
    }
    private static string AccountPassword(string? value)
    {
        if (value is null || value.Length is < 6 or > 128) throw new LobbyError(400, "A password is 6 to 128 characters.");
        return value;
    }

    public async Task<PlayerCredentials> RegisterAccount(AccountRequest request, string? authorization)
    {
        var name = AccountName(request.Name);
        var hash = HashPassword(AccountPassword(request.Password));
        // A token here is a player made before accounts, who keeps their id and so their games.
        var existing = string.IsNullOrEmpty(authorization) ? null : Authenticate(authorization);
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction();
            using (var taken = db.CreateCommand())
            {
                taken.Transaction = tx;
                taken.CommandText = "SELECT id, account FROM players WHERE account=$name OR id=$id";
                taken.Parameters.AddWithValue("$name", name); taken.Parameters.AddWithValue("$id", existing?.Id ?? "");
                using var reader = taken.ExecuteReader();
                while (reader.Read())
                {
                    if (reader.GetString(0) == existing?.Id && !reader.IsDBNull(1)) throw new LobbyError(409, "You already have an account. Sign out to register another.");
                    if (!reader.IsDBNull(1) && string.Equals(reader.GetString(1), name, StringComparison.OrdinalIgnoreCase)) throw new LobbyError(409, $"The name {name} is taken.");
                }
            }
            var id = existing?.Id ?? Guid.NewGuid().ToString("N");
            using (var save = db.CreateCommand())
            {
                save.Transaction = tx;
                save.CommandText = existing is null
                    ? "INSERT INTO players (id, name, account, password_hash) VALUES ($id, $name, $name, $hash)"
                    : "UPDATE players SET name=$name, account=$name, password_hash=$hash WHERE id=$id";
                save.Parameters.AddWithValue("$id", id); save.Parameters.AddWithValue("$name", name); save.Parameters.AddWithValue("$hash", hash);
                save.ExecuteNonQuery();
            }
            var token = AddToken(db, tx, id);
            tx.Commit();
            return new(new(id, name), token);
        }
        finally { writeGate.Release(); }
    }

    public async Task<PlayerCredentials> SignIn(AccountRequest request)
    {
        var name = request.Name?.Trim() ?? "";
        string? id = null, shown = null, hash = null;
        using (var db = Open())
        using (var find = db.CreateCommand())
        {
            find.CommandText = "SELECT id, name, password_hash FROM players WHERE account=$name AND password_hash IS NOT NULL";
            find.Parameters.AddWithValue("$name", name);
            using var reader = find.ExecuteReader();
            if (reader.Read()) { id = reader.GetString(0); shown = reader.GetString(1); hash = reader.GetString(2); }
        }
        if (hash is null || request.Password is null || request.Password.Length > 128 || !VerifyPassword(request.Password, hash))
            throw new LobbyError(401, "Wrong account name or password.");
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction();
            var token = AddToken(db, tx, id!);
            tx.Commit();
            return new(new(id!, shown!), token);
        }
        finally { writeGate.Release(); }
    }

    // Ends this sign-in only. An unknown token is already signed out.
    public async Task SignOut(string? authorization)
    {
        if (authorization is null || !authorization.StartsWith("Bearer ", StringComparison.Ordinal)) return;
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var command = db.CreateCommand();
            command.CommandText = "DELETE FROM tokens WHERE token_hash=$hash";
            command.Parameters.AddWithValue("$hash", TokenHash(authorization[7..]));
            command.ExecuteNonQuery();
        }
        finally { writeGate.Release(); }
    }

    public AccountInfo Account(string? authorization)
    {
        var player = Authenticate(authorization);
        using var db = Open(); using var command = db.CreateCommand();
        command.CommandText = "SELECT account FROM players WHERE id=$id";
        command.Parameters.AddWithValue("$id", player.Id);
        return new(player, command.ExecuteScalar() as string);
    }

    private static string AddToken(SqliteConnection db, SqliteTransaction tx, string playerId)
    {
        var token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
        using var command = db.CreateCommand(); command.Transaction = tx;
        command.CommandText = "INSERT INTO tokens VALUES ($hash, $player, $at)";
        command.Parameters.AddWithValue("$hash", TokenHash(token)); command.Parameters.AddWithValue("$player", playerId);
        command.Parameters.AddWithValue("$at", DateTimeOffset.UtcNow.ToString("O"));
        command.ExecuteNonQuery();
        return token;
    }
}

public static class AccountEndpoints
{
    public static void MapAccountEndpoints(this WebApplication app)
    {
        app.MapPost("/api/account", (AccountRequest request, HttpContext context, LobbyStore store) =>
            store.RegisterAccount(request, context.Request.Headers.Authorization.ToString())).RequireRateLimiting("sign-in");
        app.MapPost("/api/account/sign-in", (AccountRequest request, LobbyStore store) => store.SignIn(request)).RequireRateLimiting("sign-in");
        app.MapPost("/api/account/sign-out", async (HttpContext context, LobbyStore store) =>
        {
            await store.SignOut(context.Request.Headers.Authorization.ToString());
            return Results.NoContent();
        });
        app.MapGet("/api/account", (HttpContext context, LobbyStore store) => store.Account(context.Request.Headers.Authorization.ToString()));
    }
}
