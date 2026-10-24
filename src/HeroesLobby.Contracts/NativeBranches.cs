namespace HeroesLobby.Contracts;

// A verified capture format and content hash do not establish native restoration capability.
public record NativeBranchInfo(string GameId, string OwnerPlayerId, string LaunchId, string SessionName,
    int Color, long Revision, string CheckpointId, string Sha256, long Length, string Format,
    string CaptureKind, int Day, int Week, int Month, string AdapterBuild, string RestoreCapability,
    DateTimeOffset CreatedAt);
public record AcquireResumeLease(long ExpectedRevision, bool Initialize = false);
public record ResumeLease(string Token, DateTimeOffset ExpiresAt, NativeBranchInfo? Branch);
public record ResumeLeaseToken(string Token);
public record ReleaseResumeLease(bool Released);
public record PublishNativeBranch(long ExpectedRevision, string LeaseToken, string CheckpointId,
    string LaunchId, string SessionName, string AdapterBuild);
