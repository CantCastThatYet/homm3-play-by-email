# release-notes.ps1 -Version 0.1.5: the GitHub release notes for a version, from its CHANGELOG.md section
# and the install instructions. The release workflow publishes them; run it by hand to edit an old release.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$changelog = Get-Content (Join-Path $PSScriptRoot '..\CHANGELOG.md') -Encoding utf8
$start = [array]::FindIndex($changelog, [Predicate[string]] { param($line) $line.StartsWith("## [$Version]") })
if ($start -lt 0) { throw "CHANGELOG.md has no section for $Version." }
$end = [array]::FindIndex($changelog, $start + 1, [Predicate[string]] { param($line) $line.StartsWith('## ') })
if ($end -lt 0) { $end = $changelog.Length }
# The section without its heading; level-3 headings become level 2 under the release title.
# GitHub shows a line break where CHANGELOG.md wraps a line, so wrapped lines are joined.
$section = ($changelog[($start + 1)..($end - 1)] -join "`n").Trim() -replace '(?m)^### ', '## ' -replace "`n  ", ' '
@"
$section

## Install
**Players:** download HeroesPost-Setup and run it. It finds HotA, checks the version and installs. Then start HotA, open the multiplayer screen and press Heroes Post; the first time it asks for your server's address. If your HotA or HD Mod version is not supported, it says so: update both through HotA's launcher.

The zip holds the same files for installing by hand (see README.txt inside).

**Игрокам:** скачайте HeroesPost-Setup и запустите его. Он сам найдёт HotA, проверит версию и установит мод. Затем запустите HotA, откройте экран сетевой игры и нажмите Heroes Post; в первый раз он спросит адрес вашего сервера. Если версия HotA или HD Mod не поддерживается, установщик об этом скажет: обновите их через лаунчер HotA.
"@
