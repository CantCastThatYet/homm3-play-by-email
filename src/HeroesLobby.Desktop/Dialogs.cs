using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using HeroesLobby.Contracts;

namespace HeroesLobby.Desktop;

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
