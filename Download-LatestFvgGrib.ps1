[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$messageAssembly = Add-Type -AssemblyName PresentationFramework -PassThru
$downloads = [Environment]::GetFolderPath('UserProfile') + '\Downloads'
$candidate = (Get-Date).ToUniversalTime().AddHours(-5)
$forecastHours = @('006', '012', '018')

function Get-FvgGribMessage {
    param([string]$Run, [string]$Cycle, [string]$ForecastHour)

    $file = "gfs.t${Cycle}z.pgrb2.0p25.f${ForecastHour}"
    $query = @{
        file = $file
        lev_surface = 'on'; lev_10_m_above_ground = 'on'
        var_CAPE = 'on'; var_UGRD = 'on'; var_VGRD = 'on'
        subregion = 'on'; leftlon = '12'; rightlon = '14'; toplat = '47'; bottomlat = '45'
        dir = "/gfs.$Run/$Cycle/atmos"
    }
    $parameters = ($query.GetEnumerator() | ForEach-Object {
        '{0}={1}' -f $_.Key, [uri]::EscapeDataString($_.Value)
    }) -join '&'
    $url = "https://nomads.ncep.noaa.gov/cgi-bin/filter_gfs_0p25.pl?$parameters"

    $temp = [System.IO.Path]::GetTempFileName()
    try {
        Invoke-WebRequest -Uri $url -OutFile $temp -UseBasicParsing
        $bytes = [System.IO.File]::ReadAllBytes($temp)
        $magic = $bytes | Select-Object -First 4
        if ($magic.Length -ne 4 -or [System.Text.Encoding]::ASCII.GetString($magic) -ne 'GRIB') {
            return $null
        }
        return $bytes
    }
    finally {
        Remove-Item $temp -Force -ErrorAction SilentlyContinue
    }
}

for ($attempt = 0; $attempt -lt 5; $attempt++) {
    $cycleHour = [Math]::Floor($candidate.Hour / 6) * 6
    $run = Get-Date -Date $candidate.Date.AddHours($cycleHour) -Format 'yyyyMMdd'
    $cycle = $cycleHour.ToString('00')
    $output = Join-Path $downloads "FVG_GFS_${run}_${cycle}Z_evoluzione.grib2"
    $collected = [System.Collections.Generic.List[byte]]::new()
    $hoursFetched = @()

    foreach ($hour in $forecastHours) {
        try {
            $bytes = Get-FvgGribMessage -Run $run -Cycle $cycle -ForecastHour $hour
            if ($null -ne $bytes) {
                $collected.AddRange([byte[]]$bytes)
                $hoursFetched += $hour
            }
        }
        catch {
            # Questo lead time non e' ancora pubblicato per il ciclo corrente: si prova comunque con gli altri.
        }
    }

    if ($hoursFetched.Count -ge 2) {
        [System.IO.File]::WriteAllBytes($output, $collected.ToArray())
        $hoursLabel = ($hoursFetched | ForEach-Object { "+${_}h" }) -join ', '
        [System.Windows.MessageBox]::Show("GRIB scaricato in:`n$output`n`nLead time inclusi: $hoursLabel`nUsa `"Confronta / differenza`" per l'evoluzione temporale.", 'FVG GRIB Monitor', 'OK', 'Information') | Out-Null
        Start-Process explorer.exe -ArgumentList "/select,`"$output`""
        exit 0
    }
    $candidate = $candidate.AddHours(-6)
}

[System.Windows.MessageBox]::Show('Nessun ciclo GFS disponibile da NOAA NOMADS. Riprovare tra qualche minuto.', 'Download GRIB non riuscito', 'OK', 'Warning') | Out-Null
exit 1
