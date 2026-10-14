param(
    [string]$BuildDirectory = 'build-host',
    [string]$ToolchainDirectory
)
$ErrorActionPreference = 'Stop'
$taskProject = Split-Path -Parent $PSScriptRoot
$taskBuild = if ([IO.Path]::IsPathRooted($BuildDirectory)) {$BuildDirectory} else {Join-Path $taskProject $BuildDirectory}
if (!$ToolchainDirectory) {
    $taskMake = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue
    if ($taskMake) { $ToolchainDirectory = Split-Path -Parent $taskMake.Source }
}
if (!$ToolchainDirectory) {
    $taskPackageRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    $taskPackages = @(Get-ChildItem -LiteralPath $taskPackageRoot -Directory -Filter 'BrechtSanders.WinLibs.*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    foreach ($taskPackage in $taskPackages) {
        $taskCandidate = Join-Path $taskPackage.FullName 'mingw64\bin'
        if (Test-Path -LiteralPath (Join-Path $taskCandidate 'mingw32-make.exe')) {
            $ToolchainDirectory = $taskCandidate
            break
        }
    }
}
if (!$ToolchainDirectory -or !(Test-Path -LiteralPath (Join-Path $ToolchainDirectory 'gcc.exe'))) {
    throw 'Install MinGW with CMake first, or specify -ToolchainDirectory <mingw64/bin>.'
}
$taskPreviousPath = $env:PATH
try {
    $env:PATH = $ToolchainDirectory + ';' + $taskPreviousPath
    & cmake -S $PSScriptRoot -B $taskBuild -G 'MinGW Makefiles' '-DCMAKE_BUILD_TYPE=Debug'
    if ($LASTEXITCODE -ne 0) {throw 'Host configure failed'}
    & cmake --build $taskBuild --parallel
    if ($LASTEXITCODE -ne 0) {throw 'Host build failed'}
    & ctest --test-dir $taskBuild --output-on-failure --output-junit (Join-Path $taskBuild 'results.xml')
    if ($LASTEXITCODE -ne 0) {throw 'Host regression failed'}
} finally {
    $env:PATH = $taskPreviousPath
}
