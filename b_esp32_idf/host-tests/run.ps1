param([string]$BuildDirectory = 'build-host')
$ErrorActionPreference = 'Stop'
$taskProject = Split-Path -Parent $PSScriptRoot
$taskBuild = if ([IO.Path]::IsPathRooted($BuildDirectory)) {$BuildDirectory} else {Join-Path $taskProject $BuildDirectory}
& cmake -S $PSScriptRoot -B $taskBuild -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) {throw 'Host configure failed'}
& cmake --build $taskBuild --parallel
if ($LASTEXITCODE -ne 0) {throw 'Host build failed'}
& ctest --test-dir $taskBuild --output-on-failure --output-junit (Join-Path $taskBuild 'results.xml')
if ($LASTEXITCODE -ne 0) {throw 'Host regression failed'}
