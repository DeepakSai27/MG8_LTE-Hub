# MG8 bring-up

Bring-up firmware for running our own code on the Minew MG8 LTE gateway (nRF52840 + Quectel EG916Q-GL Cat.1 modem).
Built with nRF Connect SDK v2.6.1 on the `nrf52840dk_nrf52840` board definition plus an overlay. Logs go out over SEGGER RTT through the J-Link, so no UART adapter is needed.

The firmware powers up the modem, queries it, waits for network registration and then runs an AT console over RTT. LTE data is confirmed working (see Status).

## Build, flash, log

```powershell
.\build.ps1 -Pristine                    # NCS 2.6.1 toolchain at C:\ncs
nrfjprog -f nrf52 --program build\zephyr\zephyr.hex --sectorerase --verify
nrfjprog -f nrf52 --reset
```

Logs and AT console, either:
- J-Link RTT Viewer (Device `NRF52840_XXAA`, SWD, RTT channel 0): type AT commands into it.
- `JLinkGDBServerCL.exe -device NRF52840_XXAA -if SWD -speed 4000 -nohalt -noir -nogui -port 2331 -RTTTelnetPort 19021`, then `telnet localhost 19021`.

Each typed line goes to the modem followed by `\r`; to send raw bytes that need `\n`, use `AT+QISENDEX` with hex. The function button power-cycles the modem (clean `AT+QPOWD` first), re-runs the query and returns to the console.

## Hardware

| Item | Detail |
|---|---|
| MCU | nRF52840, variant AAD0, aQFN73, 1 MB flash / 256 KB RAM |
| Modem | Quectel **EG916Q-GL** (global variant), firmware `EG916QGLLGR01A05M04`, on a daughterboard joined by three 2x5 headers |
| Modem bands (from `AT+QCFG="band"`) | LTE B1, 2, 3, 4, 5, 7, 8, 12, 13, 18, 19, 20, 25, 26, 28, 34, 38, 39, 40, 41, 66 |
| Level shifter | 8-channel auto-direction shifter (TXS0108E type, marked `YF08E`) on the main board. Its OE follows the modem's VDD_EXT, so the UART lines only work while the modem is on |
| Modem supply | 3.8 V buck from USB 5 V, enabled by the nRF (not from the nRF's REGOUT0) |
| Flash | Winbond W25Q64JV, 8 MB, SPI |
| nRF supply | Internal REGOUT0 LDO at 3.0 V (no external LDO, no DC/DC inductors) |
| USB | Power only, no data lines |
| FCC ID | 2ABU6-MG8 (the FCC test build lists US bands only; production units carry the -GL modem) |

## Pin map

From the board's design documentation, confirmed on hardware where marked.

### nRF52840

| Function | Pin | Level / notes | Confirmed |
|---|---|---|---|
| LED red (LED1) | P0.08 | Active low, common anode to VDD | Yes |
| LED green (LED2) | P0.06 | Active low | Yes |
| LED blue (LED3) | P0.12 | Active low | Yes |
| Function button | P0.04 | Active low, needs internal pull-up | Yes |
| **Modem 3.8 V enable** | **P1.09** | High = modem powered | Yes |
| **Modem PWRKEY** | **P0.15** | High = key pressed (N-MOSFET pulls PWRKEY low). 1 s pulse powers it on | Yes |
| **Modem RESET_N** | **P0.13** | High = reset held (N-MOSFET). Keep low | Yes (held low, modem runs) |
| Modem W_DISABLE# | P0.31 | High = airplane mode (N-MOSFET). Keep low | Yes (held low, modem registers) |
| Modem PSM_IND | P0.30 | Input, inverted by an N-MOSFET with pull-up | No |
| **UART nRF TX → modem RXD** | **P0.14** | 115200 8N1, through the level shifter | Yes |
| **UART modem TXD → nRF RX** | **P0.21** | | Yes |
| UART RTS (to modem) | P0.24 | Held low. Flow control off | No |
| UART CTS (from modem) | P1.00 | | No |
| UART DTR (to modem) | P0.16 | Held low (awake) | No |
| UART DCD (from modem) | P0.22 | | No |
| UART RI (from modem) | P0.20 | | No |
| Modem PSM_INT | P0.17 | Through the level shifter | No |
| Flash CS | P1.01 | W25Q64JV | No |
| Flash CLK | P1.04 | | No |
| Flash DO (MISO) | P1.02 | | No |
| Flash DIO (MOSI) | P1.06 | | No |
| Accelerometer SCL / SDA / INT1 / INT2 | P0.02 / P0.29 / P0.28 / P0.03 | Not fitted on this board | No |
| 32 kHz crystal | P0.00 / P0.01 | Firmware uses the RC oscillator for now | No |
| Reset | P0.18 | Pad R / T10 | No |

P0.13 and P0.15 also go to the T5 / T6 test pads (labelled TX / RX on the board). They are **not** a debug UART on this board: driving them controls the modem's reset and PWRKEY.

### Pads

| Pad | Signal |
|---|---|
| V | nRF VDD (~3.0 V) |
| G | GND |
| C | SWDCLK |
| D | SWDIO |
| R | nRESET (P0.18) |
| TX | P0.13 = modem RESET control |
| RX | P0.15 = modem PWRKEY control |

J-Link wiring (20-pin): pin 1 VTref -> V, pin 4 GND -> G, pin 7 SWDIO -> D, pin 9 SWCLK -> C. The MG8 must be powered from its own USB plug.

### Modem daughterboard

The modem, SIM holder, RGB LED and button sit on a daughterboard joined to the main board by three 2x5 headers. They carry VDD_3.8V, VDD_EXT (modem I/O supply, ~1.8 V when the modem is on), VDD_3.3V, the LED and button lines, the modem control lines and the 1.8 V side of the UART. On the modem board the PWRKEY, RESET_N and W_DISABLE# MOSFETs sit next to the module. Header pin numbers are not recorded here; measure them if they are ever needed.

Useful measurement points with the modem on: VDD_3.8V ~3.8 V, VDD_EXT ~1.8 V (0 V = modem off).

## Pitfalls (each of these happened on the first unit)

1. **Connecting a J-Link to a stock MG8 erases it.** Minew's firmware locks the chip; J-Link's nRF52 connect script unsecures it automatically, which mass-erases the flash, even with `-NoGui 1`.
2. **No DC/DC inductors.** The DK board definition enables both DC/DC converters (`BOARD_ENABLE_DCDC`, `BOARD_ENABLE_DCDC_HV`). On the MG8 that collapses VDD to a jumping ~2.3 V and the chip browns out in a loop: no LED, erratic SWD, erases never finish. `prj.conf` disables both. Do not remove that.
3. **Legacy APPROTECT.** This AAD0 chip uses the old scheme: UICR.APPROTECT `0xFF` = open, anything else locks. Never write `0x5A` (the newer revisions' "HwDisabled"); it locks this chip. The firmware leaves APPROTECT alone.
4. **REGOUT0.** The erased default is 1.8 V, too low for the LEDs and flash. The firmware writes UICR.REGOUT0 = 3.0 V (`0xFFFFFFFC`) on boot and resets once.
5. **Never push-pull drive unknown pins.** REGOUT0 can supply ~25 mA in total. All pins the firmware drives now are known from the pin map.
6. **Pull-ups on the modem control pins assert them.** RESET, PWRKEY and W_DISABLE# go through N-MOSFETs with gate pull-downs: a pull-up on P0.13 / P0.31 holds the modem in reset or airplane mode. Drive them low explicitly.
7. `nrfjprog --recover` fails on a locked chip ("Detected CPU core ... does match expected"). Use a J-Link Commander `connect` instead, which unsecures automatically.
8. **An nRF reset cuts the modem's supply** (P1.09 drops) without `AT+QPOWD`. Not fixed yet.

## Recovering a soft-bricked unit

If VDD sits at a jumping ~2.2–2.4 V and SWD is unreliable (firmware with DC/DC enabled is on the chip):

1. Hold the V pad up externally: an ESP32 dev board **3V3** pin -> V, its GND -> G. Never 5 V.
2. Run `JLink.exe -Device NRF52840_XXAA -If SWD -Speed 1000` and `connect` (repeat until it says `Cortex-M4 identified`). It unsecures and fully erases the chip.
3. Remove the external 3.3 V. VDD should now be a steady ~1.8 V (blank chip).
4. Flash this firmware; VDD then settles at a steady ~3.0 V.

## Status (2026-10-22)

- Firmware runs stably: VDD ~3.0 V, APPROTECT open, RTT logging and AT console work.
- Modem: answers AT ~3 s after P1.09 on + 1 s PWRKEY pulse. UART 115200.
- SIM 1 (IMSI 204-04, Vodafone NL IoT): rejected by every UK network with EMM cause #15 (subscription, not hardware).
- SIM 2 (ICCID 8945...): registered roaming on Three UK (23420), LTE band 1, RSRP -111 dBm indoors with the case open. APN `cmnbiot` set automatically.
- Network use verified through the modem's own IP stack: `AT+QPING` 8.8.8.8 4/4, DNS (`AT+QIDNSGIP`), HTTP GET 200 (`AT+QHTTPGET`), TCP round trip to an echo server (`AT+QIOPEN` / `AT+QISENDEX` / `AT+QIRD`). Public IP exits via the SIM provider's gateway in Frankfurt.
- Not tested yet: BLE, W25Q64 flash.
- Next: clean modem shutdown on nRF reset, then port the hub logic (C10 tag scanning over BLE, upload over `AT+QIOPEN` TCP).
