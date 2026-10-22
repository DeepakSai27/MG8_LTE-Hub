# MG8 bring-up

Custom firmware for the Minew MG8 LTE gateway (nRF52840 + Quectel Cat.1 modem), nRF Connect SDK v2.6.1.
Read README.md first: it has the verified pin map, the pitfalls, the recovery procedure and the current status.

## Hard rules (each one broke the first unit)

- Never write UICR.APPROTECT. This nRF52840 (AAD0) uses legacy APPROTECT: 0xFF = open, any other value (including 0x5A "HwDisabled") locks the chip.
- Keep the DC/DC converters off. The MG8 has no DC/DC inductors, and enabling them browns the chip out in a loop (VDD jumps around ~2.3 V). Always build for our `mg8_nrf52840` board, which never selects `SOC_DCDC_NRF52X`/`_HV`, never for the nRF52840 DK. Don't remove the `#error` guard in `src/main.c`.
- Connecting a J-Link to a stock (Minew firmware) MG8 mass-erases it, even with `-NoGui 1`. Say so and get the user's go-ahead before any J-Link connect on a new unit.
- Never push-pull drive unknown pins. Use pull-ups only; REGOUT0 supplies ~25 mA in total.
- Before writing to UICR, flash or pins, check the chip revision and board config, and tell the user what the step actually does. Report a result only after verifying it (read back, VTref, RTT).

## Working with the hardware

- The board is powered from its own USB plug; USB carries no data. Logs come over SEGGER RTT through the J-Link.
- VTref ~3.0 V steady = healthy with our firmware; ~1.8 V steady = blank chip; jumping ~2.2–2.4 V = DC/DC brown-out loop (see the README recovery section).
- Build with `.\build.ps1` and flash with `.\tools\flash.ps1`. It refuses a locked chip, shuts the modem down cleanly, then runs `nrfjprog --program --sectorerase --verify`. `nrfjprog --recover` does not work on this chip.
- Don't use `nrfjprog --memrd`/`--memwr` on a running board. It halts the CPU, BLE then asserts, and the chip resets, which cuts the modem's supply. Read and write memory with J-Link Commander (no halt), or use the RTT console (`!help`).
- The verified pin map (modem power, PWRKEY, RESET, UART, flash, LEDs) is in README.md. Don't re-derive it.
- The TX/RX test pads are P0.13/P0.15, which are the modem's RESET and PWRKEY controls, not a debug UART. Use the RTT AT console instead.
- The hub app is in `hub/`: build it with `.\build.ps1 -App hub`, flash it with `.\tools\flash.ps1 -Hex build_hub\zephyr\zephyr.hex`. Uploads go to a test echo server unless `prod.conf` is used on purpose; never point it at production without the user's go-ahead.
- When porting from the ESP32 hub, take settings from its `sdkconfig`, not its Kconfig defaults (the tag name is `exactcomms_tag`). Tags send by indication, not notification, and only advertise after hearing the hub beacon. The tag firmware is in `C:\Projects\Exactcomms\Testing REPO's\Exactcomms_Tag`.
- Never commit Minew's documents (schematics, manuals, PDFs) to this repo. They're under NDA. Pin information goes in README.md as our own notes only.
