param([ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version = '0.2.0')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dll = Join-Path $root 'x64/Release/a/OptiScaler.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'Build Release x64 with /p:OptiScalerFgOnly=true first.' }
$bytes = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
if (-not $bytes.Contains('FG-only companion v1:')) { throw 'Not a verified FG-only build.' }
$fileVersion = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
if ($fileVersion -ne "$Version.0") { throw "Expected $Version.0; found $fileVersion" }
$name = "optiscaler-mfg-ada-fg-only-$Version"
$stage = Join-Path $root "release/$name"
$zip = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) { throw 'Release output exists; refusing overwrite.' }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item -LiteralPath $dll -Destination "$stage/OptiScaler.dll"
Copy-Item -LiteralPath "$root/redist/fg-only/OptiScaler.ini" -Destination "$stage/OptiScaler.ini"
$files = @('README.md','LICENSE','setup_windows.bat','get_streamline.ps1',
    'redist/streamline/manifest.json',"docs/RELEASE-$Version.md",'docs/DLSS-FRAME-GENERATION.md',
    'docs/CREDITS.md','docs/RTX40-MFG.md','docs/LEGACY-NR-README.md','presets/Genshin-NR.ini',
    'docs/MAVIS-INTEGRATION-20261006.md','docs/MODEL-APPLY-FPS-20261006.md',
    'Licenses/MFGAdaUnlock-RenoDx-MIT.txt','Licenses/RenoDX_ATTRIBUTION.txt')
foreach ($file in $files) {
    $target = Join-Path $stage $file
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $target
}
$payload = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
if (@($payload | Where-Object { $_.Extension -eq '.dll' -and $_.Name -ne 'OptiScaler.dll' }).Count) {
    throw 'Unexpected runtime DLL in FG-only package.'
}
$manifest = foreach ($file in $payload) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash,
        $file.FullName.Substring($stage.Length + 1).Replace('\','/')
}
# Generated packaging outputs, not source edits.
$manifest | Set-Content -LiteralPath "$stage/SHA256SUMS.txt" -Encoding ascii
Compress-Archive -Path "$stage/*" -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash -LiteralPath $zip -Algorithm SHA256
