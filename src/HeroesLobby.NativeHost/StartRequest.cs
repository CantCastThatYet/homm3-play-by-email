using System.Globalization;
using System.Text;

namespace HeroesLobby.NativeHost;

/// <summary>Explicit peer identities and native settings for the bounded two-client start experiment.</summary>
public sealed record StartRequest(string Action, uint PeerId1, uint PeerId2, string MapFile, int SimTurnThrough = 211)
{
    public const string FileName = "HeroesPost.start.ini";
    public void Stage(string isolatedRoot)
    {
        isolatedRoot = Path.GetFullPath(isolatedRoot);
        CaptureBundle.CheckRegularPath(isolatedRoot);
        var marker = Path.Combine(isolatedRoot, NativeBridge.Marker);
        CaptureBundle.CheckRegularPath(marker);
        if (File.ReadAllText(marker).Trim() != NativeBridge.MarkerContent)
            throw new InvalidOperationException("Native start requests require an adapter-owned installation.");
        if (Action is not ("HostStart" or "PeerAccept") || PeerId1 == 0 || PeerId2 == 0 || PeerId1 == PeerId2)
            throw new ArgumentException("Specify HostStart or PeerAccept and the two distinct native player IDs.");
        if (SimTurnThrough / 100 is < 1 or > 12 || SimTurnThrough / 10 % 10 is < 1 or > 4 || SimTurnThrough % 10 is < 1 or > 7)
            throw new ArgumentException("The simultaneous-turn date is decimal month/week/day, such as 211 for month 2, week 1, day 1.");
        if (MapFile.Length is < 5 or > 120 || MapFile != MapFile.Trim() ||
            MapFile.Any(c => c < 32 || c > 126 || c is '=' or ';' or '#' or '"') ||
            MapFile.IndexOfAny(['/', '\\', ':']) >= 0 || MapFile.Contains("..", StringComparison.Ordinal) ||
            !MapFile.EndsWith(".h3m", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Use a native map basename.");
        var mapPath = Path.Combine(isolatedRoot, "Maps", MapFile);
        CaptureBundle.CheckRegularPath(mapPath);
        if (!File.Exists(mapPath)) throw new FileNotFoundException("The native map is not installed.", mapPath);
        var text = string.Create(CultureInfo.InvariantCulture,
            $"[Start]\r\nVersion=1\r\nAction={Action}\r\nPeerId1={PeerId1}\r\nPeerId2={PeerId2}\r\nMapFile={MapFile}\r\nSimTurnThrough={SimTurnThrough}\r\n");
        var target = Path.Combine(isolatedRoot, FileName);
        if (File.Exists(target)) CaptureBundle.CheckRegularPath(target);
        var temporary = target + ".pending-" + Guid.NewGuid().ToString("N");
        using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { file.Write(Encoding.Unicode.GetPreamble()); file.Write(Encoding.Unicode.GetBytes(text)); file.Flush(true); }
        File.Move(temporary, target, overwrite: true);
    }
}
