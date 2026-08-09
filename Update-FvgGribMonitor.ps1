[CmdletBinding()]
param(
    [string]$InstallDirectory = $PSScriptRoot,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repository = 'gmy77/GribViewer'
$releaseUri = "https://api.github.com/repos/$repository/releases/tags/continuous"
$headers = @{ 'User-Agent' = 'FVG-GribMonitor-Updater' }
$release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
$asset = @($release.assets | Where-Object { $_.name -eq 'FVG-GribMonitor.zip' })[0]

if (-not $asset) {
    throw "La release '$($release.tag_name)' non contiene FVG-GribMonitor.zip."
}

$stateDirectory = Join-Path $env:LOCALAPPDATA 'FVG-GribMonitor'
$stateFile = Join-Path $stateDirectory 'update-state.json'
$previousState = if (Test-Path $stateFile) {
    Get-Content -Raw $stateFile | ConvertFrom-Json
}

if (-not $Force -and $previousState.releaseId -eq $release.id -and $previousState.updatedAt -eq $release.updated_at) {
    Write-Host 'FVG GRIB Monitor e gia aggiornato.'
    exit 0
}

$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("FVG-GribMonitor-" + [guid]::NewGuid())
$archive = Join-Path $temporaryDirectory $asset.name
New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null

try {
    Invoke-WebRequest -Uri $asset.browser_download_url -Headers $headers -OutFile $archive
    Expand-Archive -Path $archive -DestinationPath $temporaryDirectory -Force
    $sourceDirectory = Join-Path $temporaryDirectory 'FVG-GribMonitor'
    if (-not (Test-Path (Join-Path $sourceDirectory 'FvgGribMonitor.exe'))) {
        throw 'L''archivio di aggiornamento non contiene FvgGribMonitor.exe.'
    }

    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    Get-ChildItem -Path $sourceDirectory -Force | Copy-Item -Destination $InstallDirectory -Recurse -Force
    New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    [pscustomobject]@{
        releaseId = $release.id
        updatedAt = $release.updated_at
    } | ConvertTo-Json | Set-Content -Path $stateFile -Encoding utf8
    Write-Host "Aggiornato dal canale GitHub '$($release.tag_name)'."
}
finally {
    Remove-Item -Path $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
