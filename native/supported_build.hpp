// The one HotA build Heroes Post is made for: HotA 1.8.1 with the HD Mod it comes with. The game
// patch checks the loaded modules against these hashes and stays off otherwise; the installer
// checks the files before it installs anything.
#pragma once

namespace hp_build {
struct File {
    const wchar_t* module; // the loaded module's file name
    const wchar_t* path;   // the file in the HotA folder
    const char* sha256;
};
// The adapter relies on the order: [1] is HD_HOTA.dll, [5] is HotA's own DLL, which HD loads
// from its _181fix copy.
inline constexpr File Files[] = {
    {L"h3hota HD.exe", L"h3hota HD.exe", "5aaab925f06cccf23bb09814767590a95b84a557eb33d244800520be4f1f18de"},
    {L"HD_HOTA.dll", L"HD_HOTA.dll", "0a2c193d0c510d77080b669a710694ef282761b0ec6f6ac5b86f3166b8038550"},
    {L"HW_HOTA.dll", L"HW_HOTA.dll", "927a087c6d05d40c83de270516f48fdbd1360614259d30689179c1f7300f08db"},
    {L"patcher_x86.dll", L"patcher_x86.dll", "6134c353e5ac2a056e52bf548cdea853c4424fa5f955df1f5a1f073d784bab40"},
    {L"_HD3_.dll", L"_HD3_.dll", "3ed1852e08bae7be2461b31bd922531ccb0c46d01e5b0ecf46d0e7d0dbeeab49"},
    {L"HotA.dll", L"_HD3_Data\\HotA.dll_181fix", "1fa9ad209e756d55e9578b290c4fe6c9ba030d3b52af2085e530416a61ac10c2"},
};
inline constexpr const wchar_t* Name = L"HotA 1.8.1";
}
