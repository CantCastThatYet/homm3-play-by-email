using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using HeroesLobby.Desktop;
using HeroesLobby.NativeHost;
using HeroesLobby.Server.Relay;
using Microsoft.Extensions.DependencyInjection;
using Xunit;

[assembly: AvaloniaTestApplication(typeof(HeroesLobby.Tests.TestAppBuilder))]

namespace HeroesLobby.Tests;

public class TestAppBuilder
{
    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>().UseSkia()
        .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false });
}

public class DesktopTests
{
    private static T Control<T>(Window window, string name) where T : Control => window.FindControl<T>(name)!;
    private static void Click(Button button) { Assert.True(button.IsEffectivelyEnabled); button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)); }
    private static async Task Until(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow.AddSeconds(15);
        while (!condition() && DateTime.UtcNow < deadline) await Task.Delay(25, cancellationToken: TestContext.Current.CancellationToken);
        Assert.True(condition(), "Desktop did not reach the expected state.");
    }
    private static void Capture(Window window, string name)
    {
        var root = new DirectoryInfo(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, "global.json"))) root = root.Parent;
        var folder = Path.Combine(root!.FullName, "artifacts", "screenshots"); Directory.CreateDirectory(folder);
        using var image = window.CaptureRenderedFrame(); Assert.NotNull(image); image.Save(Path.Combine(folder, name), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);
    }

    /// <summary>Records launches instead of starting HotA; Keep returns when the test closes "HotA".</summary>
    private sealed class FakeLauncher : IHotaLauncher
    {
        public RelayEndpoint? Played;
        public readonly TaskCompletionSource Closed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task<int> Play(string root, RelayEndpoint relay, CancellationToken cancellation) { Played = relay; return Task.FromResult(4242); }
        public Task<int> Continue(string root, RelayEndpoint relay, RelayApi api, Guid game, string downloads, CancellationToken cancellation) =>
            throw new InvalidOperationException("not expected");
        public async Task Keep(int pid, RelayApi api, Action<string> report, CancellationToken cancellation)
        {
            report("Position saved on the server.");
            await Closed.Task.WaitAsync(cancellation);
        }
    }

    private sealed class Peer : IRelayPeer
    {
        public string Label => "hota";
        public void Post(byte[] frame) { }
    }

    [AvaloniaFact]
    public async Task Desktop_plays_through_the_relay_and_lists_the_players_games()
    {
        var prior = Environment.GetEnvironmentVariable("HEROES_POST_PROFILE");
        var profileDirectory = Path.Combine(Path.GetTempPath(), "heroes-post-ui", Guid.NewGuid().ToString("N"));
        Environment.SetEnvironmentVariable("HEROES_POST_PROFILE", profileDirectory);
        await using var server = new LobbyFactory(); server.UseKestrel(0); using var api = server.CreateClient();
        var launcher = new FakeLauncher();
        var window = new MainWindow(new LocalProfile { NativeGameRoot = "C:/HotA-copy", StorageDirectory = profileDirectory }, launcher); window.Show();
        try
        {
            Capture(window, "01-disconnected.png");
            Assert.False(Control<Button>(window, "PlayButton").IsEnabled);
            Control<TextBox>(window, "ServerAddress").Text = api.BaseAddress!.AbsoluteUri;
            Control<TextBox>(window, "PlayerName").Text = "Rowan";
            Click(Control<Button>(window, "ConnectButton"));
            await Until(() => Control<Button>(window, "PlayButton").IsEffectivelyEnabled);
            Assert.True(Control<StackPanel>(window, "EmptyList").IsVisible);

            // Play opens HotA on the server's relay as this player.
            Click(Control<Button>(window, "PlayButton"));
            await Until(() => launcher.Played is not null && Control<TextBlock>(window, "GameStatus").Text == "Position saved on the server.");
            var relay = server.Services.GetRequiredService<RelayServer>();
            Assert.Equal(relay.Port, launcher.Played!.Port);
            Assert.Equal(api.BaseAddress.Host, launcher.Played.Host);
            Assert.False(Control<Button>(window, "PlayButton").IsEnabled);

            // That HotA hosts a game in its own multiplayer screen, which starts, then closes.
            var hub = server.Services.GetRequiredService<RelayHub>();
            var hota = new Peer();
            static byte[] Body(FrameWriter w) => w.Finish()[4..];
            hub.Handle(hota, Body(new FrameWriter(RelayProtocol.Hello).U32(RelayProtocol.Version).Str(launcher.Played.Ticket).Str("rowan")));
            hub.Handle(hota, Body(new FrameWriter(RelayProtocol.Open).U32(1).U32(RelayProtocol.OpenCreate).Guid(Guid.NewGuid()).Guid(Guid.Empty)
                .U32(RelayProtocol.SessionMigrateHost).U32(8).U32(0).Str("Sunday\u00fcRowan").Str("").U32(0).U32(0).U32(0).U32(0)));
            hub.Handle(hota, Body(new FrameWriter(RelayProtocol.CreatePlayer).U32(2).U32(RelayProtocol.PlayerServer).Str("Rowan").Str("Rowan").Blob([])));
            byte[] start = [.. BitConverter.GetBytes(0u), .. BitConverter.GetBytes(1u), .. BitConverter.GetBytes(GameMessage.LoadGame), .. BitConverter.GetBytes(24u), .. new byte[8]];
            hub.Handle(hota, Body(new FrameWriter(RelayProtocol.Send).U32(RelayProtocol.ServerPlayerId).U32(0).U32(0).Blob(start)));
            launcher.Closed.SetResult(); hub.Disconnected(hota);
            await Until(() => Control<Button>(window, "PlayButton").IsEffectivelyEnabled);
            Click(Control<Button>(window, "RefreshButton"));
            await Until(() => Control<ItemsControl>(window, "GameList").ItemCount == 1);
            var row = Control<ItemsControl>(window, "GameList").Items.OfType<RelayGameRow>().Single();
            Assert.Equal("Sunday", row.Title);
            Assert.Equal("Away", row.Game.State);
            Assert.False(row.CanContinue); // nothing saved for the seat yet
            Capture(window, "02-your-games.png");

            // The turn limit an away seat's stand-in waits is chosen per game.
            var limit = window.GetVisualDescendants().OfType<ComboBox>().Single();
            Assert.Equal("10 minutes", limit.SelectedItem); // the server default
            limit.SelectedIndex = 4;
            await Until(() => hub.Games().Single().TurnSeconds == 3600);
        }
        catch (Exception error)
        {
            Capture(window, "failure.png");
            throw new InvalidOperationException($"{error.Message} UI message: {Control<TextBlock>(window, "MessageText").Text}", error);
        }
        finally { window.Close(); Environment.SetEnvironmentVariable("HEROES_POST_PROFILE", prior); }
    }
}
