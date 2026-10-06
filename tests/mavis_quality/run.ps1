param([string]$Provider='D:\APPS\HoYoShadeHub\OptiScaler\mfg-ada\mfg-ada-0.1.9\OptiScaler\streamline\nvngx_dlssg.dll')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../..").Path
$out=Join-Path (Split-Path $repo -Parent) '.build-temp/mfg-upstream/tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$batch=@('@echo off','call "D:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul','if errorlevel 1 exit /b 1')
foreach($name in @('thin_geometry','blackwell_kernel','quality_guard','pacing_policy','adaptive_quality_v2','adaptive_inpaint_v3','quality_build_policy')) {
 $source=Join-Path $PSScriptRoot ($name+'_tests.cpp')
 $batch+='cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /I"'+$repo+'/external/streamline" "'+$source+'" /Fo:"'+$out+'/'+$name+'.obj" /Fe:"'+$out+'/'+$name+'.exe"'
 $batch+='if errorlevel 1 exit /b 1'
 $batch+='"'+$out+'/'+$name+'.exe"'
 $batch+='if errorlevel 1 exit /b 1'
}
$batch+='cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /DMFGUNLOCK_LOCAL_LOW_OVERHEAD "'+$PSScriptRoot+'/quality_build_policy_tests.cpp" /Fo:"'+$out+'/local-policy.obj" /Fe:"'+$out+'/local-policy.exe"'
$batch+='if errorlevel 1 exit /b 1'
$batch+='"'+$out+'/local-policy.exe"'
$batch+='if errorlevel 1 exit /b 1'
$batch+='cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX "'+$repo+'/tests/fg_depth_gpu_smoke.cpp" /Fo:"'+$out+'/depth.obj" /Fe:"'+$out+'/depth.exe" /link d3d12.lib dxgi.lib d3dcompiler.lib'
$batch+='if errorlevel 1 exit /b 1'
foreach($args in @('','enhanced','enhanced menu')) {
 $batch+='"'+$out+'/depth.exe" '+$args
 $batch+='if errorlevel 1 exit /b 1'
}
$generated=Join-Path $repo 'OptiScaler/framegen/dlssg/mavis/blackwell_cubins.generated.hpp'
$variants=Join-Path $repo 'OptiScaler/framegen/dlssg/mavis/thin_geometry_cubins.generated.hpp'
if((Test-Path -LiteralPath $generated) -and (Test-Path -LiteralPath $variants) -and (Test-Path -LiteralPath $Provider)) {
 $batch+='cl /nologo /std:c++20 /EHsc /O2 /utf-8 "'+$PSScriptRoot+'/provider_mapping_tests.cpp" /Fo:"'+$out+'/provider.obj" /Fe:"'+$out+'/provider.exe"'
 $batch+='if errorlevel 1 exit /b 1'
 $batch+='"'+$out+'/provider.exe" "'+$Provider+'"'
 $batch+='if errorlevel 1 exit /b 1'
} else {
 Write-Host 'SKIP: exact provider image integration requires locally generated tables and -Provider DLL'
}
$batch+='exit /b 0'
$path=Join-Path $out 'run.cmd'
Set-Content -LiteralPath $path -Value $batch -Encoding ascii
& $path
if($LASTEXITCODE) {throw 'Mavis quality / fallback menu GPU regression failed'}
