param(
    [ValidateSet('rx','tx')][string]$Environment='rx',
    [string]$Port='',
    [switch]$Upload
)
$ErrorActionPreference='Stop'
$repoPath=Split-Path -Parent $PSScriptRoot
$pioPython=Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (!(Test-Path -LiteralPath $pioPython)) { throw 'Install the PlatformIO extension for VS Code first.' }
# ESP-IDF rejects spaces in PROJECT_DIR. Map the same tree, never copy sources.
$mapped=$false
if(Test-Path 'R:\') {
    $mapping=(& subst) -join "`n"
    if($mapping -notmatch [regex]::Escape($repoPath)){throw 'R: is in use by another path. Choose a free drive letter in this script.'}
} else {
    & subst R: $repoPath
    if($LASTEXITCODE -ne 0){throw 'Could not create the temporary R: alias.'}
    $mapped=$true
}
$entered=$false
try {
    Push-Location -LiteralPath 'R:\firmware'
    $entered=$true
    $buildArgs=@('-m','platformio','run','-d','R:\firmware','-e',$Environment)
    if($Upload){if(!$Port){throw 'Use -Port COMxx with -Upload.'};$buildArgs+=@('-t','upload','--upload-port',$Port)}
    & $pioPython @buildArgs
    if($LASTEXITCODE -ne 0){throw 'PlatformIO failed; inspect its error output.'}
} finally {
    if($entered){Pop-Location}
    if($mapped){& subst R: /D}
}
