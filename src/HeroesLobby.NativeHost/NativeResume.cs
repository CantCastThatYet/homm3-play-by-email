using System.Text;
using System.Text.Json;
using HeroesLobby.Contracts;

namespace HeroesLobby.NativeHost;

public static partial class NativeBridge
{
    public static async Task<string> CaptureCurrentBranch(int pid, CancellationToken cancellationToken)
    {
        var initial = await Request(pid, "probe", cancellationToken);
        var identity = initial.GetProperty("createdFiletime").GetString();
        var previous = initial.GetProperty("lastCaptureManifest").GetString();
        var queued = await Request(pid, "capture_current_branch", cancellationToken);
        RequireAccepted(queued, "Native checkpoint capture");
        var deadline = DateTime.UtcNow.AddSeconds(90);
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(200, cancellationToken);
            var state = await Request(pid, "probe", cancellationToken);
            if (state.GetProperty("createdFiletime").GetString() != identity)
                throw new InvalidOperationException("The original native process has exited.");
            var status = state.GetProperty("captureStatus").GetString();
            if (status == "failed") throw new InvalidOperationException(state.GetProperty("captureError").GetString());
            var manifest = state.GetProperty("lastCaptureManifest").GetString();
            if (status == "written" && !string.IsNullOrEmpty(manifest) && manifest != previous)
            {
                CaptureBundle.CheckRegularPath(manifest);
                return manifest;
            }
        }
        throw new TimeoutException("HotA has not reached a safe adventure-map checkpoint boundary. Close any in-game dialog and retry.");
    }

    public static async Task<int> Resume(string root, string nativeDirectory, string archivePath,
        int color, CancellationToken cancellationToken)
    {
        root = Path.GetFullPath(root);
        CaptureBundle.CheckRegularPath(root);
        CaptureBundle.CheckRegularPath(archivePath);
        if (File.ReadAllText(Path.Combine(root, Marker)).Trim() != MarkerContent)
            throw new InvalidOperationException("Resume requires an adapter-owned installation.");
        var relayConfig = Path.Combine(root, "HeroesPost.relay.ini");
        var relayRejoin = File.Exists(relayConfig) && File.ReadAllLines(relayConfig, RelayEndpoint.Encoding).Any(l => l.TrimStart().StartsWith("seat=", StringComparison.Ordinal));
        NativeResumeImage image;
        using (var source = File.OpenRead(archivePath)) image = NativeResumeImage.Build(source, color, relayRejoin);
        var map = image.MapFile;
        if (!File.Exists(Path.Combine(root, "Maps", map)))
            throw new InvalidOperationException($"This game was played on the map '{map}', which is not installed in {Path.Combine(root, "Maps")}.");
        var target = Path.Combine(root, "HeroesPost.resume.bin");
        if (File.Exists(target)) CaptureBundle.CheckRegularPath(target);
        var temporary = target + ".pending-" + Guid.NewGuid().ToString("N");
        using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { file.Write(image.Resume); file.Flush(flushToDisk: true); }
        File.Move(temporary, target, overwrite: true);
        // A relay rejoin first re-delivers the messages HD's queue still held at capture time,
        // then replays the relay's later messages from where the capture's engine stopped.
        if (relayRejoin && image.Consumed >= 0)
        {
            var lines = File.ReadAllLines(relayConfig, RelayEndpoint.Encoding).Where(l => !l.TrimStart().StartsWith("from=", StringComparison.Ordinal) &&
                !l.TrimStart().StartsWith("sent=", StringComparison.Ordinal) && !l.TrimStart().StartsWith("deferred=", StringComparison.Ordinal)).ToList();
            lines.Add($"from={image.Consumed}");
            if (image.Sent >= 0) lines.Add($"sent={image.Sent}");
            var deferredFile = Path.Combine(root, "HeroesPost.deferred.bin");
            File.Delete(deferredFile);
            if (image.DeferredCount != 0)
            {
                File.WriteAllBytes(deferredFile, image.Deferred);
                lines.Add("deferred=HeroesPost.deferred.bin");
            }
            File.WriteAllLines(relayConfig, lines, RelayEndpoint.Encoding);
        }
        int pid=0;
        string? launchedIdentity=null;
        try {
        pid = await Launch(root, nativeDirectory, cancellationToken,(id,created)=> {pid=id;launchedIdentity=created.ToString();});
        var startup = await Request(pid, "probe", cancellationToken);
        var identity = startup.GetProperty("createdFiletime").GetString();
        launchedIdentity=identity;
        var menuDeadline = DateTime.UtcNow.AddSeconds(30);
        while (!startup.GetProperty("nativeMenuReady").GetBoolean())
        {
            if(DateTime.UtcNow >= menuDeadline)throw new TimeoutException("The isolated client did not reach its main menu.");
            await Task.Delay(200,cancellationToken);
            startup=await Request(pid,"probe",cancellationToken);
            if(startup.GetProperty("createdFiletime").GetString()!=identity)throw new InvalidOperationException("The native process changed before recovery.");
        }
        RequireAccepted(await Request(pid, "resume_session", cancellationToken), "Native resume");
        RequireAccepted(await Request(pid, "renew_lease_fence", cancellationToken), "Native resume lease");
        new BootstrapRequest("SoloScaffold", "HeroesPost", "HeroesPostResume", "127.0.0.1", map, 26777).Stage(root);
        RequireAccepted(await Request(pid, "bootstrap_session", cancellationToken), "Native resume initialization");
        var deadline = DateTime.UtcNow.AddSeconds(120);
        var introQueued = false;
        DateTime? verifiedSince=null;
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(200, cancellationToken);
            var state = await Request(pid, "probe", cancellationToken);
            if (state.GetProperty("createdFiletime").GetString() != identity)
                throw new InvalidOperationException("The native process changed during recovery.");
            if(state.GetProperty("sessionBootstrap").GetProperty("status").GetString()=="failed")
                throw new InvalidOperationException("Native recovery initialization failed: "+state.GetProperty("sessionBootstrap").GetProperty("error").GetString());
            if(state.GetProperty("nativeStart").GetProperty("status").GetString() is "refused" or "cancelled")
                throw new InvalidOperationException("Native recovery initialization failed: "+state.GetProperty("nativeStart").GetProperty("detail").GetString());
            if (!introQueued)
            {
                var intro = await Request(pid, "experiment_acknowledge_known_intro", cancellationToken);
                introQueued = intro.TryGetProperty("ok", out var accepted) && accepted.GetBoolean();
            }
            if (!state.TryGetProperty("soloResume", out var resume)) continue;
            if (resume.TryGetProperty("error", out var error) && !string.IsNullOrEmpty(error.GetString()))
                throw new InvalidOperationException($"Native resume failed: {error.GetString()}. The failed recovery client will close.");
            if(state.GetProperty("leaseHeld").GetBoolean())throw new InvalidOperationException("The native recovery lease expired before gameplay was verified.");
            if(resume.GetProperty("transportFault").GetBoolean()||!string.IsNullOrEmpty(resume.GetProperty("journalError").GetString()))
                throw new InvalidOperationException("Native recovery transport failed: "+resume.GetProperty("transportError").GetString()+resume.GetProperty("journalError").GetString());
            var game=state.GetProperty("game");
            var valid=resume.GetProperty("verified").GetBoolean() &&
                (resume.GetProperty("playable").GetBoolean() || resume.GetProperty("completedInputHeld").GetBoolean()) &&
                game.GetProperty("adventureMapPresent").GetBoolean()&&!game.GetProperty("scenarioOpen").GetBoolean()&&
                game.GetProperty("network").GetInt32()==1&&game.GetProperty("simultaneousTurns").GetBoolean()&&game.GetProperty("localColor").GetInt32()==color;
            if(valid) {
                verifiedSince ??= DateTime.UtcNow;
                if(DateTime.UtcNow-verifiedSince.Value>=TimeSpan.FromSeconds(1))return pid;
            } else verifiedSince=null;
        }
        throw new TimeoutException("Native recovery did not verify the saved world. No turn was submitted; the failed recovery client will close.");
        }
        catch {
            // This method owns only the newly launched recovery client. Failed
            // restoration never exposes input; retain diagnostics, then close it.
            try {
                using var deadline=new CancellationTokenSource(TimeSpan.FromSeconds(3));
                var failed=await Request(pid,"probe",deadline.Token);
                if(failed.GetProperty("createdFiletime").GetString()==launchedIdentity) {
                    var evidence=Path.Combine(root,$"HeroesPost.resume-failure-{pid}.json");
                    using var record=new FileStream(evidence,FileMode.CreateNew,FileAccess.Write,FileShare.Read);
                    record.Write(Encoding.UTF8.GetBytes(failed.GetRawText()));record.Flush(true);
                }
            } catch(Exception) { }
            try {
                using var game=System.Diagnostics.Process.GetProcessById(pid);
                if(launchedIdentity is not null && game.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()==launchedIdentity &&
                    NativePath.FinalFilePath(game.MainModule!.FileName).Equals(NativePath.FinalFilePath(Path.Combine(root,Executable)),StringComparison.OrdinalIgnoreCase))
                    game.Kill();
            } catch(Exception) { }
            throw;
        }
    }

    private static void RequireAccepted(JsonElement result, string operation)
    {
        if (!result.TryGetProperty("ok", out var ok) || !ok.GetBoolean())
            throw new InvalidOperationException($"{operation} refused: {result}");
    }
}
