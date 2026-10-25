using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using HeroesLobby.NativeHost;

namespace HeroesLobby.Desktop;

public sealed class NativeSetupWindow : Window
{
    public NativeSetupWindow(LocalProfile profile)
    {
        Title = "Set up HotA"; Width = 560; SizeToContent = SizeToContent.Height; CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var folder = new TextBox { Text = profile.NativeGameRoot, PlaceholderText = "Your HotA installation folder" };
        var status = new TextBlock { TextWrapping = Avalonia.Media.TextWrapping.Wrap };
        var browse = new Button { Content = "Browse…" };
        browse.Click += async (_, _) =>
        {
            var selected = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Choose your HotA folder", AllowMultiple = false });
            if (selected.Count == 1) folder.Text = selected[0].TryGetLocalPath();
        };
        var save = new Button { Content = "Prepare HotA", Classes = { "primary" } };
        save.Click += async (_, _) =>
        {
            save.IsEnabled = browse.IsEnabled = folder.IsEnabled = false;
            try
            {
                var source = Path.GetFullPath(folder.Text?.Trim() ?? "");
                status.Text = "Checking HotA and preparing a separate copy. This can take a minute…";
                var root = await Task.Run(() =>
                {
                    NativeBridge.VerifyBuild(source);
                    return File.Exists(Path.Combine(source, NativeBridge.Marker)) && File.ReadAllText(Path.Combine(source, NativeBridge.Marker)).Trim() == NativeBridge.MarkerContent
                        ? source : NativeBridge.Prepare(source, Path.Combine(profile.StorageDirectory, "game-" + Guid.NewGuid().ToString("N")));
                });
                NativeHotaLauncher.Validate(root, NativeHotaLauncher.BuildDirectory);
                profile.NativeGameRoot = root; profile.Save(); Close(true);
            }
            catch (Exception ex) { status.Text = ex.Message; }
            finally { save.IsEnabled = browse.IsEnabled = folder.IsEnabled = true; }
        };
        Content = new StackPanel { Margin = new Thickness(28), Spacing = 14, Children =
        {
            new TextBlock { Text = "Connect Heroes Post to HotA", FontSize = 24 },
            new TextBlock { Text = "Choose your installed HotA folder. Heroes Post plays from its own copy, so your installation and saves are never changed.", TextWrapping = Avalonia.Media.TextWrapping.Wrap },
            folder, browse, status, save
        }};
    }
}
