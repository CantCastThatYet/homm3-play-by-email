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
public record CreateGame(string Name, string MapName, int Capacity, string? Password);
public record JoinGame(string? Password);
public record PlayerSettings(long Revision, int Color, string Faction, string Hero, string Bonus);
public record ReadyRequest(long Revision, bool Ready, bool NativeReady = false);
public record RevisionRequest(long Revision);
public record MemberInfo(string PlayerId, string DisplayName, int Color, string Faction, string Hero, string Bonus, bool Ready, bool NativeReady = false);
public record GameInfo(string Id, string Name, string MapName, int Capacity, string HostId,
    bool PasswordProtected, string Status, long Revision, DateTimeOffset CreatedAt, List<MemberInfo> Members);
