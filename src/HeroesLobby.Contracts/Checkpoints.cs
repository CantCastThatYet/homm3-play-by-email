namespace HeroesLobby.Contracts;

// Stored engine bytes only. This receipt is not a completed turn or a validated playable save.
public record CheckpointInfo(string Id, string GameId, string OwnerPlayerId, long BaseRevision,
    string Sha256, long Length, DateTimeOffset CreatedAt);
