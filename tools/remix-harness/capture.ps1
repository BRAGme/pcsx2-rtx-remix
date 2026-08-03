param(
    [Parameter(Mandatory = $true)][string]$OutPath,
    [string]$TitleMatch = "Rainbow Six 3"
)

Add-Type -AssemblyName System.Drawing

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Cap {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
if (-not ("Cap" -as [type])) { Add-Type -TypeDefinition $sig }
# Without this, GetClientRect returns logical px on a scaled display and
# PrintWindow renders at physical size -> we'd capture only the top-left corner.
[void][Cap]::SetProcessDPIAware()

# Find the PCSX2 process whose main window matches
$proc = Get-Process pcsx2-qtx64 -ErrorAction SilentlyContinue |
Where-Object { $_.MainWindowTitle -match $TitleMatch } | Select-Object -First 1
if (-not $proc) {
    $proc = Get-Process pcsx2-qtx64 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}
if (-not $proc) { Write-Output "NO_PROCESS"; exit 1 }

# MEASURED 2026-08-03: with -TitleMatch "." this script happily captured the PCSX2 Settings and
# Controller Settings dialogs instead of the game, because any title matches and the window
# scoring below picks the BRIGHTEST window -- a dialog full of white-on-grey text beats a dim
# path-traced scene every time. Seven "motion" captures were settings dialogs. Refuse loudly
# rather than return a plausible-looking wrong image.
if ($proc.MainWindowTitle -match 'Settings|Controller|Achievements|About|Memory Card|Graphics|Audio') {
    Write-Output ("WRONG_WINDOW title='{0}' -- a PCSX2 dialog has the main window; close it and re-run" -f $proc.MainWindowTitle)
    exit 3
}

$hwnd = $proc.MainWindowHandle
Write-Output ("main hwnd=0x{0:x} title='{1}'" -f [int64]$hwnd, $proc.MainWindowTitle)

# Enumerate child windows - the render surface is a child, not the top-level frame
$targets = @([pscustomobject]@{ H = $hwnd; Tag = "main" })
$child = [Cap]::GetWindow($hwnd, 5)   # GW_CHILD
while ($child -ne [IntPtr]::Zero) {
    if ([Cap]::IsWindowVisible($child)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][Cap]::GetClassName($child, $sb, 256)
        $targets += [pscustomobject]@{ H = $child; Tag = $sb.ToString() }
    }
    $child = [Cap]::GetWindow($child, 2)   # GW_HWNDNEXT
}

$best = $null; $bestNonBlack = -1
foreach ($t in $targets) {
    $r = New-Object Cap+RECT
    if (-not [Cap]::GetClientRect($t.H, [ref]$r)) { continue }
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -lt 64 -or $h -lt 64) { continue }

    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [Cap]::PrintWindow($t.H, $hdc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()

    # Sample a grid; count pixels that are not near-black
    $nonBlack = 0; $samples = 0
    for ($y = 0; $y -lt $h; $y += [Math]::Max(1, [int]($h / 40))) {
        for ($x = 0; $x -lt $w; $x += [Math]::Max(1, [int]($w / 40))) {
            $p = $bmp.GetPixel($x, $y); $samples++
            if (($p.R + $p.G + $p.B) -gt 30) { $nonBlack++ }
        }
    }
    $pct = if ($samples) { [math]::Round(100 * $nonBlack / $samples, 1) } else { 0 }
    Write-Output ("  hwnd=0x{0:x} class='{1}' {2}x{3} printwindow={4} nonblack={5}%" -f [int64]$t.H, $t.Tag, $w, $h, $ok, $pct)

    if ($nonBlack -gt $bestNonBlack) {
        $bestNonBlack = $nonBlack
        if ($best) { $best.Dispose() }
        $best = $bmp
    }
    else { $bmp.Dispose() }
}

if ($best) {
    $best.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $best.Dispose()
    Write-Output "SAVED $OutPath"
}
else { Write-Output "NO_CAPTURE" }
