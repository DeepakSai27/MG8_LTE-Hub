# Builds the MG8 bring-up firmware with the nRF Connect SDK v2.6.1 toolchain.
# Usage: .\build.ps1 [-App hub] [-Pristine] [-BuildDir <dir>] [-CMakeArgs '-DCONFIG_X=y', ...]
#   no -App: the bring-up firmware, built in .\build
#   -App hub: the Exactcomms hub app in .\hub, built in .\build_hub
param([string]$App = '.', [switch]$Pristine, [string]$BuildDir = '', [string[]]$CMakeArgs = @())
if (-not $BuildDir) { $BuildDir = if ($App -eq '.') { 'build' } else { "build_$App" } }

$toolchain = 'C:\ncs\toolchains\cf2149caf2'
$env:PATH = @(
    $toolchain,
    "$toolchain\mingw64\bin",
    "$toolchain\bin",
    "$toolchain\opt\bin",
    "$toolchain\opt\bin\Scripts",
    "$toolchain\opt\zephyr-sdk\arm-zephyr-eabi\bin"
) -join ';' | ForEach-Object { "$_;$env:PATH" }
$env:PYTHONPATH = "$toolchain\opt\bin;$toolchain\opt\bin\Lib;$toolchain\opt\bin\Lib\site-packages"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = "$toolchain\opt\zephyr-sdk"
$env:ZEPHYR_BASE = 'C:\ncs\v2.6.1\zephyr'

Push-Location $PSScriptRoot
try {
    $args = @('build', '-b', 'mg8_nrf52840', '-d', $BuildDir, $App)
    if ($Pristine) { $args += @('-p', 'always') }
    if ($CMakeArgs) { $args += @('--') + $CMakeArgs }
    # A failed build must not leave the previous hex behind for tools\flash.ps1 to pick up
    Remove-Item -ErrorAction SilentlyContinue (Join-Path $BuildDir 'zephyr\zephyr.hex')
    & west @args
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
