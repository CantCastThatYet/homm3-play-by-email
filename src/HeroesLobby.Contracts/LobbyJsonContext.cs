using System.Text.Json.Serialization;

namespace HeroesLobby.Contracts;

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true)]
[JsonSerializable(typeof(ServerInfo))]
[JsonSerializable(typeof(PlayerInfo))]
[JsonSerializable(typeof(PlayerCredentials))]
[JsonSerializable(typeof(RegisterPlayer))]
[JsonSerializable(typeof(CreateGame))]
[JsonSerializable(typeof(JoinGame))]
[JsonSerializable(typeof(PlayerSettings))]
[JsonSerializable(typeof(ReadyRequest))]
[JsonSerializable(typeof(RevisionRequest))]
[JsonSerializable(typeof(GameInfo))]
[JsonSerializable(typeof(List<GameInfo>))]
[JsonSerializable(typeof(CheckpointInfo))]
[JsonSerializable(typeof(List<CheckpointInfo>))]
[JsonSerializable(typeof(string))]
[JsonSerializable(typeof(BeginNativeLaunch))]
[JsonSerializable(typeof(NativeLaunchReport))]
[JsonSerializable(typeof(NativeLaunch))]
[JsonSerializable(typeof(NativeBranchInfo))]
[JsonSerializable(typeof(AcquireResumeLease))]
[JsonSerializable(typeof(ResumeLease))]
[JsonSerializable(typeof(ResumeLeaseToken))]
[JsonSerializable(typeof(ReleaseResumeLease))]
[JsonSerializable(typeof(PublishNativeBranch))]
public partial class LobbyJsonContext : JsonSerializerContext;
