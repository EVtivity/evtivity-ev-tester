/*
 * Pure J1772 / tester logic - NO Arduino dependencies.
 * Shared by the firmware (ac_tester.ino) and the host unit tests,
 * so the tests exercise the exact code that ships.
 */
#pragma once

enum CpState : unsigned char { CP_A = 0, CP_B, CP_C, CP_D };

// J1772 / IEC 61851 Control Pilot duty (%) -> offered current (A).
// A negative input means "no PWM detected" and returns -1.
inline float dutyToAmps(float duty) {
  if (duty < 0)    return -1.0f;
  if (duty < 10)   return 0.0f;             // <10%: reserved / digital comms, no basic current
  if (duty <= 85)  return duty * 0.6f;      // 6 A .. 51 A
  if (duty <= 96)  return (duty - 64) * 2.5f; // high-power AC
  return 0.0f;                              // >96%: reserved
}

// Proximity pin voltage -> connector status (per the board's R8/R9 divider, verified in SPICE:
// ~1.0 V seated, ~2.0 V release-pressed, ~3.3 V open).
inline const char* ppStatus(float v) {
  if (v < 1.5f) return "Seated";
  if (v < 2.7f) return "Release";
  return "Open";
}

// State -> the three state-select MOSFET gate levels.
// B = ST_B; C = ST_B + ST_C; D = ST_B + ST_D; A = all off.
inline void stateGates(CpState s, bool& stB, bool& stC, bool& stD) {
  stB = (s == CP_B || s == CP_C || s == CP_D);
  stC = (s == CP_C);
  stD = (s == CP_D);
}

// Beep count for a state: A=1, B=2, C=3, D=4.
inline int beepCount(CpState s) { return static_cast<int>(s) + 1; }
