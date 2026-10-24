using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace HeroesLobby.Contracts;

public sealed record NativeCaptureData(byte[] ManifestBytes, IReadOnlyDictionary<string, byte[]> Files,
    string WorldFile, string BaselineFile, string? AuditFile, string Kind, int Color,
    int Day, int Week, int Month, byte[] Participants, int DeferredCount);

/// <summary>Validates stored bytes and capture identity. Does not certify native restoration.</summary>
public static class NativeCaptureArchive
{
    public const int MaxBytes = 64 * 1024 * 1024;
    public static NativeCaptureData Read(Stream source, int expectedColor)
    {
        if (expectedColor is < 0 or > 7) throw new InvalidDataException("Invalid reserved player color.");
        if (!source.CanSeek)
        {
            using var bounded = new MemoryStream();
            var chunk = new byte[81920]; int read;
            while ((read = source.Read(chunk)) != 0)
            {
                if (bounded.Length + read > MaxBytes) throw new InvalidDataException("Capture archive is too large.");
                bounded.Write(chunk, 0, read);
            }
            bounded.Position = 0; return Read(bounded, expectedColor);
        }
        if (source.Length is < 1 or > MaxBytes) throw new InvalidDataException("Invalid capture archive length.");
        using var zip = new ZipArchive(source, ZipArchiveMode.Read, leaveOpen: true);
        if (zip.Entries.Count is < 2 or > 4101) throw new InvalidDataException("Invalid capture entry count.");
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        long expanded = 0;
        foreach (var entry in zip.Entries)
        {
            if (!ValidName(entry.FullName) || !entries.TryAdd(entry.FullName, entry) ||
                entry.Length < 0 || entry.Length > MaxBytes - expanded ||
                ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
                throw new InvalidDataException("Invalid, linked, duplicate, or oversized capture entry.");
            expanded += entry.Length;
        }
        if (!entries.TryGetValue("manifest.json", out var manifestEntry) || manifestEntry.Length > 1024 * 1024)
            throw new InvalidDataException("Missing or oversized native capture manifest.");
        var manifestBytes = ReadEntry(manifestEntry);
        using var document = JsonDocument.Parse(manifestBytes, new JsonDocumentOptions { MaxDepth = 32 });
        var root = document.RootElement;
        UniqueProperties(root);
        if (root.GetProperty("schema").GetString() != "hota-native-capture-v1" ||
            root.GetProperty("complete_restore").ValueKind != JsonValueKind.False ||
            root.GetProperty("status").GetString() != "captured" ||
            !root.GetProperty("metadata_valid").GetBoolean() || !root.GetProperty("world_date_valid").GetBoolean())
            throw new InvalidDataException("Unsupported or incomplete native capture.");
        var color = root.GetProperty("local_player").GetInt32();
        if (color != expectedColor || root.GetProperty("current_player").GetInt32() != color)
            throw new InvalidDataException("Capture is not the reserved player's own active world.");
        var date = root.GetProperty("world_date_words").EnumerateArray().Select(x => x.GetInt32()).ToArray();
        if (date.Length != 3 || date[0] is < 1 or > 7 || date[1] is < 1 or > 4 || date[2] is < 1 or > 65535)
            throw new InvalidDataException("Invalid native world date.");
        var rows = root.GetProperty("participants").EnumerateArray().ToArray();
        if (rows.Length != 8) throw new InvalidDataException("Expected eight native participant records.");
        var participants = new byte[48];
        var seen = new HashSet<int>();
        foreach (var row in rows)
        {
            var player = row.GetProperty("player").GetInt32();
            if (player is < 0 or > 7 || !seen.Add(player)) throw new InvalidDataException("Invalid participant index.");
            var fields = new[] { "ack", "done", "role", "participates", "predecessor", "successor" };
            for (var field = 0; field < fields.Length; field++)
            {
                var value = row.GetProperty(fields[field]).GetInt32();
                if (field < 4 ? value is < 0 or > 1 : value is < -1 or > 7)
                    throw new InvalidDataException("Invalid native participant state.");
                participants[player * 6 + field] = unchecked((byte)value);
            }
        }
        if (participants[color * 6 + 3] != 1) throw new InvalidDataException("Reserved player is absent from capture.");
        var save = root.GetProperty("native_save");
        if (!save.GetProperty("observed").GetBoolean() || !save.GetProperty("name_complete").GetBoolean() ||
            save.GetProperty("return_byte").GetInt32() != 1 || save.GetProperty("name").GetString() != "RMTGEXBS.BIN")
            throw new InvalidDataException("A fresh native local-world serialization is required.");
        var completion = root.GetProperty("native_completion");
        var done = completion.GetProperty("observed").GetBoolean();
        if (done && (!completion.GetProperty("marker_confirmed").GetBoolean() ||
                     !completion.GetProperty("fresh_world_serialized").GetBoolean() ||
                     completion.GetProperty("player").GetInt32() != color || participants[color * 6 + 1] != 1))
            throw new InvalidDataException("Native completion metadata does not match the reserved player.");
        if (!done && participants[color * 6 + 1] != 0) throw new InvalidDataException("Completed branch lacks a native completion marker.");

        var files = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        string? Add(JsonElement nameElement, string hash, long? length)
        {
            if (nameElement.ValueKind == JsonValueKind.Null) return null;
            var name = nameElement.GetString()!;
            if (!ValidName(name) || name.Equals("manifest.json", StringComparison.OrdinalIgnoreCase) ||
                hash.Length != 64 || !hash.All(char.IsAsciiHexDigit) || files.ContainsKey(name) ||
                !entries.TryGetValue(name, out var entry) || (length is { } expected && entry.Length != expected))
                throw new InvalidDataException("Invalid native file reference.");
            var bytes = ReadEntry(entry);
            if (!Convert.ToHexString(SHA256.HashData(bytes)).Equals(hash, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Native file hash mismatch.");
            files.Add(name, bytes); return name;
        }
        Add(root.GetProperty("raw_st_metadata"), root.GetProperty("raw_st_metadata_sha256").GetString()!, null);
        string? world = null, baseline = null, audit = null;
        var roles = new HashSet<string>(StringComparer.Ordinal);
        foreach (var stream in root.GetProperty("streams").EnumerateArray())
        {
            var role = stream.GetProperty("role").GetString()!;
            if (!roles.Add(role)) throw new InvalidDataException("Duplicate native stream role.");
            var bytes = stream.GetProperty("captured_bytes").GetInt64();
            var name = Add(stream.GetProperty("file"), stream.GetProperty("sha256").GetString()!, bytes);
            if (role is not ("local_snapshot" or "baseline_orig")) continue;
            if (name is null || bytes <= 0 || stream.GetProperty("status").GetString() != "present" ||
                stream.GetProperty("open").GetInt32() != 0 || stream.GetProperty("length").GetInt64() != bytes)
                throw new InvalidDataException("Required native world or baseline stream is unavailable.");
            if (role == "local_snapshot") world = name; else baseline = name;
        }
        var deferred = root.GetProperty("deferred");
        var packets = deferred.GetProperty("packets").EnumerateArray().ToArray();
        if (packets.Length > 4096 || deferred.GetProperty("declared_count").GetInt32() != packets.Length ||
            deferred.GetProperty("copied_count").GetInt32() != packets.Length ||
            deferred.GetProperty("status").GetString() != (packets.Length == 0 ? "empty" : "present"))
            throw new InvalidDataException("Native deferred events were not captured completely.");
        foreach (var packet in packets)
            Add(packet.GetProperty("file"), packet.GetProperty("sha256").GetString()!, packet.GetProperty("bytes").GetInt64());
        if (root.TryGetProperty("world_audit", out var auditRecord) && auditRecord.ValueKind != JsonValueKind.Null)
            audit = Add(auditRecord.GetProperty("file"), auditRecord.GetProperty("sha256").GetString()!, auditRecord.GetProperty("bytes").GetInt64());
        if (root.TryGetProperty("hd_roster", out var rosterRecord) && rosterRecord.ValueKind != JsonValueKind.Null)
            Add(rosterRecord.GetProperty("file"), rosterRecord.GetProperty("sha256").GetString()!, rosterRecord.GetProperty("bytes").GetInt64());
        if (root.TryGetProperty("hero_assignments", out var assignments) && assignments.ValueKind != JsonValueKind.Null)
            Add(assignments.GetProperty("file"), assignments.GetProperty("sha256").GetString()!, assignments.GetProperty("bytes").GetInt64());
        if (world is null || baseline is null || files.Count + 1 != entries.Count)
            throw new InvalidDataException("Capture lacks required streams or contains unreferenced files.");
        return new(manifestBytes, files, world, baseline, audit, done ? "completion" : "startup", color,
            date[0], date[1], date[2], participants, packets.Length);
    }

    private static bool ValidName(string name) => name.Length is > 0 and <= 100 && name is not ("." or "..") &&
        name.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '.' or '_');

    private static byte[] ReadEntry(ZipArchiveEntry entry)
    {
        var bytes = new byte[checked((int)entry.Length)];
        using var stream = entry.Open(); stream.ReadExactly(bytes);
        if (stream.ReadByte() != -1) throw new InvalidDataException("Native ZIP entry exceeds its declared length.");
        return bytes;
    }

    private static void UniqueProperties(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
            {
                if (!names.Add(property.Name)) throw new InvalidDataException("Duplicate native manifest property.");
                UniqueProperties(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
            foreach (var item in element.EnumerateArray()) UniqueProperties(item);
    }
}
