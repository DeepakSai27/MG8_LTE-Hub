# MG8 bring-up

Custom firmware for the Minew MG8 LTE gateway (nRF52840 + Quectel Cat.1 modem), nRF Connect SDK v2.6.1.
Read README.md first: it has the verified pin map, the pitfalls, the recovery procedure and the current status.

## Hard rules (each one broke the first unit)

- Never write UICR.APPROTECT. This nRF52840 (AAD0) uses legacy APPROTECT: 0xFF = open, any other value (including 0x5A "HwDisabled") locks the chip.
- Keep `CONFIG_BOARD_ENABLE_DCDC=n` and `CONFIG_BOARD_ENABLE_DCDC_HV=n`. The MG8 has no DC/DC inductors; enabling them browns the chip out in a loop (VDD jumps around ~2.3 V).
- Connecting a J-Link to a stock (Minew firmware) MG8 mass-erases it, even with `-NoGui 1`. Say so and get the user's go-ahead before any J-Link connect on a new unit.
- Never push-pull drive unknown pins. Use pull-ups only; REGOUT0 supplies ~25 mA in total.
- Before writing to UICR, flash or pins, check the chip revision and board config, and tell the user what the step actually does. Report a result only after verifying it (read back, VTref, RTT).

## Working with the hardware

- The board is powered from its own USB plug; USB carries no data. Logs come over SEGGER RTT through the J-Link.
- VTref ~3.0 V steady = healthy with our firmware; ~1.8 V steady = blank chip; jumping ~2.2–2.4 V = DC/DC brown-out loop (see the README recovery section).
- Flash with `nrfjprog -f nrf52 --program build\zephyr\zephyr.hex --sectorerase --verify`; it never auto-erases a locked chip. `nrfjprog --recover` does not work on this chip.
- The production board differs from the FCC test photos (no TXS0108E found). Work from photos and measurements of the actual board, not the FCC photos.
