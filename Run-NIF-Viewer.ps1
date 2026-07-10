$ErrorActionPreference = "Stop"

$repositoryRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDirectory = Join-Path $repositoryRoot "build"
$viewerExecutable = Join-Path $repositoryRoot "viewer\LU NIF Viewer.exe"
$viewerArguments = $args

Write-Host "Building the canonical LU NIF Viewer..."
& cmake -S $repositoryRoot -B $buildDirectory
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

& cmake --build $buildDirectory --target lu_nif_viewer_canonical --config RelWithDebInfo -- /m:1
if ($LASTEXITCODE -ne 0) {
    throw "Canonical viewer build failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path -LiteralPath $viewerExecutable)) {
    throw "The canonical viewer was not published to: $viewerExecutable"
}

Write-Host "Launching: $viewerExecutable"
& $viewerExecutable @viewerArguments
