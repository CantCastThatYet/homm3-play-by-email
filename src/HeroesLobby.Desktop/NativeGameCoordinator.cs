using System.Text.Json;
using HeroesLobby.Contracts;
using HeroesLobby.NativeHost;

namespace HeroesLobby.Desktop;

public interface INativeGameRuntime
{
    void Validate(string root, string map);
    Task<int> Launch(string root, CancellationToken cancellation);
    Task<uint> Connect(int pid, string root, NativeLaunch run, bool host, int color, CancellationToken cancellation);
    Task Start(int pid, string root, NativeLaunch run, bool host, CancellationToken cancellation);
    Task WaitForMap(int pid, int color, CancellationToken cancellation);
    Task<int> Resume(string root, string archivePath, int color, CancellationToken cancellation) =>
        Task.FromException<int>(new NotSupportedException("This native runtime does not support verified branch restoration."));
    Task<string?> CaptureCurrentBranch(int pid, CancellationToken cancellation) => Task.FromResult<string?>(null);
    Task<string?> PollCompletedCapture(int pid, CancellationToken cancellation) => Task.FromResult<string?>(null);
    Task ArmCompletionCapture(int pid, CancellationToken cancellation) => Task.CompletedTask;
    Task Fence(int pid, CancellationToken cancellation) => Task.FromException(new NotSupportedException("Native lease fencing is unavailable."));
    Task RenewLeaseFence(int pid, CancellationToken cancellation) => Task.FromException(new NotSupportedException("Native lease fencing is unavailable."));
    bool IsRunning(int pid) => throw new NotSupportedException("Native process identity checks are unavailable.");
    string AdapterBuild => "";
}

public sealed partial class NativeGameRuntime : INativeGameRuntime
{
    public static string BuildDirectory => Path.Combine(AppContext.BaseDirectory, "native");
    private readonly string nativeDirectory;
    private readonly Action<int, long, string>? processStarted;
    public NativeGameRuntime(string? nativeBuildDirectory = null, Action<int, long, string>? processStarted = null)
    {
        nativeDirectory = Path.GetFullPath(nativeBuildDirectory ?? BuildDirectory);
        this.processStarted = processStarted;
    }
    public void Validate(string root, string map)
    {
        if (!OperatingSystem.IsWindows()) throw new InvalidOperationException("Original HotA startup currently requires Windows.");
        if (string.IsNullOrWhiteSpace(root)) throw new InvalidOperationException("Choose your HotA folder with Set up HotA first.");
        NativeBridge.VerifyBuild(root);
        if (File.ReadAllText(Path.Combine(root, NativeBridge.Marker)).Trim() != NativeBridge.MarkerContent)
            throw new InvalidOperationException("Set up HotA to create an isolated game copy first.");
        if (!File.Exists(Path.Combine(nativeDirectory, "HeroesPost.Native.dll")) || !File.Exists(Path.Combine(nativeDirectory, "HeroesPost.Injector.exe")))
            throw new InvalidOperationException("This app package is missing its native adapter. Extract the complete updated package.");
        if (map.IndexOfAny(['/', '\\', ':']) >= 0 || !File.Exists(Path.Combine(root, "Maps", map)))
            throw new InvalidOperationException("The selected map is not installed in this HotA copy.");
    }
    public async Task<int> Launch(string root, CancellationToken cancellation)
    {
        var pid = await NativeBridge.Launch(root, nativeDirectory, cancellation,
            (id, created) => processStarted?.Invoke(id, checked((long)created), Path.GetFullPath(root)));
        RememberProcess(pid); return pid;
    }
    public async Task<uint> Connect(int pid, string root, NativeLaunch run, bool host, int color, CancellationToken cancellation)
    {
        var state = await NativeBridge.BootstrapNetwork(pid, root,
            new(host ? "Host" : "Join", host ? "Red" : "Blue", run.SessionName, run.HostAddress, run.MapFile, 24777 + color), cancellation);
        return state.GetProperty("localPlayerId").GetUInt32();
    }
    public async Task Start(int pid, string root, NativeLaunch run, bool host, CancellationToken cancellation)
    {
        await NativeBridge.StartNetwork(pid, root, new(host ? "HostStart" : "PeerAccept",
            run.Players.Single(p => p.PlayerId == run.HostId).NativePlayerId,
            run.Players.Single(p => p.PlayerId != run.HostId).NativePlayerId, run.MapFile, run.SimTurnThrough), cancellation);
    }
    public async Task WaitForMap(int pid, int color, CancellationToken cancellation)
    {
        string? created = null;
        while (true)
        {
            var probe = await NativeBridge.Request(pid, "probe", cancellation);
            var identity = probe.GetProperty("createdFiletime").GetString();
            created ??= identity;
            if (created != identity) throw new InvalidOperationException("The HotA process changed during startup.");
            var state = probe.GetProperty("nativeStart");
            if (state.GetProperty("status").GetString() == "refused") throw new InvalidOperationException(state.GetProperty("detail").GetString());
            var game = probe.GetProperty("game");
            if (game.GetProperty("adventureMapPresent").GetBoolean() && !game.GetProperty("scenarioOpen").GetBoolean() &&
                game.GetProperty("dateValid").GetBoolean() && game.GetProperty("simultaneousTurns").GetBoolean() &&
                game.GetProperty("localColor").GetInt32() == color && game.GetProperty("currentColor").GetInt32() == color) return;
            await Task.Delay(250, cancellation);
        }
    }
}

/// <summary>Drives one member's launch; the server holds coordination, never a game process.</summary>
public sealed partial class NativeGameCoordinator(INativeGameRuntime runtime)
{
    private readonly HashSet<string> handled = [];
    private Task? active;
    public string Status { get; private set; } = "";
    public event Action? Changed;
    public Task? Active => active;
    private void SetStatus(string value) { Status = value; Changed?.Invoke(); }

    public void Observe(LobbyClient client, GameInfo game, LocalProfile profile)
    {
        if (game.Status != "Launching" || !game.Members.Any(m => m.PlayerId == client.Player!.Id) || handled.Contains(game.Id)) return;
        if (active is { IsCompleted: false }) return;
        handled.Add(game.Id);
        active = Task.Run(() => Run(client, game.Id, profile));
    }

    private async Task Run(LobbyClient client, string gameId, LocalProfile profile)
    {
        var root = profile.NativeGameRoot;
        NativeLaunch? run = null; uint nativeId = 0; bool owned = false;
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(4));
        try
        {
            run = await client.Native(gameId);
            runtime.Validate(root, run.MapFile);
            var guard = Path.Combine(root, ".heroespost-launch-" + run.Id);
            try { using var file = new FileStream(guard, FileMode.CreateNew, FileAccess.Write, FileShare.Read); owned = true; }
            catch (IOException) when (File.Exists(guard))
            { SetStatus("This game was already launched by this HotA copy. Use the open client, or close it and choose Resume HotA to load your saved turn."); return; }
            var host = client.Player!.Id == run.HostId;
            var color = run.Players.Single(p => p.PlayerId == client.Player.Id).Color;
            if (!host)
            {
                SetStatus("Waiting for the host to open HotA…");
                run = await Wait(client, gameId, r => r.Players.Single(p => p.PlayerId == r.HostId).Phase == "Hosted", timeout.Token);
            }
            SetStatus("Opening HotA…");
            var pid = await runtime.Launch(root, timeout.Token);
            SetStatus(host ? "Hosting the native game…" : "Joining the native game…");
            nativeId = await runtime.Connect(pid, root, run, host, color, timeout.Token);
            run = await client.ReportNative(gameId, new(run.Id, host ? "Hosted" : "Joined", nativeId));
            run = await Wait(client, gameId, r => r.Players.All(p => p.NativePlayerId != 0), timeout.Token);
            if (!host)
            {
                SetStatus("Accepting the game settings…");
                await runtime.Start(pid, root, run, false, timeout.Token);
                await client.ReportNative(gameId, new(run.Id, "Armed", nativeId));
            }
            else
            {
                SetStatus("Waiting for your friend's HotA client…");
                run = await Wait(client, gameId, r => r.Players.Single(p => p.PlayerId != r.HostId).Phase == "Armed", timeout.Token);
                await runtime.Start(pid, root, run, true, timeout.Token);
            }
            await runtime.WaitForMap(pid, color, timeout.Token);
            run = await client.ReportNative(gameId, new(run.Id, "Started", nativeId));
            run = await Wait(client, gameId, r => r.Players.All(p => p.Phase == "Started"), timeout.Token);
            try { await BeginBranchTracking(client, run, pid, profile, timeout.Token); }
            catch (Exception captureError) { SetStatus("HotA started, but its initial branch was not stored: " + captureError.Message); }
        }
        catch (Exception error)
        {
            var message = error is OperationCanceledException ? "HotA startup timed out. Check both clients and the host PC address." : error.Message;
            if (message.Length > 800) message = message[..800];
            SetStatus("Could not start HotA: " + message);
            // Validation failures must be visible to both members; duplicate-owner guards returned above.
            if (run is not null && (owned || !string.IsNullOrWhiteSpace(message)))
                try { await client.ReportNative(gameId, new(run.Id, "Failed", nativeId, message)); } catch (Exception) { }
        }
    }
    private async Task<NativeLaunch> Wait(LobbyClient client, string id, Func<NativeLaunch, bool> ready, CancellationToken cancellation)
    {
        while (true)
        {
            cancellation.ThrowIfCancellationRequested();
            var run = await client.Native(id);
            if (run.Status == "Failed") throw new InvalidOperationException(run.Players.First(p => p.Phase == "Failed").Error);
            if (ready(run)) return run;
            await Task.Delay(500, cancellation);
        }
    }
}
