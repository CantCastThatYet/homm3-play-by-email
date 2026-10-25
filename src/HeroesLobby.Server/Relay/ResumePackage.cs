using System.Text;
using HeroesLobby.Contracts;

namespace HeroesLobby.Server.Relay;

/// <summary>
/// The in-game Continue's download (native/seat_keeper.cpp reads it). Little-endian: "HPK1", u32
/// color, session name and seat name (u32 length + Latin-1 bytes, as the relay compares them), u32
/// relay messages the capture had consumed, u32 messages it had sent (0xFFFFFFFF when unknown), map
/// file (u32 length + UTF-8), the resume image and the deferred messages (u32 length + bytes each),
/// the deferred message count, and the seat's relay id.
/// </summary>
public static class ResumePackage
{
    public const uint Magic = 0x314B5048; // HPK1

    public static byte[] Write(string session, string seat, uint seatId, NativeResumeImage image)
    {
        using var stream = new MemoryStream();
        using (var w = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true))
        {
            void Blob(byte[] bytes) { w.Write((uint)bytes.Length); w.Write(bytes); }
            w.Write(Magic); w.Write((uint)image.Color);
            Blob(Encoding.Latin1.GetBytes(session)); Blob(Encoding.Latin1.GetBytes(seat));
            w.Write((uint)image.Consumed); w.Write(image.Sent >= 0 ? (uint)image.Sent : uint.MaxValue);
            Blob(Encoding.UTF8.GetBytes(image.MapFile));
            Blob(image.Resume); Blob(image.Deferred);
            w.Write((uint)image.DeferredCount);
            w.Write(seatId);
        }
        return stream.ToArray();
    }
}
