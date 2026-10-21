# Runs a pin-reading J-Link script and reports which nRF52840 pins changed during the run.
# Usage: .\capture_pins.ps1 capture_pins_15s.jlink
#        .\capture_pins.ps1 read_pins_pullup_pulldown.jlink
# Prints every P0 / P1 IN sample, then the pins that were low in at least one sample.
param([Parameter(Mandatory = $true)][string]$Script)

$jlink = 'C:\Program Files\SEGGER\JLink_V818\JLink.exe'
$out = & $jlink -NoGui 1 -ExitOnError 1 -Device NRF52840_XXAA -If SWD -Speed 1000 -CommandFile (Resolve-Path $Script)
$out | Select-String 'VTref|unsecure|Could not|Error' | ForEach-Object { $_.Line }

$p0 = @($out | Select-String '^50000510 = ([0-9A-F]+)' | ForEach-Object { [Convert]::ToUInt32($_.Matches[0].Groups[1].Value, 16) })
$p1 = @($out | Select-String '^50000810 = ([0-9A-F]+)' | ForEach-Object { [Convert]::ToUInt32($_.Matches[0].Groups[1].Value, 16) })

"samples: $($p0.Count)"
"P0: " + (($p0 | ForEach-Object { '{0:X8}' -f $_ }) -join ' ')
"P1: " + (($p1 | ForEach-Object { '{0:X4}' -f $_ }) -join ' ')

foreach ($port in @(@{ Name = 'P0'; Values = $p0; Bits = 32 }, @{ Name = 'P1'; Values = $p1; Bits = 16 })) {
    for ($b = 0; $b -lt $port.Bits; $b++) {
        $lows = @($port.Values | Where-Object { ($_ -band (1 -shl $b)) -eq 0 }).Count
        if ($lows -gt 0) { '{0}.{1:D2} low in {2}/{3} samples' -f $port.Name, $b, $lows, $port.Values.Count }
    }
}
