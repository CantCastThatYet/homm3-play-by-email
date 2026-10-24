using System.Text;
using HeroesLobby.NativeHost;
using Xunit;

namespace HeroesLobby.Tests;

public class BootstrapRequestTests
{
    [Fact]
    public void Start_request_rejects_wrong_identity_date_and_path_without_replacing_valid_request()
    {
        var root = Path.Combine(Path.GetTempPath(), "heroes-post-start-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "Maps"));
        File.WriteAllText(Path.Combine(root, NativeBridge.Marker), NativeBridge.MarkerContent);
        File.WriteAllBytes(Path.Combine(root, "Maps", "[HotA] Test.h3m"), [1]);
        var valid = new StartRequest("PeerAccept", 1, uint.MaxValue, "[HotA] Test.h3m", 211);
        valid.Stage(root);
        var file = Path.Combine(root, StartRequest.FileName);
        var bytes = File.ReadAllBytes(file);
        Assert.Contains("PeerId2=4294967295\r\n", Encoding.Unicode.GetString(bytes));
        foreach (var invalid in new[] { valid with { PeerId2 = 1 }, valid with { PeerId1 = 0 }, valid with { Action = "Host" },
            valid with { SimTurnThrough = 218 }, valid with { SimTurnThrough = -111 }, valid with { MapFile = "../[HotA] Test.h3m" },
            valid with { MapFile = "Test\r\nAction=HostStart.h3m" } })
            Assert.ThrowsAny<ArgumentException>(() => invalid.Stage(root));
        Assert.Equal(bytes, File.ReadAllBytes(file));
    }

    [Fact]
    public void Stages_bounded_native_input_only_in_owned_installation()
    {
        var root = Path.Combine(Path.GetTempPath(), "heroes-post-bootstrap-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root); File.WriteAllText(Path.Combine(root, NativeBridge.Marker), NativeBridge.MarkerContent);
        Directory.CreateDirectory(Path.Combine(root, "Maps")); File.WriteAllBytes(Path.Combine(root, "Maps", "Small.h3m"), [1]);
        new BootstrapRequest("Host", "Red", "Local test", "127.0.0.1", "Small.h3m", 24777).Stage(root);
        var bytes = File.ReadAllBytes(Path.Combine(root, BootstrapRequest.FileName));
        Assert.Equal(Encoding.Unicode.GetPreamble(), bytes[..2]);
        Assert.Contains("MapFile=Small.h3m\r\n", Encoding.Unicode.GetString(bytes));
        Assert.Contains("Port=24777\r\n", Encoding.Unicode.GetString(bytes));
        var original = bytes;
        foreach (var bad in new[]
        {
            new BootstrapRequest("Host", "Red\r\nAction=Join", "Test", "127.0.0.1", "Small.h3m"),
            new BootstrapRequest("Host", "Red", "Test", "127.0.0.1", "../Small.h3m"),
            new BootstrapRequest("Join", "Red", "Test", "https://example.com", ""),
            new BootstrapRequest("Host", "Red", "Test", "127.0.0.1", "C:Small.h3m"),
            new BootstrapRequest("Host", "Red", "Test", "127.0.0.1", "", -1),
            new BootstrapRequest("Host", "Red", "Test", "127.0.0.1", "", 65536)
        }) Assert.ThrowsAny<ArgumentException>(() => bad.Stage(root));
        Assert.Equal(original, File.ReadAllBytes(Path.Combine(root, BootstrapRequest.FileName)));
        File.WriteAllText(Path.Combine(root, NativeBridge.Marker), "different-owner");
        Assert.Throws<InvalidOperationException>(() => new BootstrapRequest("Join", "Green", "Test", "127.0.0.1", "").Stage(root));
    }
}
