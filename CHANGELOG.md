# Changelog

All notable changes to the public EVtivity EV Tester (AC / J1772) release.
This project follows [Keep a Changelog](https://keepachangelog.com/) and dates are ISO 8601.

## [Unreleased]

## [v0.1.22] - 2026-09-27

### Added
- Initial open-source release: XIAO ESP32-S3 firmware, the KiCad board design, and
  order-ready JLCPCB Gerbers / BOM / CPL.
- Hardware and assembly guide (`docs/HARDWARE.md`): what to buy, the connector pinout, and
  how to wire a J1772 inlet to the board.
- 3D-printable enclosure (`enclosure/`): STL meshes for the top, mount, and bottom parts.
- User manual (`USER-MANUAL.html`): modes, buttons, LCD, web UI, and MQTT reference.
- Parts-to-buy list (`AC-tester-BOM-amazon.html`) for the modules and hardware JLCPCB does not assemble.
- MQTT console (`mqtt-test.py`): terminal client that sends mode and song commands and shows the
  device's published state. Offers to start a local Mosquitto broker if none is running.

Board revision and firmware version: v0.1.22.
