# Does mesh identity churn while the player stands still?
#
# This is the deciding test for reading B of the vertex-explosion plan. If identity churns, the
# Geometry Hash debug view repaints each object a different colour every frame and Remix's
# temporal denoiser can never accumulate. If it holds, the colours are stable frame to frame.
#
# Why a dedicated script rather than rotwalk.ps1: rotwalk moves the camera, so any diff between
# its captures conflates churn with a changed view. And MEASURED -- rotwalk's FIRST capture comes
# back essentially black (lit_px 11,100 vs 2,343,600 on the second), because the render surface
# has not painted by the time it fires. Comparing capture 1 to capture 2 measures "unrendered vs
# rendered" and reads a meaningless 138/255. So: settle first, then N stills, no input at all,
# and compare only adjacent pairs that are both actually lit.

param(
    [string]$Bin = "C:\Users\Tristan\Documents\GitHub\pcsx2\bin",
    [string]$Tag = "gs",
    [string]$Iso = "Tom Clancy's Rainbow Six 3 (USA).iso",
    [int]$Slot = 9,
    [int]$Stable = 0,
    [int]$DebugView = 277,
    [int]$Warmup = 20,
    [int]$Shots = 4,
    [int]$GapMs = 2000,
    [string]$TitleMatch = "Rainbow Six 3"
)

$scratch = "C:\Users\Tristan\AppData\Local\Temp\claude\C--Users-Tristan-Documents-GitHub\867daea0-c913-4e87-8427-856883dcdb0c\scratchpad"
$isoPath = Join-Path "E:\PS2 Games" $Iso

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Fg {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
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
if (-not ("Fg" -as [type])) { Add-Type -TypeDefinition $sig }

cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
cmd /c "taskkill /F /T /IM NvRemixBridge.exe" 2>&1 | Out-Null
Start-Sleep -Seconds 6

if (Test-Path "$Bin\logs\emulog.txt") { Set-Content "$Bin\logs\emulog.txt" "" -NoNewline -EA SilentlyContinue }
$t = Get-Content "$Bin\inis\PCSX2.ini" -Raw
Set-Content "$Bin\inis\PCSX2.ini" ($t -replace '(?m)^Renderer = \d+(?=\r?$)', 'Renderer = 16') -NoNewline

foreach ($n in @('REUSEHANDLE','REUSEPOOL','INSTBUDGET','NODRAWINSTANCE','SUBMITDELAY','NODEBUGSCENE',
                 'MESHCAP','MESHBUDGET','MESHQUANT','DRAWDUMP','UCODEDUMP','DUMP','NOCAM','IDTOL',
                 'EXPLODEK','BATCH','FSTZ','FSTFLAT','MINW','MAXW')) {
    Set-Item -Path "Env:PCSX2_REMIX_$n" -Value "" -EA SilentlyContinue
}
$env:PCSX2_REMIX_STABLEID = "$Stable"
$env:PCSX2_REMIX_STATSFRAMES = "120"
$env:PCSX2_REMIX_TEXDUMP = "0"
if ($DebugView -gt 0) { $env:DXVK_RTX_DEBUG_VIEW_INDEX = "$DebugView" }
else { Remove-Item Env:DXVK_RTX_DEBUG_VIEW_INDEX -EA SilentlyContinue }

$p = Start-Process -FilePath "$Bin\pcsx2-qtx64.exe" `
    -ArgumentList '-batch', '-fastboot', '-state', "$Slot", $isoPath `
    -WorkingDirectory $Bin -PassThru

Start-Sleep -Seconds $Warmup
if ($p.HasExited) {
    Write-Output ("DIED code=0x{0:X8}" -f ([uint32]($p.ExitCode -band 0xFFFFFFFFL)))
    exit 1
}

$hwnd = (Get-Process -Id $p.Id).MainWindowHandle
[void][Fg]::ShowWindow($hwnd, 9)
Write-Output ("focused=" + [Fg]::Force($hwnd))

# No input is sent at any point. The camera does not move.
for ($i = 1; $i -le $Shots; $i++) {
    & "$scratch\capture.ps1" -OutPath "$scratch\${Tag}_$i.png" -TitleMatch $TitleMatch | Select-Object -Last 1
    if ($i -lt $Shots) { Start-Sleep -Milliseconds $GapMs }
}

if ($p.HasExited) { Write-Output ("DIED DURING code=0x{0:X8}" -f ([uint32]($p.ExitCode -band 0xFFFFFFFFL))) }
cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null

Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'Remix: frame ' -EA SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 1
"--- verdict ---"
Select-String -Path "$Bin\logs\emulog.txt" -Pattern 'explode >' -EA SilentlyContinue |
    ForEach-Object { ($_.Line -split '\|' | Where-Object { $_ -match 'explode' }).Trim() } |
    Select-Object -Last 1
