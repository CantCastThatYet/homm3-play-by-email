using System.Net;
using System.Text;

namespace HeroesLobby.NativeHost;

/// <summary>Development input for the native setup adapter, scoped to one isolated copy.</summary>
public sealed record BootstrapRequest(string Action, string PlayerName, string SessionName, string Address, string MapFile, int Port = 0)
{
    public const string FileName = "HeroesPost.session.ini";

    public void Stage(string isolatedRoot)
    {
        isolatedRoot = Path.GetFullPath(isolatedRoot);
        CaptureBundle.CheckRegularPath(isolatedRoot);
        var marker = Path.Combine(isolatedRoot, NativeBridge.Marker);
        CaptureBundle.CheckRegularPath(marker);
        if (File.ReadAllText(marker).Trim() != NativeBridge.MarkerContent)
            throw new InvalidOperationException("Native setup requests require an isolated adapter-owned installation.");
        if (Action is not ("Host" or "Join" or "SoloScaffold")) throw new ArgumentException("Unknown native setup action.");
        CheckField(PlayerName, 20, nameof(PlayerName)); CheckField(SessionName, 63, nameof(SessionName));
        if (Port is < 0 or > 65535 or (> 0 and < 1024)) throw new ArgumentOutOfRangeException(nameof(Port), "Use 0 for the native default or a TCP/IP port from 1024 through 65535.");
        if (!IPAddress.TryParse(Address, out var address) || address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
            throw new ArgumentException("The initial native TCP/IP experiment requires an IPv4 address.");
        if (MapFile.Length > 0)
        {
            CheckField(MapFile, 120, nameof(MapFile));
            if (MapFile != Path.GetFileName(MapFile) || MapFile.IndexOfAny(['/', '\\', ':']) >= 0 ||
                !MapFile.EndsWith(".h3m", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Select a map filename from the isolated Maps directory.");
            var mapPath = Path.Combine(isolatedRoot, "Maps", MapFile);
            CaptureBundle.CheckRegularPath(mapPath);
            if (!File.Exists(mapPath)) throw new FileNotFoundException("The selected native map is not installed.", mapPath);
        }
        // The adapter reads a bounded UTF-16 request. Values cannot contain new
        // lines; brackets within a value allow standard [HotA] map filenames.
        var text = $"[Session]\r\nVersion=1\r\nAction={Action}\r\nPlayerName={PlayerName}\r\nSessionName={SessionName}\r\nAddress={address}\r\nMapFile={MapFile}\r\nPort={Port}\r\n";
        var target = Path.Combine(isolatedRoot, FileName);
        if (File.Exists(target)) CaptureBundle.CheckRegularPath(target);
        var temporary = target + ".pending-" + Guid.NewGuid().ToString("N");
        var preamble = Encoding.Unicode.GetPreamble(); var bytes = Encoding.Unicode.GetBytes(text);
        using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { file.Write(preamble); file.Write(bytes); file.Flush(flushToDisk: true); }
        File.Move(temporary, target, overwrite: true);
    }

    private static void CheckField(string value, int max, string field)
    {
        if (value.Length is < 1 || value.Length > max || value != value.Trim() ||
            value.Any(c => c < 32 || c > 126 || c is '=' or ';' or '#' or '"'))
            throw new ArgumentException($"{field} must use 1–{max} plain ASCII characters without INI syntax for this native experiment.");
    }
}
