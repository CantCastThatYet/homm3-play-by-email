[CmdletBinding()]
param([switch]$Test, [string]$VcVarsPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$workspaceRoot = Split-Path $nativeRoot -Parent
$outputRoot = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $workspaceRoot 'artifacts/native/win-x86' }
$vcvars = $VcVarsPath
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Required installed MSVC x86 environment not found: $vcvars" }
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$buildFile = Join-Path $outputRoot 'build-native.cmd'
$script = @"
@echo off
call "$vcvars" >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "$outputRoot"
cl /nologo /MP4 /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" /LD "$nativeRoot\adapter.cpp" "$nativeRoot\common.cpp" "$nativeRoot\capture.cpp" "$nativeRoot\completion_observer.cpp" "$nativeRoot\menu_dispatch.cpp" "$nativeRoot\session_bootstrap.cpp" "$nativeRoot\session_request.cpp" "$nativeRoot\scenario_config.cpp" "$nativeRoot\directplay_port.cpp" "$nativeRoot\scenario_start.cpp" "$nativeRoot\adventure_actions.cpp" "$nativeRoot\departure_pause.cpp" "$nativeRoot\map_intro.cpp" "$nativeRoot\completed_leave.cpp" "$nativeRoot\world_resume.cpp" "$nativeRoot\memfile_import.cpp" "$nativeRoot\resume_session.cpp" "$nativeRoot\solo_transport.cpp" "$nativeRoot\solo_journal.cpp" "$nativeRoot\dplay_relay.cpp" "$nativeRoot\lobby_ui.cpp" "$nativeRoot\lobby_client.cpp" "$nativeRoot\seat_keeper.cpp" "$nativeRoot\json_lite.cpp" /Zi /Fe:HeroesPost.Native.dll /link /DEBUG /OPT:REF /OPT:ICF /DEF:"$nativeRoot\adapter.def" /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /MP4 /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\injector.cpp" "$nativeRoot\common.cpp" /Fe:HeroesPost.Injector.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /LD "$nativeRoot\version_proxy.cpp" /Fe:version.dll /link /DEF:"$nativeRoot\version_proxy.def" /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /MP4 /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\smoke_test.cpp" "$nativeRoot\common.cpp" /Fe:Native.SmokeTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
exit /b 0
"@
[System.IO.File]::WriteAllText($buildFile, $script, [System.Text.Encoding]::ASCII)
& $env:ComSpec /c $buildFile
if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)." }
if ($Test) {
    & (Join-Path $nativeRoot 'test.ps1') -VcVarsPath $vcvars -OutputDirectory $outputRoot
}
Write-Output "Native x86 outputs: $outputRoot"
