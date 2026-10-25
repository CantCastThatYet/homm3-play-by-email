using HeroesLobby.NativeHost;

namespace HeroesLobby.Desktop;

/// <summary>Starts this player's HotA copy on the relay and keeps their seat saved while it runs.</summary>
public interface IHotaLauncher
{
    Task<int> Play(string root, RelayEndpoint relay, CancellationToken cancellation);
    Task<int> Continue(string root, RelayEndpoint relay, RelayApi api, Guid game, string downloads, CancellationToken cancellation);
    /// <summary>Returns when HotA closes.</summary>
    Task Keep(int pid, RelayApi api, Action<string> report, CancellationToken cancellation);
}

public sealed class NativeHotaLauncher(string? nativeBuildDirectory = null) : IHotaLauncher
{
    public static string BuildDirectory => Path.Combine(AppContext.BaseDirectory, "native");
    private readonly string native = Path.GetFullPath(nativeBuildDirectory ?? BuildDirectory);

    /// <summary>Checks an owned copy of the supported HotA build and this package's adapter.</summary>
    public static void Validate(string root, string nativeDirectory)
    {
        if (!OperatingSystem.IsWindows()) throw new InvalidOperationException("HotA currently requires Windows.");
        if (string.IsNullOrWhiteSpace(root)) throw new InvalidOperationException("Choose your HotA folder with Set up HotA first.");
        NativeBridge.VerifyBuild(root);
        if (File.ReadAllText(Path.Combine(root, NativeBridge.Marker)).Trim() != NativeBridge.MarkerContent)
            throw new InvalidOperationException("Set up HotA to create a separate game copy first.");
        if (!File.Exists(Path.Combine(nativeDirectory, "HeroesPost.Native.dll")) || !File.Exists(Path.Combine(nativeDirectory, "HeroesPost.Injector.exe")))
            throw new InvalidOperationException("This app package is missing its native adapter. Extract the complete package.");
    }

    public Task<int> Play(string root, RelayEndpoint relay, CancellationToken cancellation)
    {
        Validate(root, native);
        return RelayPlay.Play(root, native, relay, Timeout(cancellation, 2));
    }

    public Task<int> Continue(string root, RelayEndpoint relay, RelayApi api, Guid game, string downloads, CancellationToken cancellation)
    {
        Validate(root, native);
        return RelayPlay.Continue(root, native, relay, api, game, downloads, Timeout(cancellation, 5));
    }

    public Task Keep(int pid, RelayApi api, Action<string> report, CancellationToken cancellation) =>
        new SeatKeeper(pid, api, report).Run(cancellation);

    private static CancellationToken Timeout(CancellationToken cancellation, int minutes)
    {
        var source = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        source.CancelAfter(TimeSpan.FromMinutes(minutes));
        return source.Token;
    }
}
