# package.ps1: the player download. Builds the game patch, then
#   HeroesPost-Setup-<version>.exe  the installer, carrying both DLLs (native/installer.cpp)
#   HeroesPost-<version>.zip         the same two files for installing by hand, with a readme
#   SHA256SUMS.txt
# in artifacts/release. Players choose the server in the game.
[CmdletBinding()]
param([string]$Version = '', [string]$VcVarsPath = '', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$workspaceRoot = Split-Path $nativeRoot -Parent
$outputRoot = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $workspaceRoot 'artifacts/release' }

if (-not $VcVarsPath) {
    $VcVarsPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
    if (-not (Test-Path -LiteralPath $VcVarsPath)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($vs) { $VcVarsPath = Join-Path $vs 'VC\Auxiliary\Build\vcvars32.bat' }
        }
    }
}
if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw "MSVC x86 environment not found: $VcVarsPath" }

if (-not $Version) {
    # Through cmd: Windows PowerShell turns git's "no tags" message into a terminating error.
    $tag = & $env:ComSpec /c "git -C `"$workspaceRoot`" describe --tags --abbrev=0 2>nul"
    $Version = if ($LASTEXITCODE -eq 0 -and $tag) { $tag.TrimStart('v') } else { '0.1.0' }
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 1.2.3: $Version" }

$build = Join-Path $workspaceRoot 'artifacts/native/release-x86'
& (Join-Path $nativeRoot 'build.ps1') -VcVarsPath $VcVarsPath -OutputDirectory $build | Out-Null

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$rcText = { param($s) $s.Replace('\', '\\').Replace('"', '""') }
$numbers = ($Version.Split('.') + '0') -join ','
$rc = @"
#include <winver.h>
1 24 "$(& $rcText (Join-Path $nativeRoot 'installer.manifest'))"
101 RCDATA "$(& $rcText (Join-Path $build 'version.dll'))"
102 RCDATA "$(& $rcText (Join-Path $build 'HeroesPost.Native.dll'))"
STRINGTABLE
BEGIN
    2 "$Version"
END
VS_VERSION_INFO VERSIONINFO
FILEVERSION $numbers
PRODUCTVERSION $numbers
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "CompanyName", "Heroes Post"
            VALUE "FileDescription", "Heroes Post Setup"
            VALUE "FileVersion", "$Version"
            VALUE "ProductName", "Heroes Post for HotA"
            VALUE "ProductVersion", "$Version"
            VALUE "OriginalFilename", "HeroesPost-Setup.exe"
            VALUE "LegalCopyright", "MIT License, Heroes Post contributors"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
"@
[System.IO.File]::WriteAllText((Join-Path $build 'installer.rc'), $rc, [System.Text.Encoding]::ASCII)

$setup = "HeroesPost-Setup-$Version.exe"
$script = @"
@echo off
call "$VcVarsPath" >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "$build"
rc /nologo /fo installer.res installer.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\installer.cpp" installer.res /Fe:"$outputRoot\$setup" /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
exit /b 0
"@
$cmd = Join-Path $build 'build-installer.cmd'
[System.IO.File]::WriteAllText($cmd, $script, [System.Text.Encoding]::ASCII)
& $env:ComSpec /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Installer build failed ($LASTEXITCODE)." }
Remove-Item (Join-Path $outputRoot '*.obj') -ErrorAction SilentlyContinue

# The zip: the two DLLs and the readme (with a BOM, for Notepad).
$stage = Join-Path $build 'zip'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item (Join-Path $build 'version.dll'), (Join-Path $build 'HeroesPost.Native.dll') $stage
$readme = [System.IO.File]::ReadAllText((Join-Path $nativeRoot 'install-readme.txt')).Replace("`r`n", "`n").Replace("`n", "`r`n")
[System.IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme, (New-Object System.Text.UTF8Encoding $true))
$zip = Join-Path $outputRoot "HeroesPost-$Version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

$sums = foreach ($f in (Join-Path $outputRoot $setup), $zip) { "$((Get-FileHash $f -Algorithm SHA256).Hash.ToLower())  $(Split-Path $f -Leaf)" }
[System.IO.File]::WriteAllLines((Join-Path $outputRoot 'SHA256SUMS.txt'), [string[]]$sums)
Write-Output "Heroes Post $Version"
Get-ChildItem $outputRoot -File | ForEach-Object { Write-Output ("  {0}  {1:N0} bytes" -f $_.Name, $_.Length) }
