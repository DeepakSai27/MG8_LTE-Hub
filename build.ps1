# Builds the MG8 bring-up firmware with the nRF Connect SDK v2.6.1 toolchain.
# Usage: .\build.ps1 [-Pristine]
param([switch]$Pristine)

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
    $args = @('build', '-b', 'nrf52840dk_nrf52840', '-d', 'build', '.')
    if ($Pristine) { $args += @('-p', 'always') }
    & west @args
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
