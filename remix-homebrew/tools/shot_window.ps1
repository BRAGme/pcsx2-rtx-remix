# shot_window.ps1 -Out <png> [-ProcName pcsx2-qtx64]
# Passive capture of ONE process's window contents. Uses PrintWindow(PW_RENDERFULLCONTENT),
# which asks DWM for that window's own composed surface, so other windows on top are never
# included. Falls back to a screen copy ONLY if the target is the foreground window. Sends no
# input and changes no focus.
param([Parameter(Mandatory=$true)][string]$Out, [string]$ProcName = "pcsx2-qtx64")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
}
"@
[void][W]::SetProcessDPIAware()
$p = Get-Process $ProcName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "no window for $ProcName" }
$h = $p.MainWindowHandle
if ([W]::IsIconic($h)) { throw "window is minimized" }
$r = New-Object W+RECT
[void][W]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $hh = $r.B - $r.T
if ($w -le 0 -or $hh -le 0) { throw "bad rect" }

function Test-Dark($bmp) {
  # sample a coarse grid; "dark" = nearly everything below 12/255
  $n = 0; $dark = 0
  for ($y = 0; $y -lt $bmp.Height; $y += [Math]::Max(1, [int]($bmp.Height / 24))) {
    for ($x = 0; $x -lt $bmp.Width; $x += [Math]::Max(1, [int]($bmp.Width / 32))) {
      $c = $bmp.GetPixel($x, $y); $n++
      if (($c.R + $c.G + $c.B) -lt 36) { $dark++ }
    }
  }
  return ($dark -ge ($n * 0.98))
}

$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [W]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$method = "PrintWindow"
if (-not $ok -or (Test-Dark $bmp)) {
  # "In front" means the foreground window belongs to the target PROCESS: clicking into the
  # game focuses the render child, not the main window, so a handle comparison would fail.
  $fgpid = [uint32]0; [void][W]::GetWindowThreadProcessId([W]::GetForegroundWindow(), [ref]$fgpid)
  if ($fgpid -eq [uint32]$p.Id) {
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $method = "CopyFromScreen(foreground)"
  } else {
    $g.Dispose(); $bmp.Dispose()
    throw "PrintWindow gave a blank frame and the target is not the foreground window; refusing a screen copy"
  }
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"saved $Out ${w}x${hh} via $method"
