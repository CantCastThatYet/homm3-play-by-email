using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using HeroesLobby.Contracts;
using HeroesLobby.NativeHost;

namespace HeroesLobby.Desktop;

/// <summary>A relay game in which this player has a seat, as shown in Your games.</summary>
public sealed record RelayGameRow(RelayGameInfo Game, bool HotaRunning)
{
    public static readonly (string Label, int Seconds)[] Limits =
        [("1 minute", 60), ("5 minutes", 300), ("10 minutes", 600), ("15 minutes", 900), ("1 hour", 3600), ("4 hours", 14400), ("1 day", 86400), ("3 days", 259200)];
    // HotA names a session "<game><separator><host player>".
    public string Title => new string(Game.Name.TakeWhile(c => c is >= ' ' and < (char)0x7F).ToArray()) is { Length: > 0 } title ? title : Game.Name;
    public string Players => "Seats: " + string.Join(", ", Game.Players) + " · you are " + Game.Seat;
    public string State => Game.State switch
    {
        "Lobby" => "Setting up in HotA.",
        "Playing" => "Your HotA is playing this seat.",
        "StandIn" => "You are away. The server is playing your seat: it finishes turns you ended and ends others after the turn limit.",
        _ when Game.CaptureKind is null => "You are away, and nothing of your seat is saved yet.",
        _ => "You are away. The server will play your seat until you continue.",
    };
    // CaptureDate is month/week/day.
    public string Saved => Game.CaptureDate?.Split('/') is [var month, var week, var day]
        ? $"Saved on the server: month {month}, week {week}, day {day}" + (Game.CaptureKind == "completion" ? ", turn ended" : "")
        : "Nothing saved yet";
    public List<string> LimitLabels { get; } = Limits.Select(l => l.Label).ToList();
    public int LimitIndex => Array.FindIndex(Limits, l => l.Seconds >= Game.TurnSeconds) is var i and >= 0 ? i : Limits.Length - 1;
    public bool CanContinue => !HotaRunning && Game.Started && Game.State is "Away" or "StandIn" && Game.CaptureKind is not null;
    public string Key => $"{Game.Id}|{Game.State}|{Game.CaptureKind}|{Game.CaptureDate}|{Game.TurnSeconds}|{Game.LiveClients}|{string.Join(",", Game.Players)}|{HotaRunning}";
}

public partial class MainWindow : Window
{
    private LocalProfile profile = new();
    private LobbyClient? client;
    private RelayApi? relayApi;
    private HttpClient? relayHttp;
    private RelayServerInfo? relayInfo;
    private List<RelayGameRow> rows = [];
    private bool busy, refreshing, profileFailed, rendering;
    private readonly DispatcherTimer poll = new() { Interval = TimeSpan.FromSeconds(5) };
    private readonly IHotaLauncher launcher;
    private int? hotaPid;
    private CancellationTokenSource? keeping;

    public MainWindow() : this(null, null) { }
    public MainWindow(LocalProfile? suppliedProfile, IHotaLauncher? hotaLauncher)
    {
        launcher = hotaLauncher ?? new NativeHotaLauncher();
        InitializeComponent();
        try { profile = suppliedProfile ?? LocalProfile.Load(); }
        catch (Exception ex) when (ex is IOException or System.Text.Json.JsonException or UnauthorizedAccessException)
        {
            profileFailed = true; ShowMessage($"Cannot read your saved identity: {ex.Message} Restore {LocalProfile.FilePath} before connecting.");
        }
        ServerAddress.Text = profile.ServerAddress; PlayerName.Text = profile.DisplayName;
        UpdateSetup();
        poll.Tick += async (_, _) => { if (client is not null && !busy) await RefreshSafely(); };
        poll.Start();
        Closed += async (_, _) => { poll.Stop(); keeping?.Cancel(); relayHttp?.Dispose(); if (client is not null) await client.DisposeAsync(); };
    }

    private bool HotaRunning => hotaPid is not null;

    private void UpdateSetup() => SetupState.Text = string.IsNullOrWhiteSpace(profile.NativeGameRoot)
        ? "Set up HotA to play from this app." : "HotA is ready: " + profile.NativeGameRoot;

    private async void ConnectClick(object? sender, RoutedEventArgs e) => await Run(async () =>
    {
        if (client is not null)
        {
            var old = client; client = null; relayApi = null; relayInfo = null; relayHttp?.Dispose(); relayHttp = null;
            await old.DisposeAsync(); rows = [];
            ConnectionState.Text = "Disconnected · your seats are kept"; ServerName.Text = "Connect to your community, then play.";
        }
        else
        {
            if (profileFailed) throw new InvalidOperationException("Restore your local profile before connecting; it holds your existing player identities.");
            var pending = new LobbyClient(ServerAddress.Text ?? "");
            try
            {
                var info = await pending.Connect(profile, PlayerName.Text ?? "");
                var token = profile.Identities[pending.Address.AbsoluteUri].Token;
                relayHttp = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { BaseAddress = pending.Address, Timeout = TimeSpan.FromSeconds(90) };
                relayApi = new RelayApi(relayHttp, token);
                relayInfo = await relayApi.Info(CancellationToken.None);
                client = pending;
                PlayerName.Text = pending.Player!.DisplayName;
                ServerName.Text = info.Name + (relayInfo.StandIns ? "" : " · this server has no stand-ins: an away player's turn waits for them");
                await RefreshCore();
            }
            catch { if (client == pending) client = null; await pending.DisposeAsync(); throw; }
        }
        Render();
    });

    private void UpdateControls()
    {
        var connected = client is not null;
        ConnectButton.Content = connected ? "Disconnect" : "Connect to server";
        ConnectButton.IsEnabled = NativeSetupButton.IsEnabled = !busy && !HotaRunning;
        ServerAddress.IsEnabled = PlayerName.IsEnabled = !connected && !busy;
        RefreshButton.IsEnabled = connected && !busy;
        PlayButton.IsEnabled = connected && !busy && !HotaRunning && relayInfo is not null;
        if (connected) ConnectionState.Text = $"Connected as {client!.Player!.DisplayName}";
    }

    private async Task RefreshCore()
    {
        if (relayApi is null || refreshing) return;
        refreshing = true;
        try
        {
            var games = await relayApi.Games(CancellationToken.None);
            var fresh = games.Select(g => new RelayGameRow(g, HotaRunning)).ToList();
            if (!fresh.Select(r => r.Key).SequenceEqual(rows.Select(r => r.Key))) { rows = fresh; Render(); }
        }
        finally { refreshing = false; }
    }

    private async Task RefreshSafely()
    {
        try { await RefreshCore(); }
        catch (Exception ex) { ShowMessage(Describe(ex)); ConnectionState.Text = "Server unreachable · retrying"; }
    }

    private void Render()
    {
        rendering = true;
        GameList.ItemsSource = rows.ToList();
        rendering = false;
        EmptyList.IsVisible = rows.Count == 0;
        EmptyTitle.Text = client is null ? "The hall awaits" : "No games yet";
        EmptyDescription.Text = client is null ? "Connect to a server to see your games." : "Press Play HotA, then host or join a game in HotA's TCP/IP multiplayer screen. Your games appear here.";
        GameCount.Text = client is null ? "GAMES ON THIS SERVER" : $"YOUR GAMES · {rows.Count}";
        UpdateControls();
    }

    private async Task Run(Func<Task> action)
    {
        if (busy) return;
        busy = true; MessagePanel.IsVisible = false; UpdateControls();
        try { await action(); }
        catch (Exception ex) { ShowMessage(Describe(ex)); }
        finally { busy = false; UpdateControls(); }
    }

    private static string Describe(Exception ex) => ex is HttpRequestException ? "Could not reach the server. Check its address and that it is running." :
        ex is TaskCanceledException ? "The server took too long to respond. Try again." : ex.Message;
    private void ShowMessage(string message) { MessageText.Text = message; MessagePanel.IsVisible = true; }
    private void Status(string text) => Dispatcher.UIThread.Post(() => GameStatus.Text = text);
    private async void RefreshClick(object? sender, RoutedEventArgs e) => await Run(RefreshCore);
    private void DismissClick(object? sender, RoutedEventArgs e) => MessagePanel.IsVisible = false;

    private async void NativeSetupClick(object? sender, RoutedEventArgs e)
    {
        if (await new NativeSetupWindow(profile).ShowDialog<bool>(this)) UpdateSetup();
    }

    private RelayEndpoint Endpoint() =>
        new(client!.Address.Host, relayInfo!.Port, profile.Identities[client.Address.AbsoluteUri].Token, client.Player!.DisplayName);

    private bool RequireSetup()
    {
        if (!string.IsNullOrWhiteSpace(profile.NativeGameRoot)) return true;
        ShowMessage("Click Set up HotA first: Heroes Post plays from its own copy of your HotA installation.");
        return false;
    }

    private async void PlayClick(object? sender, RoutedEventArgs e)
    {
        if (client is null || relayApi is null || HotaRunning || !RequireSetup()) return;
        await Run(async () =>
        {
            GameStatus.Text = "Opening HotA…";
            var pid = await launcher.Play(profile.NativeGameRoot, Endpoint(), CancellationToken.None);
            Keep(pid, "HotA is open. Choose New Game, Multiplayer, TCP/IP, then Host or Join.");
        });
    }

    private async void ContinueClick(object? sender, RoutedEventArgs e)
    {
        if ((sender as Control)?.DataContext is not RelayGameRow row || client is null || relayApi is null || HotaRunning || !RequireSetup()) return;
        await Run(async () =>
        {
            GameStatus.Text = $"Taking back your seat in {row.Title}. The server saves the stand-in's position first; this can take a minute…";
            var downloads = Path.Combine(profile.StorageDirectory, "downloads");
            var pid = await launcher.Continue(profile.NativeGameRoot, Endpoint(), relayApi, row.Game.Id, downloads, CancellationToken.None);
            Keep(pid, $"You are back in {row.Title}.");
            await RefreshCore();
        });
    }

    private async void TurnLimitChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (rendering || sender is not ComboBox { DataContext: RelayGameRow row } box || relayApi is null) return;
        if (box.SelectedIndex < 0 || box.SelectedIndex == row.LimitIndex) return;
        var seconds = RelayGameRow.Limits[box.SelectedIndex].Seconds;
        await Run(async () => { await relayApi.SetTurnLimit(row.Game.Id, seconds, CancellationToken.None); await RefreshCore(); });
    }

    /// <summary>Keeps the seat saved on the server until HotA closes.</summary>
    private void Keep(int pid, string message)
    {
        hotaPid = pid; GameStatus.Text = message;
        keeping = new CancellationTokenSource();
        var api = relayApi!; var token = keeping.Token;
        _ = Task.Run(async () =>
        {
            try { await launcher.Keep(pid, api, Status, token); }
            catch (Exception error) when (error is not OperationCanceledException) { Status("Saving your position stopped: " + error.Message); }
            Dispatcher.UIThread.Post(async () =>
            {
                if (hotaPid != pid) return;
                hotaPid = null; GameStatus.Text = "HotA closed. Your seat is kept; choose Continue to come back.";
                rows = rows.Select(r => r with { HotaRunning = false }).ToList(); Render();
                await RefreshSafely();
            });
        }, CancellationToken.None);
        rows = rows.Select(r => r with { HotaRunning = true }).ToList(); Render();
    }
}
