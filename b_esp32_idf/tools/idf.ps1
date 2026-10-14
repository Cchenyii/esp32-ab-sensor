param(
    [ValidateSet('build', 'menuconfig', 'flash', 'monitor', 'flash-monitor', 'size', 'reconfigure')]
    [string]$Action = 'build',
    [string]$Port,
    [string]$BuildDirectory,
    [string]$ProjectVersion,
    [string]$SdkConfig,
    [string]$EimConfig = 'C:\Espressif\tools\eim_idf.json'
)

$ErrorActionPreference = 'Stop'
$taskProject = Split-Path -Parent $PSScriptRoot
$taskPinnedVersion = (Get-Content -LiteralPath (Join-Path $taskProject '.idf-version') -Raw).Trim()
$taskConfig = Get-Content -LiteralPath $EimConfig -Raw | ConvertFrom-Json
$taskSetup = @($taskConfig.idfInstalled | Where-Object { $_.name -eq $taskPinnedVersion })
if ($taskSetup.Count -ne 1) {
    throw "Install the pinned ESP-IDF $taskPinnedVersion using EIM first."
}
$taskSetup = $taskSetup[0]
if (!(Test-Path -LiteralPath $taskSetup.activationScript)) {
    throw "ESP-IDF activation script does not exist: $($taskSetup.activationScript)"
}

if ($Action -in @('flash', 'monitor', 'flash-monitor') -and [string]::IsNullOrWhiteSpace($Port)) {
    throw 'Specify the USB serial port explicitly, for example -Port COM7.'
}

. $taskSetup.activationScript
$taskIdfScript = Join-Path $taskSetup.path 'tools\idf.py'
$taskVersionOutput = & $taskSetup.python $taskIdfScript --version
if ($LASTEXITCODE -ne 0 -or $taskVersionOutput.Trim() -ne "ESP-IDF $taskPinnedVersion") {
    throw "Unexpected ESP-IDF version: $taskVersionOutput"
}

$taskArguments = @('-C', $taskProject)
if ($BuildDirectory) {
    $taskBuildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $taskProject $BuildDirectory }
    $taskArguments += @('-B', $taskBuildPath)
}
if ($ProjectVersion) {
    if ($ProjectVersion -notmatch '^[A-Za-z0-9_.+-]{1,31}$') { throw 'Invalid firmware version.' }
}
$taskArguments += @('-D', "B_OTA_BUILD_VERSION=$ProjectVersion")
if ($SdkConfig) {
    $taskSdkConfigPath = if ([IO.Path]::IsPathRooted($SdkConfig)) { $SdkConfig } else { Join-Path $taskProject $SdkConfig }
    $taskArguments += @('-D', "SDKCONFIG=$taskSdkConfigPath")
}
if ($Port) { $taskArguments += @('-p', $Port) }
if ($Action -eq 'flash-monitor') {
    $taskArguments += @('flash', 'monitor')
} else {
    $taskArguments += $Action
}
& $taskSetup.python $taskIdfScript @taskArguments
if ($LASTEXITCODE -ne 0) { throw "idf.py exited with code $LASTEXITCODE" }
