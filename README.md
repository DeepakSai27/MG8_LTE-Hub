# MG8 bring-up

Bring-up firmware for running our own code on the Minew MG8 LTE gateway (nRF52840 + Quectel EG916Q-GL Cat.1 modem).
Built with nRF Connect SDK v2.6.1 for our own board definition, `mg8_nrf52840` (in [boards/arm/mg8_nrf52840](boards/arm/mg8_nrf52840)). Logs go out over SEGGER RTT through the J-Link, so no UART adapter is needed.

At boot the firmware:
1. reports the external flash
2. starts BLE advertising and scanning
3. powers up the modem, queries it and waits for network registration
4. runs an AT console over RTT

Every part of the board has been checked (see Status).

## Exactcomms hub app (`hub/`)

A port of the ESP32-S3 hub (`exactcomms_esp32-s3-hub`) to the MG8. It downloads Exactcomms tags over BLE and uploads their records over LTE.

```powershell
.\build.ps1 -App hub                                   # builds into build_hub
.\tools\flash.ps1 -Hex build_hub\zephyr\zephyr.hex
```

**The three devices:**

| Device | Role | Advertises |
|---|---|---|
| E2 beacon | Fixed in a location | Eddystone-UID, namespace `AAAA BBBB CCCC DDDD EEEE` |
| Tag (`exactcomms_tag`) | Carried around; records the beacons it passes | Nothing, until it hears a hub *and* has records stored |
| Hub (MG8) | Collects from tags, uploads over LTE | Hub beacon: Eddystone-UID, namespace `AAAA BBBB CCCC DDDD AAAA` |

**The download flow** (as the ESP32 hub and tag firmware 2.0.1 implement it):
1. The hub advertises its beacon.
2. A tag that hears it, has records and is past its 30 s cool-down advertises `exactcomms_tag` for a short time.
3. The hub connects and discovers service `0x010B`: handshake `0x1401`, data `0x1402` (config `0x1403` is not used yet).
4. The hub enables **indications** on both characteristics. The CCC value is 2; with notifications the tag cannot send. The hub then writes its free record count (uint16) to the handshake characteristic.
5. The tag indicates `mac|fw|battery mV hex|uptime ms hex`. The hub writes `2` to the data characteristic to start.
6. The tag indicates packets of 4 records. It sends the next one only after the hub confirms the previous one, and deletes what it has handed over. It stops after the offered count and disconnects.
7. Each packet becomes one upload item: 8 strings `rssi|battery|beacon|tag|fw|YYYY-mm-dd HH:MM:SS|uptime`, the same format as the ESP32 hub.

**Uploads:**
- The hub opens a TCP connection through the modem (`AT+QIOPEN`/`QISEND`/`QIRD`) for each item. Any reply from the server is the ACK.
- After 5 failures it moves main → AZ2 → AZ3, and returns to the main server after 120 min.
- It queues a status ping at start-up and every 15 min.
- The clock comes from NTP through the modem.

**Test mode (default):**
- Uploads go to the public echo server `tcpbin.com:4242`, with a newline added so the echo server replies.
- `hub/prod.conf` holds the production servers. Use them only on purpose: `.\build.ps1 -App hub -CMakeArgs '-DEXTRA_CONF_FILE=prod.conf'`.

**Settings** come from the ESP32 hub's real `sdkconfig`, not its Kconfig defaults:
- The tag name is `exactcomms_tag`, not the Kconfig default `exactcomms_c10`.
- The upload queue is 24 items instead of 50, because of RAM.

**LED:**
- red: no LTE
- blue: tag connected
- green: LTE up and last upload OK
- yellow: LTE up but the last upload failed

**Verified on 2026-10-22:**
- Tag `EB:C7:43:A2:71:82` (fw 2.0.1) handed over 96 records per session, one session a minute, with new records each time. All items were uploaded and acknowledged.
- Each session takes 24 packets in about 2.6 s. An upload over LTE takes about 1.2 s.
- 8 tags (fw 2.0.1-release and 1.6.0-r): 30 sessions in 4 minutes, every upload acknowledged.
- About 1 in 8 connections drops right after being made (`0x3e`). The tag is picked up again on its next advertisement, and no records are lost.
- Production (`prod.conf`, built into `build_hub_prod`): the main server `52.51.154.7:9007` acknowledges every upload with `HTTP/1.1 200 OK`. The hub ID it receives is the MG8's BLE address (`cb:16:4a:4c:7d:57` on the first unit). The last status-ping field is the LTE signal in dBm, where the ESP32 hub sends Wi-Fi RSSI.

**Robustness:**
- **A tag that answers the handshake but sends nothing is dropped after 5 s.** Tags with nothing left do this; without the limit, each one would block every other tag for 60 s.
- **The hub offers one packet less than it can hold.** A tag once sent 99 records when offered 96. A packet that didn't fit would be lost, because the tag deletes records the hub has confirmed.
- **Three AT commands timing out in a row power-cycle the modem**, instead of waiting 5 minutes without LTE.

**Debug** (off by default):
- `CONFIG_HUB_LOG_NEARBY` logs every named or Eddystone BLE device once.
- `CONFIG_HUB_WATCH_ADDR_PREFIX` logs the raw advertising data of a comma-separated list of address prefixes. `"*"` logs every device.
- A tag only advertises flags and the name `exactcomms_tag`. Its firmware version comes from the handshake.

**Not done yet:**
- writing tag config (`0x1403`)
- MQTT remote logs and config, OTA
- buffering in the W25Q64 flash
- a watchdog

## Build, flash, log

```powershell
.\build.ps1 -Pristine                    # NCS 2.6.1 toolchain at C:\ncs, board mg8_nrf52840
.\tools\flash.ps1                        # clean modem shutdown, program, verify, reset
```

`build.ps1` also takes `-BuildDir <dir>` and `-CMakeArgs '-DCONFIG_X=y'` for test variants. `tools\flash.ps1` refuses to touch a locked chip; see [tools/README.md](tools/README.md) for how it works and why it avoids `nrfjprog` while the firmware runs.

Logs and AT console, either:
- J-Link RTT Viewer (Device `NRF52840_XXAA`, SWD, RTT channel 0): type AT commands into it.
- `JLinkGDBServerCL.exe -device NRF52840_XXAA -if SWD -speed 4000 -nohalt -noir -nogui -port 2331 -RTTTelnetPort 19021`, then `telnet localhost 19021`.

Each typed line goes to the modem followed by `\r`; to send raw bytes that need `\n`, use `AT+QISENDEX` with hex. Lines starting with `!` are handled by the firmware:

| Command | Does |
|---|---|
| `!on` | Power-cycle the modem (clean `AT+QPOWD` first), query it, wait for registration |
| `!off` | Clean modem shutdown and supply off |
| `!flash` | Erase the last 4 KB sector of the W25Q64, write 256 bytes, read back and compare |
| `!ble` | BLE scan summary (also printed every 30 s) |
| `!help` | List these |

The function button does the same as `!on`.

## Hardware

| Item | Detail |
|---|---|
| MCU | nRF52840, variant AAD0, aQFN73, 1 MB flash / 256 KB RAM |
| Modem | Quectel **EG916Q-GL** (global variant), firmware `EG916QGLLGR01A05M04`, on a daughterboard joined by three 2x5 headers |
| Modem bands (from `AT+QCFG="band"`) | LTE B1, 2, 3, 4, 5, 7, 8, 12, 13, 18, 19, 20, 25, 26, 28, 34, 38, 39, 40, 41, 66 |
| Level shifter | 8-channel auto-direction shifter (TXS0108E type, marked `YF08E`) on the main board. Its OE follows the modem's VDD_EXT, so the UART lines only work while the modem is on |
| Modem supply | 3.8 V buck from USB 5 V, enabled by the nRF (not from the nRF's REGOUT0) |
| Flash | Winbond W25Q64JV (JEDEC ID `ef 40 17`), 8 MB, SPI on SPIM2 at 8 MHz |
| nRF supply | Internal REGOUT0 LDO at 3.0 V (no external LDO, no DC/DC inductors) |
| Clocks | 32 MHz crystal, and a 32.768 kHz crystal on P0.00/P0.01 (fitted, used by the firmware) |
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
| Flash CS | P1.01 | W25Q64JV | Yes (JEDEC ID, erase / write / read back) |
| Flash CLK | P1.04 | | Yes |
| Flash DO (MISO) | P1.02 | | Yes |
| Flash DIO (MOSI) | P1.06 | | Yes |
| Accelerometer SCL / SDA / INT1 / INT2 | P0.02 / P0.29 / P0.28 / P0.03 | Not fitted on this board | No |
| 32.768 kHz crystal | P0.00 / P0.01 | LFCLKSTAT source = Xtal, RTC1 counts 32768/s | Yes |
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
2. **No DC/DC inductors.** The nRF52840 DK board definition enables both DC/DC converters. On the MG8 that collapses VDD to a jumping ~2.3 V and the chip browns out in a loop: no LED, erratic SWD, erases never finish. The `mg8_nrf52840` board never selects `SOC_DCDC_NRF52X` / `SOC_DCDC_NRF52X_HV`, and `src/main.c` has an `#error` if either is on. The boot log prints `DCDCEN 0/0`. Never build this firmware for the DK board.
3. **Legacy APPROTECT.** This AAD0 chip uses the old scheme: UICR.APPROTECT `0xFF` = open, anything else locks. Never write `0x5A` (the newer revisions' "HwDisabled"); it locks this chip. The firmware leaves APPROTECT alone.
4. **REGOUT0.** The erased default is 1.8 V, too low for the LEDs and flash. The firmware writes UICR.REGOUT0 = 3.0 V (`0xFFFFFFFC`) on boot and resets once.
5. **Never push-pull drive unknown pins.** REGOUT0 can supply ~25 mA in total. All pins the firmware drives now are known from the pin map.
6. **Pull-ups on the modem control pins assert them.** RESET, PWRKEY and W_DISABLE# go through N-MOSFETs with gate pull-downs: a pull-up on P0.13 / P0.31 holds the modem in reset or airplane mode. Drive them low explicitly.
7. `nrfjprog --recover` fails on a locked chip ("Detected CPU core ... does match expected"). Use a J-Link Commander `connect` instead, which unsecures automatically.
8. **An nRF reset cuts the modem's supply** (P1.09 drops) without `AT+QPOWD`. For flashing and debugger resets, `tools\flash.ps1` asks the firmware for a clean shutdown first (GPREGRET2 mailbox). Watchdog, fault and power-loss resets still cut it uncleanly.
9. **`nrfjprog` halts the CPU on every connect**, and with BLE running a halt makes the radio scheduler assert and reset the chip. Don't poke a running board with `nrfjprog --memrd`/`--memwr`. Use J-Link Commander, which reads and writes memory without halting.

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
- W25Q64 flash: JEDEC ID `ef 40 17`, 8 MB from SFDP. Erasing a 4 KB sector takes 55 ms, writing 256 bytes 1 ms, and the data reads back identical. The first bytes of the chip (`78 56 34 12 01 ff ...`) are left over from Minew's firmware.
- BLE: the passive scan hears ~20 devices in 30 s, so the receiver works. Advertising as `MG8-bringup` (non-connectable, random static address) shows up in a phone scanner app, so the transmitter works too.
- 32.768 kHz crystal: fitted and running.
- Clean modem shutdown before flashing: works, about 5 s.
- Next: port the hub logic (C10 tag scanning over BLE, upload over `AT+QIOPEN` TCP).
