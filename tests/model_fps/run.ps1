$ErrorActionPreference='Stop'
$out=Join-Path $PSScriptRoot '../../../.build-temp/model-fps-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$code=@('@echo off','call "D:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul')
$code+='cl /nologo /std:c++20 /EHsc /O2 /utf-8 "'+$PSScriptRoot+'/model_fps_tests.cpp" /Fo:"'+$out+'/tests.obj" /Fe:"'+$out+'/tests.exe"'
$code+='if errorlevel 1 exit /b 1'
$code+='"'+$out+'/tests.exe"'
$code+='exit /b %errorlevel%'
$p=Join-Path $out 'build.cmd'
Set-Content -LiteralPath $p -Value $code -Encoding ascii
& $p
if($LASTEXITCODE) {throw 'Model ownership / measured FPS regression failed'}
