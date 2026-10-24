using System.IO.Compression;
using System.Text;
using HeroesLobby.Contracts;
using Xunit;

namespace HeroesLobby.Tests;

public sealed class NativeCaptureArchiveTests
{
    private static byte[] Rewrite(Action<ZipArchive> edit)
    {
        using var memory = new MemoryStream();
        memory.Write(StartedGameFixture.Capture());
        using (var archive = new ZipArchive(memory, ZipArchiveMode.Update, leaveOpen: true)) edit(archive);
        return memory.ToArray();
    }

    [Fact]
    public void Rejects_changed_world_bytes_and_unreferenced_payloads()
    {
        var changed = Rewrite(zip =>
        {
            var entry = zip.GetEntry("world.bin")!;
            using var stream = entry.Open(); stream.Position = 0; stream.WriteByte(99);
        });
        Assert.Throws<InvalidDataException>(() => NativeCaptureArchive.Read(new MemoryStream(changed), 0));
        var extra = Rewrite(zip => zip.CreateEntry("unexpected.bin"));
        Assert.Throws<InvalidDataException>(() => NativeCaptureArchive.Read(new MemoryStream(extra), 0));
    }

    [Theory]
    [InlineData("../world.bin")]
    [InlineData("folder/world.bin")]
    [InlineData("WORLD.BIN")]
    public void Rejects_traversal_paths_and_case_aliases(string name)
    {
        var bytes = Rewrite(zip => zip.CreateEntry(name));
        Assert.Throws<InvalidDataException>(() => NativeCaptureArchive.Read(new MemoryStream(bytes), 0));
    }

    [Fact]
    public void Rejects_duplicate_manifest_properties_instead_of_last_value_wins()
    {
        var bytes = Rewrite(zip =>
        {
            var entry = zip.GetEntry("manifest.json")!;
            string text;
            using (var reader = new StreamReader(entry.Open())) text = reader.ReadToEnd();
            entry.Delete();
            using var writer = new StreamWriter(zip.CreateEntry("manifest.json").Open(), new UTF8Encoding(false));
            writer.Write("{\"local_player\":7," + text[1..]);
        });
        Assert.Throws<InvalidDataException>(() => NativeCaptureArchive.Read(new MemoryStream(bytes), 0));
    }
}
