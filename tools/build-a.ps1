param(
    [string]$ArduinoCli = 'arduino-cli',
    [string]$ConfigFile = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskFqbn = 'esp32:esp32:esp32:FlashMode=dio,FlashFreq=80,FlashSize=4M,PartitionScheme=default,CPUFreq=240,UploadSpeed=921600'
$taskArgs = @('compile', '--fqbn', $taskFqbn, '--warnings', 'all', '--jobs', '8',
    '--build-path', (Join-Path $taskRoot 'build-a'), '--output-dir', (Join-Path $taskRoot 'build-a/output'))
if ($ConfigFile) { $taskArgs += @('--config-file', $ConfigFile) }
$taskArgs += (Join-Path $taskRoot 'a_esp32')
& $ArduinoCli @taskArgs
if ($LASTEXITCODE -ne 0) { throw "A build failed with exit code $LASTEXITCODE" }
