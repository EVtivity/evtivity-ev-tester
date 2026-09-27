# Contributing

Thanks for your interest in the EVtivity EV Tester. Issues and pull requests are welcome.

## Reporting issues

Open a GitHub issue. Helpful details:

- What you were doing (charger model, mode, WiFi on/off).
- For firmware: the serial log (115200 baud) and the Arduino-ESP32 core version.
- For hardware: photos, the board revision, and what you observed.

## Firmware changes

- Target board: **Seeed XIAO ESP32-S3**, Arduino-ESP32 core 3.x.
- Build before you submit:
  ```sh
  arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 firmware/ac_tester
  ```
- Keep the style of the surrounding code: terse comments, no dead code, minimal blast
  radius. Comments explain *why* when it is not obvious, not *what*.
- The pure J1772 logic in `cp_logic.h` has host-side reasoning behind it; keep it clean
  and standard-accurate.

## Hardware changes

- The design is KiCad. Open `jlcpcb/ac-tester-production.kicad_pro`.
- If you change the board, regenerate and include the updated Gerbers
  (`ac-tester-gerbers.zip`), BOM (`ac-tester-bom.csv`), and CPL (`ac-tester-cpl.csv`) so the
  order files stay in sync with the design. Describe the change and why in the PR.
- The tester is a low-voltage J1772 signaling instrument. Do not add anything that draws or
  switches mains or high-voltage DC.

## Pull requests

- One focused change per PR, with a clear description.
- By submitting a contribution you agree to license it under the same terms as this project:
  firmware under Apache-2.0, hardware under CERN-OHL-P-2.0 (see [`LICENSE`](LICENSE)).
- Please confirm your work is your own or properly attributed.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating you
agree to uphold it.
