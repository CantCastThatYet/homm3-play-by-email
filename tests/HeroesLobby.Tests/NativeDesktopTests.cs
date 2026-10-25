using System.Collections.Concurrent;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.Interactivity;
using HeroesLobby.Contracts;
using HeroesLobby.Desktop;
using Xunit;

namespace HeroesLobby.Tests;

public class NativeDesktopTests
{
    private static T Control<T>(Window window, string name) where T : Control => window.FindControl<T>(name)!;
    private static void Click(Window window, string name)
    {
        var button = Control<Button>(window, name); Assert.True(button.IsEffectivelyEnabled); button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
    }
    private static async Task Until(Func<bool> ready)
    {
        var deadline = DateTime.UtcNow.AddSeconds(150);
        while (!ready() && DateTime.UtcNow < deadline) await Task.Delay(100, TestContext.Current.CancellationToken);
        Assert.True(ready(), "Lobby/native startup did not finish before the test deadline.");
    }
    [AvaloniaFact]
    public async Task Two_ready_lobby_windows_coordinate_one_native_start_without_extra_commands()
    {
        var live = Environment.GetEnvironmentVariable("HEROES_POST_NATIVE_PROOF") == "1";
        var folder = Path.Combine(Path.GetTempPath(), "heroes-post-native-ui", Guid.NewGuid().ToString("N")); Directory.CreateDirectory(folder);
        await using var server = new LobbyFactory(); server.UseKestrel(0); using var api = server.CreateClient();
        var profiles = new List<LocalProfile>(); var identities = new List<PlayerCredentials>();
        for (var i = 0; i < 2; ++i)
        {
            var result = await api.PostAsJsonAsync("api/players", new RegisterPlayer(i == 0 ? "Red" : "Blue"), TestContext.Current.CancellationToken);
            var identity = (await result.Content.ReadFromJsonAsync<PlayerCredentials>(TestContext.Current.CancellationToken))!; identities.Add(identity);
            var root = live ? Environment.GetEnvironmentVariable(i == 0 ? "HEROES_POST_NATIVE_HOST_ROOT" : "HEROES_POST_NATIVE_PEER_ROOT")! : Path.Combine(folder, "game" + i);
            Assert.False(string.IsNullOrWhiteSpace(root)); Directory.CreateDirectory(root);
            var profile = new LocalProfile { ServerAddress = api.BaseAddress!.AbsoluteUri, DisplayName = identity.Player.DisplayName,
                NativeGameRoot = root, HostAddress = "127.0.0.1", StorageDirectory = Path.Combine(folder, "profile" + i) };
            profile.Identities[api.BaseAddress.AbsoluteUri] = new(identity.Player.Id, identity.Player.DisplayName, identity.Token); profiles.Add(profile);
        }
        api.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", identities[0].Token);
        var created = await api.PostAsJsonAsync("api/games", new CreateGame("Lobby startup proof", "Judgement Day.h3m", 2, null), TestContext.Current.CancellationToken);
        var game = (await created.Content.ReadFromJsonAsync<GameInfo>(TestContext.Current.CancellationToken))!;
        api.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", identities[1].Token);
        (await api.PostAsJsonAsync($"api/games/{game.Id}/join", new JoinGame(null), TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
        var fake = new RecordingRuntime(); INativeGameRuntime runtime = live ? new NativeGameRuntime() : fake;
        var windows = profiles.Select(p => new MainWindow(p, runtime)).ToArray();
        foreach (var window in windows) { window.Show(); Click(window, "ConnectButton"); }
        try
        {
            await Until(() => windows.All(w => Control<ListBox>(w, "GameList").ItemCount == 1 && Control<Button>(w, "CreateButton").IsEffectivelyEnabled));
            foreach (var window in windows) Control<ListBox>(window, "GameList").SelectedIndex = 0;
            Assert.False(Control<Button>(windows[0], "StartGameButton").IsEnabled);
            Assert.True(Control<TextBlock>(windows[0], "StartRequirements").IsVisible);
            Assert.Contains("Waiting for ready: Red, Blue", Control<TextBlock>(windows[0], "StartRequirements").Text);
            Assert.Contains("HotA setup not confirmed: Red, Blue", Control<TextBlock>(windows[0], "StartRequirements").Text);
            Click(windows[0], "ReadyButton");
            await Until(() => Control<ItemsControl>(windows[1], "Members").Items.OfType<MemberRow>().Any(m => m.Member.Color == 0 && m.Member.Ready));
            await Until(() => Control<TextBlock>(windows[0], "StartRequirements").Text?.Contains("Waiting for ready: Blue.", StringComparison.Ordinal) == true);
            Assert.False(Control<Button>(windows[0], "StartGameButton").IsEnabled);
            Click(windows[1], "ReadyButton");
            await Until(() => windows.All(w => Control<TextBlock>(w, "DetailStatus").Text == "ON THE MAP") || windows.Any(w => Control<TextBlock>(w, "DetailStatus").Text == "START FAILED"));
            foreach (var window in windows) Assert.True(Control<TextBlock>(window, "DetailStatus").Text == "ON THE MAP", Control<TextBlock>(window, "NativeStatus").Text);
            foreach (var window in windows) Click(window, "RefreshButton");
            if (!live) { Assert.Equal(2, fake.Launches); Assert.Equal(2, fake.Starts); Assert.True(fake.PeerArmed); }
            var run = (await api.GetFromJsonAsync<NativeLaunch>($"api/games/{game.Id}/native", TestContext.Current.CancellationToken))!;
            Assert.Equal("Playing", run.Status); Assert.All(run.Players, p => Assert.Equal("Started", p.Phase));
            if (live)
            {
                var output = Environment.GetEnvironmentVariable("HEROES_POST_NATIVE_PROOF_OUTPUT") ?? throw new InvalidOperationException("Select the live proof artifact path.");
                File.WriteAllText(output, JsonSerializer.Serialize(run, LobbyJsonContext.Default.NativeLaunch));
            }
        }
        finally { foreach (var window in windows) window.Close(); }
    }
    private sealed class RecordingRuntime : INativeGameRuntime
    {
        public int Launches, Starts; public bool PeerArmed;
        private readonly TaskCompletionSource map = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public void Validate(string root, string name) { Assert.Equal("Judgement Day.h3m", name); }
        public Task<int> Launch(string root, CancellationToken cancellation) => Task.FromResult(Interlocked.Increment(ref Launches));
        public Task<uint> Connect(int pid, string root, NativeLaunch run, bool host, int color, CancellationToken cancellation) => Task.FromResult(host ? 1u : 22u);
        public Task Start(int pid, string root, NativeLaunch run, bool host, CancellationToken cancellation)
        {
            Interlocked.Increment(ref Starts); Assert.All(run.Players, p => Assert.NotEqual(0u, p.NativePlayerId));
            if (host) { Assert.True(PeerArmed); map.SetResult(); } else PeerArmed = true;
            return Task.CompletedTask;
        }
        public Task WaitForMap(int pid, int color, CancellationToken cancellation) => map.Task.WaitAsync(cancellation);
    }
}
