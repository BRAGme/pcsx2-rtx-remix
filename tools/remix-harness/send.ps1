param(
    [Parameter(Mandatory = $true)][string]$Keys,   # comma list e.g. "RETURN,RETURN,X"
    [int]$HoldMs = 90,
    [int]$GapMs = 700
)

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Inp {
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

    // Force foreground past Windows' foreground lock by attaching input queues.
    public static bool ForceForeground(IntPtr h) {
        uint target = GetWindowThreadProcessId(h, IntPtr.Zero);
        uint self = GetCurrentThreadId();
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        AttachThreadInput(self, target, true);
        AttachThreadInput(fg, target, true);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetActiveWindow(h);
        SetFocus(h);
        AttachThreadInput(fg, target, false);
        AttachThreadInput(self, target, false);
        return GetForegroundWindow() == h;
    }
}
'@
if (-not ("Inp" -as [type])) { Add-Type -TypeDefinition $sig }

$VK = @{
    'RETURN' = 0x0D; 'ESCAPE' = 0x1B; 'UP' = 0x26; 'DOWN' = 0x28; 'LEFT' = 0x25; 'RIGHT' = 0x27
    'X' = 0x58; 'C' = 0x43; 'S' = 0x53; 'A' = 0x41; 'BACK' = 0x08; 'SPACE' = 0x20
    'F1' = 0x70; 'F2' = 0x71; 'F3' = 0x72; 'W' = 0x57; 'D' = 0x44; 'Q' = 0x51; 'E' = 0x45
    'I' = 0x49; 'J' = 0x4A; 'K' = 0x4B; 'L' = 0x4C   # right stick: look up/left/down/right
}

$proc = Get-Process pcsx2-qtx64 -ErrorAction SilentlyContinue |
Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { Write-Output "NO_PROCESS"; exit 1 }

[void][Inp]::ShowWindow($proc.MainWindowHandle, 9)   # SW_RESTORE
$ok = [Inp]::ForceForeground($proc.MainWindowHandle)
Start-Sleep -Milliseconds 400
$fg = [Inp]::GetForegroundWindow()
$focused = ($fg -eq $proc.MainWindowHandle)
Write-Output ("target=0x{0:x} foreground=0x{1:x} focused={2}" -f `
        [int64]$proc.MainWindowHandle, [int64]$fg, $focused)
if (-not $focused) { Write-Output "WARN: window not focused - keystrokes will go elsewhere"; exit 2 }

function Send-Key([byte]$code) {
    $scan = [byte]([Inp]::MapVirtualKey([uint32]$code, 0))
    [Inp]::keybd_event($code, $scan, 0, [UIntPtr]::Zero)          # down
    Start-Sleep -Milliseconds $HoldMs
    [Inp]::keybd_event($code, $scan, 2, [UIntPtr]::Zero)          # KEYEVENTF_KEYUP
}

foreach ($k in ($Keys -split ',')) {
    $name = $k.Trim().ToUpper()
    $alt = $false
    if ($name -like 'ALT+*') { $alt = $true; $name = $name.Substring(4) }
    if (-not $VK.ContainsKey($name)) { Write-Output "  skip unknown key '$name'"; continue }

    if ($alt) { [Inp]::keybd_event(0x12, [byte]([Inp]::MapVirtualKey(0x12, 0)), 0, [UIntPtr]::Zero) }
    Send-Key ([byte]$VK[$name])
    if ($alt) { [Inp]::keybd_event(0x12, [byte]([Inp]::MapVirtualKey(0x12, 0)), 2, [UIntPtr]::Zero) }

    Write-Output ("  sent {0}{1}" -f $(if ($alt) { 'ALT+' }else { '' }), $name)
    Start-Sleep -Milliseconds $GapMs
}
