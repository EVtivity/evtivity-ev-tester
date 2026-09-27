# Hardware and assembly guide

How to build the EVtivity EV Tester and wire it to a charger. If you ordered the board
fully assembled from JLCPCB, skip to [Modules you add](#modules-you-add).

## What you need

The board in `jlcpcb/` is only the carrier PCB and its surface-mount parts. To have a
working tester you also need:

| Item | Notes |
|---|---|
| The carrier PCB | Order it from `jlcpcb/` (see the main README). SMT-assembled is easiest. |
| Seeed XIAO ESP32-S3 | The microcontroller. Plugs into the J5/J6 sockets. |
| U.FL / IPEX antenna | **Required for WiFi** (the XIAO has no onboard antenna). |
| 16x2 I2C LCD (1602, PCF8574 backpack, address 0x27) | The display. Connects to J3. |
| A J1772 inlet (vehicle-side socket) | **This is how it connects to a charger** - see below. |
| 5V power supply, 5.5x2.1 mm center-positive | Or just power the XIAO over USB-C. |
| Connectors, headers, standoffs, hookup wire | See `AC-tester-BOM-amazon.html`. |

The `AC-tester-BOM-amazon.html` file is a complete shopping list for the parts you add
yourself, **except the J1772 inlet**, which you choose based on your connector type (see below).

## Connectors and pinout

| Ref | Connector | Pins | Purpose |
|---|---|---|---|
| **J1** | JST-XH, 3-pin | **CP, PP, GND** | J1772 signal inlet - wire your inlet here |
| **J2** | Barrel jack, 5.5x2.1 mm | 5V, GND | 5 V power in (optional; USB-C also powers it) |
| **J3** | JST-XH, 4-pin | SCL, SDA, 5V, GND | 16x2 I2C LCD |
| **SW1** | 2-pin header | MODE, GND | MODE button (cycle Ready -> Charging -> Suspended) |
| **SW2** | 2-pin header | SELECT, GND | SELECT button (toggle the stats view) |
| **J5 / J6** | 1x7 sockets | - | Seeed XIAO ESP32-S3 (USB-C toward the board's bottom edge) |

Firmware GPIO map (XIAO pads), if you want to modify the firmware:

| Function | XIAO pad | GPIO |
|---|---|---|
| CP pilot sense | D0 | GPIO1 |
| Proximity (analog) | D1 | GPIO2 |
| Fault inject | D2 | GPIO3 |
| State B gate | D3 | GPIO4 |
| I2C SDA (LCD) | D4 | GPIO5 |
| I2C SCL (LCD) | D5 | GPIO6 |
| SELECT button | D7 | GPIO44 |
| State C gate | D8 | GPIO7 |
| Buzzer | D9 | GPIO8 |
| MODE button | D10 | GPIO9 |

## How it connects to a charger (the important part)

This tester emulates the **vehicle** side of J1772, so the charger's connector plugs into a
**J1772 inlet** (the vehicle-side socket). You wire three signals from that inlet to **J1**:

| J1772 inlet pin | Wire to J1 |
|---|---|
| Control Pilot (CP) | **CP** |
| Proximity (PP) | **PP** |
| Protective Earth / Ground (PE) | **GND** |

The inlet's AC power pins (L1, L2/N) are **not connected** - this is a low-voltage signaling
tester and never carries mains current.

On a typical J1772 charge port the signal wires are color-coded:

| Wire (typical) | Signal | Wire to J1 |
|---|---|---|
| Green, 14 AWG | Control Pilot (CP) | CP |
| Orange, 18 AWG | Proximity (PP) | PP |
| Green, 10 AWG | Protective Earth / Ground (PE) | GND |

The remaining heavy wires (white/black, L1/L2 or line/neutral) are the AC power pins - leave
them unconnected. Confirm the exact color/pin mapping against your specific port's own wiring
instructions, then run the three signal wires into the J1 JST-XH housing (use 22-24 AWG for
the JST crimps, spliced to the port's heavier signal wires as needed).

Sourcing the inlet: any Type 1 / SAE J1772 inlet (vehicle socket) works - for example a
Duosida 32 A port or an EV West "HTG" J1772 charge port. If you use the Duosida/EV West port,
EV West's guide "Best practices for wiring the Duosida 32A J1772 Charge Port" (HTG-23-005)
walks through soldering the port pins.

## 3D-printable enclosure

A three-part printed case is in [`../enclosure/`](../enclosure). Print `EV-Tester-case-top.stl`,
`EV-Tester-case-mount.stl`, and `EV-Tester-case-bottom.stl` in PLA or PETG at 0.2 mm layers. The
board mounts on its M3 standoffs, and the LCD, buttons, barrel jack, and J1 cable exit through the
case openings.

## Assembly

1. If you hand-soldered the SMT parts, do those first. Ordering the board SMT-assembled skips
   this entirely.
2. Solder the through-hole connectors: the J5/J6 XIAO sockets, J1 (J1772 signal), J2 (barrel
   jack), J3 (LCD), SW1/SW2 (buttons), and the buzzer.
3. Plug the **XIAO ESP32-S3** into J5/J6 with the USB-C facing the board's bottom edge, and clip
   on the **U.FL antenna**.
4. Mount the **LCD** on the four M2.5 standoffs and connect it to J3 (SCL/SDA/5V/GND, straight
   1:1 cable).
5. Wire your **J1772 inlet** to J1 (CP/PP/GND) as above.
6. Power it: plug into the XIAO's USB-C (also how you flash it), or use a 5 V supply on J2.

## First power-on

- The LCD shows the boot banner and plays a short jingle, then lands on the Ready screen.
- With no charger connected it stays in State A (Ready / Not Connected).
- Plug a charger's connector into your J1772 inlet, then press **MODE** to step
  Ready -> Charging: the charger should detect the "vehicle," start a session, and advertise
  its offered current, which appears on the LCD and web UI.
- To set up WiFi/MQTT, hold **MODE** for 5 s to open the `EVtivity-EV-Tester-XXXX` setup
  access point, join it from your phone, and enter your details. See `USER-MANUAL.html` for the
  full operating reference.
