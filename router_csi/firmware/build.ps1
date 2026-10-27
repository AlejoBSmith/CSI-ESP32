param(
    [ValidateSet('router_rx')][string]$Environment='router_rx',
    [string]$Port='',
    [switch]$Upload
)
$ErrorActionPreference='Stop'
$repoPath=Split-Path -Parent $PSScriptRoot
$pioPython=Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (!(Test-Path -LiteralPath $pioPython)) { throw 'Install the PlatformIO extension for VS Code first.' }
# ESP-IDF rejects spaces in PROJECT_DIR. Map the same tree, never copy sources.
$mapped=$false
if(Test-Path 'S:\') {
    $mapping=(& subst) -join "`n"
    if($mapping -notmatch [regex]::Escape($repoPath)){throw 'S: is in use by another path. Choose a free drive letter in this script.'}
} else {
    & subst S: $repoPath
    if($LASTEXITCODE -ne 0){throw 'Could not create the temporary S: alias.'}
    $mapped=$true
}
$entered=$false
try {
    Push-Location -LiteralPath 'S:\firmware'
    $entered=$true
    $buildArgs=@('-m','platformio','run','-d','S:\firmware','-e',$Environment)
    if($Upload){if(!$Port){throw 'Use -Port COMxx with -Upload.'};$buildArgs+=@('-t','upload','--upload-port',$Port)}
    & $pioPython @buildArgs
    if($LASTEXITCODE -ne 0){throw 'PlatformIO failed; inspect its error output.'}
} finally {
    if($entered){Pop-Location}
    if($mapped){& subst S: /D}
}
