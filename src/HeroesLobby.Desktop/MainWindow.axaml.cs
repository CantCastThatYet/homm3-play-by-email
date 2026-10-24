using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Threading;
using HeroesLobby.Contracts;

namespace HeroesLobby.Desktop;

public record GameRow(GameInfo Game, string CurrentPlayer)
{
    public string Name => Game.Name;
    public string Map => Game.MapName;
    public string SeatCount => $"{Game.Members.Count} / {Game.Capacity}";
    public string Status => Game.Status switch { "Launching" => "STARTING HOTA", "Playing" => "IN GAME", "LaunchFailed" => "START FAILED", "Prepared" => "LOBBY LOCKED", _ => Game.Members.Any(m => m.PlayerId == CurrentPlayer) ? "YOUR GAME" : "GATHERING PLAYERS" };
    public string Access => Game.PasswordProtected ? "Password" : "Open";
}
public record MemberRow(MemberInfo Member, string HostId, string CurrentPlayer)
{
    private static readonly string[] Palette = ["#D86B66", "#749FE5", "#C9B48A", "#81BC87", "#E0A065", "#B28CCC", "#71C3BD", "#D997B1"];
    public string Name => Member.DisplayName + (Member.PlayerId == CurrentPlayer ? " · You" : "") + (Member.PlayerId == HostId ? " · Host" : "");
    public string Preferences => $"{Protocol.Colors[Member.Color]} · {Member.Faction} · {Member.Hero} · {Member.Bonus}";
    public string Ready => Member.Ready ? "READY" : "CHOOSING";
    public IBrush ColorBrush => Brush.Parse(Palette[Member.Color]);
}

public partial class MainWindow : Window
{
    private LocalProfile profile = new();
    private LobbyClient? client;
    private List<GameInfo> games = [];
    private GameInfo? selected;
    private string? selectedId;
    private string? settingsKey;
    private bool busy, refreshing, rebuilding, profileFailed;
    private readonly DispatcherTimer poll = new() { Interval = TimeSpan.FromSeconds(5) };
    private readonly NativeGameCoordinator native;
    private readonly INativeGameRuntime runtime;
    private readonly HashSet<string> launchAttempts = [];

    public MainWindow() : this(null, null) { }
    public MainWindow(LocalProfile? suppliedProfile, INativeGameRuntime? nativeRuntime)
    {
        runtime = nativeRuntime ?? new NativeGameRuntime(); native = new(runtime);
        InitializeComponent();
        ColorChoice.ItemsSource = Protocol.Colors; FactionChoice.ItemsSource = Protocol.Factions; BonusChoice.ItemsSource = Protocol.Bonuses;
        try { profile = suppliedProfile ?? LocalProfile.Load(); }
        catch (Exception ex) when (ex is IOException or System.Text.Json.JsonException or UnauthorizedAccessException)
        {
            profileFailed = true; ShowMessage($"Cannot read your saved identity: {ex.Message} Restore {LocalProfile.FilePath} before connecting.");
        }
        ServerAddress.Text = profile.ServerAddress; PlayerName.Text = profile.DisplayName;
        NativeStatus.Text = string.IsNullOrWhiteSpace(profile.NativeGameRoot) ? "Set up HotA to launch games from this lobby." : "HotA configured. Both ready players will launch automatically.";
        native.Changed += () => Dispatcher.UIThread.Post(() => { NativeStatus.Text = native.Status; UpdateConnection(); });
        poll.Tick += async (_, _) => { if (client is not null && !busy) await RefreshSafely(); };
        poll.Start();
        Closed += async (_, _) => { poll.Stop(); if (client is not null) await client.DisposeAsync(); };
    }

    private async void ConnectClick(object? sender, RoutedEventArgs e) => await Run(async () =>
    {
        if (client is not null)
        {
            var old = client; client = null; await old.DisposeAsync(); games.Clear(); selected = null; selectedId = null; settingsKey = null;
            ConnectionState.Text = "Disconnected · your seats are reserved"; ServerName.Text = "Connect to your community, then choose a game.";
        }
        else
        {
            if (profileFailed) throw new InvalidOperationException("Restore your local profile before connecting; it holds your existing player identities.");
            var pending = new LobbyClient(ServerAddress.Text ?? "");
            try
            {
                var info = await pending.Connect(profile, PlayerName.Text ?? "");
                client = pending;
                pending.Changed += () => Dispatcher.UIThread.Post(async () => { if (!busy && client == pending) await RefreshSafely(); });
                PlayerName.Text = pending.Player!.DisplayName;
                ServerName.Text = info.Name;
                await RefreshCore();
            }
            catch { if (client == pending) client = null; await pending.DisposeAsync(); throw; }
        }
        RenderList(); UpdateConnection();
    });

    private void UpdateConnection()
    {
        var connected = client is not null;
        ConnectButton.Content = connected ? "Disconnect" : "Connect to server";
        ConnectButton.IsEnabled = !busy && native.Active is not { IsCompleted: false };
        NativeSetupButton.IsEnabled = !busy && native.Active is not { IsCompleted: false };
        ServerAddress.IsEnabled = PlayerName.IsEnabled = !connected && !busy;
        CreateButton.IsEnabled = RefreshButton.IsEnabled = MineOnly.IsEnabled = connected && !busy;
        if (connected) ConnectionState.Text = $"Connected as {client!.Player!.DisplayName}\n{(client.Live ? "Live updates connected" : "Refreshing every 5 seconds")}";
    }
    private async Task RefreshCore()
    {
        if (client is null || refreshing) return;
        refreshing = true; var current = client;
        try
        {
            var result = await current.Games();
            if (client != current) return;
            games = result; RenderList(); UpdateConnection();
            foreach (var game in result.Where(g => g.Members.Any(m => m.PlayerId == current.Player!.Id)))
            {
                native.Observe(current, game, profile);
                if (current.NativeStartupAvailable && !string.IsNullOrWhiteSpace(profile.NativeGameRoot) &&
                    game.HostId == current.Player!.Id && game.Status == "Lobby" && game.Members.Count == 2 && game.Members.All(m => m.Ready && m.NativeReady) &&
                    launchAttempts.Add($"{game.Id}:{game.Revision}"))
                {
                    try { await current.BeginNative(game.Id, new(game.Revision, profile.HostAddress)); }
                    catch (Exception ex) { ShowMessage(Describe(ex)); }
                }
            }
        }
        finally { refreshing = false; }
    }
    private async Task RefreshSafely()
    {
        try { await RefreshCore(); }
        catch (Exception ex) { ShowMessage(Describe(ex)); ConnectionState.Text = "Server unreachable · retrying\nYour seat remains reserved on the server."; }
    }
    private void RenderList()
    {
        if (GameList is null) return;
        rebuilding = true;
        var id = selectedId; var text = Search.Text?.Trim() ?? ""; var player = client?.Player?.Id ?? "";
        var visible = games.Where(g => (MineOnly.IsChecked != true || g.Members.Any(m => m.PlayerId == player)) &&
            (g.Name.Contains(text, StringComparison.OrdinalIgnoreCase) || g.MapName.Contains(text, StringComparison.OrdinalIgnoreCase))).Select(g => new GameRow(g, player)).ToList();
        GameList.ItemsSource = visible;
        var row = visible.FirstOrDefault(g => g.Game.Id == id);
        GameList.SelectedItem = row; selected = row?.Game;
        EmptyList.IsVisible = visible.Count == 0;
        EmptyTitle.Text = client is null ? "The hall awaits" : games.Count == 0 ? "Be the first to gather" : "No matching games";
        EmptyDescription.Text = client is null ? "Connect to a server to see its games." : games.Count == 0 ? "Create a game and invite your friends to this server." : "Try another search or turn off My games.";
        GameCount.Text = client is null ? "GAME DIRECTORY" : $"{visible.Count} GAME{(visible.Count == 1 ? "" : "S")}  /  {games.Sum(g => g.Members.Count)} RESERVED SEATS";
        rebuilding = false; RenderDetail();
    }
    private void RenderDetail()
    {
        var game = selected; var me = game?.Members.FirstOrDefault(m => m.PlayerId == client?.Player?.Id);
        DetailsScroll.IsVisible = game is not null; NoSelection.IsVisible = game is null;
        if (game is null) { settingsKey = null; return; }
        var open = game.Status == "Lobby";
        DetailTitle.Text = game.Name; DetailMap.Text = $"{game.MapName} · {game.Members.Count} of {game.Capacity} seats";
        DetailStatus.Text = game.Status switch { "Launching" => "STARTING HOTA", "Playing" => "ON THE MAP", "LaunchFailed" => "START FAILED", "Prepared" => "PARTY PREPARED", _ => "ASSEMBLING THE PARTY" };
        DetailHint.Text = game.Status switch
        {
            "Launching" => "Opening and connecting both HotA clients. Keep both lobby apps open until the map appears.",
            "Playing" or "Started" => "Resume your latest saved position even when friends are offline. Close an existing HotA window first. This prototype saves completed turns but cannot advance the shared day yet.",
            "LaunchFailed" => "HotA could not start. Check the startup message on both players' apps. Create a new lobby after fixing the issue.",
            _ when client?.NativeStartupAvailable != true => "This server needs the updated startup API. Update the server to launch HotA from this lobby.",
            _ when string.IsNullOrWhiteSpace(profile.NativeGameRoot) => "Click Set up HotA first. This startup build supports Judgement Day, two players, Red/Blue, and Random starting choices.",
            _ when game.Members.Any(m => m.Ready && !m.NativeReady) => "Both players need to Set up HotA in the updated app, then mark ready again. Older ready flags do not launch a game.",
            _ => "Both ready players launch automatically. Use Judgement Day, host Red, guest Blue, and Random starting choices."
        };
        Members.ItemsSource = game.Members.OrderBy(m => m.Color).Select(m => new MemberRow(m, game.HostId, client?.Player?.Id ?? "")).ToList();
        JoinButton.IsVisible = me is null;
        JoinButton.IsEnabled = !busy && open && game.Members.Count < game.Capacity;
        JoinButton.Content = !open ? "Lobby locked" : game.Members.Count >= game.Capacity ? "All seats taken" : "Join this game";
        SettingsPanel.IsVisible = me is not null; SettingsPanel.IsEnabled = open && !busy;
        MemberActions.IsVisible = me is not null;
        ResumeGameButton.IsVisible = me is not null && game.Status is "Playing" or "Started";
        ResumeGameButton.IsEnabled = !busy && native.Active is not { IsCompleted: false };
        LeaveButton.IsEnabled = open && !busy;
        LockButton.IsVisible = game.HostId == client?.Player?.Id;
        LockButton.IsVisible = LockButton.IsVisible && game.Status is "Lobby" or "Prepared" && string.IsNullOrWhiteSpace(profile.NativeGameRoot);
        StartGameButton.IsVisible = game.HostId == client?.Player?.Id && game.Status is "Lobby" or "Prepared";
        StartGameButton.IsEnabled = !busy && client?.NativeStartupAvailable == true && game.Members.Count == 2 && game.Members.All(m => m.Ready);
        LockButton.Content = open ? "Lock ready lobby" : "Reopen lobby";
        LockButton.IsEnabled = !busy && (!open || game.Members.Count >= 2 && game.Members.All(m => m.Ready));
        if (me is null) return;
        var key = $"{game.Id}|{me.Color}|{me.Faction}|{me.Hero}|{me.Bonus}";
        if (key != settingsKey)
        {
            ColorChoice.SelectedIndex = me.Color; FactionChoice.SelectedItem = me.Faction; HeroChoice.Text = me.Hero; BonusChoice.SelectedItem = me.Bonus; settingsKey = key;
        }
        ReadyButton.Content = me.Ready ? "Not ready" : "Mark ready";
    }
    private async Task Run(Func<Task> action)
    {
        if (busy) return;
        busy = true; MessagePanel.IsVisible = false; UpdateConnection(); RenderDetail();
        try { await action(); }
        catch (Exception ex) { ShowMessage(Describe(ex)); if (client is not null) await RefreshSafely(); }
        finally { busy = false; UpdateConnection(); RenderDetail(); }
    }
    private static string Describe(Exception ex) => ex is HttpRequestException ? "Could not reach the server. Check its address and that it is running." : ex is TaskCanceledException ? "The server took too long to respond. Try again." : ex.Message;
    private void ShowMessage(string message) { MessageText.Text = message; MessagePanel.IsVisible = true; }
    private async Task SelectGame(GameInfo game) { selectedId = game.Id; selected = game; await RefreshCore(); }
    private async void RefreshClick(object? sender, RoutedEventArgs e) => await Run(RefreshCore);
    private void DismissClick(object? sender, RoutedEventArgs e) => MessagePanel.IsVisible = false;
    private void SearchChanged(object? sender, TextChangedEventArgs e) => RenderList();
    private void FilterChanged(object? sender, RoutedEventArgs e) => RenderList();
    private void SelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (rebuilding || GameList.SelectedItem is not GameRow row) return;
        // Replacing the directory can emit a deferred deselection. Only a real selection
        // should change the detail pane; RenderList handles removed/filtered games.
        selectedId = row.Game.Id;
        selected = games.FirstOrDefault(g => g.Id == selectedId);
        settingsKey = null; RenderDetail();
    }
    private async void CreateClick(object? sender, RoutedEventArgs e)
    {
        if (client is null || busy) return;
        var request = await new CreateGameWindow(!string.IsNullOrWhiteSpace(profile.NativeGameRoot)).ShowDialog<CreateGame?>(this);
        if (request is not null) await Run(async () => { var game = await client.Create(request); MineOnly.IsChecked = false; Search.Text = ""; await SelectGame(game); });
    }
    private async void JoinClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null || busy) return;
        var game = selected; string? password = null;
        if (game.PasswordProtected)
        {
            password = await PromptWindow.Ask(this, "A place among friends", $"Enter the password for {game.Name}.", "Join game", true);
            if (password is null) return;
        }
        await Run(async () => await SelectGame(await client.Join(game.Id, password)));
    }
    private async void SettingsClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null) return;
        var game = selected;
        await Run(async () => await SelectGame(await client.Settings(game.Id, new(game.Revision, ColorChoice.SelectedIndex,
            FactionChoice.SelectedItem as string ?? "Random", HeroChoice.Text ?? "Random", BonusChoice.SelectedItem as string ?? "Random"))));
    }
    private async void ReadyClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null) return;
        var game = selected; var me = game.Members.First(m => m.PlayerId == client.Player!.Id);
        if (ColorChoice.SelectedIndex != me.Color || (string?)FactionChoice.SelectedItem != me.Faction || HeroChoice.Text != me.Hero || (string?)BonusChoice.SelectedItem != me.Bonus)
        { ShowMessage("Save your changed preferences before marking yourself ready."); return; }
        await Run(async () =>
        {
            var nativeReady = !string.IsNullOrWhiteSpace(profile.NativeGameRoot);
            if (!me.Ready && nativeReady) await Task.Run(() => runtime.Validate(profile.NativeGameRoot, "Judgement Day.h3m"));
            await SelectGame(await client.Ready(game.Id, new(game.Revision, !me.Ready, nativeReady)));
        });
    }
    private async void LockClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null) return;
        var game = selected; await Run(async () => await SelectGame(await client.Action(game.Id, game.Status == "Lobby" ? "lock" : "reopen", game.Revision)));
    }
    private async void NativeSetupClick(object? sender, RoutedEventArgs e)
    {
        if (await new NativeSetupWindow(profile).ShowDialog<bool>(this))
        {
            NativeStatus.Text = "HotA configured. Both ready players will launch automatically.";
            launchAttempts.Clear(); await RefreshSafely(); RenderDetail();
        }
    }
    private async void StartGameClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null) return;
        if (string.IsNullOrWhiteSpace(profile.NativeGameRoot)) { NativeSetupClick(sender, e); return; }
        var game = selected;
        await Run(async () => { await client.BeginNative(game.Id, new(game.Revision, profile.HostAddress)); await RefreshCore(); });
    }
    private async void ResumeGameClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null || busy) return;
        var game = selected;
        await Run(async () => await native.Resume(client, game, profile));
    }
    private async void LeaveClick(object? sender, RoutedEventArgs e)
    {
        if (selected is null || client is null || busy) return;
        var game = selected;
        var confirm = await PromptWindow.Ask(this, "Release your seat?", "Disconnecting keeps your place. Leaving removes you from this game and transfers hosting if needed.", "Leave game", false);
        if (confirm is not null) await Run(async () => { await client.Action(game.Id, "leave", game.Revision); await RefreshCore(); });
    }
}
