using System.Text;
using System.Text.Json;

namespace HeroesLobby.Contracts;

/// <summary>
/// What a HotA client needs to resume a captured relay seat: the adapter's HeroesPost.resume.bin
/// image, the messages HD's simultaneous-turn queue still held (u32 length + message each), and
/// where the relay replay starts.
/// </summary>
public sealed record NativeResumeImage(byte[] Resume, byte[] Deferred, int DeferredCount, long Consumed, long Sent, string MapFile, int Color, string Kind)
{
    public const string DefaultMap = "Judgement Day.h3m";

    /// <summary>Checks a capture bundle and builds its resume image. Captures with queued messages need a relay rejoin.</summary>
    public static NativeResumeImage Build(Stream archive, int color, bool relayRejoin)
    {
        var capture = NativeCaptureArchive.Read(archive, color);
        using var document = JsonDocument.Parse(capture.ManifestBytes);
        var manifest = document.RootElement;
        if (capture.AuditFile is null || !manifest.TryGetProperty("hd_roster", out var roster) || roster.ValueKind == JsonValueKind.Null)
            throw new InvalidDataException("This older checkpoint lacks the native world audit and human-seat data required for solo resume.");
        // Over the relay, the rejoining client re-delivers these to HD's simultaneous-turn queue.
        if (capture.DeferredCount != 0 && !relayRejoin)
            throw new InvalidDataException("This checkpoint has pending native events which cannot yet be replayed during solo resume.");
        var rosterBytes = capture.Files[roster.GetProperty("file").GetString()!];
        if (!manifest.TryGetProperty("hero_assignments", out var assignmentRecord) || assignmentRecord.ValueKind == JsonValueKind.Null)
            throw new InvalidDataException("This checkpoint does not contain HotA's simultaneous-turn hero assignments.");
        var assignments = capture.Files[assignmentRecord.GetProperty("file").GetString()!];
        var dayOne = manifest.GetProperty("streams").EnumerateArray().Single(s => s.GetProperty("role").GetString() == "baseline_day_one");
        if (dayOne.GetProperty("status").GetString() != "present" || dayOne.GetProperty("open").GetInt32() != 0 ||
            dayOne.GetProperty("length").GetInt64() != dayOne.GetProperty("captured_bytes").GetInt64())
            throw new InvalidDataException("The day-one native baseline is not a complete closed stream.");
        var dayOneBytes = capture.Files[dayOne.GetProperty("file").GetString()!];
        var st = capture.Files[manifest.GetProperty("raw_st_metadata").GetString()!];
        if (rosterBytes.Length != 187 || st.Length != 156)
            throw new InvalidDataException("The native roster or simultaneous-turn metadata has the wrong size.");
        // The scaffold starts the capture's own scenario; the native loader then checks
        // geometry and the entire source audit. Older captures predate map_file.
        var map = manifest.TryGetProperty("map_file", out var mapFile) && mapFile.GetString() is { Length: > 0 } recorded ? recorded : DefaultMap;
        if (map.IndexOfAny(['/', '\\', ':']) >= 0) throw new InvalidDataException("The capture names an invalid map file.");

        using var image = new MemoryStream();
        using (var writer = new BinaryWriter(image, Encoding.UTF8, leaveOpen: true))
        {
            writer.Write(0x31525048u); // HPR1
            writer.Write(1u); writer.Write((uint)color); writer.Write((uint)capture.Day);
            writer.Write((uint)capture.Week); writer.Write((uint)capture.Month);
            writer.Write(capture.Kind == "completion" ? 1u : 0u);
            foreach (var bytes in new[] { capture.Files[capture.WorldFile], capture.Files[capture.BaselineFile],
                         capture.Files[capture.AuditFile], rosterBytes, st, assignments, dayOneBytes })
            { writer.Write((uint)bytes.Length); writer.Write(bytes); }
        }
        using var deferred = new MemoryStream();
        foreach (var packet in manifest.GetProperty("deferred").GetProperty("packets").EnumerateArray())
        {
            var bytes = capture.Files[packet.GetProperty("file").GetString()!];
            deferred.Write(BitConverter.GetBytes(bytes.Length)); deferred.Write(bytes);
        }
        long Number(string name) => manifest.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number ? value.GetInt64() : -1;
        return new(image.ToArray(), deferred.ToArray(), capture.DeferredCount, Number("relay_consumed"), Number("relay_sent"), map, color, capture.Kind);
    }
}
