using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using HeroesLobby.Contracts;
using Microsoft.Data.Sqlite;
using Xunit;

namespace HeroesLobby.Tests;

public class AccountTests
{
    private static CancellationToken Cancel => TestContext.Current.CancellationToken;
    private static HttpClient Signed(LobbyFactory app, string token)
    {
        var http = app.CreateClient();
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
        return http;
    }
    private static async Task<PlayerCredentials> Credentials(HttpResponseMessage response)
    {
        response.EnsureSuccessStatusCode();
        return (await response.Content.ReadFromJsonAsync<PlayerCredentials>(Cancel))!;
    }

    [Fact]
    public async Task An_account_signs_in_from_any_computer_and_signs_out_one_at_a_time()
    {
        await using var app = new LobbyFactory(); using var http = app.CreateClient();
        var registered = await Credentials(await http.PostAsJsonAsync("/api/account", new AccountRequest("Gem", "crystal dragon"), Cancel));
        Assert.Equal("Gem", registered.Player.DisplayName);
        using var home = Signed(app, registered.Token);
        var game = await (await home.PostAsJsonAsync("/api/games", new CreateGame("Campaign", "Map", 2, null), Cancel)).Content.ReadFromJsonAsync<GameInfo>(Cancel);

        // Another computer: the name is not case-sensitive, the player and their games are the same.
        var laptop = await Credentials(await http.PostAsJsonAsync("/api/account/sign-in", new AccountRequest(" gem ", "crystal dragon"), Cancel));
        Assert.Equal(registered.Player.Id, laptop.Player.Id); Assert.NotEqual(registered.Token, laptop.Token);
        using var away = Signed(app, laptop.Token);
        Assert.Equal(registered.Player.Id, (await away.GetFromJsonAsync<GameInfo>($"/api/games/{game!.Id}", Cancel))!.HostId);
        var account = (await away.GetFromJsonAsync<AccountInfo>("/api/account", Cancel))!;
        Assert.Equal("Gem", account.Name); Assert.Equal(registered.Player.Id, account.Player.Id);

        Assert.Equal(HttpStatusCode.NoContent, (await away.PostAsync("/api/account/sign-out", null, Cancel)).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await away.GetAsync("/api/account", Cancel)).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await home.GetAsync("/api/account", Cancel)).StatusCode);
    }

    [Fact]
    public async Task Wrong_passwords_taken_names_and_bad_names_are_refused()
    {
        await using var app = new LobbyFactory(); using var http = app.CreateClient();
        await Credentials(await http.PostAsJsonAsync("/api/account", new AccountRequest("Sandro", "necromancy"), Cancel));
        Assert.Equal(HttpStatusCode.Unauthorized, (await http.PostAsJsonAsync("/api/account/sign-in", new AccountRequest("Sandro", "necromancer"), Cancel)).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await http.PostAsJsonAsync("/api/account/sign-in", new AccountRequest("Nobody", "necromancy"), Cancel)).StatusCode);
        Assert.Equal(HttpStatusCode.Conflict, (await http.PostAsJsonAsync("/api/account", new AccountRequest("SANDRO", "another one"), Cancel)).StatusCode);
        foreach (var bad in new[] { "ab", "a name far too long here", "two  spaces", "semi;colon", "Ярослав" })
            Assert.Equal(HttpStatusCode.BadRequest, (await http.PostAsJsonAsync("/api/account", new AccountRequest(bad, "long enough"), Cancel)).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await http.PostAsJsonAsync("/api/account", new AccountRequest("Crag Hack", "short"), Cancel)).StatusCode);
    }

    [Fact]
    public async Task A_player_from_before_accounts_registers_and_keeps_their_games()
    {
        await using var app = new LobbyFactory(); using var http = app.CreateClient();
        var old = await Credentials(await http.PostAsJsonAsync("/api/players", new RegisterPlayer("Player"), Cancel));
        using var signed = Signed(app, old.Token);
        Assert.Null((await signed.GetFromJsonAsync<AccountInfo>("/api/account", Cancel))!.Name);
        var game = await (await signed.PostAsJsonAsync("/api/games", new CreateGame("Old game", "Map", 2, null), Cancel)).Content.ReadFromJsonAsync<GameInfo>(Cancel);

        var claimed = await Credentials(await signed.PostAsJsonAsync("/api/account", new AccountRequest("Tarnum", "barbarian king"), Cancel));
        Assert.Equal(old.Player.Id, claimed.Player.Id);
        Assert.Equal(HttpStatusCode.Conflict, (await signed.PostAsJsonAsync("/api/account", new AccountRequest("Tarnum2", "barbarian king"), Cancel)).StatusCode);
        var again = await Credentials(await http.PostAsJsonAsync("/api/account/sign-in", new AccountRequest("Tarnum", "barbarian king"), Cancel));
        using var later = Signed(app, again.Token);
        Assert.Equal(old.Player.Id, (await later.GetFromJsonAsync<GameInfo>($"/api/games/{game!.Id}", Cancel))!.HostId);
        Assert.Equal("Tarnum", (await later.GetFromJsonAsync<PlayerInfo>("/api/me", Cancel))!.DisplayName);
    }

    [Fact]
    public async Task A_version_one_database_keeps_its_players_and_tokens()
    {
        var database = Path.Combine(Path.GetTempPath(), "heroes-post-tests", Guid.NewGuid().ToString("N"), "lobby.db");
        Directory.CreateDirectory(Path.GetDirectoryName(database)!);
        const string token = "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";
        var hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(token)));
        using (var db = new SqliteConnection($"Data Source={database};Pooling=False"))
        {
            db.Open(); using var cmd = db.CreateCommand();
            cmd.CommandText = $"""
                CREATE TABLE players (id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT NOT NULL UNIQUE);
                CREATE TABLE games (id TEXT PRIMARY KEY, document TEXT NOT NULL);
                INSERT INTO players VALUES ('p1', 'Old player', '{hash}');
                PRAGMA user_version=1;
                """;
            cmd.ExecuteNonQuery();
        }
        await using var app = new LobbyFactory(database);
        using var signed = Signed(app, token);
        var account = (await signed.GetFromJsonAsync<AccountInfo>("/api/account", Cancel))!;
        Assert.Equal("p1", account.Player.Id); Assert.Equal("Old player", account.Player.DisplayName); Assert.Null(account.Name);
    }
}
