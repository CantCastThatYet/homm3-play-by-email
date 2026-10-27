using System.Buffers.Binary;
using System.Security.Cryptography;
using HeroesLobby.Server.Relay;
using Xunit;

namespace HeroesLobby.Tests;

public sealed class CaptureDeltaTests
{
    private static readonly byte[] Basis = Enumerable.Range(0, 4096).Select(i => (byte)(i * 7 + i / 256)).ToArray();

    // Copies of the base and literals, framed as native/capture_delta.cpp writes them.
    private static byte[] Frame(byte[] basis, byte[] result, params object[] ops)
    {
        var w = new MemoryStream();
        void U32(long v) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, (uint)v); w.Write(b); }
        w.Write("HPD1"u8); U32(1);
        w.Write(SHA256.HashData(basis)); U32(basis.Length);
        w.Write(SHA256.HashData(result)); U32(result.Length);
        foreach (var op in ops)
            if (op is (int offset, int count)) { w.WriteByte(0); U32(offset); U32(count); }
            else { var bytes = (byte[])op; w.WriteByte(1); U32(bytes.Length); w.Write(bytes); }
        return w.ToArray();
    }

    private static readonly byte[] Inserted = [1, 2, 3, 4, 5];
    private static byte[] Edited => [.. Basis[..1000], .. Inserted, .. Basis[1000..]];

    [Fact]
    public void CopiesAndLiteralsRebuildTheCapture()
    {
        var delta = CaptureDelta.Read(Frame(Basis, Edited, (0, 1000), Inserted, (1000, 3096)), 1 << 20);
        Assert.Equal(Convert.ToHexStringLower(SHA256.HashData(Basis)), delta.BaseSha256);
        Assert.Equal(Edited, delta.Apply(Basis));
    }

    [Fact]
    public void AnotherBaseIsRefused()
    {
        var other = (byte[])Basis.Clone(); other[10] ^= 1;
        var delta = CaptureDelta.Read(Frame(Basis, Edited, (0, 1000), Inserted, (1000, 3096)), 1 << 20);
        Assert.Throws<InvalidDataException>(() => delta.Apply(other));
    }

    [Fact]
    public void AWrongResultIsRefused()
    {
        // The operations build something other than the checksum names.
        var delta = CaptureDelta.Read(Frame(Basis, Edited, (0, 1000), new byte[] { 9, 9, 9, 9, 9 }, (1000, 3096)), 1 << 20);
        Assert.Throws<InvalidDataException>(() => delta.Apply(Basis));
    }

    [Theory]
    [InlineData(4000, 200)] // past the base's end
    [InlineData(0, 5000)]   // longer than the result
    public void CopiesOutOfBoundsAreRefused(int offset, int count)
    {
        var delta = CaptureDelta.Read(Frame(Basis, Edited, (offset, count)), 1 << 20);
        Assert.Throws<InvalidDataException>(() => delta.Apply(Basis));
    }

    [Fact]
    public void MalformedFramesAreRefused()
    {
        var frame = Frame(Basis, Edited, (0, 1000), Inserted, (1000, 3096));
        Assert.Throws<InvalidDataException>(() => CaptureDelta.Read(frame[..50], 1 << 20));
        var version = (byte[])frame.Clone(); version[4] = 2;
        Assert.Throws<InvalidDataException>(() => CaptureDelta.Read(version, 1 << 20));
        Assert.Throws<InvalidDataException>(() => CaptureDelta.Read(frame, 1000)); // larger than the server takes
        var truncated = CaptureDelta.Read(frame[..^3], 1 << 20);
        Assert.Throws<InvalidDataException>(() => truncated.Apply(Basis));
        var unknown = CaptureDelta.Read([.. frame, 7], 1 << 20);
        Assert.Throws<InvalidDataException>(() => unknown.Apply(Basis));
    }
}
