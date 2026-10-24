using System.Diagnostics;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text.Json;
using HeroesLobby.Contracts;
using HeroesLobby.NativeHost;

namespace HeroesLobby.Desktop;

public sealed partial class NativeGameRuntime
{
    private readonly ConcurrentDictionary<int, long> processIdentities = new();
    private void RememberProcess(int pid) { using var process = Process.GetProcessById(pid); processIdentities[pid] = process.StartTime.ToUniversalTime().ToFileTimeUtc(); }
    private void RequireProcess(int pid) { if (!IsRunning(pid)) throw new InvalidOperationException("The owned HotA process closed or its identity changed."); }
    public async Task<int> Resume(string root, string archivePath, int color, CancellationToken cancellation)
    {
        var pid = await NativeBridge.Resume(root, nativeDirectory, archivePath, color, cancellation); RememberProcess(pid);
        processStarted?.Invoke(pid, processIdentities[pid], Path.GetFullPath(root)); return pid;
    }
    public async Task<string?> CaptureCurrentBranch(int pid, CancellationToken cancellation) =>
        await NativeBridge.CaptureCurrentBranch(pid, cancellation);
    public bool IsRunning(int pid)
    {
        try { using var process = Process.GetProcessById(pid); return !process.HasExited && processIdentities.TryGetValue(pid, out var created) && process.StartTime.ToUniversalTime().ToFileTimeUtc() == created; }
        catch (ArgumentException) { return false; }
    }
    public string AdapterBuild => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine(nativeDirectory, "HeroesPost.Native.dll")))).ToLowerInvariant();
    public async Task ArmCompletionCapture(int pid, CancellationToken cancellation)
    {
        RequireProcess(pid);
        var reply = await NativeBridge.Request(pid, "experiment_snapshot_next_completion", cancellation);
        if (!reply.TryGetProperty("ok", out var ok) || !ok.GetBoolean())
            throw new InvalidOperationException("Native completion capture could not be armed.");
    }
    public async Task<string?> PollCompletedCapture(int pid, CancellationToken cancellation)
    {
        RequireProcess(pid);
        var probe = await NativeBridge.Request(pid, "probe", cancellation);
        if (probe.GetProperty("captureStatus").GetString() == "failed")
            throw new InvalidOperationException(probe.GetProperty("captureError").GetString());
        if (probe.GetProperty("captureStatus").GetString() != "written") return null;
        var path = probe.GetProperty("lastCaptureManifest").GetString();
        if (string.IsNullOrEmpty(path)) return null;
        using var manifest = JsonDocument.Parse(await File.ReadAllTextAsync(path, cancellation));
        var completion = manifest.RootElement.GetProperty("native_completion");
        return completion.GetProperty("observed").GetBoolean() && completion.GetProperty("marker_confirmed").GetBoolean() &&
            completion.GetProperty("fresh_world_serialized").GetBoolean() ? path : null;
    }
    private async Task FenceCommand(int pid, string command, CancellationToken cancellation)
    {
        RequireProcess(pid);
        var response = await NativeBridge.Request(pid, command, cancellation);
        if (!response.TryGetProperty("ok", out var ok) || !ok.GetBoolean())
            throw new InvalidOperationException("Native lease fence command was refused.");
    }
    public Task Fence(int pid, CancellationToken cancellation) => FenceCommand(pid, "hold_session", cancellation);
    public Task RenewLeaseFence(int pid, CancellationToken cancellation) => FenceCommand(pid, "renew_lease_fence", cancellation);
}

public sealed partial class NativeGameCoordinator
{
    private readonly Dictionary<string, Task> branchMonitors = [];
    public Task? BranchTracking(string gameId) => branchMonitors.TryGetValue(gameId, out var monitor) ? monitor : null;

    public Task Resume(LobbyClient client, GameInfo game, LocalProfile profile)
    {
        if (active is { IsCompleted: false } || branchMonitors.TryGetValue(game.Id, out var running) && !running.IsCompleted)
            throw new InvalidOperationException("A native client is already active for this seat.");
        if (game.Status is not ("Playing" or "Started") || !game.Members.Any(m => m.PlayerId == client.Player?.Id))
            throw new InvalidOperationException("Only a member of a started game can resume its branch.");
        return active = RunResume(client, game, profile);
    }

    private async Task RunResume(LobbyClient client, GameInfo game, LocalProfile profile)
    {
        ResumeLease? lease = null; bool monitoring = false, restoreAttempted = false; int? ownedPid = null;
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        try
        {
            SetStatus("Fetching your authoritative branch…");
            var branch = await client.Branch(game.Id);
            var run = await client.Native(game.Id);
            if (branch.OwnerPlayerId != client.Player!.Id || branch.GameId != game.Id || branch.LaunchId != run.Id ||
                branch.SessionName != run.SessionName || branch.Color != game.Members.Single(m => m.PlayerId == client.Player.Id).Color)
                throw new InvalidDataException("The saved branch does not match your seat and native session.");
            runtime.Validate(profile.NativeGameRoot, run.MapFile);
            if (!runtime.AdapterBuild.Equals(branch.AdapterBuild, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("This branch was captured by a different native adapter build. Compatible restoration has not been verified.");
            lease = await client.AcquireResume(game.Id, branch.Revision);
            timeout.CancelAfter(TimeSpan.FromSeconds(90)); // Shorter than the server's 120s lease.
            branch = lease.Branch ?? throw new InvalidDataException("The server returned a lease without a branch.");
            var archive = NewArchivePath(profile, game.Id);
            SetStatus("Downloading and checking your saved native branch…");
            await client.DownloadBranch(branch, archive, timeout.Token);
            SetStatus("Restoring your HotA branch…");
            restoreAttempted = true;
            var pid = await runtime.Resume(profile.NativeGameRoot, archive, branch.Color, timeout.Token);
            ownedPid = pid;
            // NativeBridge returns only after its actual restoration and input-state probes pass.
            lease = await client.RenewResume(game.Id, lease.Token);
            await runtime.RenewLeaseFence(pid, timeout.Token);
            await runtime.ArmCompletionCapture(pid, timeout.Token);
            branchMonitors[game.Id] = MonitorBranch(client, run, pid, profile, lease, null);
            monitoring = true;
            SetStatus(branch.CaptureKind == "completion"
                ? "Your submitted turn is restored. This build cannot advance to the next day yet."
                : "Game restored. Play your turn, then wait for the lobby to confirm it is saved before closing HotA.");
        }
        catch (Exception error) { SetStatus("Could not resume HotA: " + error.Message); throw; }
        finally
        {
            if (!monitoring && lease is not null && ((!restoreAttempted) || (ownedPid is not null && await FenceOrStopped(ownedPid.Value))))
                try { await client.ReleaseResume(game.Id, lease.Token); } catch (Exception) { }
        }
    }

    private async Task BeginBranchTracking(LobbyClient client, NativeLaunch run, int pid, LocalProfile profile, CancellationToken cancellation)
    {
        // Fakes and older runtimes explicitly expose no adapter identity/capture capability.
        if (string.IsNullOrEmpty(runtime.AdapterBuild)) { SetStatus("HotA is on the map. This runtime cannot store or restore native branches."); return; }
        ResumeLease lease;
        try { lease = await client.AcquireResume(run.GameId, 0, initialize: true); }
        catch { await FenceOrStopped(pid); throw; }
        bool monitoring = false;
        try
        {
            await runtime.RenewLeaseFence(pid, cancellation);
            var manifest = await runtime.CaptureCurrentBranch(pid, cancellation) ?? throw new InvalidOperationException("This adapter cannot capture an initial native branch.");
            var branch = await UploadBranch(client, run, profile, lease, manifest, cancellation);
            lease = lease with { Branch = branch };
            await runtime.ArmCompletionCapture(pid, cancellation);
            branchMonitors[run.GameId] = MonitorBranch(client, run, pid, profile, lease, manifest);
            monitoring = true;
            SetStatus("HotA is ready. Your starting position is saved and can be resumed alone.");
        }
        finally { if (!monitoring && await FenceOrStopped(pid)) try { await client.ReleaseResume(run.GameId, lease.Token); } catch (Exception) { } }
    }

    private async Task<NativeBranchInfo> UploadBranch(LobbyClient client, NativeLaunch run, LocalProfile profile,
        ResumeLease lease, string manifest, CancellationToken cancellation)
    {
        var archive = NewArchivePath(profile, run.GameId);
        CaptureBundle.Write(manifest, archive);
        var hash = Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(archive, cancellation))).ToLowerInvariant();
        var checkpoint = await client.UploadCapture(run.GameId, archive, "branch-" + hash, cancellation);
        return await client.PublishBranch(run.GameId, new(lease.Branch?.Revision ?? 0, lease.Token, checkpoint.Id,
            run.Id, run.SessionName, runtime.AdapterBuild));
    }

    private async Task MonitorBranch(LobbyClient client, NativeLaunch run, int pid, LocalProfile profile, ResumeLease lease, string? lastManifest)
    {
        bool release = false;
        try
        {
            var renewAt = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(25);
            while (runtime.IsRunning(pid))
            {
                await Task.Delay(1000);
                if (DateTimeOffset.UtcNow >= renewAt)
                {
                    lease = await client.RenewResume(run.GameId, lease.Token);
                    using var fenceTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                    await runtime.RenewLeaseFence(pid, fenceTimeout.Token);
                    renewAt = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(25);
                }
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
                var manifest = await runtime.PollCompletedCapture(pid, timeout.Token);
                if (manifest is not null && manifest != lastManifest)
                {
                    var branch = await UploadBranch(client, run, profile, lease, manifest, timeout.Token);
                    lease = lease with { Branch = branch }; lastManifest = manifest;
                    SetStatus($"Turn saved (revision {branch.Revision}). You can close HotA. Next-day merging is not implemented yet.");
                    await runtime.ArmCompletionCapture(pid, timeout.Token);
                }
            }
            SetStatus("HotA closed. Resume will reopen your latest saved position.");
            release = true;
        }
        catch (Exception error)
        {
            release = await FenceOrStopped(pid);
            SetStatus(release ? "HotA is held because branch tracking stopped: " + error.Message :
                "Connection lost; the native lease deadline will hold gameplay. The server lease remains reserved until expiry. " + error.Message);
        }
        finally { if (release) try { await client.ReleaseResume(run.GameId, lease.Token); } catch (Exception) { } }
    }

    private async Task<bool> FenceOrStopped(int pid)
    {
        try {
            if (!runtime.IsRunning(pid)) return true;
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5)); await runtime.Fence(pid, timeout.Token); return true;
        }
        catch (Exception) { return false; }
    }

    private static string NewArchivePath(LocalProfile profile, string gameId)
    {
        if (gameId.Length != 32 || !gameId.All(char.IsAsciiHexDigit)) throw new InvalidDataException("Invalid game identity.");
        var folder = Path.Combine(profile.StorageDirectory, "branches", gameId);
        Directory.CreateDirectory(folder);
        for (var current = new DirectoryInfo(folder); current is not null; current = current.Parent)
            if ((current.Attributes & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Linked branch storage is not supported.");
        return Path.Combine(folder, Guid.NewGuid().ToString("N") + ".zip");
    }
}
