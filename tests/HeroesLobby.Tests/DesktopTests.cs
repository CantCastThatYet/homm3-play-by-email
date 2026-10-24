using System.Net.Http.Headers;
using System.Net.Http.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using HeroesLobby.Contracts;
using HeroesLobby.Desktop;
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

    [AvaloniaFact]
    public async Task Desktop_can_connect_create_configure_lock_and_reconnect_to_persistent_lobby()
    {
        var prior = Environment.GetEnvironmentVariable("HEROES_POST_PROFILE");
        Environment.SetEnvironmentVariable("HEROES_POST_PROFILE", Path.Combine(Path.GetTempPath(), "heroes-post-ui", Guid.NewGuid().ToString("N")));
        await using var server = new LobbyFactory(); server.UseKestrel(0); using var api = server.CreateClient();
        var window = new MainWindow(); window.Show();
        try
        {
            Capture(window, "01-disconnected.png");
            Control<TextBox>(window, "ServerAddress").Text = api.BaseAddress!.AbsoluteUri;
            Control<TextBox>(window, "PlayerName").Text = "Rowan";
            Click(Control<Button>(window, "ConnectButton"));
            await Until(() => Control<Button>(window, "CreateButton").IsEffectivelyEnabled);
            Assert.False(Control<Border>(window, "MessagePanel").IsVisible);
            Click(Control<Button>(window, "CreateButton"));
            await Until(() => window.OwnedWindows.OfType<CreateGameWindow>().Any());
            var dialog = window.OwnedWindows.OfType<CreateGameWindow>().Single();
            var inputs = dialog.GetVisualDescendants().OfType<TextBox>().ToList();
            inputs[0].Text = "The Long Campaign"; inputs[1].Text = "Islands of the North - XL";
            Capture(dialog, "02-create-game.png");
            Click(dialog.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Create game")));
            await Until(() => Control<ListBox>(window, "GameList").ItemCount == 1 && Control<Button>(window, "SaveSettingsButton").IsEffectivelyEnabled);
            var game = (await api.GetFromJsonAsync<List<GameInfo>>("api/games", cancellationToken: TestContext.Current.CancellationToken))!.Single();
            var registration = await api.PostAsJsonAsync("api/players", new RegisterPlayer("Mira"), cancellationToken: TestContext.Current.CancellationToken); registration.EnsureSuccessStatusCode();
            var friend = (await registration.Content.ReadFromJsonAsync<PlayerCredentials>(cancellationToken: TestContext.Current.CancellationToken))!;
            api.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", friend.Token);
            (await api.PostAsJsonAsync($"api/games/{game.Id}/join", new JoinGame(null), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
            await Until(() => Control<ItemsControl>(window, "Members").ItemCount == 2);
            Control<ComboBox>(window, "FactionChoice").SelectedItem = "Rampart";
            Control<ComboBox>(window, "BonusChoice").SelectedItem = "Gold";
            Click(Control<Button>(window, "SaveSettingsButton"));
            await Until(() => Control<Button>(window, "ReadyButton").IsEffectivelyEnabled && Control<ItemsControl>(window, "Members").Items.OfType<MemberRow>().Any(m => m.Member.Faction == "Rampart"));
            Click(Control<Button>(window, "ReadyButton"));
            await Until(() => Equals(Control<Button>(window, "ReadyButton").Content, "Not ready") && Control<Button>(window, "ReadyButton").IsEffectivelyEnabled);
            game = (await api.GetFromJsonAsync<GameInfo>($"api/games/{game.Id}", cancellationToken: TestContext.Current.CancellationToken))!;
            (await api.PutAsJsonAsync($"api/games/{game.Id}/ready", new ReadyRequest(game.Revision, true), cancellationToken: TestContext.Current.CancellationToken)).EnsureSuccessStatusCode();
            await Until(() => Control<Button>(window, "LockButton").IsEffectivelyEnabled);
            Capture(window, "03-ready-lobby.png");
            Click(Control<Button>(window, "LockButton"));
            await Until(() => Control<TextBlock>(window, "DetailStatus").Text == "PARTY PREPARED" && Control<Button>(window, "ConnectButton").IsEffectivelyEnabled);
            Assert.False(Control<StackPanel>(window, "SettingsPanel").IsEffectivelyEnabled);
            Capture(window, "04-prepared-lobby.png");
            Click(Control<Button>(window, "ConnectButton"));
            await Until(() => Equals(Control<Button>(window, "ConnectButton").Content, "Connect to server") && Control<Button>(window, "ConnectButton").IsEffectivelyEnabled);
            Click(Control<Button>(window, "ConnectButton"));
            await Until(() => Control<ListBox>(window, "GameList").ItemCount == 1 && Control<Button>(window, "CreateButton").IsEffectivelyEnabled);
            Control<ListBox>(window, "GameList").SelectedIndex = 0;
            Assert.Equal("PARTY PREPARED", Control<TextBlock>(window, "DetailStatus").Text);
            Assert.Equal(2, Control<ItemsControl>(window, "Members").ItemCount);
            Assert.False(Control<Button>(window, "JoinButton").IsVisible);
        }
        catch (Exception error)
        {
            Capture(window, "failure.png");
            throw new InvalidOperationException($"{error.Message} UI message: {Control<TextBlock>(window, "MessageText").Text}; Ready: {Control<Button>(window, "ReadyButton").Content}", error);
        }
        finally { window.Close(); Environment.SetEnvironmentVariable("HEROES_POST_PROFILE", prior); }
    }
}
