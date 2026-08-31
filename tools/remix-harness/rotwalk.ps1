param(
    # The repo build, resolved from this script's location so a fresh checkout needs no editing.
    [string]$Bin = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\bin")),
    # Where captures land. Anywhere writable will do; it is created if missing.
    [string]$Scratch = (Join-Path $env:TEMP "remix-harness"),
    [int]$Stable = 1,
    [string]$Tag = "rw",
    # Title selection. Defaults are Rainbow Six 3 slot 9 (the warehouse state); SOCOM is
    # -Iso "SOCOM Combined Assault.iso" -Slot 3.
    [string]$Iso = "Tom Clancy's Rainbow Six 3 (USA).iso",
    # Folder holding the test images. Defaults to $env:PCSX2_TEST_ISO_DIR; see the check below.
    [string]$IsoDir = $env:PCSX2_TEST_ISO_DIR,
    [int]$Slot = 9,
    # Arbitrary PCSX2_REMIX_* overrides, applied after the stale-env clear so they win.
    # Same idiom as arm.ps1 -Env. Step 1b wants @{DRAWDUMP=30}.
    [hashtable]$Extra = @{},
    # Remix debug view index; 0 leaves the normal render view. Step 1c wants 277 (Geometry Hash).
    [int]$DebugView = 0,
    # Seconds to wait for boot+state-load before the first capture. 22 suits Rainbow Six 3;
    # SOCOM dies ~20 s in, so it needs this well under that or every capture reads DIED.
    [int]$Warmup = 22,
    # Window title to capture. MUST identify the game window -- '.' matches PCSX2 dialogs too.
    [string]$TitleMatch = 'Rainbow Six 3'
)

New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
# The folder holding your PS2 test images. No default is baked in -- set
# PCSX2_TEST_ISO_DIR once, or pass -IsoDir, so a fresh checkout needs no editing and no
# machine's layout ends up in the repo. Checked here rather than at boot because PCSX2
# accepts a bad path and only reports "Requested filename does not exist" seconds later,
# which reads as a crashed run rather than a typo.
if (-not $IsoDir) {
    throw 'Set PCSX2_TEST_ISO_DIR to the folder holding your PS2 test images, or pass -IsoDir. Example: $env:PCSX2_TEST_ISO_DIR = "D:\PS2 Games"'
}
$isoPath = Join-Path $IsoDir $Iso

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Inp2 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public static bool ForceForeground(IntPtr h) {
        uint target = GetWindowThreadProcessId(h, IntPtr.Zero);
        uint self = GetCurrentThreadId();
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        AttachThreadInput(self, target, true);
        AttachThreadInput(fg, target, true);
        BringWindowToTop(h); SetForegroundWindow(h); SetActiveWindow(h); SetFocus(h);
        AttachThreadInput(fg, target, false);
        AttachThreadInput(self, target, false);
        return GetForegroundWindow() == h;
    }
}
'@
if (-not ("Inp2" -as [type])) { Add-Type -TypeDefinition $sig }

function Hold([int]$vk, [int]$ms) {
    if ($script:GameHwnd) { [void][Inp2]::ForceForeground($script:GameHwnd) }
    $scan = [Inp2]::MapVirtualKey([uint32]$vk, 0)
    [Inp2]::keybd_event([byte]$vk, [byte]$scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $ms
    [Inp2]::keybd_event([byte]$vk, [byte]$scan, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
}

cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
cmd /c "taskkill /F /T /IM NvRemixBridge.exe" 2>&1 | Out-Null
Start-Sleep -Seconds 7

if (Test-Path "$Bin\logs\emulog.txt") { Set-Content "$Bin\logs\emulog.txt" "" -NoNewline -EA SilentlyContinue }

# Temporary keyboard bindings for the sticks; restored by the caller from PCSX2.ini.rwbak.
Copy-Item "$Bin\inis\PCSX2.ini" "$Bin\inis\PCSX2.ini.rwbak" -Force
$t = Get-Content "$Bin\inis\PCSX2.ini" -Raw
$t = $t -replace '(?m)^Renderer = \d+(?=\r?$)', 'Renderer = 16'
$t = $t -replace '(?m)^LUp = .*$', 'LUp = Keyboard/W'
$t = $t -replace '(?m)^LDown = .*$', 'LDown = Keyboard/S'
$t = $t -replace '(?m)^LLeft = .*$', 'LLeft = Keyboard/A'
$t = $t -replace '(?m)^LRight = .*$', 'LRight = Keyboard/D'
$t = $t -replace '(?m)^RUp = .*$', 'RUp = Keyboard/I'
$t = $t -replace '(?m)^RDown = .*$', 'RDown = Keyboard/K'
$t = $t -replace '(?m)^RLeft = .*$', 'RLeft = Keyboard/J'
$t = $t -replace '(?m)^RRight = .*$', 'RRight = Keyboard/L'
Set-Content "$Bin\inis\PCSX2.ini" $t -NoNewline

foreach ($n in @('REUSEHANDLE','REUSEPOOL','INSTBUDGET','NODRAWINSTANCE','SUBMITDELAY','NODEBUGSCENE',
                 'MESHCAP','MESHBUDGET','MESHQUANT','DRAWDUMP','UCODEDUMP','DUMP','NOCAM','IDTOL',
                 'EXPLODEK','BATCH','FSTZ','FSTFLAT','MINW','MAXW')) {
    Set-Item -Path "Env:PCSX2_REMIX_$n" -Value "" -EA SilentlyContinue
}
$env:PCSX2_REMIX_STABLEID = "$Stable"
$env:PCSX2_REMIX_STATSFRAMES = "120"
$env:PCSX2_REMIX_TEXDUMP = "0"
# Applied last so an explicit -Extra beats both the clear list and the defaults above.
foreach ($k in $Extra.Keys) { Set-Item -Path "Env:PCSX2_REMIX_$k" -Value ([string]$Extra[$k]) }

if ($DebugView -gt 0) { $env:DXVK_RTX_DEBUG_VIEW_INDEX = "$DebugView" }
else { Remove-Item Env:DXVK_RTX_DEBUG_VIEW_INDEX -EA SilentlyContinue }

$p = Start-Process -FilePath "$Bin\pcsx2-qtx64.exe" `
    -ArgumentList '-batch', '-fastboot', '-state', "$Slot", $isoPath `
    -WorkingDirectory $Bin -PassThru

Start-Sleep -Seconds $Warmup
if ($p.HasExited) {
    Copy-Item "$Bin\inis\PCSX2.ini.rwbak" "$Bin\inis\PCSX2.ini" -Force
    Remove-Item "$Bin\inis\PCSX2.ini.rwbak" -Force
    Write-Output ("DIED code=0x{0:X8}" -f ([uint32]($p.ExitCode -band 0xFFFFFFFFL)))
    exit 1
}

$hwnd = (Get-Process -Id $p.Id).MainWindowHandle
[void][Inp2]::ShowWindow($hwnd, 9)
$script:GameHwnd = $hwnd
$focused = [Inp2]::ForceForeground($hwnd)
Write-Output "focused=$focused"

& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_1_still.png" -TitleMatch $TitleMatch | Select-Object -Last 1
Start-Sleep -Seconds 2
& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_2_still2.png" -TitleMatch $TitleMatch | Select-Object -Last 1

# Rotate: hold look-left in steps, capturing a quarter turn at a time.
Hold 0x4A 900      # J
& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_3_rot90.png" -TitleMatch $TitleMatch | Select-Object -Last 1
Hold 0x4A 900
& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_4_rot180.png" -TitleMatch $TitleMatch | Select-Object -Last 1
Hold 0x4A 1800
& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_5_rot360.png" -TitleMatch $TitleMatch | Select-Object -Last 1

# Walk forward, then back.
Hold 0x57 1200     # W
& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_6_fwd.png" -TitleMatch $TitleMatch | Select-Object -Last 1
Hold 0x53 1200     # S
& "$PSScriptRoot\capture.ps1" -OutPath "$Scratch\${Tag}_7_back.png" -TitleMatch $TitleMatch | Select-Object -Last 1

if ($p.HasExited) { Write-Output ("DIED DURING code=0x{0:X8}" -f ([uint32]($p.ExitCode -band 0xFFFFFFFFL))) }
cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
Start-Sleep -Seconds 2
Copy-Item "$Bin\inis\PCSX2.ini.rwbak" "$Bin\inis\PCSX2.ini" -Force
Remove-Item "$Bin\inis\PCSX2.ini.rwbak" -Force

Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'Remix: frame ' -EA SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 2

# The A-vs-B verdict, plus the positive-work check (submitted > 0) that any arm needs before a
# clean reading can be trusted -- a run that submitted nothing reads clean for the wrong reason.
"--- verdict ---"
Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'explode >' -EA SilentlyContinue |
    ForEach-Object { ($_.Line -split '\|' | Where-Object { $_ -match 'explode' }).Trim() } |
    Select-Object -Last 2
Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'extent-reject|nonfinite|fst recovered' -EA SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 1
if (Test-Path "$Bin\logs\remix_draws.txt") {
    $ex = @(Select-String -Path "$Bin\logs\remix_draws.txt" -Pattern 'EXPLODE' -EA SilentlyContinue)
    "EXPLODE offender lines: $($ex.Count)"
    $ex | ForEach-Object { $_.Line } | Select-Object -First 5
}
