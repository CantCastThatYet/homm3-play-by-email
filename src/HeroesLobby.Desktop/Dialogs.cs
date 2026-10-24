using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using HeroesLobby.Contracts;

namespace HeroesLobby.Desktop;

public sealed class CreateGameWindow : Window
{
    public CreateGameWindow(bool nativeStartup = false)
    {
        Title = "Create game · Heroes Post"; Width = 460; SizeToContent = SizeToContent.Height; CanResize = false; WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var name = new TextBox { PlaceholderText = "e.g. Sunday strategists", MaxLength = 64 };
        var map = new TextBox { PlaceholderText = "e.g. Our giant adventure", MaxLength = 100 };
        var capacity = new ComboBox { ItemsSource = Enumerable.Range(2, 7).ToList(), SelectedIndex = 2 };
        if (nativeStartup) { map.Text = "Judgement Day.h3m"; map.IsReadOnly = true; capacity.SelectedIndex = 0; capacity.IsEnabled = false; }
        var password = new TextBox { PlaceholderText = "Optional · share with your friends", PasswordChar = '●', MaxLength = 128 };
        var error = new TextBlock { TextWrapping = Avalonia.Media.TextWrapping.Wrap, Foreground = Avalonia.Media.Brushes.Salmon };
        var cancel = new Button { Content = "Cancel" }; cancel.Click += (_, _) => Close(null);
        var create = new Button { Content = "Create game", Classes = { "primary" } };
        create.Click += (_, _) =>
        {
            if (string.IsNullOrWhiteSpace(name.Text) || string.IsNullOrWhiteSpace(map.Text)) { error.Text = "Give your game a name and a map label."; return; }
            Close(new CreateGame(name.Text.Trim(), map.Text.Trim(), (int)capacity.SelectedItem!, password.Text));
        };
        Content = new StackPanel
        {
            Margin = new Thickness(28), Spacing = 12,
            Children =
            {
                new TextBlock { Text = "Gather your party", FontSize = 26 },
                new TextBlock { Text = "Create a persistent lobby on this server.", Classes = { "muted" } },
                Label("Game name"), name, Label("Map label"), map,
                new TextBlock { Text = nativeStartup ? "The verified two-player map. Both ready players will open HotA automatically." : "Set up HotA to create a playable two-player game on Judgement Day.", Classes = { "muted" }, FontSize = 11, TextWrapping = Avalonia.Media.TextWrapping.Wrap },
                Label("Player capacity"), capacity, Label("Game password"), password, error,
                new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 12, Margin = new Thickness(0,8,0,0), Children = { cancel, create } }
            }
        };
    }
    private static TextBlock Label(string text) => new() { Text = text, Classes = { "label" }, Margin = new Thickness(0,4,0,0) };
}

public sealed class PromptWindow : Window
{
    private PromptWindow(string title, string description, string action, bool password)
    {
        Title = title; Width = 440; SizeToContent = SizeToContent.Height; CanResize = false; WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var input = new TextBox { PasswordChar = '●', PlaceholderText = "Game password", IsVisible = password, MaxLength = 128 };
        var cancel = new Button { Content = "Cancel" }; cancel.Click += (_, _) => Close(null);
        var accept = new Button { Content = action, Classes = { "primary" } }; accept.Click += (_, _) => Close(password ? input.Text ?? "" : "yes");
        Content = new StackPanel { Margin = new Thickness(28), Spacing = 18, Children = {
            new TextBlock { Text = title, FontSize = 24 },
            new TextBlock { Text = description, TextWrapping = Avalonia.Media.TextWrapping.Wrap, Classes = { "muted" } }, input,
            new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 10, Children = { cancel, accept } }
        }};
    }
    public static Task<string?> Ask(Window owner, string title, string description, string action, bool password) => new PromptWindow(title, description, action, password).ShowDialog<string?>(owner);
}
