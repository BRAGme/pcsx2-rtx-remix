param(
    [int]$Launches = 20,
    [int]$Slot = 2,
    [string]$Iso = "SOCOM Combined Assault.iso",
    [int]$Live = 25,
    [string]$Name = "arm",
    # The repo build, resolved from this script's location so a fresh checkout needs no editing.
    [string]$Bin = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\bin")),
    [hashtable]$Env = @{}
)

$isoPath = Join-Path "E:\PS2 Games" $Iso

$t = Get-Content "$Bin\inis\PCSX2.ini" -Raw
$t = $t -replace '(?m)^Renderer = \d+(?=\r?$)', 'Renderer = 16'
Set-Content "$Bin\inis\PCSX2.ini" $t -NoNewline

foreach ($n in @('STABLEID','REUSEHANDLE','REUSEPOOL','INSTBUDGET','NODRAWINSTANCE','SUBMITDELAY',
                 'NODEBUGSCENE','MESHCAP','MESHBUDGET','MESHQUANT','DRAWDUMP','UCODEDUMP','DUMP',
                 'NOCAM','IDTOL','SUN','LIGHTRADIANCE','LIGHTRADIUS','STARTUPDELAY','EXPLODEK',
                 'BATCH','FSTZ','FSTFLAT','MINW','MAXW')) {
    Set-Item -Path "Env:PCSX2_REMIX_$n" -Value "" -EA SilentlyContinue
}
$env:PCSX2_REMIX_STATSFRAMES = "100000"
$env:PCSX2_REMIX_TEXDUMP = "0"
foreach ($k in $Env.Keys) { Set-Item -Path "Env:PCSX2_REMIX_$k" -Value ([string]$Env[$k]) }

$ok = 0
for ($i = 1; $i -le $Launches; $i++) {
    cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
    cmd /c "taskkill /F /T /IM NvRemixBridge.exe" 2>&1 | Out-Null
    Start-Sleep -Seconds 6
    $p = Start-Process -FilePath "$Bin\pcsx2-qtx64.exe" `
        -ArgumentList '-batch', '-fastboot', '-state', "$Slot", $isoPath `
        -WorkingDirectory $Bin -PassThru
    if ($p.WaitForExit($Live * 1000)) {
        Write-Output ("  {0}: DIED 0x{1:X8}" -f $i, [uint32]$p.ExitCode)
    }
    else {
        $ok++
        Write-Output ("  {0}: alive" -f $i)
        cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
    }
}
cmd /c "taskkill /F /T /IM pcsx2-qtx64.exe" 2>&1 | Out-Null
Write-Output ("ARM {0} -> survived {1}/{2}" -f $Name, $ok, $Launches)
