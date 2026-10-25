using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace HeroesLobby.NativeHost;

public static partial class NativeBridge
{
    public const string Marker = ".heroespost-owned-copy";
    public const string MarkerContent = "HeroesPost.Native.v1";
    public const string Executable = "h3hota HD.exe";
    // This is an experimental adapter for one inspected build, not a general HotA SDK.
    public static IReadOnlyDictionary<string, string> Fingerprints { get; } = new Dictionary<string, string>
    {
        [Executable] = "5AAAB925F06CCCF23BB09814767590A95B84A557EB33D244800520BE4F1F18DE",
        ["_HD3_.dll"] = "3ED1852E08BAE7BE2461B31BD922531CCB0C46D01E5B0ECF46D0E7D0DBEEAB49",
        ["HD_HOTA.dll"] = "0A2C193D0C510D77080B669A710694EF282761B0EC6F6AC5B86F3166B8038550",
        ["HW_HOTA.dll"] = "927A087C6D05D40C83DE270516F48FDBD1360614259D30689179C1F7300F08DB",
        ["_HD3_Data/HotA.dll_181fix"] = "1FA9AD209E756D55E9578B290C4FE6C9BA030D3B52AF2085E530416A61AC10C2",
        ["patcher_x86.dll"] = "6134C353E5AC2A056E52BF548CDEA853C4424FA5F955DF1F5A1F073D784BAB40"
    };

    public static void VerifyBuild(string root)
    {
        foreach (var (relative, expected) in Fingerprints)
        {
            var path = Path.Combine(root, relative);
            RejectLink(path);
            using var file = File.OpenRead(path);
            var actual = Convert.ToHexString(SHA256.HashData(file));
            if (actual != expected) throw new InvalidOperationException($"Unsupported build: {relative} has SHA-256 {actual}. No game hooks were enabled.");
        }
    }

    public static string Prepare(string source, string destination)
    {
        source = Path.GetFullPath(source); destination = Path.GetFullPath(destination);
        var compare = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        if (source.Equals(destination, compare) || destination.StartsWith(source.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, compare) ||
            source.StartsWith(destination.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, compare))
            throw new InvalidOperationException("The source and isolated copy must be separate, non-nested directories.");
        if (Directory.Exists(destination) || File.Exists(destination)) throw new InvalidOperationException("Choose a new destination directory. Existing installations are never overwritten.");
        RejectLink(source); VerifyBuild(source);
        // A marker is written only after a complete copy and verification. A failed copy is
        // retained for inspection and cannot be launched by this tool.
        Directory.CreateDirectory(destination);
        CopyDirectory(source, destination, true);
        Directory.CreateDirectory(Path.Combine(destination, "Games"));
        VerifyBuild(destination);
        File.WriteAllText(Path.Combine(destination, Marker), MarkerContent, new UTF8Encoding(false));
        return destination;
    }

    private static void CopyDirectory(string source, string destination, bool top)
    {
        foreach (var entry in Directory.EnumerateFileSystemEntries(source))
        {
            var name = Path.GetFileName(entry);
            if (top && (name.Equals("games", StringComparison.OrdinalIgnoreCase) || name.Equals("saves", StringComparison.OrdinalIgnoreCase) ||
                name.Equals(Marker, StringComparison.OrdinalIgnoreCase))) continue;
            RejectLink(entry);
            var target = Path.Combine(destination, name);
            if (Directory.Exists(entry)) { Directory.CreateDirectory(target); CopyDirectory(entry, target, false); }
            else File.Copy(entry, target, false);
        }
    }

    private static void RejectLink(string path)
    {
        for (var current = Path.GetFullPath(path); current is not null; current = Path.GetDirectoryName(current))
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidOperationException($"Linked paths are not accepted for the isolated native experiment: {current}");
    }

    public static async Task<JsonElement> Request(int pid, string command, CancellationToken cancellationToken)
    {
        using var requestDeadline=CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        requestDeadline.CancelAfter(TimeSpan.FromSeconds(5));
        cancellationToken=requestDeadline.Token;
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("The original HotA adapter currently requires Windows.");
        if (pid <= 0 || command is not ("probe" or "hold_session" or "renew_lease_fence" or "enable_telemetry" or "capture_next_save" or "capture_current_branch" or "resume_session" or "capture_next_completion" or "experiment_snapshot_next_completion" or "bootstrap_session" or "cancel_bootstrap" or "start_session" or "cancel_start" or "experiment_end_turn" or "experiment_hold_completed_departure" or "experiment_acknowledge_known_intro" or "experiment_leave_completed_session" or "experiment_lobby_click") && !System.Text.RegularExpressions.Regex.IsMatch(command, "^experiment_click_tile_[0-9]{1,3}_[0-9]{1,3}_[01]$")) throw new ArgumentException("Unsupported native adapter request.");
        using var pipe = new NamedPipeClientStream(".", $"HeroesPost.Native.v1.{pid}", PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await pipe.ConnectAsync(cancellationToken);
        var request = Encoding.UTF8.GetBytes("{\"command\":\"" + command + "\"}\n");
        await pipe.WriteAsync(request, cancellationToken); await pipe.FlushAsync(cancellationToken);
        using var reader = new StreamReader(pipe, Encoding.UTF8, false, 4096, true);
        var buffer = new StringBuilder(); var character = new char[1];
        while (buffer.Length < 1024 * 1024)
        {
            if (await reader.ReadAsync(character.AsMemory(), cancellationToken) == 0 || character[0] == '\n')
            {
                using var document = JsonDocument.Parse(buffer.ToString());
                return document.RootElement.Clone();
            }
            buffer.Append(character[0]);
        }
        throw new InvalidDataException("Native adapter response exceeded 1 MiB.");
    }

    public static async Task<int> Launch(string root, string nativeDirectory, CancellationToken cancellationToken, Action<int,long>? onLaunched=null, bool armSaveCapture=true)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("The original HotA adapter currently requires Windows.");
        root = Path.GetFullPath(root); nativeDirectory = Path.GetFullPath(nativeDirectory);
        RejectLink(root); RejectLink(nativeDirectory);
        if (File.ReadAllText(Path.Combine(root, Marker)).Trim() != MarkerContent) throw new InvalidOperationException("This is not an adapter-owned test installation.");
        VerifyBuild(root);
        var dll = Path.Combine(nativeDirectory, "HeroesPost.Native.dll");
        var injector = Path.Combine(nativeDirectory, "HeroesPost.Injector.exe");
        if (!File.Exists(dll) || !File.Exists(injector)) throw new FileNotFoundException("Build native/build.ps1 before launching the adapter experiment.");
        var canonicalRoot = Path.GetDirectoryName(NativePath.FinalFilePath(Path.Combine(root, Executable)))!;
        foreach (var existing in Process.GetProcessesByName("h3hota HD").Concat(Process.GetProcessesByName("h3hota")))
        {
            using (existing)
            {
                if (existing.HasExited) continue;
                string existingRoot;
                try
                {
                    var existingExecutable = existing.MainModule?.FileName
                        ?? throw new InvalidOperationException("The process executable path is unavailable.");
                    existingExecutable = Path.GetFullPath(existingExecutable);
                    RejectLink(existingExecutable);
                    existingExecutable = NativePath.FinalFilePath(existingExecutable);
                    RejectLink(existingExecutable);
                    existingRoot = Path.GetDirectoryName(existingExecutable)
                        ?? throw new InvalidOperationException("The process installation directory is unavailable.");
                    var existingMarker = Path.Combine(existingRoot, Marker);
                    RejectLink(existingMarker);
                    if (File.ReadAllText(existingMarker).Trim() != MarkerContent)
                        throw new InvalidOperationException("The running client is not an adapter-owned isolated copy.");
                }
                catch (Exception error) when (error is System.ComponentModel.Win32Exception or IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or NotSupportedException)
                {
                    // A process disappearing during inspection is no longer a collision.
                    if (existing.HasExited) continue;
                    throw new InvalidOperationException($"Cannot safely inspect running HotA client PID {existing.Id}: {error.Message} Close that client before this test; this tool will not stop it.", error);
                }
                if (Path.TrimEndingDirectorySeparator(existingRoot).Equals(Path.TrimEndingDirectorySeparator(canonicalRoot), StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException($"HotA client PID {existing.Id} already uses this isolated installation. Choose a different prepared directory; this tool will not stop it.");
            }
        }
        // Native startup cannot reach its menu callback while the opening movie
        // runs. Change only this already validated, unused adapter-owned copy.
        var settingsDirectory = Path.Combine(root, "_HD3_Data", "Settings");
        RejectLink(Path.Combine(root, "_HD3_Data")); RejectLink(settingsDirectory);
        var settingsPath = Path.Combine(settingsDirectory, "hota.ini");
        RejectLink(settingsPath);
        var settingsBytes = File.ReadAllBytes(settingsPath);
        var settingsText = System.Text.Encoding.Latin1.GetString(settingsBytes);
        // HD's start-up update check replaces HD_HOTA.dll with a newer, unsupported
        // build and restarts through its launcher; owned copies stay pinned.
        var startupSettings = settingsText.Replace("<Show Intro> = 1", "<Show Intro> = 0", StringComparison.Ordinal)
            .Replace("<Update.CheckAtStart> = 1", "<Update.CheckAtStart> = 0", StringComparison.Ordinal);
        if (startupSettings != settingsText) File.WriteAllBytes(settingsPath, System.Text.Encoding.Latin1.GetBytes(startupSettings));
        var executable = Path.Combine(root, Executable);
        using var game = Process.Start(new ProcessStartInfo(executable) { WorkingDirectory = root, UseShellExecute = false })
            ?? throw new InvalidOperationException("Could not start the isolated game.");
        Console.WriteLine($"Isolated game PID: {game.Id}");
        var start = game.StartTime.ToUniversalTime().ToFileTimeUtc().ToString(System.Globalization.CultureInfo.InvariantCulture);
        onLaunched?.Invoke(game.Id,game.StartTime.ToUniversalTime().ToFileTimeUtc());
        var loader = new ProcessStartInfo(injector) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = nativeDirectory,
            RedirectStandardOutput=true, RedirectStandardError=true };
        foreach (var arg in new[] { "--pid", game.Id.ToString(), "--expected-exe", executable, "--dll", dll, "--created-filetime", start, "--isolated-root", root }) loader.ArgumentList.Add(arg);
        using var injection = Process.Start(loader) ?? throw new InvalidOperationException("Could not start the native loader.");
        var loaderOutput=injection.StandardOutput.ReadToEndAsync(cancellationToken);
        var loaderError=injection.StandardError.ReadToEndAsync(cancellationToken);
        await injection.WaitForExitAsync(cancellationToken);
        var loaderDetail=(await loaderOutput)+(await loaderError);
        if (injection.ExitCode != 0) throw new InvalidOperationException($"Native loader exited with {injection.ExitCode}: {loaderDetail.Trim()}. The test game was left open for inspection.");
        JsonElement probe = default;
        var deadline = DateTime.UtcNow.AddSeconds(45);
        while (!game.HasExited && DateTime.UtcNow < deadline)
        {
            try
            {
                using var requestTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken); requestTimeout.CancelAfter(TimeSpan.FromSeconds(2));
                probe = await Request(game.Id, "probe", requestTimeout.Token);
                if (probe.TryGetProperty("fingerprintAccepted", out var accepted) && accepted.ValueKind == JsonValueKind.True) break;
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { }
            catch (IOException) { }
            await Task.Delay(250, cancellationToken);
        }
        if (probe.ValueKind == JsonValueKind.Undefined || !probe.TryGetProperty("fingerprintAccepted", out var ready) || ready.ValueKind != JsonValueKind.True)
            throw new InvalidOperationException($"The adapter did not accept the running build. Last probe: {probe}. The test game was left open.");
        Console.WriteLine(probe);
        var enabled = await Request(game.Id, "enable_telemetry", cancellationToken);
        // Modules may finish loading before the native window/message pump exists.
        deadline = DateTime.UtcNow.AddSeconds(30);
        while (enabled.TryGetProperty("error", out var error) && error.GetProperty("code").GetString() == "not_ready" && DateTime.UtcNow < deadline)
        {
            await Task.Delay(250, cancellationToken);
            enabled = await Request(game.Id, "enable_telemetry", cancellationToken);
        }
        Console.WriteLine(enabled);
        if (!enabled.TryGetProperty("ok", out var ok) || ok.ValueKind != JsonValueKind.True) throw new InvalidOperationException("Native telemetry could not be enabled.");
        deadline = DateTime.UtcNow.AddSeconds(20);
        while (DateTime.UtcNow < deadline)
        {
            probe = await Request(game.Id, "probe", cancellationToken);
            var telemetry = probe.GetProperty("telemetry");
            if (telemetry.GetProperty("status").GetString() == "active") break;
            if (telemetry.GetProperty("status").GetString() == "failed") throw new InvalidOperationException($"Native hooks failed: {telemetry}");
            await Task.Delay(100, cancellationToken);
        }
        if (probe.GetProperty("telemetry").GetProperty("status").GetString() != "active")
            throw new InvalidOperationException("The game thread did not acknowledge native hooks. The test client was left open.");
        if (!armSaveCapture) return game.Id;
        var capture = await Request(game.Id, "capture_next_save", cancellationToken); Console.WriteLine(capture);
        if (!capture.TryGetProperty("ok", out var armed) || armed.ValueKind != JsonValueKind.True)
            throw new InvalidOperationException("Native capture could not be armed; inspect the response above.");
        Console.WriteLine("Native observation armed. Create/load a test game through HotA, play a move, then save using HotA. This does not start an asynchronous session yet.");
        return game.Id;
    }

    public static async Task WatchSave(int pid, string name, CancellationToken cancellationToken)
    {
        // Development experiment: an initial native autosave can consume a one-shot
        // capture before the requested manual save. Re-arm between completed captures.
        string? priorStatus = null;
        while (true)
        {
            var probe = await Request(pid, "probe", cancellationToken);
            var status = probe.GetProperty("captureStatus").GetString();
            if (status != priorStatus) { Console.WriteLine($"Capture: {status}; native save: {probe.GetProperty("lastNativeSave").GetProperty("name").GetString()}"); priorStatus = status; }
            if (status == "failed") throw new InvalidOperationException(probe.GetProperty("captureError").GetString());
            if (status == "written")
            {
                var manifest = probe.GetProperty("lastCaptureManifest").GetString()!;
                using var data = JsonDocument.Parse(File.ReadAllText(manifest));
                if (data.RootElement.TryGetProperty("native_save", out var save) &&
                    save.TryGetProperty("name", out var savedName) && string.Equals(Path.GetFileNameWithoutExtension(savedName.GetString()), name, StringComparison.OrdinalIgnoreCase))
                { Console.WriteLine($"Captured requested native save: {manifest}"); return; }
                Console.WriteLine(await Request(pid, "capture_next_save", cancellationToken));
            }
            else if (status == "idle") Console.WriteLine(await Request(pid, "capture_next_save", cancellationToken));
            await Task.Delay(200, cancellationToken);
        }
    }

    public static async Task<JsonElement> BootstrapNetwork(int pid, string isolatedRoot, BootstrapRequest request, CancellationToken cancellationToken)
    {
        var probe = await Request(pid, "probe", cancellationToken);
        if (!probe.GetProperty("fingerprintAccepted").GetBoolean() || probe.GetProperty("telemetry").GetProperty("status").GetString() != "active")
            throw new InvalidOperationException("The pinned native adapter must be active before session setup.");
        var expected = NativePath.FinalFilePath(Path.Combine(Path.GetFullPath(isolatedRoot), Executable));
        var actual = NativePath.FinalFilePath(probe.GetProperty("modules")[0].GetProperty("path").GetString()!);
        if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("The requested PID belongs to a different installation.");
        var created = probe.GetProperty("createdFiletime").GetString();
        var menuDeadline = DateTime.UtcNow.AddSeconds(30);
        while (probe.TryGetProperty("nativeMenuReady", out var menuReady) && !menuReady.GetBoolean())
        {
            if (DateTime.UtcNow >= menuDeadline) throw new TimeoutException("The native main menu is not ready for a new session.");
            await Task.Delay(200, cancellationToken);
            probe = await Request(pid, "probe", cancellationToken);
            if (probe.GetProperty("createdFiletime").GetString() != created) throw new InvalidOperationException("The native process changed before setup.");
        }
        request.Stage(isolatedRoot);
        var result = await Request(pid, "bootstrap_session", cancellationToken);
        if (!result.GetProperty("ok").GetBoolean()) throw new InvalidOperationException($"Native bootstrap refused: {result}");
        var deadline = DateTime.UtcNow.AddSeconds(45);
        string? prior = null;
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(100, cancellationToken);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken); timeout.CancelAfter(TimeSpan.FromSeconds(3));
            probe = await Request(pid, "probe", timeout.Token);
            if (probe.GetProperty("createdFiletime").GetString() != created) throw new InvalidOperationException("The native process changed during setup.");
            var state = probe.GetProperty("sessionBootstrap"); var status = state.GetProperty("status").GetString();
            if (status != prior) { Console.WriteLine($"Native session: {status}"); prior = status; }
            if (status == "failed") throw new InvalidOperationException($"Native setup failed: {state.GetProperty("error").GetString()}");
            if (state.GetProperty("menuStatus").GetInt32() == 4)
            {
                await Request(pid, "cancel_bootstrap", cancellationToken);
                throw new InvalidOperationException("The native main menu changed before setup could begin.");
            }
            if (status == "awaiting_scenario_configuration") return state.Clone();
        }
        throw new TimeoutException("Native setup did not reach the scenario stage. Inspect the native status before retrying; the client was left open.");
    }

    public static async Task<JsonElement> StartNetwork(int pid, string root, StartRequest request, CancellationToken cancellationToken)
    {
        var probe = await Request(pid, "probe", cancellationToken);
        if (!probe.GetProperty("fingerprintAccepted").GetBoolean() || probe.GetProperty("telemetry").GetProperty("status").GetString() != "active")
            throw new InvalidOperationException("The native adapter is not active for the pinned build.");
        var executable = probe.GetProperty("modules").EnumerateArray().Single(m => m.GetProperty("name").GetString() == Executable).GetProperty("path").GetString()!;
        if (!NativePath.FinalFilePath(executable).Equals(NativePath.FinalFilePath(Path.Combine(root, Executable)), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The native PID belongs to a different installation.");
        var created = probe.GetProperty("createdFiletime").GetString();
        request.Stage(root);
        var queued = await Request(pid, "start_session", cancellationToken);
        if (!queued.GetProperty("ok").GetBoolean()) throw new InvalidOperationException($"Native start was rejected: {queued}");
        var deadline = DateTime.UtcNow.AddSeconds(90); string? prior = null;
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(200, cancellationToken);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken); timeout.CancelAfter(TimeSpan.FromSeconds(3));
            probe = await Request(pid, "probe", timeout.Token);
            if (probe.GetProperty("createdFiletime").GetString() != created) throw new InvalidOperationException("The native process changed during start.");
            var state = probe.GetProperty("nativeStart"); var status = state.GetProperty("status").GetString();
            if (status != prior) { Console.WriteLine($"Native start: {status}; {state.GetProperty("detail").GetString()}"); prior = status; }
            if (status is "refused" or "cancelled") throw new InvalidOperationException($"Native start did not complete: {state}");
            if ((request.Action == "PeerAccept" && status == "peer_armed" && state.GetProperty("setupReady").GetBoolean()) || status is "host_start_returned" or "peer_accepted") return state.Clone();
        }
        throw new TimeoutException("Native start is still pending. Inspect the client and nativeStart status before retrying.");
    }

    public static async Task<string> WatchCompletion(int pid, bool serializeWorld, CancellationToken cancellationToken)
    {
        var initial = await Request(pid, "probe", cancellationToken);
        var created = initial.GetProperty("createdFiletime").GetString();
        var command = serializeWorld ? "experiment_snapshot_next_completion" : "capture_next_completion";
        var armed = await Request(pid, command, cancellationToken);
        if (!armed.GetProperty("ok").GetBoolean())
            throw new InvalidOperationException($"Could not arm completed-turn capture: {armed}");
        Console.WriteLine("Waiting for the native multiplayer completed-turn marker. This does not enable safe logout yet.");
        while (true)
        {
            await Task.Delay(200, cancellationToken);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(3));
            var probe = await Request(pid, "probe", timeout.Token);
            if (probe.GetProperty("createdFiletime").GetString() != created) throw new InvalidOperationException("The original native process has exited.");
            var status = probe.GetProperty("captureStatus").GetString();
            if (status == "failed") throw new InvalidOperationException(probe.GetProperty("captureError").GetString());
            if (status != "written") continue;
            var path = probe.GetProperty("lastCaptureManifest").GetString()!;
            CaptureBundle.CheckRegularPath(path);
            using var stream = File.OpenRead(path);
            if (stream.Length > 1024 * 1024) throw new InvalidDataException("Native manifest exceeds the expected limit.");
            using var manifest = JsonDocument.Parse(stream);
            var completion = manifest.RootElement.GetProperty("native_completion");
            if (!completion.GetProperty("observed").GetBoolean() || !completion.GetProperty("marker_confirmed").GetBoolean() ||
                completion.GetProperty("fresh_world_serialized").GetBoolean() != serializeWorld)
                throw new InvalidDataException("The capture does not match the requested native completion experiment.");
            return path;
        }
    }
}
