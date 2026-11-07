<#
Jak Enhanced Trilogy: the whole release, from a clean commit to the published GitHub release and
the OpenGOAL launcher's mods.json (what was done by hand up to v0.14.7, see ENHANCED.md "Releasing").

    & ([scriptblock]::Create([IO.File]::ReadAllText('scripts\enhanced\release.ps1'))) -Version v0.14.8
    & ([scriptblock]::Create([IO.File]::ReadAllText('scripts\enhanced\release.ps1'))) -Version v0.14.8 -Publish
    ... -Version v0.14.8 -Publish -Resume       (a stopped run goes on from the first step not done)

Without -Publish it is a DRY RUN: it prints every command it would run and does only the safe local
checks (clean tree, the release on GitHub, programs, notes, previous ZIPs, tools, shaders, mods.json
on a copy); the only thing it writes is the draft notes, when they are missing. No build, no
packages, no push, no upload.

Steps: clean -> release (on GitHub) -> gate -> build -> notes -> full -> light -> push -> draft ->
upload (metadata + patches, Setup, ZIP) -> publish (--latest) -> light-upload -> verify -> mods -> mods-push.
State: local\release\<version>\state.json under the release root (-Resume skips the steps done).
Pushes: the release commit only after a check of what goes out (Read-Host, or -ConfirmPush <sha>);
the mods.json commit by itself when it is the only commit that goes out.
Stops by throwing (never exit: run as a scriptblock, exit would close the caller's shell);
$global:LASTEXITCODE is 0 or 1 at the end.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Publish,
    [switch]$SkipBuild,
    [switch]$SkipGate,
    [switch]$Resume,
    [string]$Root = '',           # the tree the release is made from (default: the git tree of the current folder)
    [string]$OutDir = '',         # where the packages go (default: <Root>\out\release)
    [string[]]$PrevDirs = @(),    # where the previous full ZIPs are (default: OutDir + the main tree's out\release)
    [string[]]$PatchFrom = @(),   # versions to patch from (default: the 3 newest full ZIPs before this one)
    [string]$ConfirmPush = '',    # the release commit's sha (>= 7 chars), checked by a human: push without asking
    [int]$Jobs = 12,
    [int]$Tries = 15
)

$ErrorActionPreference = 'Continue'
$Repo = 'stichlola/jak-enhanced-trilogy'
$Remote = 'enhanced'
$Py = "$env:LOCALAPPDATA\Python\pythoncore-3.14-64\python.exe"
$VsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$Vcvars = "$VsRoot\VC\Auxiliary\Build\vcvars64.bat"
$CMake = "$VsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Llvm = "$VsRoot\VC\Tools\Llvm\x64\bin"
$Exes = @('gk.exe', 'goalc.exe', 'extractor.exe', 'decompiler.exe')
$ProgramSources = @('game', 'common', 'decompiler', 'goalc')   # make_release.py PROGRAM_SOURCES
$Steps = @('clean', 'release', 'gate', 'build', 'notes', 'full', 'light', 'push', 'draft', 'upload',
           'publish', 'light-upload', 'verify', 'mods', 'mods-push')
$Dry = -not $Publish
$script:Results = [ordered]@{}
$script:Checks = New-Object System.Collections.Generic.List[string]
$script:Failed = $false

$StopMark = 'RELEASE-STOP: '

function Say([string]$m) { Write-Host "$(Get-Date -Format HH:mm:ss) $m" }
function Stop-Release([string]$m) { throw ($StopMark + $m) }
# a check: in a dry run all are made and listed; with -Publish the first failure stops
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    $line = "{0,-4} {1}{2}" -f $(if ($ok) { 'OK' } else { 'FAIL' }), $name, $(if ($detail) { ": $detail" } else { '' })
    $script:Checks.Add($line)
    Write-Host "  $line"
    if (-not $ok) {
        $script:Failed = $true
        if (-not $Dry) { Stop-Release "$name$(if ($detail) { ": $detail" })" }
    }
}
function Would([string]$cmd) { Write-Host "  [dry] $cmd" }
function Native([string]$exe, [string[]]$a, [switch]$Quiet) {
    # runs a program, prints its output as it comes, returns the lines; exit code in $LASTEXITCODE
    $lines = New-Object System.Collections.Generic.List[string]
    & $exe @a 2>&1 | ForEach-Object { $s = "$_"; $lines.Add($s); if (-not $Quiet) { Write-Host "    $s" } }
    return , $lines
}
function GitQ { $o = & git.exe -C $Root @args 2>$null; return $o }   # (no param block: --flags pass through)
function VersionOf([string]$v) { return [version]($v.TrimStart('v')) }
function Utf8([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# --- state (local\release\<version>\state.json) ---
function Load-State {
    $s = @{ version = $Version; commit = ''; steps = @{} }
    if (Test-Path $StateFile) {
        $j = [IO.File]::ReadAllText($StateFile) | ConvertFrom-Json
        $s.commit = [string]$j.commit
        foreach ($p in $j.steps.PSObject.Properties) { $s.steps[$p.Name] = [string]$p.Value }
    }
    return $s
}
function Save-State {
    if ($Dry) { return }
    New-Item -ItemType Directory -Force (Split-Path $StateFile) | Out-Null
    Utf8 $StateFile (([pscustomobject]@{ version = $State.version; commit = $State.commit; steps = $State.steps }) |
        ConvertTo-Json -Depth 4)
}
function Done([string]$step) { return $State.steps.ContainsKey($step) }
function Mark([string]$step) { $State.steps[$step] = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); Save-State }
function Step([string]$name, [scriptblock]$body) {
    if ($Resume -and (Done $name)) { $script:Results[$name] = "done before ($($State.steps[$name]))"; Say "== ${name}: done before, skipped"; return }
    Say "== $name"
    $t = Get-Date
    & $body
    $secs = [int]((Get-Date) - $t).TotalSeconds
    if ($Dry) { if (-not $script:Results.Contains($name)) { $script:Results[$name] = "dry ($secs s)" } }
    else { if (-not $script:Results.Contains($name)) { $script:Results[$name] = "done ($secs s)" }; Mark $name }
}

# --- GitHub ---
function Get-Release {
    # $null when there is no release with this tag; throws when gh can't tell (network, login)
    $out = & gh release view $Version -R $Repo --json 'tagName,isDraft,assets' 2>&1
    $text = ($out | Where-Object { $_ -is [string] }) -join "`n"
    if ($LASTEXITCODE -eq 0) { return ($text | ConvertFrom-Json) }
    $err = ($out | ForEach-Object { "$_" }) -join ' '
    if ($err -match 'release not found|Not Found') { return $null }
    Stop-Release "gh release view: $err"
}
function Test-Up([string]$path) {
    $name = Split-Path $path -Leaf
    $rel = Get-Release
    if (-not $rel) { return $false }
    $a = @($rel.assets | Where-Object { $_.name -eq $name })
    return ($a.Count -eq 1 -and $a[0].state -eq 'uploaded' -and [int64]$a[0].size -eq (Get-Item $path).Length)
}
function Up([string]$path) {
    # each file its own retry loop (a slow line: 408s, TLS and DNS timeouts); --clobber replaces a broken asset
    $name = Split-Path $path -Leaf
    $mb = [math]::Round((Get-Item $path).Length / 1MB, 1)
    if ($Dry) { Would "gh release upload $Version -R $Repo --clobber `"$path`"   ($mb MB, up to $Tries tries, 30 s apart)"; return $true }
    for ($i = 1; $i -le $Tries; $i++) {
        try { if (Test-Up $path) { Say "$name is up"; return $true } } catch { Say "check failed: $($_.Exception.Message)" }
        Say "$name ($mb MB) try $i"
        & gh release upload $Version -R $Repo --clobber $path 2>&1 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -eq 0) {
            try { if (Test-Up $path) { Say "$name done"; return $true } } catch { }
        }
        Start-Sleep -Seconds 30
    }
    try { return (Test-Up $path) } catch { return $false }
}

# --- main ---
try {
    if ($Version -notmatch '^v\d+\.\d+\.\d+$') { Stop-Release "-Version must be vX.Y.Z (got '$Version')" }
    if (-not $Root) { $Root = (& git rev-parse --show-toplevel 2>$null) }
    if (-not $Root -or -not (Test-Path "$Root\scripts\enhanced\make_release.py")) {
        Stop-Release "no release root (run it from the repo, or -Root <tree>): '$Root'"
    }
    $Root = (Resolve-Path $Root).Path
    if (-not $OutDir) { $OutDir = Join-Path $Root 'out\release' }
    $MainTree = (((& git -C $Root worktree list --porcelain) | Select-Object -First 1) -replace '^worktree ', '') -replace '/', '\'
    if (-not $PrevDirs) { $PrevDirs = @($OutDir, (Join-Path $MainTree 'out\release')) | Select-Object -Unique }
    $StateFile = Join-Path $Root "local\release\$Version\state.json"
    $Bin = Join-Path $Root 'out\build\Release-static\bin'
    $Notes = Join-Path $OutDir "notes-$Version.md"
    $Plain = $Version.TrimStart('v')
    $State = Load-State
    if (-not $Resume -and -not $Dry) { $State.steps = @{} }
    $head = (GitQ rev-parse HEAD)

    Say "Jak Enhanced Trilogy ${Version}: $(if ($Dry) { 'DRY RUN (nothing built, pushed or uploaded; -Publish to do it)' } else { 'PUBLISH' })"
    Say "root $Root (HEAD $($head.Substring(0, 9)), branch $(GitQ rev-parse --abbrev-ref HEAD))$(if ($Root -ieq $MainTree) { ', the main tree' })"
    Say "out  $OutDir"
    if ($Resume) {
        if (-not (Test-Path $StateFile)) { Stop-Release "-Resume: no state file $StateFile" }
        # resumed on the commit the packages came from, or that plus the mods.json commit
        $extra = @(GitQ diff --name-only $State.commit HEAD)
        & git -C $Root merge-base --is-ancestor $State.commit HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or ($extra | Where-Object { $_ -ne 'mods.json' })) {
            Stop-Release "-Resume: HEAD is not the commit of the packages ($($State.commit)): start again without -Resume"
        }
        Say "resuming: done $(@($State.steps.Keys) -join ', ')"
    }
    if (-not $State.commit -or -not $Resume) { $State.commit = $head }

    # ---- 0. the tree is the commit (make_release.py copies the working tree, not HEAD)
    Step 'clean' {
        $dirty = @(GitQ status --porcelain)
        Check 'clean tree' ($dirty.Count -eq 0) $(if ($dirty.Count) { "$($dirty.Count) changes: $(($dirty | Select-Object -First 5) -join '; ')" })
        foreach ($t in @($Py, 'gh', 'git')) { Check "tool $t" ([bool](Get-Command $t -ErrorAction SilentlyContinue)) }
        $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ }
        Check 'Inno Setup ISCC.exe' ([bool]$iscc)
        # HANDMADE: out of git, so a new worktree hasn't got them (make_release.py stops)
        $hmPy = "import sys; sys.path.insert(0, r'$Root\scripts\enhanced'); import make_release as m; " +
            "[print(f'{d}|{n}|{sum(1 for f in (m.ROOT / d).rglob(chr(42)) if f.is_file() and not any(f.match(p) for p in m.NOT_HANDMADE)) if (m.ROOT / d).is_dir() else 0}') for d, n in m.HANDMADE]"
        $hm = Native $Py @('-c', $hmPy) -Quiet
        Check 'HANDMADE list read' (@($hm | Where-Object { $_ -match '\|\d+\|\d+$' }).Count -gt 0) (($hm | Select-Object -Last 1) -as [string])
        foreach ($l in $hm) {
            $p = $l -split '\|'
            if ($p.Count -eq 3) { Check "HANDMADE $($p[0])" ([int]$p[2] -ge [int]$p[1]) "$($p[2]) files (at least $($p[1]))" }
        }
    }

    # ---- 0b. no release yet, or a draft left behind (resumed, never a second one)
    Step 'release' {
        $rel = Get-Release
        if (-not $rel) { Check "release $Version not on GitHub yet" $true }
        elseif ($rel.isDraft) { Check "release $Version is a draft left behind: resumed" $true "$(@($rel.assets).Count) assets" }
        else { Check "release $Version not published yet" ($Resume -and (Done 'publish')) 'already published (only -Resume after this pipeline published it)' }
    }

    # ---- 1. the gate (its own process: an exit in it can't end this script)
    Step 'gate' {
        $gate = Join-Path $Root 'scripts\enhanced\release_gate.ps1'
        if ($SkipGate) { $script:Results['gate'] = 'skipped (-SkipGate)'; Say 'skipped (-SkipGate)'; return }
        if (-not (Test-Path $gate)) { $script:Results['gate'] = 'none (no release_gate.ps1 in this tree)'; Say 'no release_gate.ps1: skipped'; return }
        $cmd = "& ([scriptblock]::Create([IO.File]::ReadAllText('$gate')))"
        if ($Dry) { Would "powershell -NoProfile -NonInteractive -Command `"$cmd`"   (env JAK_RELEASE_VERSION=$Version, cwd $Root)"; return }
        $env:JAK_RELEASE_VERSION = $Version
        Push-Location $Root
        try { & powershell.exe -NoProfile -NonInteractive -Command $cmd 2>&1 | ForEach-Object { Write-Host "    $_" }; $code = $LASTEXITCODE }
        finally { Pop-Location }
        Check 'release gate' ($code -eq 0) "exit $code"
    }

    # ---- 2. the static build (Release-static: no runtime DLLs), configured first in a new tree
    Step 'build' {
        $cache = Join-Path $Root 'out\build\Release-static\CMakeCache.txt'
        if ($SkipBuild) { $script:Results['build'] = 'skipped (-SkipBuild)'; Say 'skipped (-SkipBuild)' }
        else {
            Check 'vcvars64 + cmake' ((Test-Path $Vcvars) -and (Test-Path $CMake))
            $cfg = "`"$CMake`" -S . -B out/build/Release-static -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTATICALLY_LINK=true -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
            $bld = "`"$CMake`" --build out/build/Release-static --target gk goalc extractor decompiler -j $Jobs"
            $bat = @('@echo off', "call `"$Vcvars`" >nul", "set `"PATH=$Llvm;%PATH%`"", "cd /d `"$Root`"",
                'if exist out\build\Release-static\CMakeCache.txt goto build', 'echo CONFIGURE', $cfg,
                'if errorlevel 1 goto cfgfail', ':build', $bld, 'echo BUILD EXIT %ERRORLEVEL%', 'exit /b 0',
                ':cfgfail', 'echo CONFIGURE FAILED', 'exit /b 1') -join "`r`n"
            if ($Dry) {
                if (-not (Test-Path $cache)) { Would "(not configured yet: slow) $cfg" }
                Would "cmd /c build.bat: vcvars64; $bld   (cwd $Root)"
            } else {
                $batPath = Join-Path (Split-Path $StateFile) 'build.bat'
                New-Item -ItemType Directory -Force (Split-Path $batPath) | Out-Null
                [IO.File]::WriteAllText($batPath, $bat + "`r`n", [Text.Encoding]::ASCII)
                $out = Native 'cmd.exe' @('/c', $batPath)
                Check 'build' (@($out | Where-Object { $_ -match '^BUILD EXIT 0\s*$' }).Count -eq 1) ($out | Select-Object -Last 1)
            }
        }
        # the programs are there and not older than their sources (make_release.py only warns)
        $last = [int64](GitQ log -1 --format=%ct '--' @ProgramSources)
        foreach ($e in $Exes) {
            $p = Join-Path $Bin $e
            if (-not (Test-Path $p)) { Check "program $e" ($Dry -and -not $SkipBuild) 'missing (the build makes it)'; continue }
            $m = [int64]([DateTimeOffset](Get-Item $p).LastWriteTimeUtc).ToUnixTimeSeconds()
            Check "program $e" (($m -ge $last) -or ($Dry -and -not $SkipBuild)) $(if ($m -lt $last) { 'older than the last commit to its sources' })
        }
    }

    # ---- 3. the notes: a draft from the commits if missing, to be rewritten in player language
    $fulls = @()
    foreach ($d in $PrevDirs) {
        if (Test-Path $d) {
            $fulls += Get-ChildItem $d -Filter 'Jak-Enhanced-Trilogy-v*.zip' | Where-Object { $_.Name -match '^Jak-Enhanced-Trilogy-(v\d+\.\d+\.\d+)\.zip$' }
        }
    }
    $byVer = [ordered]@{}
    foreach ($f in $fulls) { $v = $f.Name -replace '^Jak-Enhanced-Trilogy-(v[\d.]+)\.zip$', '$1'; if (-not $byVer.Contains($v)) { $byVer[$v] = $f.FullName } }
    $older = @($byVer.Keys | Where-Object { (VersionOf $_) -lt (VersionOf $Version) } | Sort-Object { VersionOf $_ } -Descending)
    if ($PatchFrom) { $bases = @($PatchFrom | ForEach-Object { 'v' + $_.TrimStart('v') }) } else { $bases = @($older | Select-Object -First 3) }
    $patchZips = @()
    foreach ($b in $bases) { if ($byVer.Contains($b)) { $patchZips += $byVer[$b] } }

    Step 'notes' {
        Check 'previous full ZIPs' ($bases.Count -gt 0 -and $patchZips.Count -eq $bases.Count) "patch from $($bases -join ', ') (found: $(@($byVer.Keys) -join ', '))"
        if (-not (Test-Path $Notes)) {
            # the head (intro, the donation line) from the newest notes there are
            $prev = @($PrevDirs | Where-Object { Test-Path $_ } | ForEach-Object { Get-ChildItem $_ -Filter 'notes-v*.md' } |
                Where-Object { $_.Name -match '^notes-v\d+\.\d+\.\d+\.md$' -and $_.Name -ne "notes-$Version.md" } |
                Sort-Object { VersionOf ($_.BaseName -replace '^notes-', '') } -Descending)
            $headText = ''
            if ($prev) { $headText = ([IO.File]::ReadAllText($prev[0].FullName) -split '## Which one to download')[0].TrimEnd() }
            if (-not $headText) {
                $eur = [char]0x20AC
                $headText = "Jak Enhanced Trilogy: Jak 1, Jak II and Jak 3 on PC (OpenGOAL) with modern graphics and new things to do. No game files are included: everything is made from your own discs.`n`n" +
                    "**Jak Enhanced is 100% free**, and always will be. I make it on my own in my spare time: if you'd like to support my work, you can buy me a coffee: [1 $eur a sweet](https://paypal.me/mymusicminis/1EUR) $([char]0xB7) [3 $eur a coffee](https://paypal.me/mymusicminis/3EUR) $([char]0xB7) [any amount](https://paypal.me/mymusicminis). Entirely optional, it unlocks nothing."
            }
            $dots = [char]0x2026
            $patchCell = ($bases | ForEach-Object { "$dots-patch-$_.zip" }) -join ' / '
            # the commits since the last published release
            $subjects = @()
            $lastTag = ''
            try { $lastTag = (& gh release view -R $Repo --json tagName --jq .tagName 2>$null) } catch { }
            $since = ''
            if ($lastTag) { $since = (& gh api "repos/$Repo/commits/$lastTag" --jq .sha 2>$null) }
            if ($since) { & git -C $Root cat-file -e "$since^{commit}" 2>$null; if ($LASTEXITCODE -ne 0) { $since = '' } }
            if ($since) { $subjects = @(GitQ log --no-merges --format=%s "$since..HEAD") } else { $subjects = @(GitQ log --no-merges --format=%s -40) }
            $list = ($subjects | ForEach-Object { "- $_" }) -join "`n"
            $body = "$headText`n`n## Which one to download`n`n| File | What it is | For whom |`n|---|---|---|`n" +
                "| **Setup-Jak-Enhanced-Trilogy-$Version.exe** | The installer. It asks for your disc images, extracts and compiles the games, optionally makes the HD textures on your graphics card, and can add the game to Steam. | Most players: pick this one. |`n" +
                "| **Jak-Enhanced-Trilogy-$Version.zip** | The same game without an installer: unzip it anywhere, put your .iso files in the ISO folder, double-click `"Jak Enhanced Trilogy.bat`". | Who prefers a portable folder. |`n" +
                "| **$patchCell** | Only the files changed since that version (the in-game update uses them). | Who updates by hand. |`n" +
                "| **windows-jak-enhanced-light-$Version.zip** | Jak Enhanced Light, for the official OpenGOAL launcher. | Who plays with the OpenGOAL launcher. |`n`n" +
                "Already installed? The game offers the update when it starts; saves and settings are kept, and the download is small (only the changed files).`n`n" +
                "<!-- DRAFT: made by release.ps1 from the commits since $(if ($lastTag) { $lastTag } else { 'the last 40' }). Rewrite New and Fixed in plain player language, then delete this line and the commit list. -->`n`n" +
                "## New`n`n- `n`n## Fixed`n`n- `n`nJak Enhanced Light gets the same game fixes.`n`n" +
                "Something wrong or a wish? F1 in the game $([char]0x2192) Send feedback, or `"Report a problem.bat`".`n`n" +
                "<!-- DRAFT commits:`n$list`n-->`n"
            New-Item -ItemType Directory -Force $OutDir | Out-Null
            Utf8 $Notes $body
            Say "draft notes written: $Notes ($($subjects.Count) commits since $(if ($lastTag) { $lastTag } else { '?' }))"
        }
        $text = [IO.File]::ReadAllText($Notes)
        Check 'notes name the version' ($text.Contains($Version))
        $named = @([regex]::Matches($text, '-patch-(v\d+\.\d+\.\d+)\.zip') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
        Check 'notes name the patches made' ((@($named) -join ',') -eq (@($bases | Sort-Object -Unique) -join ',')) "notes: $($named -join ', '); patches: $($bases -join ', ')"
        $draft = $text.Contains('<!-- DRAFT')
        Check 'notes rewritten (no DRAFT marker)' (-not $draft) $(if ($draft) { "edit $Notes" })
    }

    # ---- 4. the packages (never --skip-shader-check); any WARNING about the tree or the programs stops
    function Make([string]$step, [string[]]$extra) {
        $a = @("$Root\scripts\enhanced\make_release.py", '--version', $Version, '--notes', $Notes, '--out', $OutDir) + $extra
        if ($Dry) { Would "`"$Py`" $($a -join ' ')"; return }
        $out = Native $Py $a
        Check "make_release $step" ($LASTEXITCODE -eq 0) "exit $LASTEXITCODE"
        $warn = @($out | Where-Object { $_ -match '^WARNING:' -and $_ -notmatch 'placed\.json' })
        Check "make_release $step without warnings" ($warn.Count -eq 0) ($warn -join ' | ')
    }
    $Setup = Join-Path $OutDir "Setup-Jak-Enhanced-Trilogy-$Version.exe"
    $Zip = Join-Path $OutDir "Jak-Enhanced-Trilogy-$Version.zip"
    $Patches = @($bases | ForEach-Object { Join-Path $OutDir "Jak-Enhanced-Trilogy-$Version-patch-$_.zip" })
    $Light = Join-Path $OutDir "windows-jak-enhanced-light-$Version.zip"
    $Meta = Join-Path $OutDir 'metadata.json'
    function Sized([string]$path, [double]$minMB, [double]$maxMB) {
        $ok = Test-Path $path
        $mb = 0; if ($ok) { $mb = [math]::Round((Get-Item $path).Length / 1MB, 1) }
        Check "file $(Split-Path $path -Leaf)" ($ok -and $mb -ge $minMB -and $mb -le $maxMB) "$(if ($ok) { "$mb MB" } else { 'missing' }) (expected $minMB-$maxMB MB)"
    }
    Step 'full' {
        if ($Dry) {
            # the shader check make_release.py starts with (glslang, ASCII only): safe to run now
            $null = Native $Py @("$Root\scripts\enhanced\check_shaders.py") -Quiet
            Check 'shader check' ($LASTEXITCODE -eq 0) "exit $LASTEXITCODE"
        }
        $extra = @('--installer'); if ($patchZips) { $extra += @('--patch-from') + $patchZips }
        Make 'full' $extra
        if (-not $Dry) { Sized $Zip 200 1500; Sized $Setup 100 1500; foreach ($p in $Patches) { Sized $p 1 150 } }
    }
    Step 'light' {
        Make 'light' @('--light')
        if (-not $Dry) {
            Sized $Light 200 1500
            Check 'metadata.json' ((Test-Path $Meta) -and ([IO.File]::ReadAllText($Meta) -match 'supportedGames')) $Meta
        }
    }

    # ---- 5. push: the release commit, after a look at what goes out (gh release create targets it)
    function Push-Checked([string]$what, [bool]$auto) {
        if ($Dry) {
            $ahead = @(GitQ log --oneline "$Remote/main..HEAD")
            Would "git fetch $Remote main; git push $Remote HEAD:main   ($what; $($ahead.Count) commits ahead of the last fetched $Remote/main)"
            $ahead | Select-Object -First 15 | ForEach-Object { Write-Host "      $_" }
            return
        }
        & git -C $Root fetch $Remote main 2>&1 | ForEach-Object { Write-Host "    $_" }
        Check "fetch $Remote main" ($LASTEXITCODE -eq 0)
        & git -C $Root merge-base --is-ancestor "$Remote/main" HEAD
        Check "HEAD is ahead of $Remote/main (fast-forward)" ($LASTEXITCODE -eq 0)
        $ahead = @(GitQ log --oneline "$Remote/main..HEAD")
        if ($ahead.Count -eq 0) { Say 'nothing to push'; return }
        Say "$($ahead.Count) commits go out:"
        $ahead | ForEach-Object { Write-Host "      $_" }
        $sha = (GitQ rev-parse HEAD)
        $ok = $auto -or ($ConfirmPush.Length -ge 7 -and $sha.StartsWith($ConfirmPush))
        if (-not $ok) {
            try { $a = Read-Host "Push them to $Remote/main? Type the first 7 characters of $sha" } catch { $a = '' }
            $ok = ($a.Length -ge 7 -and $sha.StartsWith($a))
        }
        Check "push confirmed ($what)" $ok 'not confirmed: check the commits, then -Resume -ConfirmPush <sha>'
        & git -C $Root push $Remote HEAD:main 2>&1 | ForEach-Object { Write-Host "    $_" }
        Check "git push $Remote HEAD:main" ($LASTEXITCODE -eq 0)
    }
    Step 'push' { Push-Checked 'the release commit' $false }

    # ---- 6. upload: a draft, small files first, Setup and ZIP, publish, Light last
    Step 'draft' {
        $rel = $null; if (-not $Dry) { $rel = Get-Release }
        if ($rel) { Say "draft there already ($(@($rel.assets).Count) assets): resumed"; return }
        $cmd = "gh release create $Version -R $Repo --draft --target $($State.commit) --title `"Jak Enhanced Trilogy $Version`" --notes-file `"$Notes`""
        if ($Dry) { Would $cmd; return }
        & gh release create $Version -R $Repo --draft --target $State.commit --title "Jak Enhanced Trilogy $Version" --notes-file $Notes 2>&1 | ForEach-Object { Write-Host "    $_" }
        Check 'draft release created' ($LASTEXITCODE -eq 0)
    }
    Step 'upload' {
        foreach ($f in @($Meta) + $Patches + @($Setup, $Zip)) {
            if (-not $Dry -or (Test-Path $f)) { Check "uploaded $(Split-Path $f -Leaf)" (Up $f) }
            else { Would "gh release upload $Version -R $Repo --clobber `"$f`"   (made by the packages step)" }
        }
    }
    Step 'publish' {
        if ($Dry) { Would "gh release edit $Version -R $Repo --draft=false --latest"; return }
        & gh release edit $Version -R $Repo --draft=false --latest 2>&1 | ForEach-Object { Write-Host "    $_" }
        Check 'published --latest' ($LASTEXITCODE -eq 0)
    }
    Step 'light-upload' {
        if (-not $Dry -or (Test-Path $Light)) { Check "uploaded $(Split-Path $Light -Leaf)" (Up $Light) }
        else { Would "gh release upload $Version -R $Repo --clobber `"$Light`"   (made by the light step)" }
    }
    Step 'verify' {
        if ($Dry) { Would "gh release view $Version -R $Repo --json isDraft,assets   (isDraft false, every asset uploaded, sizes = local)"; return }
        $rel = Get-Release
        Check 'release published' ($rel -and -not $rel.isDraft)
        foreach ($f in @($Meta) + $Patches + @($Setup, $Zip, $Light)) {
            $a = @($rel.assets | Where-Object { $_.name -eq (Split-Path $f -Leaf) })
            Check "asset $(Split-Path $f -Leaf)" ($a.Count -eq 1 -and $a[0].state -eq 'uploaded' -and [int64]$a[0].size -eq (Get-Item $f).Length) $(if ($a) { "$($a[0].state), $($a[0].size) bytes" } else { 'missing' })
        }
    }

    # ---- 7. mods.json: the launcher's list points at the Light asset (only once it is up)
    $url = "https://github.com/$Repo/releases/download/$Version/windows-jak-enhanced-light-$Version.zip"
    $modsPy = @'
import json, sys
src, dst, ver, url, stamp = sys.argv[1:6]
with open(src, encoding="utf-8") as f:
    d = json.load(f)
vs = d["mods"]["jak-enhanced-light"]["versions"]
if vs and vs[0]["version"] == ver:
    print("mods.json: " + ver + " is the first entry already")
else:
    vs.insert(0, {"version": ver, "publishedDate": stamp,
                  "assets": {"windows": url, "linux": None, "macos": None},
                  "supportedGames": ["jak1", "jak2", "jak3"]})
    d["lastUpdated"] = stamp
with open(dst, "w", encoding="utf-8", newline="\n") as f:
    f.write(json.dumps(d, indent=2, ensure_ascii=False) + "\n")
with open(dst, encoding="utf-8") as f:
    first = json.load(f)["mods"]["jak-enhanced-light"]["versions"][0]
assert first["version"] == ver and first["assets"]["windows"] == url, first
print("mods.json: first entry " + first["version"] + " " + first["publishedDate"])
'@
    Step 'mods' {
        $mods = Join-Path $Root 'mods.json'
        $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        $tmp = Join-Path ([IO.Path]::GetTempPath()) "jak-release-mods-$Plain"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $updater = Join-Path $tmp 'mods_update.py'
        Utf8 $updater $modsPy
        if ($Dry) {
            # on a copy: the edit parses and keeps the file's format (only the new entry differs)
            $copy = Join-Path $tmp 'mods.json'
            $null = Native $Py @('-I', $updater, $mods, $copy, $Plain, $url, $stamp)
            $before = @(Get-Content $mods).Count; $after = @(Get-Content $copy).Count
            $diff = @(& git -C $Root diff --no-index --numstat -- $mods $copy 2>$null)
            Check 'mods.json edit (on a copy)' ($LASTEXITCODE -le 1 -and $after -gt $before) "$before -> $after lines; $($diff -join ' ')"
            Would "python mods_update.py mods.json $Plain <url> <now>; curl -I $url (200); git commit -m `"mods.json: $Version`" -- mods.json"
            return
        }
        $code = (& curl.exe -s -o NUL -I -L -w '%{http_code}' $url)
        Check 'Light asset answers 200' ($code -eq '200') "HTTP $code $url"
        $null = Native $Py @('-I', $updater, $mods, $mods, $Plain, $url, $stamp)
        Check 'mods.json updated' ($LASTEXITCODE -eq 0)
        $changed = @(GitQ status --porcelain '--' mods.json)
        if ($changed.Count) {
            & git -C $Root commit -m "mods.json: $Version" -m 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>' -- mods.json 2>&1 | ForEach-Object { Write-Host "    $_" }
            Check 'mods.json committed' ($LASTEXITCODE -eq 0)
        }
    }
    Step 'mods-push' {
        # by itself only when it is the one commit going out (nothing else gets published)
        $auto = $false
        if (-not $Dry) {
            & git -C $Root fetch $Remote main 2>$null
            $ahead = @(GitQ log --format=%H "$Remote/main..HEAD")
            $files = @(GitQ diff --name-only "$Remote/main" HEAD)
            $auto = ($ahead.Count -eq 1 -and $files.Count -eq 1 -and $files[0] -eq 'mods.json')
        }
        Push-Checked 'the mods.json commit' $auto
    }
}
catch {
    $script:Failed = $true
    $msg = $_.Exception.Message
    if ($msg.StartsWith($StopMark)) {
        Say "STOPPED: $($msg.Substring($StopMark.Length))"
        if (-not $Dry) { Say "fix it, then run again with -Resume (state: $StateFile)" }
    } else { Say "ERROR: $msg ($($_.InvocationInfo.PositionMessage))" }
}
finally {
    Write-Host ''
    Write-Host "==== $Version $(if ($Dry) { 'dry run' } else { 'release' }) summary"
    foreach ($s in $Steps) {
        $r = 'not reached'
        if ($script:Results.Contains($s)) { $r = $script:Results[$s] }
        Write-Host ("  {0,-13} {1}" -f $s, $r)
    }
    $fails = @($script:Checks | Where-Object { $_ -like 'FAIL*' })
    Write-Host "  checks: $($script:Checks.Count - $fails.Count) OK, $($fails.Count) FAIL"
    $fails | ForEach-Object { Write-Host "    $_" }
    $global:LASTEXITCODE = $(if ($script:Failed) { 1 } else { 0 })
}
