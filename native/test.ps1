[CmdletBinding()]
param([string]$VcVarsPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$workspaceRoot = Split-Path $nativeRoot -Parent
$outputRoot = if ($OutputDirectory) { [System.IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $workspaceRoot 'artifacts/native/win-x86' }
if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw "Required installed MSVC x86 environment not found: $VcVarsPath" }
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$testBuild = Join-Path $outputRoot 'build-native-tests.cmd'
$script = @"
@echo off
call "$VcVarsPath" >nul
if errorlevel 1 exit /b 1
cd /d "$outputRoot"
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\capture_test.cpp" "$nativeRoot\capture.cpp" /Fe:Native.CaptureTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\completion_observer_test.cpp" "$nativeRoot\completion_observer.cpp" /Fe:Native.CompletionTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\memfile_import_test.cpp" "$nativeRoot\memfile_import.cpp" "$nativeRoot\common.cpp" /Fe:Native.MemFileTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\session_request_test.cpp" "$nativeRoot\session_request.cpp" "$nativeRoot\common.cpp" /Fe:Native.SessionRequestTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\directplay_port_test.cpp" /Fe:Native.PortTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\departure_pause_test.cpp" /Fe:Native.DepartureTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\adventure_actions_test.cpp" "$nativeRoot\common.cpp" /Fe:Native.AdventureTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\map_intro_test.cpp" "$nativeRoot\common.cpp" /Fe:Native.IntroTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\world_resume_test.cpp" "$nativeRoot\world_resume.cpp" "$nativeRoot\memfile_import.cpp" "$nativeRoot\common.cpp" /Fe:Native.WorldResumeTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\solo_journal_test.cpp" "$nativeRoot\solo_journal.cpp" "$nativeRoot\common.cpp" /Fe:Native.SoloJournalTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\solo_transport_test.cpp" "$nativeRoot\dplay_relay.cpp" "$nativeRoot\common.cpp" /Fe:Native.SoloTransportTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /MP4 /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\resume_session_test.cpp" "$nativeRoot\world_resume.cpp" "$nativeRoot\memfile_import.cpp" "$nativeRoot\dplay_relay.cpp" "$nativeRoot\common.cpp" /Fe:Native.ResumeSessionTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"$nativeRoot" "$nativeRoot\dplay_relay_test.cpp" "$nativeRoot\dplay_relay.cpp" "$nativeRoot\common.cpp" /Fe:Native.RelayTest.exe /link /DYNAMICBASE /NXCOMPAT
if errorlevel 1 exit /b 1
exit /b 0
"@
[System.IO.File]::WriteAllText($testBuild, $script, [System.Text.Encoding]::ASCII)
& $env:ComSpec /c $testBuild
if ($LASTEXITCODE -ne 0) { throw "Native capture test build failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.CaptureTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native capture tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.CompletionTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native completion tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.MemFileTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native MemFile tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.SessionRequestTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native session request tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.PortTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native DirectPlay port tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.DepartureTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native departure state tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.AdventureTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native adventure gate tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.IntroTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native known intro tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.SmokeTest.exe') (Join-Path $outputRoot 'HeroesPost.Native.dll')
if ($LASTEXITCODE -ne 0) { throw "Native smoke tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.WorldResumeTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native world recovery tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.SoloJournalTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native solo journal tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.ResumeSessionTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native resume session tests failed ($LASTEXITCODE)." }
& (Join-Path $outputRoot 'Native.SoloTransportTest.exe')
if ($LASTEXITCODE -ne 0) { throw "Native solo transport tests failed ($LASTEXITCODE)." }
# This script never rebuilds the adapter DLL or injector and never starts the game.
