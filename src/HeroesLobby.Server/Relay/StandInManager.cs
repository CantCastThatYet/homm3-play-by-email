using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using HeroesLobby.NativeHost;

namespace HeroesLobby.Server.Relay;

/// <summary>
/// Plays away seats with the server's own HotA copies. A stand-in resumes the seat's latest
/// capture and rejoins the relay in the player's place. It completes a turn the player already
/// ended, and ends any other turn once the game's turn limit has passed. When the player returns,
/// the stand-in is captured and closed, and the player resumes from that capture.
///
/// Configuration: StandIn:Copies (prepared HotA copies, separated by ';'), StandIn:NativeDirectory
/// (adapter build), StandIn:TurnSeconds (default turn limit, 600), StandIn:GraceSeconds (10).
/// </summary>
public sealed class StandInManager(RelayHub hub, RelayCaptures captures, RelayServer relay, IConfiguration config,
    ILogger<StandInManager> logger) : BackgroundService
{
    public string Ticket { get; } = Convert.ToHexString(RandomNumberGenerator.GetBytes(24));
    private readonly ILogger log = logger;
    private readonly RelayCaptures store = captures;
    private int RelayPort => relay.Port;
    private readonly string[] copies = (config["StandIn:Copies"] ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
        .Select(Path.GetFullPath).ToArray();
    private readonly string? native = config["StandIn:NativeDirectory"] is { Length: > 0 } directory ? Path.GetFullPath(directory) : null;
    private readonly TimeSpan grace = TimeSpan.FromSeconds(config.GetValue("StandIn:GraceSeconds", 10));
    private readonly object gate = new();
    private readonly Dictionary<(Guid, uint), StandIn> active = [];
    private readonly Dictionary<(Guid, uint), DateTimeOffset> paused = [];
    private readonly Dictionary<(Guid, uint), int> failures = [];
    private readonly HashSet<string> reported = [];

    public bool Enabled => OperatingSystem.IsWindows() && copies.Length > 0 && native is not null;
    public int DefaultTurnSeconds { get; } = TurnSecondsSetting(config);
    public static int TurnSecondsSetting(IConfiguration config) => config.GetValue("StandIn:TurnSeconds", 600);

    public RelayAccount Account => new("standin", "Stand-in", StandIn: true);

    protected override async Task ExecuteAsync(CancellationToken stopping)
    {
        if (!Enabled) { logger.LogInformation("Stand-ins are disabled; set StandIn:Copies and StandIn:NativeDirectory on a Windows server to enable them"); return; }
        await relay.Started;
        logger.LogInformation("Stand-ins enabled with {Count} HotA copies; default turn limit {Seconds}s", copies.Length, DefaultTurnSeconds);
        while (!stopping.IsCancellationRequested)
        {
            try { Schedule(stopping); }
            catch (Exception error) { logger.LogError(error, "Stand-in scheduling failed"); }
            try { await Task.Delay(2000, stopping); } catch (OperationCanceledException) { }
        }
        List<StandIn> running;
        lock (gate) running = active.Values.ToList();
        foreach (var standIn in running) standIn.Kill();
    }

    private void Report(string key, string message, params object[] args)
    {
        lock (gate) if (!reported.Add(key)) return;
        logger.LogWarning(message, args);
    }

    private void Schedule(CancellationToken stopping)
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var game in hub.Games().Where(g => g.Started))
            foreach (var seat in game.Seats.Where(s => s.Away && now - s.AwaySince >= grace))
            {
                var key = (game.Instance, seat.Id);
                var capture = store.Latest(game.Instance, seat.Id);
                if (capture is null) { Report($"nocapture:{key}", "Seat {Seat} in {Game} is away but has no capture; no stand-in can play it", seat.Name, game.Name); continue; }
                lock (gate)
                {
                    if (active.ContainsKey(key) || paused.GetValueOrDefault(key) > now) continue;
                    var copy = copies.FirstOrDefault(c => active.Values.All(a => a.Copy != c));
                    if (copy is null) { Report($"nocopy:{key}", "No free HotA copy for a stand-in for seat {Seat} in {Game}", seat.Name, game.Name); continue; }
                    var standIn = new StandIn(this, game.Instance, game.Name, seat, capture, copy);
                    active[key] = standIn;
                    _ = Task.Run(async () =>
                    {
                        var failed = false;
                        try { failed = !await standIn.Run(stopping); }
                        catch (Exception error) { failed = true; logger.LogError(error, "Stand-in for seat {Seat} in {Game} failed", seat.Name, game.Name); }
                        finally { standIn.Kill(); }
                        lock (gate)
                        {
                            active.Remove(key);
                            // Back off after failures so a broken capture does not relaunch HotA every few seconds.
                            var count = failures[key] = failed ? failures.GetValueOrDefault(key) + 1 : 0;
                            if (failed) paused[key] = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(Math.Min(600, 30 << Math.Min(count, 5)));
                        }
                    }, CancellationToken.None);
                }
            }
    }

    /// <summary>Stops a seat's stand-in, if any, after storing its current position, and keeps it stopped while the player reconnects.</summary>
    public async Task Release(Guid game, uint seat, CancellationToken cancellation)
    {
        StandIn? standIn;
        lock (gate)
        {
            paused[(game, seat)] = DateTimeOffset.UtcNow + TimeSpan.FromMinutes(3);
            active.TryGetValue((game, seat), out standIn);
        }
        if (standIn is not null) await standIn.HandBack(cancellation);
    }

    private int TurnSeconds(Guid game) => hub.Games().FirstOrDefault(g => g.Instance == game)?.TurnSeconds ?? DefaultTurnSeconds;

    private sealed class StandIn(StandInManager owner, Guid game, string session, RelaySeatView seat, RelayCapture capture, string copy)
    {
        public string Copy { get; } = copy;
        private readonly TaskCompletionSource handedBack = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private volatile bool handBack;
        private int pid;
        private ILogger Log => owner.log;

        public async Task HandBack(CancellationToken cancellation)
        {
            handBack = true;
            await handedBack.Task.WaitAsync(TimeSpan.FromSeconds(90), cancellation);
        }

        public void Kill()
        {
            try { if (pid != 0) { using var process = Process.GetProcessById(pid); if (!process.HasExited) process.Kill(); } }
            catch (ArgumentException) { }
            catch (InvalidOperationException) { }
            handedBack.TrySetResult();
        }

        /// <returns>False when the stand-in could not be started or its process died.</returns>
        public async Task<bool> Run(CancellationToken stopping)
        {
            new RelayEndpoint("127.0.0.1", owner.RelayPort, owner.Ticket, "standin").WriteRejoin(Copy, session, seat.Name);
            Log.LogInformation("Stand-in for seat {Seat} in {Game} resumes its {Kind} capture of day {Month}/{Week}/{Day} in {Copy}",
                seat.Name, session, capture.Kind, capture.Month, capture.Week, capture.Day, Copy);
            using (var start = CancellationTokenSource.CreateLinkedTokenSource(stopping))
            {
                start.CancelAfter(TimeSpan.FromMinutes(3));
                pid = await NativeBridge.Resume(Copy, owner.native!, capture.Path, capture.Color, start.Token);
            }
            Log.LogInformation("Stand-in for seat {Seat} in {Game} is playing as PID {Pid}", seat.Name, session, pid);
            var turn = new TurnState(capture.Day, capture.Week, capture.Month);
            string? lastManifest = null;
            var capturing = false;
            DateTimeOffset? modalSince = null, combatSince = null, lastKey = null, lastEnd = null, lastCapture = null;
            var autoCombat = false;
            while (!stopping.IsCancellationRequested)
            {
                if (!Running()) { Log.LogWarning("Stand-in PID {Pid} for seat {Seat} exited", pid, seat.Name); return handBack; }
                JsonElement probe;
                try { probe = await NativeBridge.Request(pid, "probe", stopping); }
                catch (Exception error) when (error is IOException or TimeoutException or OperationCanceledException && !stopping.IsCancellationRequested)
                { await Task.Delay(1000, stopping); continue; }
                var state = Probe.Read(probe);
                var now = DateTimeOffset.UtcNow;
                var manifest = probe.GetProperty("lastCaptureManifest").GetString();
                var captureStatus = probe.GetProperty("captureStatus").GetString();
                if (captureStatus == "written" && !string.IsNullOrEmpty(manifest) && manifest != lastManifest)
                {
                    lastManifest = manifest; capturing = false;
                    if (Store(manifest) is { Kind: "current" }) turn.Captured = true;
                }
                if (captureStatus == "failed" && capturing) { Log.LogWarning("Stand-in capture failed: {Error}", probe.GetProperty("captureError").GetString()); capturing = false; }

                if (turn.NewDay(state)) Log.LogInformation("Stand-in for seat {Seat} in {Game} reached day {Month}/{Week}/{Day}", seat.Name, session, state.Month, state.Week, state.Day);

                if (handBack)
                {
                    // A finished turn is already stored by its completion capture; otherwise capture the turn in progress.
                    if (state.Done || await CaptureNow(stopping)) { Kill(); return true; }
                    continue;
                }

                // Battles are fought by auto-combat; every dialog is accepted.
                if (state.Combat) { combatSince ??= now; if (!autoCombat && now - combatSince > TimeSpan.FromSeconds(3)) { Key(0x41); autoCombat = true; lastKey = now; } }
                else { combatSince = null; autoCombat = false; }
                if (state.Modal > 0) modalSince ??= now; else modalSince = null;
                if (state.Modal > 0 && (!state.Combat || autoCombat) && now - modalSince > TimeSpan.FromSeconds(2) && (lastKey is null || now - lastKey > TimeSpan.FromSeconds(2)))
                { Key(0x0D); lastKey = now; }

                var idle = state.Modal == 0 && !state.Combat && !state.Done && !capturing;
                // Each new day is stored once, so a returning player starts from it rather than from the day before.
                if (idle && !turn.Captured && (lastCapture is null || now - lastCapture > TimeSpan.FromSeconds(15)))
                {
                    lastCapture = now;
                    capturing = await Command("capture_current_branch");
                }
                else if (idle && now - turn.Started >= TimeSpan.FromSeconds(owner.TurnSeconds(game)) && (lastEnd is null || now - lastEnd > TimeSpan.FromSeconds(10)))
                {
                    if (!probe.GetProperty("completionArmed").GetBoolean()) await Command("experiment_snapshot_next_completion");
                    Log.LogInformation("Turn limit reached; stand-in ends seat {Seat}'s turn in {Game}", seat.Name, session);
                    await Command("experiment_end_turn"); lastEnd = now;
                }
                await Task.Delay(1000, stopping);
            }
            return true;
        }

        private async Task<bool> CaptureNow(CancellationToken stopping)
        {
            var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(60);
            while (DateTimeOffset.UtcNow < deadline && Running())
            {
                try
                {
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
                    timeout.CancelAfter(TimeSpan.FromSeconds(30));
                    var manifest = await NativeBridge.CaptureCurrentBranch(pid, timeout.Token);
                    Store(manifest);
                    return true;
                }
                catch (Exception error) when (error is InvalidOperationException or TimeoutException or IOException or OperationCanceledException && !stopping.IsCancellationRequested)
                {
                    // Busy with a dialog or battle; keep accepting dialogs and retry.
                    Log.LogInformation("Stand-in for seat {Seat} cannot be captured yet: {Error}", seat.Name, error.Message);
                    Key(0x0D);
                    await Task.Delay(2000, stopping);
                }
            }
            Log.LogWarning("Stand-in for seat {Seat} could not be captured; the player resumes from the last stored capture", seat.Name);
            return true;
        }

        private RelayCapture? Store(string manifest)
        {
            var bundle = Path.Combine(Path.GetTempPath(), $"heroespost-standin-{Guid.NewGuid():N}.hpcap");
            try
            {
                CaptureBundle.Write(manifest, bundle);
                return owner.store.Store(game, seat.Id, File.ReadAllBytes(bundle), "standin");
            }
            catch (Exception error) { Log.LogWarning("Stand-in capture of seat {Seat} was not stored: {Error}", seat.Name, error.Message); return null; }
            finally { try { File.Delete(bundle); } catch (IOException) { } }
        }

        private async Task<bool> Command(string command)
        {
            try
            {
                var reply = await NativeBridge.Request(pid, command, CancellationToken.None);
                if (reply.TryGetProperty("ok", out var ok) && ok.GetBoolean()) return true;
                Log.LogInformation("Stand-in {Command} refused: {Reply}", command, reply.ToString());
            }
            catch (Exception error) when (error is IOException or TimeoutException or OperationCanceledException) { }
            return false;
        }

        private bool Running()
        {
            try { using var process = Process.GetProcessById(pid); return !process.HasExited; }
            catch (ArgumentException) { return false; }
        }

        private void Key(int key)
        {
            try
            {
                using var process = Process.GetProcessById(pid);
                var window = process.MainWindowHandle;
                if (window == 0) return;
                var scan = key switch { 0x0D => 0x1C, 0x41 => 0x1E, _ => 0 };
                var parameter = (nint)(1 | scan << 16);
                PostMessage(window, 0x100, key, parameter);
                Thread.Sleep(100);
                PostMessage(window, 0x101, key, parameter | unchecked((nint)0xC0000000));
            }
            catch (ArgumentException) { }
        }
    }

    [DllImport("user32.dll", EntryPoint = "PostMessageW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(nint window, uint message, nint wParam, nint lParam);

    private readonly record struct Probe(int Day, int Week, int Month, int Modal, bool Combat, bool Done)
    {
        public static Probe Read(JsonElement probe)
        {
            var game = probe.GetProperty("game");
            var date = game.GetProperty("date");
            var local = game.GetProperty("localColor").GetInt32();
            var phase = probe.GetProperty("nativePhase");
            var rows = phase.GetProperty("participants");
            var done = local is >= 0 and < 8 && rows[local][1].GetInt32() == 1;
            return new(date.GetProperty("day").GetInt32(), date.GetProperty("week").GetInt32(), date.GetProperty("month").GetInt32(),
                phase.GetProperty("modalDepth").GetInt32(), game.TryGetProperty("combatActive", out var combat) && combat.GetBoolean(), done);
        }
    }

    /// <summary>The seat's turn on the current day: when it began and whether it was captured.</summary>
    private sealed class TurnState(int day, int week, int month)
    {
        private (int, int, int) date = (day, week, month);
        public DateTimeOffset Started { get; private set; } = DateTimeOffset.UtcNow;
        // The capture the stand-in resumed from already covers the day it started on.
        public bool Captured { get; set; } = true;
        public bool NewDay(Probe probe)
        {
            if ((probe.Day, probe.Week, probe.Month) == date) return false;
            date = (probe.Day, probe.Week, probe.Month);
            Started = DateTimeOffset.UtcNow; Captured = false;
            return true;
        }
    }
}
