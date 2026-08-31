# A/B runner for the Rainbow Six 3 camera clip-split experiment.
#
# Swaps ONE hash-verified file (pcsx2-qtx64.exe, +pdb) into the user's deployed install and boots
# save state 9 under a fixed wall-time cap, alternating arm A / arm B every round. Everything else
# about the configuration stays the user's own.
#
# Knobs are passed through the ENVIRONMENT, never by editing the install's inis. The backend records
# which PCSX2_REMIX_* vars were already set before it writes anything and then never overwrites
# them (RemixPaths.cpp:84-116 "These are never written, ever"; the per-game .conf loses the same
# way, RemixMaterials.h:100-110). That is the documented A/B control surface. This deliberately
# diverges from arm.ps1:13-15, which rewrites the ini Renderer line; here Renderer = 16 is
# asserted instead, so the script never mutates the configuration it is measuring.
#
# Traps encoded here, all from tools\remix-harness\README.md:
#   - exit codes are signed (:78-79), so they are formatted through -band 0xFFFFFFFF, NOT the
#     [uint32] cast arm.ps1:36 still carries, which throws on 0xFEFEFEFE and hides the death;
#   - a build can silently fail to link and leave the OLD binary in place (:83-85), so the copied
#     exe is re-hashed in the install directory every single launch;
#   - Get-Process is not liveness (:75-77), so DATA/NO-DATA is decided by the parser from log
#     content, never from here.

param(
    [int]$Rounds     = 12,
    # First round number to run. The floor rule in the plan extends a short run by 4 rounds at a
    # time up to 20; starting at 13 appends rounds 13-16 to an existing results directory instead
    # of overwriting launch-01..12, since the log filename is keyed on the round number.
    [int]$StartRound = 1,
    [int]$Live       = 60,
    # The deployed install this swaps builds into. No default is baked in -- set
    # PCSX2_TEST_INSTALL once, or pass -Install.
    [string]$Install = $env:PCSX2_TEST_INSTALL,
    # Working area for the run. Anywhere writable will do; $Results is created, $Stash is an INPUT
    # -- populate $Stash\armA and $Stash\armB with the two builds' pcsx2-qtx64.exe (+pdb) first.
    [string]$Scratch = (Join-Path $env:TEMP "remix-harness"),
    [string]$Stash   = (Join-Path $Scratch "stash"),
    [string]$Results = (Join-Path $Scratch "results"),
    [string]$Fingerprint = (Join-Path $Results "fingerprint.txt"),
    [string]$IsoName = "Tom Clancy's Rainbow Six 3 (USA).iso",
    # Folder holding the test images. Defaults to $env:PCSX2_TEST_ISO_DIR; checked below.
    [string]$IsoDir  = $env:PCSX2_TEST_ISO_DIR,
    [int]$Slot       = 9,
    # An explicit state FILE, not a slot index.
    #
    # -state N resolves through [Folders] Savestates, which in this install points at
    # "..\pcsx2-v1.7.5641-windows-x64-Qt\sstates" -- a different directory that holds slots 01-06
    # and no slot 09. -state 9 therefore hits the README's missing-state fake death (:80-82):
    # "Savestate file does not exist", a clean exit 0 at ~1.0 s, measured here on the smoke round.
    # -statefile takes the path verbatim (QtHost.cpp:2232 -> VMManager.cpp:1383-1384), so it loads
    # the exact file step 1 fingerprinted and touches none of the user's directories.
    [string]$StatePath = "",
    [int]$Settle     = 6
)

$ErrorActionPreference = "Stop"

# Neither path is baked in -- set PCSX2_TEST_ISO_DIR and PCSX2_TEST_INSTALL once, or pass
# -IsoDir and -Install, so a fresh checkout needs no editing and no machine's layout ends up
# in the repo. Checked here rather than at boot because PCSX2 accepts a bad path and only
# reports "Requested filename does not exist" seconds later, which reads as a crashed run
# rather than a typo. $StatePath is derived here too, not in the param block: its default
# calls Join-Path $Install, which throws during parameter binding when $Install is empty --
# before any of these checks could run.
if (-not $IsoDir) {
    throw 'Set PCSX2_TEST_ISO_DIR to the folder holding your PS2 test images, or pass -IsoDir. Example: $env:PCSX2_TEST_ISO_DIR = "D:\PS2 Games"'
}
if (-not $Install) {
    throw 'Set PCSX2_TEST_INSTALL to your deployed PCSX2 install, or pass -Install. Example: $env:PCSX2_TEST_INSTALL = "D:\Emulators\PCSX2 RTX Remix"'
}
$Iso = Join-Path $IsoDir $IsoName
if (-not $StatePath) {
    $StatePath = Join-Path $Install ("sstates\SLUS-20883 (21CC1EC3).{0:D2}.p2s" -f $Slot)
}

function Kill-Emu {
    cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe >nul 2>&1"
    cmd /c "taskkill /F /T /IM NvRemixBridge.exe >nul 2>&1"
    $global:LASTEXITCODE = 0

    # PCSX2's sentry-native crash reporter. Every death leaves one of these behind, and while it
    # writes its dump it holds a handle to pcsx2-qtx64.exe -- which is the file the next arm has to
    # overwrite, so the swap fails with "being used by another process" (measured on the smoke
    # round: two 6 s retries were not enough). Reaped only when BOTH conditions hold: the parent is
    # gone (a true orphan) and it is not the unrelated Corsair iCUE handler, which runs
    # continuously under its own parent and must be left alone.
    try {
        $live = @{}
        Get-Process -ErrorAction SilentlyContinue | ForEach-Object { $live[$_.Id] = $true }
        Get-CimInstance Win32_Process -Filter "Name='crashpad_handler.exe'" -ErrorAction SilentlyContinue |
            Where-Object { -not $live.ContainsKey([int]$_.ParentProcessId) } |
            Where-Object { -not ($_.CommandLine -and $_.CommandLine -match 'Corsair') } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    } catch { }
}

# Blocks until the destination exe can actually be opened for writing.
#
# Get-Process is not a liveness test (README :75-77) and neither is a fixed sleep: what matters is
# whether the file is free, so that is what is tested. Returns $false on timeout.
function Wait-Writable($path, $timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $fs = [IO.File]::Open($path, 'Open', 'Write', 'None')
            $fs.Close()
            return $true
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    return $false
}

# The config stack, as recorded by step 1. Entries are "SHA256  SIZE  PATH".
#
# Files above ~100 MB are fingerprinted once at step 1 but NOT re-hashed per launch: the only such
# entry is the 2.6 GB ISO, which is read-only media that nothing in this pipeline writes, and
# hashing it 24+ times would cost more wall time than the measurement itself.
#
# inis\PCSX2.ini is likewise excluded from the BYTE comparison, because PCSX2 owns that file and
# rewrites it on every shutdown -- measured on the smoke round, where one launch moved its hash
# from 3D34CA3B... to 51CE5C18... with every setting this experiment depends on unchanged. Byte
# equality there would abort the run on the emulator's own bookkeeping. What actually matters about
# it is asserted semantically instead, by Assert-IniInvariants below.
function Read-Fingerprint($path) {
    if (-not (Test-Path -LiteralPath $path)) { throw "fingerprint not found: $path" }
    $rows = @()
    foreach ($line in (Get-Content -LiteralPath $path)) {
        if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
        $m = [regex]::Match($line, '^(?<h>[0-9A-Fa-f]{64})\s+(?<s>\d+)\s+(?<p>.+)$')
        if (-not $m.Success) { continue }
        if ([long]$m.Groups['s'].Value -gt 100000000) { continue }
        if ($m.Groups['p'].Value -like '*inis\PCSX2.ini') { continue }
        $rows += [pscustomobject]@{ Hash = $m.Groups['h'].Value; Path = $m.Groups['p'].Value }
    }
    if ($rows.Count -eq 0) { throw "fingerprint has no usable rows: $path" }
    return $rows
}

# The two settings in PCSX2.ini that would silently invalidate the measurement if the emulator or
# the GUI moved them: the wrong renderer measures a different backend entirely, and no file logging
# means no counters at all. The [Remix] knobs in that file are deliberately NOT asserted -- the env
# pins outrank them by design (RemixPaths.cpp:115-116), which is the whole point of using env vars.
function Assert-IniInvariants($path) {
    $t = Get-Content -LiteralPath $path -Raw
    if ($t -notmatch '(?m)^Renderer = 16\s*$')            { throw "CONFIG DRIFT: Renderer is no longer 16 in $path" }
    if ($t -notmatch '(?m)^EnableFileLogging = true\s*$') { throw "CONFIG DRIFT: EnableFileLogging is no longer true in $path" }
}

function Assert-NoConfigDrift($rows) {
    foreach ($r in $rows) {
        if (-not (Test-Path -LiteralPath $r.Path)) { throw "CONFIG DRIFT: file vanished -- $($r.Path)" }
        $now = (Get-FileHash -LiteralPath $r.Path -Algorithm SHA256).Hash
        if ($now -ne $r.Hash) {
            throw "CONFIG DRIFT: $($r.Path)`n  expected $($r.Hash)`n  found    $now"
        }
    }
}

# ---------------------------------------------------------------- pre-flight

if (-not (Test-Path -LiteralPath $Iso))     { throw "iso not found: $Iso" }
if (-not (Test-Path -LiteralPath $Install)) { throw "install not found: $Install" }

if (-not (Test-Path -LiteralPath $StatePath)) { throw "save state not found: $StatePath" }

$iniPath = Join-Path $Install "inis\PCSX2.ini"
Assert-IniInvariants $iniPath

$fpRows = Read-Fingerprint $Fingerprint
Assert-NoConfigDrift $fpRows

$armSha = @{}
foreach ($arm in @('A','B')) {
    $src = Join-Path $Stash "arm$arm\pcsx2-qtx64.exe"
    if (-not (Test-Path -LiteralPath $src)) { throw "stash missing: $src" }
    $armSha[$arm] = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
}
if ($armSha['A'] -eq $armSha['B']) { throw "armA and armB exes are identical -- the patch did not compile in" }

foreach ($arm in @('A','B')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Results "arm$arm") | Out-Null
}

$runlog = Join-Path $Results "runlog.csv"
if (-not (Test-Path -LiteralPath $runlog)) {
    "round,arm,exe_sha,start,end,wall_s,exit,outcome" | Set-Content -LiteralPath $runlog
}

$emulog  = Join-Path $Install "logs\emulog.txt"
$dxvklog = Join-Path $Install "RemixGames\SLUS-20883\logs\remix-dxvk.log"

Write-Output "abcam: $Rounds rounds x 2 arms, ${Live}s cap"
Write-Output "  state $StatePath"
Write-Output "  armA $($armSha['A'])"
Write-Output "  armB $($armSha['B'])"
Write-Output "  results -> $Results"

# ------------------------------------------------------------------ env pins
#
# Cleared first so a stale shell cannot leak a knob into one arm and not the other; the whole
# PCSX2_REMIX_* namespace is enumerated rather than the fixed list arm.ps1:17-22 carries, because
# a list goes stale every time a knob is added.
function Set-EnvPins {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'PCSX2_REMIX_*' } |
        ForEach-Object { Remove-Item -LiteralPath "Env:$($_.Name)" -ErrorAction SilentlyContinue }
    foreach ($n in @('DXVK_RTX_DEBUG_VIEW_INDEX','DXVK_LOG_PATH','DXVK_CAPTURE_PATH','DEFAULT_MODS_DIR')) {
        Remove-Item -LiteralPath "Env:$n" -ErrorAction SilentlyContinue
    }

    # Identical in both arms by construction. LIGHTMODE=1 needs NODEBUGSCENE=0 to do anything at
    # all -- place_fill_lights early-outs on no_debug_scene() (RemixSubmit.cpp:1421) and the user's
    # global ini holds NODEBUGSCENE = true, so LIGHTMODE alone creates no lights.
    # SKY=2 + SKYORDER=4 is the user's 2026-08-08 known-good sky rule; it selects the first 4 draws
    # of a frame (classify_sky, RemixSubmit.cpp:2794-2799), which is camera-independent by
    # construction and therefore cannot interact with the patch under test.
    # STATSFRAMES=60 makes the counter block flush about once a second, so a 0x60D0DEAD device loss
    # costs at most ~1 s of data (RemixSubmit.cpp:3362-3369).
    $env:PCSX2_REMIX_LIGHTMODE    = "1"
    $env:PCSX2_REMIX_NODEBUGSCENE = "0"
    $env:PCSX2_REMIX_SKY          = "2"
    $env:PCSX2_REMIX_SKYORDER     = "4"
    $env:PCSX2_REMIX_NOCAM        = "0"
    $env:PCSX2_REMIX_STATSFRAMES  = "60"
    $env:PCSX2_REMIX_TEXDUMP      = "0"
}

# ------------------------------------------------------------------ the loop

for ($r = $StartRound; $r -le $Rounds; $r++) {
    foreach ($arm in @('A','B')) {
        $tag = "{0:D2}" -f $r
        Set-EnvPins
        Assert-NoConfigDrift $fpRows
        Assert-IniInvariants $iniPath

        Kill-Emu
        Start-Sleep -Seconds $Settle

        # Swap the binary, then prove the binary that is about to run is the one asked for.
        $srcExe = Join-Path $Stash "arm$arm\pcsx2-qtx64.exe"
        $srcPdb = Join-Path $Stash "arm$arm\pcsx2-qtx64.pdb"
        $dstExe = Join-Path $Install "pcsx2-qtx64.exe"
        $dstPdb = Join-Path $Install "pcsx2-qtx64.pdb"

        $copied = $false
        for ($attempt = 1; $attempt -le 4; $attempt++) {
            if (-not (Wait-Writable $dstExe 90)) {
                Write-Output "  round $tag arm $arm : exe still locked after 90s, killing and retrying"
                Kill-Emu; Start-Sleep -Seconds $Settle; continue
            }
            try {
                Copy-Item -LiteralPath $srcExe -Destination $dstExe -Force
                if (Test-Path -LiteralPath $srcPdb) { Copy-Item -LiteralPath $srcPdb -Destination $dstPdb -Force }
            } catch {
                Write-Output "  round $tag arm $arm : copy failed ($($_.Exception.Message)), retrying"
                Kill-Emu; Start-Sleep -Seconds $Settle; continue
            }
            $now = (Get-FileHash -LiteralPath $dstExe -Algorithm SHA256).Hash
            if ($now -eq $armSha[$arm]) { $copied = $true; break }
            Write-Output "  round $tag arm $arm : WRONG BINARY in place ($now), retrying"
            Kill-Emu; Start-Sleep -Seconds $Settle
        }
        if (-not $copied) { throw "WRONG BINARY: could not place arm $arm exe after 4 attempts -- aborting run" }

        Remove-Item -LiteralPath $emulog -Force -ErrorAction SilentlyContinue

        $start = Get-Date
        # ONE pre-quoted command line, not an array.
        #
        # Start-Process's array form does not reliably quote elements containing spaces: with both
        # the state path and the iso path carrying spaces, PCSX2 received
        # "-statefile <install-dir-up-to-the-first-space>" and glued the remainder and the
        # iso path together into a single
        # bogus boot filename ("Requested filename '...' does not exist", measured on the smoke
        # round). A single string is passed to CreateProcess verbatim, so the quotes here are the
        # ones the child actually sees.
        $argLine = '-batch -fastboot -statefile "{0}" "{1}"' -f $StatePath, $Iso
        $p = Start-Process -FilePath $dstExe `
            -ArgumentList $argLine `
            -WorkingDirectory $Install -PassThru

        $exitText = "n/a"
        if ($p.WaitForExit($Live * 1000)) {
            $outcome = "died"
            # Signed-safe. [uint32] throws on 0xFEFEFEFE (README :78-79) and swallows the death.
            $exitText = ('0x{0:X8}' -f ($p.ExitCode -band 0xFFFFFFFF))
        }
        else {
            $outcome = "survived-to-cap"
            Kill-Emu
            [void]$p.WaitForExit(10000)
            try { $exitText = ('0x{0:X8}' -f ($p.ExitCode -band 0xFFFFFFFF)) } catch { $exitText = "killed" }
        }
        $end = Get-Date
        $wall = [math]::Round(($end - $start).TotalSeconds, 1)

        Kill-Emu
        Start-Sleep -Milliseconds 1500

        $dstLog = Join-Path $Results "arm$arm\launch-$tag.log"
        if (Test-Path -LiteralPath $emulog) {
            Copy-Item -LiteralPath $emulog -Destination $dstLog -Force
        } else {
            $outcome = "$outcome/NOLOG"
        }
        if (Test-Path -LiteralPath $dxvklog) {
            Copy-Item -LiteralPath $dxvklog -Destination (Join-Path $Results "arm$arm\launch-$tag.dxvk.log") -Force
        }

        ('{0},{1},{2},{3},{4},{5},{6},{7}' -f $r, $arm, $armSha[$arm],
            $start.ToString('o'), $end.ToString('o'), $wall, $exitText, $outcome) |
            Add-Content -LiteralPath $runlog

        Write-Output ("  round {0} arm {1} : {2,-16} exit {3} wall {4}s" -f $tag, $arm, $outcome, $exitText, $wall)
    }
}

Kill-Emu
Write-Output "abcam: done -- $runlog"
