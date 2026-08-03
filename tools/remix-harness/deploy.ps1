# Boot into SOCOM's deploy MENU (slot 3), survive the fragile startup window there, then press
# Cross to load into the mission from inside an already-stable session.
#
# Why: the 0x60D0DEAD hang lands within a fraction of a second of "renderer is live", and surviving
# that window buys minutes. Slot 3 is a light scene (~3 draws/frame) and boots reliably; slot 1/2
# load the full mission straight into the fragile window and die in ~5 s, which is why every
# in-mission capture attempt has failed and why STABLEID could not be measured there.
#
# The menu is INVISIBLE in Remix mode -- D3D11 is surfaceless and sprites are never submitted -- so
# this navigates blind and watches the counters for mission entry instead of the screen. Mission
# entry is detected by scene extent: the menu sits at scene r ~4 / maxpos ~6, a mission at
# ~11,785 / ~11,794.

param(
    [string]$Bin = "C:\Users\Tristan\Documents\GitHub\pcsx2\bin",
    [string]$Tag = "dep",
    [int]$Stable = 0,
    [int]$DebugView = 0,
    [int]$Warmup = 18,        # long enough that startup is definitively survived
    # MEASURED on D3D11, where the screen is actually visible: slot 3 is the KINGFISHER mission
    # BRIEFING (objectives list, gold three-pointed emblem at the bottom -- the thing that renders
    # as a giant blown-out triangle in Remix mode). Exactly ONE Cross press loads the mission.
    #
    # The old default of 8-10 presses at a 2.2 s gap is why entry was unreliable: the mission takes
    # longer than one gap to load, so presses kept landing during the load and then in-game --
    # pausing, firing, opening menus. Press once, then WAIT for entry rather than pressing again.
    [int]$Presses = 3,
    [int]$PressGapMs = 8000,
    # Seconds to wait for the mission to load after a press before giving up and pressing again.
    [int]$EntryWaitSec = 100,
    [int]$SettleSec = 25,     # after entry, let the mission run and accumulate
    [hashtable]$Extra = @{}
)

$scratch = "C:\Users\Tristan\AppData\Local\Temp\claude\C--Users-Tristan-Documents-GitHub\867daea0-c913-4e87-8427-856883dcdb0c\scratchpad"
$isoPath = "E:\PS2 Games\SOCOM Combined Assault.iso"

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Dep {
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
    public static bool Force(IntPtr h) {
        uint target = GetWindowThreadProcessId(h, IntPtr.Zero);
        uint self = GetCurrentThreadId();
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        AttachThreadInput(self, target, true); AttachThreadInput(fg, target, true);
        BringWindowToTop(h); SetForegroundWindow(h); SetActiveWindow(h); SetFocus(h);
        AttachThreadInput(fg, target, false); AttachThreadInput(self, target, false);
        return GetForegroundWindow() == h;
    }
}
'@
if (-not ("Dep" -as [type])) { Add-Type -TypeDefinition $sig }

function Tap([int]$vk, [int]$ms = 120) {
    if ($script:Hwnd) { [void][Dep]::Force($script:Hwnd) }
    $scan = [Dep]::MapVirtualKey([uint32]$vk, 0)
    [Dep]::keybd_event([byte]$vk, [byte]$scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $ms
    [Dep]::keybd_event([byte]$vk, [byte]$scan, 2, [UIntPtr]::Zero)
}

# Liveness that cannot be fooled by a zombie: threads > 0 AND an advancing Remix frame counter.
function LastFrame {
    $m = Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'Remix: frame (\d+)' -EA SilentlyContinue |
         Select-Object -Last 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return -1
}
function SceneExtent {
    $m = Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'maxpos (\d+)/' -EA SilentlyContinue |
         Select-Object -Last 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return -1
}
function Alive([System.Diagnostics.Process]$p) {
    if ($p.HasExited) { return $false }
    try { $p.Refresh(); return ($p.Threads.Count -gt 0) } catch { return $false }
}

cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
cmd /c "taskkill /F /T /IM NvRemixBridge.exe" 2>&1 | Out-Null
Start-Sleep -Seconds 6
if (Test-Path "$Bin\logs\emulog.txt") { Set-Content "$Bin\logs\emulog.txt" "" -NoNewline }

Copy-Item "$Bin\inis\PCSX2.ini" "$Bin\inis\PCSX2.ini.depbak" -Force
$t = Get-Content "$Bin\inis\PCSX2.ini" -Raw
$t = $t -replace '(?m)^Renderer = \d+(?=\r?$)', 'Renderer = 16'
# Face buttons and menu navigation on the keyboard. Cross is what advances the deploy screens.
$t = $t -replace '(?m)^Cross = .*$',    'Cross = Keyboard/Return'
$t = $t -replace '(?m)^Circle = .*$',   'Circle = Keyboard/Backspace'
$t = $t -replace '(?m)^Start = .*$',    'Start = Keyboard/Space'
$t = $t -replace '(?m)^Up = .*$',       'Up = Keyboard/Up'
$t = $t -replace '(?m)^Down = .*$',     'Down = Keyboard/Down'
$t = $t -replace '(?m)^Left = .*$',     'Left = Keyboard/Left'
$t = $t -replace '(?m)^Right = .*$',    'Right = Keyboard/Right'
$t = $t -replace '(?m)^LUp = .*$',      'LUp = Keyboard/W'
$t = $t -replace '(?m)^LDown = .*$',    'LDown = Keyboard/S'
$t = $t -replace '(?m)^LLeft = .*$',    'LLeft = Keyboard/A'
$t = $t -replace '(?m)^LRight = .*$',   'LRight = Keyboard/D'
Set-Content "$Bin\inis\PCSX2.ini" $t -NoNewline

foreach ($n in @('REUSEHANDLE','REUSEPOOL','INSTBUDGET','NODRAWINSTANCE','SUBMITDELAY','NODEBUGSCENE',
                 'MESHCAP','MESHBUDGET','MESHQUANT','DRAWDUMP','UCODEDUMP','DUMP','NOCAM','IDTOL',
                 'EXPLODEK','BATCH','FSTZ','FSTFLAT','MINW','MAXW','UNTEXZ')) {
    Set-Item -Path "Env:PCSX2_REMIX_$n" -Value "" -EA SilentlyContinue
}
$env:PCSX2_REMIX_STABLEID = "$Stable"
$env:PCSX2_REMIX_STATSFRAMES = "60"
$env:PCSX2_REMIX_TEXDUMP = "0"
foreach ($k in $Extra.Keys) { Set-Item -Path "Env:PCSX2_REMIX_$k" -Value ([string]$Extra[$k]) }
if ($DebugView -gt 0) { $env:DXVK_RTX_DEBUG_VIEW_INDEX = "$DebugView" }
else { Remove-Item Env:DXVK_RTX_DEBUG_VIEW_INDEX -EA SilentlyContinue }

# Slot 3 = the deploy menu: light scene, survives startup where the mission states do not.
$p = Start-Process -FilePath "$Bin\pcsx2-qtx64.exe" `
    -ArgumentList '-batch', '-fastboot', '-state', '3', $isoPath `
    -WorkingDirectory $Bin -PassThru

Start-Sleep -Seconds $Warmup
if (-not (Alive $p)) {
    Copy-Item "$Bin\inis\PCSX2.ini.depbak" "$Bin\inis\PCSX2.ini" -Force
    Remove-Item "$Bin\inis\PCSX2.ini.depbak" -Force
    Write-Output ("DIED IN MENU code=0x{0:X8}" -f ([uint32]($p.ExitCode -band 0xFFFFFFFFL)))
    exit 1
}

$script:Hwnd = (Get-Process -Id $p.Id).MainWindowHandle
[void][Dep]::ShowWindow($script:Hwnd, 9)
Write-Output ("menu survived {0}s, focused={1}, frame={2}, maxpos={3}" -f $Warmup, [Dep]::Force($script:Hwnd), (LastFrame), (SceneExtent))

# Blind Cross presses. The menu cannot be seen, so watch scene extent for mission entry.
$entered = $false
# The variable is ELAPSED TIME, not press count, and it took three arms to establish that:
#
#   presses every 2.2 s, 8-10 of them (~25 s total)   entered ~1 in 4
#   one press then a 24 s poll, repeated (~75 s)      entered 3 of 5   <-- press 3 always took
#   4 presses front-loaded 2.5 s apart (~32 s)        entered 0 of 6
#
# Front-loading fails and long-spacing works, so the briefing simply does not accept input until
# roughly 70 s of wall time. On D3D11 one press at 20 s sufficed, because it runs at 59.93 fps
# against 28-40 here -- the briefing's own timeline advances in game frames, so Remix's lower
# framerate stretches it in wall-clock.
#
# So: keep pressing across a long window, checking between presses. Total exposure is ~2 minutes,
# which slot 3 tolerates -- it is the light scene, and it is the mission states that die in ~5 s.
$deadline = $EntryWaitSec
$elapsed = 0
$press = 0
while ($elapsed -lt $deadline) {
    if (-not (Alive $p)) { Write-Output ("  died during nav at t+{0}s" -f $elapsed); break }

    $ext = SceneExtent
    if ($ext -gt 1000) {
        Write-Output ("  -> MISSION ENTERED at t+{0}s after {1} press(es) (extent {2}, frame {3})" `
            -f $elapsed, $press, $ext, (LastFrame))
        $entered = $true
        break
    }

    Tap 0x0D    # Return -> Cross
    $press++
    Start-Sleep -Milliseconds $PressGapMs
    $elapsed += [int]($PressGapMs / 1000)
}

if (-not $entered -and (Alive $p)) {
    Write-Output ("  no entry within {0}s after {1} presses (maxpos {2})" -f $deadline, $press, (SceneExtent))
}

if ($entered -and (Alive $p)) {
    # Let it accumulate, capturing as it goes -- this is the window that never existed before.
    for ($s = 1; $s -le 3; $s++) {
        Start-Sleep -Seconds ([int]($SettleSec / 3))
        if (-not (Alive $p)) { Write-Output "  died during settle $s"; break }
        & "$scratch\capture.ps1" -OutPath "$scratch\${Tag}_$s.png" -TitleMatch "Combined Assault" | Select-Object -Last 1
    }
}

if (Alive $p) { Write-Output "STILL ALIVE at end" } else { Write-Output "exited" }
cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
Start-Sleep -Seconds 2
Copy-Item "$Bin\inis\PCSX2.ini.depbak" "$Bin\inis\PCSX2.ini" -Force
Remove-Item "$Bin\inis\PCSX2.ini.depbak" -Force

"--- final counters ---"
Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'Remix: frame ' -EA SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 1
Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'Z->w fit' -EA SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 1
