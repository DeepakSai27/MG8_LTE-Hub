# MG8 bring-up

Bring-up firmware for running our own code on the Minew MG8 LTE gateway (nRF52840 + Quectel Cat.1 modem).
Built with nRF Connect SDK v2.6.1 on the `nrf52840dk_nrf52840` board definition plus an overlay. Logs go out over SEGGER RTT through the J-Link, so no UART adapter is needed.

The firmware currently searches for the modem's power, PWRKEY and UART pins. On the first unit it found nothing (see Status).

## Build, flash, log

```powershell
.\build.ps1 -Pristine                    # NCS 2.6.1 toolchain at C:\ncs
nrfjprog -f nrf52 --program build\zephyr\zephyr.hex --sectorerase --verify
nrfjprog -f nrf52 --reset
JLinkRTTLogger.exe -Device NRF52840_XXAA -If SWD -Speed 4000 -RTTChannel 0 rtt.log
```

Press the MG8 function button to rerun the search.

## Hardware

| Item | Detail |
|---|---|
| MCU | nRF52840, variant AAD0, aQFN73, 1 MB flash / 256 KB RAM |
| Modem | Quectel LTE Cat.1 module (Qualcomm QCX216 inside), FCC build bands B2/4/7/25/66/38/41 |
| Flash | Winbond W25Q64JV (pins unknown) |
| USB | Power only, no data lines |
| FCC ID | 2ABU6-MG8 (user manual and internal photos are public; the photos show a test board that differs from production) |

### Pads and pins (verified with J-Link register probing)

| Pad / function | nRF52840 pin | Notes |
|---|---|---|
| SWD | C = SWCLK, D = SWDIO | V = VDD (~3.0 V), G = GND, R = unconfirmed |
| Debug pad TX | P0.13 | |
| Debug pad RX | P0.15 | |
| LED red | P0.08 | Active low (common anode) |
| LED green | P0.06 | Active low |
| LED blue | P0.12 | Active low |
| Function button | P0.04 | Active low, needs the internal pull-up |
| P0.09 / P0.10 | Held low on the board | Guessed PWRKEY / RESET transistor bases, NOT confirmed |
| P0.30 + P1.00 | Unknown | Went low together once while probing near the TX pad |

J-Link wiring (20-pin): pin 1 VTref -> V, pin 4 GND -> G, pin 7 SWDIO -> D, pin 9 SWCLK -> C. The MG8 must be powered from its own USB plug.

## Pitfalls (each of these happened on the first unit)

1. **Connecting a J-Link to a stock MG8 erases it.** Minew's firmware locks the chip; J-Link's nRF52 connect script unsecures it automatically, which mass-erases the flash, even with `-NoGui 1`.
2. **No DC/DC inductors.** The DK board definition enables both DC/DC converters (`BOARD_ENABLE_DCDC`, `BOARD_ENABLE_DCDC_HV`). On the MG8 that collapses VDD to a jumping ~2.3 V and the chip browns out in a loop: no LED, erratic SWD, erases never finish. `prj.conf` disables both. Do not remove that.
3. **Legacy APPROTECT.** This AAD0 chip uses the old scheme: UICR.APPROTECT `0xFF` = open, anything else locks. Never write `0x5A` (the newer revisions' "HwDisabled"); it locks this chip. The firmware leaves APPROTECT alone.
4. **REGOUT0.** The erased default is 1.8 V, too low for the LEDs and flash. The firmware writes UICR.REGOUT0 = 3.0 V (`0xFFFFFFFC`) on boot and resets once.
5. **Never push-pull drive unknown pins.** The search only uses pull-ups. REGOUT0 can supply ~25 mA in total.
6. `nrfjprog --recover` fails on a locked chip ("Detected CPU core ... does match expected"). Use a J-Link Commander `connect` instead, which unsecures automatically.

## Recovering a soft-bricked unit

If VDD sits at a jumping ~2.2–2.4 V and SWD is unreliable (firmware with DC/DC enabled is on the chip):

1. Hold the V pad up externally: an ESP32 dev board **3V3** pin -> V, its GND -> G. Never 5 V. The LED goes blue then red if the old bring-up firmware is on it.
2. Run `JLink.exe -Device NRF52840_XXAA -If SWD -Speed 1000` and `connect` (repeat until it says `Cortex-M4 identified`). It unsecures and fully erases the chip.
3. Remove the external 3.3 V. VDD should now be a steady ~1.8 V (blank chip).
4. Flash this firmware; VDD then settles at a steady ~3.0 V.

## Status (2026-10-21)

- Firmware runs stably: VDD 2.94 V, APPROTECT open, RTT logging works.
- Modem search: with all 37 unknown pins pulled up and P0.09 / P0.10 each pulsed high for 1.2 s, no pin was ever seen driven high, so no UART pair was tried. The production board likely uses a different level shifter than the FCC test board (no TXS0108E found), and the pull-down sense may not detect its idle-high lines.
- Next: photograph both sides of the production board and the Quectel label, then measure the modem supply and VDD_EXT with a multimeter to find its power enable, PWRKEY and UART.
