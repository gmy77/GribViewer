[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$messageAssembly = Add-Type -AssemblyName PresentationFramework -PassThru
$downloads = [Environment]::GetFolderPath('UserProfile') + '\Downloads'
$candidate = (Get-Date).ToUniversalTime().AddHours(-5)

for ($attempt = 0; $attempt -lt 5; $attempt++) {
    $cycleHour = [Math]::Floor($candidate.Hour / 6) * 6
    $run = Get-Date -Date $candidate.Date.AddHours($cycleHour) -Format 'yyyyMMdd'
    $cycle = $cycleHour.ToString('00')
    $file = "gfs.t${cycle}z.pgrb2.0p25.f006"
    $query = @{
        file = $file; lev_surface = 'on'; var_CAPE = 'on'; var_UGRD = 'on'; var_VGRD = 'on'
        leftlon = '12'; rightlon = '14'; toplat = '47'; bottomlat = '45'; dir = "/gfs.$run/$cycle/atmos"
    }
    $parameters = ($query.GetEnumerator() | ForEach-Object {
        '{0}={1}' -f $_.Key, [uri]::EscapeDataString($_.Value)
    }) -join '&'
    $url = "https://nomads.ncep.noaa.gov/cgi-bin/filter_gfs_0p25.pl?$parameters"
    $output = Join-Path $downloads "FVG_GFS_${run}_${cycle}Z_f006.grib2"

    try {
        Invoke-WebRequest -Uri $url -OutFile $output -UseBasicParsing
        if ((Get-Item $output).Length -gt 1024) {
            [System.Windows.MessageBox]::Show("GRIB scaricato in:`n$output", 'FVG GRIB Monitor', 'OK', 'Information') | Out-Null
            Start-Process explorer.exe -ArgumentList "/select,`"$output`""
            exit 0
        }
        Remove-Item $output -Force
    }
    catch {
        Remove-Item $output -Force -ErrorAction SilentlyContinue
    }
    $candidate = $candidate.AddHours(-6)
}

[System.Windows.MessageBox]::Show('Nessun ciclo GFS disponibile da NOAA NOMADS. Riprovare tra qualche minuto.', 'Download GRIB non riuscito', 'OK', 'Warning') | Out-Null
exit 1
