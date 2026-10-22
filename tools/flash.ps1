# Flashes the MG8 with a clean modem shutdown first.
# Usage: .\tools\flash.ps1 [-Hex build\zephyr\zephyr.hex] [-TimeoutS 30]
#
# The firmware polls NRF_POWER->GPREGRET2: 0xA5 asks it to send AT+QPOWD and switch the modem
# supply off, and it answers 0x5A when done (~5 s). Without this, the reset at the end of a flash
# cuts the modem's 3.8 V supply while it is running. Firmware that predates the mailbox never
# answers; the script then flashes anyway after the timeout.
#
# The mailbox goes through J-Link Commander, not nrfjprog: nrfjprog halts the CPU on every
# connect, and halting while BLE is active makes the radio scheduler assert and reset the chip.
#
# Only for a chip running our firmware (unlocked). Connecting J-Link Commander to a locked chip,
# such as a stock MG8, mass-erases it.
param([string]$Hex = "$PSScriptRoot\..\build\zephyr\zephyr.hex", [int]$TimeoutS = 30)

if (-not (Test-Path $Hex)) {
    Write-Error "No hex file at $Hex (did the build fail?). Not touching the board."
    exit 1
}

$jlink = 'C:\Program Files\SEGGER\JLink_V818\JLink.exe'
$script = Join-Path $env:TEMP 'mg8_mailbox.jlink'

function Invoke-JLink([string[]]$Commands) {
    (@('connect') + $Commands + 'exit') -join "`n" | Set-Content -Encoding ascii $script
    & $jlink -Device NRF52840_XXAA -If SWD -Speed 4000 -AutoConnect 1 -NoGui 1 -CommandFile $script 2>&1
}

# Refuse to go on if the chip is locked: a Commander connect would erase it. Reads the nRF
# CTRL-AP APPROTECTSTATUS (AP 1, register 0x0C; 1 = open) with raw SWD, without connecting to
# the core, so it neither runs the unlock script nor halts the CPU. AP reads are posted: the
# second read returns the value.
@(
    'si SWD', 'speed 1000', 'SWDSelect',
    'SWDWriteDP 1 0x50000000',   # power up the debug domain
    'SWDWriteDP 2 0x01000000',   # select CTRL-AP, bank 0
    'SWDReadAP 3', 'SWDReadAP 3', 'exit'
) -join "`n" | Set-Content -Encoding ascii $script
$status = & $jlink -NoGui 1 -CommandFile $script 2>&1 | Select-String 'Read AP register 3' | Select-Object -Last 1
if ("$status" -notmatch '= 0x00000001$') {
    Write-Error "Chip not reachable or APPROTECT is active ($status). Not touching it."
    exit 1
}

Invoke-JLink @('w4 0x40000520 0xA5') | Out-Null
Write-Host 'Waiting for the firmware to shut the modem down...'
$done = $false
for ($i = 0; $i -lt $TimeoutS; $i++) {
    Start-Sleep 1
    if ((Invoke-JLink @('mem32 0x40000520 1')) -match '^40000520 = 0000005A') { $done = $true; break }
}
if ($done) { Write-Host "Modem shut down cleanly after $($i + 1) s." }
else { Write-Warning "No answer in $TimeoutS s (older firmware?). Flashing anyway." }

nrfjprog -f nrf52 --program $Hex --sectorerase --verify
if ($LASTEXITCODE -ne 0) {
    # Seen once right after the shutdown: a write error at 0x0 that a plain retry fixed
    Write-Warning 'Programming failed, retrying once'
    nrfjprog -f nrf52 --program $Hex --sectorerase --verify
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
nrfjprog -f nrf52 --reset
