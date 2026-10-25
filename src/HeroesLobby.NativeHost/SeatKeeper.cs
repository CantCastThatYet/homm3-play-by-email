using System.Diagnostics;
using System.Text.Json;

namespace HeroesLobby.NativeHost;

/// <summary>
/// Keeps the server's copy of a player's seat current while their HotA runs, so a stand-in or a
/// later Continue resumes where the player left off. It uploads a capture when the player ends a
/// turn, once at the start of each day, and after the player has acted during a turn.
/// </summary>
public sealed class SeatKeeper(int pid, RelayApi api, Action<string> report)
{
    /// <summary>Minimum time between captures of a turn in progress.</summary>
    public TimeSpan ActionInterval { get; init; } = TimeSpan.FromSeconds(20);

    public async Task Run(CancellationToken cancellation)
    {
        string? lastManifest = null, created = null;
        (int, int, int)? capturedDay = null;
        long sentAtCapture = -1;
        var lastCapture = DateTimeOffset.MinValue;
        var capturing = false;
        var reportedWait = false;
        DateTimeOffset? sequentialSince = null;
        while (!cancellation.IsCancellationRequested && Running())
        {
            await Task.Delay(1000, cancellation);
            JsonElement probe;
            try { probe = await NativeBridge.Request(pid, "probe", cancellation); }
            catch (Exception error) when (error is IOException or TimeoutException or OperationCanceledException && !cancellation.IsCancellationRequested) { continue; }
            var identity = probe.GetProperty("createdFiletime").GetString();
            if ((created ??= identity) != identity) return;
            var relay = probe.GetProperty("relay");
            var game = probe.GetProperty("game");
            if (!Guid.TryParse(relay.GetProperty("instance").GetString(), out var instance) || !relay.GetProperty("sessionOpen").GetBoolean() ||
                !game.GetProperty("adventureMapPresent").GetBoolean() || game.GetProperty("network").GetInt32() != 1) continue;
            // HD switches simultaneous turns on a moment after the map appears.
            if (!game.GetProperty("simultaneousTurns").GetBoolean())
            {
                sequentialSince ??= DateTimeOffset.UtcNow;
                if (!reportedWait && DateTimeOffset.UtcNow - sequentialSince > TimeSpan.FromSeconds(15))
                { report("This game does not use simultaneous turns, so your seat cannot be saved for leaving and returning."); reportedWait = true; }
                continue;
            }
            sequentialSince = null;

            var status = probe.GetProperty("captureStatus").GetString();
            var manifest = probe.GetProperty("lastCaptureManifest").GetString();
            if (status == "written" && !string.IsNullOrEmpty(manifest) && manifest != lastManifest)
            {
                lastManifest = manifest; capturing = false;
                var completion = IsCompletion(manifest);
                try
                {
                    await api.UploadCapture(instance, manifest, cancellation);
                    report(completion ? "Turn ended and saved on the server. You can close HotA; a stand-in will finish the day for you if needed."
                        : $"Position saved on the server at {DateTime.Now:HH:mm}.");
                }
                catch (Exception error) when (error is InvalidOperationException or HttpRequestException or IOException or InvalidDataException)
                { report("Could not save your position on the server: " + error.Message); }
            }
            if (status == "failed" && capturing) { capturing = false; report("HotA could not capture your position: " + probe.GetProperty("captureError").GetString()); }

            var date = game.GetProperty("date");
            var day = (date.GetProperty("day").GetInt32(), date.GetProperty("week").GetInt32(), date.GetProperty("month").GetInt32());
            var local = game.GetProperty("localColor").GetInt32();
            var phase = probe.GetProperty("nativePhase");
            var done = local is >= 0 and < 8 && phase.GetProperty("participants")[local][1].GetInt32() == 1;
            var idle = phase.GetProperty("modalDepth").GetInt32() == 0 && !(game.TryGetProperty("combatActive", out var combat) && combat.GetBoolean());
            var busy = capturing || status is "armed" or "copying" or "writing";

            // The End Turn capture is taken by the game itself at its completion marker.
            if (!done && !busy && !probe.GetProperty("completionArmed").GetBoolean())
                await Command("experiment_snapshot_next_completion", cancellation);
            var sent = relay.GetProperty("sent").GetInt64();
            var now = DateTimeOffset.UtcNow;
            if (!done && idle && !capturing && status is not ("copying" or "writing") &&
                (capturedDay != day || sent != sentAtCapture && now - lastCapture >= ActionInterval))
            {
                if (await Command("capture_current_branch", cancellation))
                { capturing = true; capturedDay = day; sentAtCapture = sent; lastCapture = now; }
            }
        }
    }

    private static bool IsCompletion(string manifest)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllBytes(manifest));
            return document.RootElement.GetProperty("native_completion").GetProperty("observed").GetBoolean();
        }
        catch (Exception error) when (error is IOException or JsonException or KeyNotFoundException) { return false; }
    }

    private async Task<bool> Command(string command, CancellationToken cancellation)
    {
        try
        {
            var reply = await NativeBridge.Request(pid, command, cancellation);
            return reply.TryGetProperty("ok", out var ok) && ok.GetBoolean();
        }
        catch (Exception error) when (error is IOException or TimeoutException or OperationCanceledException && !cancellation.IsCancellationRequested) { return false; }
    }

    private bool Running()
    {
        try { using var process = Process.GetProcessById(pid); return !process.HasExited; }
        catch (ArgumentException) { return false; }
    }
}

/// <summary>Starts HotA for play over the relay, or resumes a seat handed back by the server.</summary>
public static class RelayPlay
{
    public static async Task<int> Play(string root, string nativeDirectory, RelayEndpoint relay, CancellationToken cancellation)
    {
        relay.WritePlay(root);
        return await NativeBridge.Launch(root, nativeDirectory, cancellation, armSaveCapture: false);
    }

    public static async Task<int> Continue(string root, string nativeDirectory, RelayEndpoint relay, RelayApi api, Guid game, string downloads, CancellationToken cancellation)
    {
        var claim = await api.Claim(game, cancellation);
        Directory.CreateDirectory(downloads);
        var archive = Path.Combine(downloads, $"{game:N}-{DateTime.UtcNow:yyyyMMddHHmmss}.hpcap");
        await api.DownloadCapture(game, archive, cancellation);
        relay.WriteRejoin(root, claim.Session, claim.Seat);
        return await NativeBridge.Resume(root, nativeDirectory, archive, claim.Color, cancellation);
    }
}
