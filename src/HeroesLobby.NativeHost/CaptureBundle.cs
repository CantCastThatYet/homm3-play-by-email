using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace HeroesLobby.NativeHost;

/// <summary>Packages immutable native evidence. It does not promote it to a restorable turn.</summary>
public static class CaptureBundle
{
    public const long MaxBytes = 64 * 1024 * 1024;
    private const int MaxManifestBytes = 1024 * 1024;

    public static void Write(string manifestPath, string destination)
    {
        manifestPath = Path.GetFullPath(manifestPath);
        CheckRegularPath(manifestPath);
        var root = Path.GetDirectoryName(manifestPath)!;
        using var manifestFile = File.OpenRead(manifestPath);
        if (manifestFile.Length > MaxManifestBytes) throw new InvalidDataException("Native manifest is too large.");
        using var buffer = new MemoryStream(); manifestFile.CopyTo(buffer);
        var manifestBytes = buffer.ToArray();
        using var manifest = JsonDocument.Parse(manifestBytes);
        var data = manifest.RootElement;
        if (data.GetProperty("schema").GetString() != "hota-native-capture-v1" ||
            data.GetProperty("complete_restore").ValueKind != JsonValueKind.False)
            throw new InvalidDataException("Unsupported native capture schema or restore claim.");
        var files = new SortedDictionary<string, (string Hash, long? Length)>(StringComparer.OrdinalIgnoreCase);
        void Add(JsonElement name, string hash, long? length)
        {
            if (name.ValueKind == JsonValueKind.Null) return;
            var file = name.GetString() ?? throw new InvalidDataException("Missing capture filename.");
            // Native writer publishes flat, generated filenames; never follow paths from JSON.
            if (file.Length is < 1 or > 100 || file.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not ('-' or '.' or '_')) ||
                file is "." or ".." || file.Equals("manifest.json", StringComparison.OrdinalIgnoreCase) ||
                hash.Length != 64 || !hash.All(char.IsAsciiHexDigit) || length < 0)
                throw new InvalidDataException("Invalid capture file reference.");
            if (!files.TryAdd(file, (hash, length))) throw new InvalidDataException("Duplicate capture file reference.");
        }
        Add(data.GetProperty("raw_st_metadata"), data.GetProperty("raw_st_metadata_sha256").GetString()!, null);
        foreach (var property in new[] { "world_audit", "hd_roster", "hero_assignments" })
            if (data.TryGetProperty(property, out var extra) && extra.ValueKind != JsonValueKind.Null)
                Add(extra.GetProperty("file"), extra.GetProperty("sha256").GetString()!, extra.GetProperty("bytes").GetInt64());
        foreach (var stream in data.GetProperty("streams").EnumerateArray())
            Add(stream.GetProperty("file"), stream.GetProperty("sha256").GetString()!, stream.GetProperty("captured_bytes").GetInt64());
        foreach (var packet in data.GetProperty("deferred").GetProperty("packets").EnumerateArray())
            Add(packet.GetProperty("file"), packet.GetProperty("sha256").GetString()!, packet.GetProperty("bytes").GetInt64());
        if (files.Count > 4100) throw new InvalidDataException("Too many capture files.");
        using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        using (var archive = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
        {
            static ZipArchiveEntry Entry(ZipArchive zip, string name)
            {
                var entry = zip.CreateEntry(name, CompressionLevel.NoCompression);
                entry.LastWriteTime = new DateTimeOffset(1980, 1, 1, 0, 0, 0, TimeSpan.Zero);
                return entry;
            }
            using (var entry = Entry(archive, "manifest.json").Open()) entry.Write(manifestBytes);
            long total = manifestBytes.Length;
            var chunk = new byte[81920];
            foreach (var (name, expected) in files)
            {
                var path = Path.Combine(root, name); CheckRegularPath(path);
                using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                if (file.Length > MaxBytes - total || (expected.Length is { } length && length != file.Length))
                    throw new InvalidDataException("Capture file length is invalid or exceeds the bundle limit.");
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                using var entry = Entry(archive, name).Open();
                long copied = 0; int count;
                while ((count = file.Read(chunk)) != 0)
                {
                    copied += count; total += count;
                    if (total > MaxBytes) throw new InvalidDataException("Capture grew beyond the bundle limit.");
                    hash.AppendData(chunk, 0, count); entry.Write(chunk, 0, count);
                }
                if (copied != file.Length || !Convert.ToHexString(hash.GetHashAndReset()).Equals(expected.Hash, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Capture file does not match its published manifest.");
            }
        }
        if (output.Length > MaxBytes) throw new InvalidDataException("Capture archive exceeds the server upload limit.");
        output.Flush(flushToDisk: true);
    }

    internal static void CheckRegularPath(string path)
    {
        for (var current = Path.GetFullPath(path); current is not null; current = Path.GetDirectoryName(current))
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Linked capture or outbox paths are not supported.");
    }
}
