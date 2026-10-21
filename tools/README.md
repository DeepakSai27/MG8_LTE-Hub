# MG8 J-Link tools

J-Link Commander scripts used to probe and recover the MG8's nRF52840. Run them from this folder.

```powershell
$jlink = 'C:\Program Files\SEGGER\JLink_V818\JLink.exe'
& $jlink -NoGui 1 -ExitOnError 1 -Device NRF52840_XXAA -If SWD -Speed 1000 -CommandFile identify.jlink
& $jlink -NoGui 1 -CommandFile vtref.jlink          # no target connection needed
.\capture_pins.ps1 capture_pins_15s.jlink           # runs a pin script and decodes the result
```

**Any script with `connect` mass-erases a locked chip.** A stock MG8 with Minew firmware is locked, so the first connect wipes it. Only `vtref.jlink` is safe on a stock unit.

| Script | What it does | Changes the chip? |
|---|---|---|
| `vtref.jlink` | Target voltage only. ~3.0 V steady = our firmware, ~1.8 V steady = blank, jumping ~2.3 V = DC/DC brown-out loop | No |
| `identify.jlink` | Chip ID, BLE address, UICR APPROTECT / REGOUT0, start of flash | No (but see the warning above) |
| `reset_reason.jlink` | RESETREAS and two PIN_CNF registers | No |
| `read_pins_pullup_pulldown.jlink` | Every GPIO with pull-up, then pull-down. Pins that ignore the pull are driven by something on the board | GPIO config until reset |
| `capture_pins_15s.jlink` | Every GPIO with pull-up, sampled for 15 s. Tap a pad to G during the run to find the pin behind it (found TX = P0.13, RX = P0.15, button = P0.04 this way). Decode with `capture_pins.ps1` | GPIO config until reset |
| `led_test.jlink` | Red, green, blue LED for 2 s each | GPIO config until reset |
| `uicr_regout0_3v0.jlink` | UICR.REGOUT0 = 3.0 V, then reset (the firmware does this on boot) | Writes UICR |
| `uicr_nfcpins_gpio.jlink` | UICR.NFCPINS = GPIO, then reset (the firmware sets it too) | Writes UICR |
| `unlock_retry_loop.jlink` | Recovery: 60 connect attempts, then reads flash / UICR (all FF = blank and open). Use with the V pad held at 3.3 V, see the main README | Mass-erases |

`capture_pins.ps1` lists every P0 / P1 sample and the pins that read low in at least one sample.

The pin scripts reconfigure every GPIO, including the LED pins, and the setting stays until the next reset. After `read_pins_pullup_pulldown.jlink` the LED pins are left with a pull-down, so the firmware's LED goes off and green glows dimly. That is not the modem. Reset or replug the MG8 to get the firmware's LEDs back.

Rules for new scripts: pull-ups only on unknown pins (never push-pull drive them), never write UICR.APPROTECT.
