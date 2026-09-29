$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install MSVC C++ Build Tools to run the native test.' }
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$out=Join-Path $repo '.pio\native-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$batch=Join-Path $out 'run.cmd'
@"
@echo off
call "$vcvars" >nul
cd /d "$out"
cl /nologo /EHsc /O2 /std:c++17 /I"$repo\firmware\main" /I"$repo\firmware\common" "$repo\tests\test_detector_native.cpp" "$repo\firmware\main\detector.cpp" /Fe:detector-test.exe
if errorlevel 1 exit /b 1
detector-test.exe --selftest
"@ | Set-Content -LiteralPath $batch -Encoding ASCII
& cmd /c $batch
if ($LASTEXITCODE -ne 0) { throw 'Native DSP/LED test failed.' }
