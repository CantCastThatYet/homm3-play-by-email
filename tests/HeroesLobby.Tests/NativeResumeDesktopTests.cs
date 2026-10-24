using Avalonia.Controls;
using System.Net.Http.Json;
using Avalonia.Headless.XUnit;
using Avalonia.Interactivity;
using HeroesLobby.Contracts;
using HeroesLobby.Desktop;
using Xunit;

namespace HeroesLobby.Tests;

public sealed class NativeResumeDesktopTests
{
    private static T Find<T>(Window w, string name) where T : Control => w.FindControl<T>(name)!;
    private static async Task Until(Func<bool> ready)
    {
        var end = DateTime.UtcNow.AddSeconds(15);
        while (!ready() && DateTime.UtcNow < end) await Task.Delay(30, TestContext.Current.CancellationToken);
        Assert.True(ready());
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Tracking_failure_releases_lease_only_after_confirmed_native_fence(bool fenceSucceeds)
    {
        await using var f = new StartedGameFixture(kestrel: true); await f.Initialize();
        var lease = await f.Acquire(f.Host, 0, true);
        var capture = await f.Upload(f.Host, StartedGameFixture.Capture());
        await f.Publish(f.Host, f.Publication(lease, capture));
        (await f.Host.PostAsJsonAsync(f.Prefix + "/resume/release", new ResumeLeaseToken(lease.Token), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        var profile = new LocalProfile { NativeGameRoot = "synthetic-root", StorageDirectory = Path.Combine(Path.GetTempPath(), "heroes-fence", Guid.NewGuid().ToString("N")) };
        profile.Identities[f.Host.BaseAddress!.AbsoluteUri] = new(f.HostIdentity.Player.Id, "Red", f.HostIdentity.Token);
        await using var client = new LobbyClient(f.Host.BaseAddress.AbsoluteUri); await client.Connect(profile, "Red");
        var runtime = new FenceRuntime(fenceSucceeds); var coordinator = new NativeGameCoordinator(runtime);
        await coordinator.Resume(client, f.Game, profile);
        await Until(() => runtime.FenceCalls == 1 && coordinator.Status.Contains(fenceSucceeds ? "HotA is held" : "lease remains reserved"));
        Assert.Equal(1, runtime.Renewals);
        // Fence returns before lease release. Wait for the HTTP release in the confirmed branch.
        await Task.Delay(150, TestContext.Current.CancellationToken);
        using var retry = await f.Host.PostAsJsonAsync(f.Prefix + "/resume/acquire", new AcquireResumeLease(1), TestContext.Current.CancellationToken);
        Assert.Equal(fenceSucceeds ? System.Net.HttpStatusCode.OK : System.Net.HttpStatusCode.Conflict, retry.StatusCode);
    }

    [AvaloniaFact]
    public async Task Offline_member_can_click_resume_and_native_failure_is_visible_without_claiming_success()
    {
        await using var f = new StartedGameFixture(kestrel: true); await f.Initialize();
        var lease = await f.Acquire(f.Host, 0, true);
        var capture = await f.Upload(f.Host, StartedGameFixture.Capture());
        await f.Publish(f.Host, f.Publication(lease, capture));
        (await f.Host.PostAsJsonAsync(f.Prefix + "/resume/release", new ResumeLeaseToken(lease.Token), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        f.Peer.Dispose(); // No peer lobby window, SignalR connection, runtime, or presence heartbeat exists.
        var profile = new LocalProfile { NativeGameRoot = "synthetic-root", ServerAddress = f.Host.BaseAddress!.AbsoluteUri,
            DisplayName = "Red", StorageDirectory = Path.Combine(Path.GetTempPath(), "heroes-resume-ui", Guid.NewGuid().ToString("N")) };
        profile.Identities[profile.ServerAddress] = new(f.HostIdentity.Player.Id, "Red", f.HostIdentity.Token);
        var runtime = new UnsupportedResumeRuntime();
        var window = new MainWindow(profile, runtime); window.Show();
        try
        {
            Find<Button>(window, "ConnectButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            await Until(() => Find<ListBox>(window, "GameList").ItemCount == 1 && Find<Button>(window, "CreateButton").IsEnabled);
            Find<ListBox>(window, "GameList").SelectedIndex = 0;
            var resume = Find<Button>(window, "ResumeGameButton"); Assert.True(resume.IsVisible); Assert.True(resume.IsEffectivelyEnabled);
            Assert.False(Find<StackPanel>(window, "SettingsPanel").IsEffectivelyEnabled);
            resume.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            await Until(() => Find<Border>(window, "MessagePanel").IsVisible && resume.IsEnabled &&
                Find<TextBlock>(window, "NativeStatus").Text?.StartsWith("Could not resume HotA:", StringComparison.Ordinal) == true);
            Assert.Equal(1, runtime.Calls);
            Assert.Contains("native_restore_not_verified", Find<TextBlock>(window, "MessageText").Text);
            Assert.StartsWith("Could not resume HotA:", Find<TextBlock>(window, "NativeStatus").Text);
            // A throwing launcher may have created a process without returning its PID.
            // Keep the lease until expiry rather than permit an unfenced concurrent copy.
            var retry = await f.Host.PostAsJsonAsync(f.Prefix + "/resume/acquire", new AcquireResumeLease(1), TestContext.Current.CancellationToken);
            Assert.Equal(System.Net.HttpStatusCode.Conflict, retry.StatusCode);
        }
        finally { window.Close(); }
    }

    private sealed class UnsupportedResumeRuntime : INativeGameRuntime
    {
        public int Calls;
        public string AdapterBuild => StartedGameFixture.Build;
        public void Validate(string root, string map) { }
        public Task<int> Resume(string root, string archivePath, int color, CancellationToken cancellation)
        {
            Assert.True(File.Exists(archivePath)); Assert.Equal(0, color); Calls++;
            return Task.FromException<int>(new InvalidOperationException("native_restore_not_verified"));
        }
        public Task<int> Launch(string root, CancellationToken c) => throw new InvalidOperationException("Resume must not start a new game.");
        public Task<uint> Connect(int pid, string root, NativeLaunch run, bool host, int color, CancellationToken c) => throw new NotSupportedException();
        public Task Start(int pid, string root, NativeLaunch run, bool host, CancellationToken c) => throw new NotSupportedException();
        public Task WaitForMap(int pid, int color, CancellationToken c) => throw new NotSupportedException();
    }

    private sealed class FenceRuntime(bool fenceSucceeds) : INativeGameRuntime
    {
        public int FenceCalls, Renewals;
        public string AdapterBuild => StartedGameFixture.Build;
        public void Validate(string root, string map) { }
        public bool IsRunning(int pid) => true;
        public Task<int> Resume(string root, string archive, int color, CancellationToken c) => Task.FromResult(1234);
        public Task RenewLeaseFence(int pid, CancellationToken c) { Renewals++; return Task.CompletedTask; }
        public Task Fence(int pid, CancellationToken c) { Interlocked.Increment(ref FenceCalls); return fenceSucceeds ? Task.CompletedTask : Task.FromException(new IOException("pipe unavailable")); }
        public Task<string?> PollCompletedCapture(int pid, CancellationToken c) => Task.FromException<string?>(new IOException("tracking connection lost"));
        public Task<int> Launch(string root, CancellationToken c) => throw new NotSupportedException();
        public Task<uint> Connect(int pid, string root, NativeLaunch run, bool host, int color, CancellationToken c) => throw new NotSupportedException();
        public Task Start(int pid, string root, NativeLaunch run, bool host, CancellationToken c) => throw new NotSupportedException();
        public Task WaitForMap(int pid, int color, CancellationToken c) => throw new NotSupportedException();
    }
}
