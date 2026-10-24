using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace HeroesLobby.NativeHost;

internal static class NativePath
{
    // File-handle resolution also normalizes Windows short-name aliases. Callers
    // still reject reparse points before opening files in an owned installation.
    internal static string FinalFilePath(string path)
    {
        using var file = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var buffer = new StringBuilder(32768);
        var length = GetFinalPathNameByHandleW(file, buffer, (uint)buffer.Capacity, 0);
        if (length == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (length >= buffer.Capacity) throw new IOException("The resolved executable path is too long.");
        var resolved = buffer.ToString();
        if (resolved.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) return @"\\" + resolved[8..];
        return resolved.StartsWith(@"\\?\", StringComparison.Ordinal) ? resolved[4..] : resolved;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    private static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint capacity, uint flags);
}
