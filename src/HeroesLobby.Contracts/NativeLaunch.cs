namespace HeroesLobby.Contracts;

public record BeginNativeLaunch(long Revision, string HostAddress);
public record NativeLaunchReport(string AttemptId, string Phase, uint NativePlayerId, string? Error = null);
public record NativeLaunchPlayer(string PlayerId, int Color, string Phase, uint NativePlayerId, string? Error);
public record NativeLaunch(string Id, string GameId, string HostId, string SessionName, string HostAddress,
    string MapFile, int SimTurnThrough, string Status, List<NativeLaunchPlayer> Players);
