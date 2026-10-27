using System.Buffers.Binary;
using System.Security.Cryptography;

namespace HeroesLobby.Server.Relay;

/// <summary>
/// A capture sent as a difference from the seat's latest stored capture (native/capture_delta.hpp):
/// "HPD1", version 1, the base's SHA-256 and length, the result's SHA-256 and length, then
/// operations to the end: 0 = copy (u32 base offset, u32 length), 1 = literal (u32 length, bytes).
/// </summary>
public sealed record CaptureDelta(string BaseSha256, int BaseLength, string Sha256, int Length, ReadOnlyMemory<byte> Operations)
{
    private const int Header = 4 + 4 + 32 + 4 + 32 + 4;

    public static CaptureDelta Read(byte[] frame, int maxLength)
    {
        if (frame.Length < Header || !frame.AsSpan(0, 4).SequenceEqual("HPD1"u8) || BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(4)) != 1)
            throw new InvalidDataException("This is not a capture difference this server reads.");
        var baseLength = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(40));
        var length = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(76));
        if (baseLength > maxLength || length > maxLength) throw new InvalidDataException("The capture is too large.");
        return new(Convert.ToHexStringLower(frame.AsSpan(8, 32)), (int)baseLength, Convert.ToHexStringLower(frame.AsSpan(44, 32)), (int)length,
            frame.AsMemory(Header));
    }

    /// <summary>The capture rebuilt from its base; throws for another base, bad operations or a wrong result.</summary>
    public byte[] Apply(byte[] basis)
    {
        if (basis.Length != BaseLength || Convert.ToHexStringLower(SHA256.HashData(basis)) != BaseSha256)
            throw new InvalidDataException("The difference is from another capture.");
        var result = new byte[Length]; var written = 0;
        var ops = Operations.ToArray(); var at = 0;
        uint U32() { if (ops.Length - at < 4) throw new InvalidDataException("The difference is truncated."); var v = BinaryPrimitives.ReadUInt32LittleEndian(ops.AsSpan(at)); at += 4; return v; }
        while (at < ops.Length)
        {
            var kind = ops[at++];
            if (kind == 0)
            {
                long offset = U32(), count = U32();
                if (offset + count > basis.Length || count > Length - written) throw new InvalidDataException("A copy leaves its bounds.");
                basis.AsSpan((int)offset, (int)count).CopyTo(result.AsSpan(written)); written += (int)count;
            }
            else if (kind == 1)
            {
                long count = U32();
                if (count > ops.Length - at || count > Length - written) throw new InvalidDataException("A literal leaves its bounds.");
                ops.AsSpan(at, (int)count).CopyTo(result.AsSpan(written)); written += (int)count; at += (int)count;
            }
            else throw new InvalidDataException("The difference has an unknown operation.");
        }
        if (written != Length || Convert.ToHexStringLower(SHA256.HashData(result)) != Sha256)
            throw new InvalidDataException("The rebuilt capture does not match its checksum.");
        return result;
    }
}
