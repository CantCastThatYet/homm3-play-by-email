namespace HeroesLobby.Contracts;

public static class Protocol
{
    public const int Version = 1;
    public static readonly string[] Colors = ["Red", "Blue", "Tan", "Green", "Orange", "Purple", "Teal", "Pink"];
    public static readonly string[] Factions = ["Random", "Castle", "Rampart", "Tower", "Inferno", "Necropolis", "Dungeon", "Stronghold", "Fortress", "Conflux", "Cove", "Factory"];
    public static readonly string[] Bonuses = ["Random", "Artifact", "Gold", "Resource"];
}

public record ServerInfo(string Name, int ProtocolVersion, bool GameIntegrationAvailable, bool NativeStartupAvailable = false);
public record PlayerInfo(string Id, string DisplayName);
public record RegisterPlayer(string DisplayName);
public record PlayerCredentials(PlayerInfo Player, string Token);
/// <summary>Signs in to an account, or registers one (POST /api/sessions, POST /api/accounts).</summary>
public record AccountRequest(string Name, string Password);
/// <summary>Who a token belongs to (GET /api/account). Name signs in to the account; null for a player made before
/// accounts, who has no password yet and keeps their games by registering with that token.</summary>
public record AccountInfo(PlayerInfo Player, string? Name);
public record CreateGame(string Name, string MapName, int Capacity, string? Password);
public record JoinGame(string? Password);
public record PlayerSettings(long Revision, int Color, string Faction, string Hero, string Bonus);
public record ReadyRequest(long Revision, bool Ready, bool NativeReady = false);
public record RevisionRequest(long Revision);
public record MemberInfo(string PlayerId, string DisplayName, int Color, string Faction, string Hero, string Bonus, bool Ready, bool NativeReady = false);
public record GameInfo(string Id, string Name, string MapName, int Capacity, string HostId,
    bool PasswordProtected, string Status, long Revision, DateTimeOffset CreatedAt, List<MemberInfo> Members);

/// <summary>The game relay and its stand-ins (GET /api/relay).</summary>
public sealed record RelayServerInfo(int Port, bool StandIns, int DefaultTurnSeconds);
/// <summary>A relay game in which the player has a seat. State: Lobby, Playing, Away or StandIn. Seats: every player in it.</summary>
public sealed record RelayGameInfo(Guid Id, string Name, bool Started, string Seat, uint SeatId, string State,
    int TurnSeconds, int LiveClients, string? CaptureKind, string? CaptureDate, string[] Players, List<RelaySeatInfo>? Seats = null);
/// <summary>One player of a relay game. State: Online (their HotA is connected), Away (seat kept, since Since) or StandIn (the server plays it).</summary>
public sealed record RelaySeatInfo(string Name, string State, DateTimeOffset? Since);
/// <summary>A seat handed back to its player: resume the latest capture and rejoin as this seat.</summary>
public sealed record RelayClaim(Guid Id, string Session, string Seat, int Color, string Kind, string MapFile);
public sealed record RelayTurnLimit(int TurnSeconds);
/// <summary>A relay game anyone on the server can see. Name and Host are HotA's session name halves (Latin-1 bytes).</summary>
public sealed record RelayOpenGame(Guid Id, string Name, string Host, bool Started, int Players, int MaxPlayers, bool Password);
public sealed record RelayChatLine(long Id, string Name, string Text, DateTimeOffset At);
/// <summary>The in-game lobby's state for one player, pushed over the relay connection whenever it changes. Chat lines travel separately.</summary>
public sealed record RelayLobby(string Player, int DefaultTurnSeconds, List<RelayGameInfo> Mine, List<RelayOpenGame> Games);
