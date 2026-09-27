/*
 * EVtivity EV Tester for AC J1772 - firmware
 * Board: Seeed XIAO ESP32-S3, socketed on the carrier PCB (jlcpcb/).
 * Framework: Arduino-ESP32 core 3.x.
 *
 * What it does
 *  - Emulates the EV side of a J1772 AC session by switching the state-select resistors with the
 *    on-board MOSFETs. The UI exposes three deliberate modes (no auto-advance), cycled by MODE:
 *    Ready (State A, idle/reset), Charging (handshake in State B then State C), Suspended (back to
 *    State B while plugged = SuspendedEV). (cp_logic.h models the full J1772 standard incl. State D.)
 *  - Reads the Control Pilot PWM (the EVSE's offered current) and the Proximity pin.
 *  - 16x2 I2C LCD shows the mode and offered current/power; the web UI mirrors it.
 *  - MODE cycles Ready->Charging->Suspended (starting Charging needs a live charger). STATS toggles
 *    the stats view. Buzzer plays a distinct tone per mode.
 *  - Fun page in the web UI (/fun) plays full melodies on the buzzer, non-blocking.
 *  - Optional MQTT remote control: <topic>/mode sets the mode, <topic>/song plays songs; publishes
 *    retained <topic>/state and <topic>/availability. Configured on the setup page.
 *
 * Pin map: see docs/HARDWARE.md (these are XIAO GPIO numbers).
 */

#include <Wire.h>
#include "LiquidCrystal_I2C.h"   // vendored in this sketch (see header note)
#include <WiFi.h>
#include "esp_wifi.h"   // esp_wifi_set_max_tx_power (pre-Aug-2025 XIAO ESP32-S3 weak-WiFi fix)
#include "esp_system.h" // esp_reset_reason (boot diagnostics)
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include "cp_logic.h"   // pure J1772 logic, shared with the host unit tests
#include "web_ui.h"     // PAGE_MAIN / PAGE_CONFIG served in station / AP mode

// Uncomment for the Wokwi simulation: readCpDuty() then returns a simulated, ramping duty so the
// offered-amps readout works without EVSE hardware (no pin read, no jumper).
// #define SIM_CP_PWM 1
#ifdef SIM_CP_PWM
int simDutyPct = 10;
#endif

// ---------------------------- Pin map (XIAO GPIO) ----------------------------
constexpr uint8_t PIN_SENSE = 1;   // D0  - CP pilot, clamped 0..3.3V (read as digital for duty)
constexpr uint8_t PIN_PP    = 2;   // D1  - proximity, analog (ADC1)
constexpr uint8_t PIN_FAULT = 3;   // D2  - dedicated FAULT pin (drives Q7 to short NODEX behind D1 -> collapses the positive pilot half -> charger Faults). Strapping pin; R21 holds it low so boot is safe. High = inject fault, low = normal.
constexpr uint8_t PIN_ST_B  = 4;   // D3  - State B gate (Q1)
constexpr uint8_t PIN_SDA   = 5;   // D4  - I2C SDA to LCD
constexpr uint8_t PIN_SCL   = 6;   // D5  - I2C SCL to LCD

// XIAO ESP32-S3 built-in amber LED (GPIO21, active-low). A visible "alive" sign on any power source.
#define LED_ON  LOW
#define LED_OFF HIGH
constexpr uint8_t PIN_BTN_STATS = 44; // D7  - STATS button (active-low, internal pullup)
constexpr uint8_t PIN_ST_C  = 7;   // D8  - State C gate (Q2)
constexpr uint8_t PIN_BUZZ  = 8;   // D9  - buzzer drive (piezo via series R10), LEDC tone
constexpr uint8_t PIN_BTN_MODE = 9;   // D10 - MODE button (active-low, internal pullup)

// ---------------------------- User config ------------------------------------
#define  FW_VERSION "v0.1.22"    // shown on the boot splash; tracks the board rev (BOARD_REV in the board design)
float    LINE_VOLTAGE = 240.0f;  // nominal AC line for the OFFERED-power estimate (set 120 or 240)
const uint8_t LCD_ADDR = 0x27;   // hardcoded; verified on this unit via I2C scan. (0x3F = PCF8574AT variant.)
constexpr int      BUZZ_FREQ    = 4000;  // Hz - piezo drive; tune to the fitted THT piezo resonance (JESSINIE 1407 ~ few kHz)
constexpr uint32_t REFRESH_MS   = 400;   // live-value redraw period
constexpr uint32_t SCROLL_MS    = 700;   // marquee step for LCD lines longer than 16 chars

// ---------------------------- State ------------------------------------------
// CpState and the J1772 logic live in cp_logic.h (shared with the host tests). The UI exposes three
// deliberate modes, each a manual MODE press (no auto-advance), cycling Ready -> Charging -> Suspended:
//   READY   = CP State A (+12V). Idle/reset home; clears the session. Charger sees Available.
//   CHARGE  = handshake in State B (read the offered PWM) then State C. Charger goes Preparing->Charging.
//   SUSPEND = back to State B while plugged: present but not drawing. Charger reports SuspendedEV.
enum UiMode : uint8_t { MODE_READY = 0, MODE_CHARGE = 1, MODE_SUSPEND = 2 };
const char* MODE_DESC[] = { "Ready", "Charging", "Suspended" };  // shown on LCD/web

enum View : uint8_t { VIEW_STATE = 0, VIEW_STATS };

// Note type for the buzzer, defined up here so the Arduino auto-generated prototypes see it.
struct Note { int freq; int ms; };

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

UiMode   mode  = MODE_READY;
View     view  = VIEW_STATE;
bool     pilotPresentNow = false;    // whether a charger pilot is on the line (updated in loop)
uint32_t lastRefresh = 0;

// Charger-offer debounce: pulseIn can miss a PWM period under WiFi load, so raw reads are noisy. Any valid
// read means the charger is offering; it clears only after a few consecutive misses, so neither the main
// label (Charging/Preparing) nor the STATS readout flickers. Reset on each mode change.
bool     offering    = false;
uint8_t  offerMiss   = 0;
float    offeredDuty = -1.0f;
float    offeredAmps = -1.0f;

// button state. STATS = debounced tap. MODE = tap (cycle mode) vs 5s-hold (enter setup AP).
bool     statsDown = false;  uint32_t statsMs = 0;
bool     modeDown = false;  uint32_t modeDownMs = 0;  bool modeHeldFired = false;
bool     faultActive = false;   // pilot-fault injection engaged (both buttons held while actively charging)

// ---------------------------- WiFi / web -------------------------------------
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;
bool        apMode = false;
bool        webUp = false;   // web server started - only true once the TCP/IP stack is up (STA or AP)
uint32_t    saveRebootAt = 0;   // deferred reboot after a settings save (0 = none); performed from loop()
char        AP_SSID[28] = "EVtivity-EV-Tester";    // + a per-device suffix set in setup() (unique per unit)
char        DEFAULT_TOPIC[24] = "evtester";        // per-device MQTT base topic; suffix set in setup(), matches AP_SSID
String      staSsid = "";                          // SSID we are joining, for the info screen
String      staPass = "";                          // stored WiFi password, to re-issue bounded join attempts
volatile uint8_t wifiFailReason = 0;               // last STA disconnect reason (0 = none), for the info screen
constexpr uint8_t  WIFI_MAX_ATTEMPTS = 5;          // bounded join tries, then give up and power the radio down
constexpr uint32_t WIFI_ATTEMPT_MS   = 10000;      // per-attempt window before the next try
uint8_t     wifiAttempt = 0;                        // current attempt (1..MAX; 0 = idle/connected)
uint32_t    wifiAttemptMs = 0;                      // when the current attempt started
bool        wifiGaveUp = false;                     // exhausted the tries; radio off, tester runs offline
bool        wifiWasConnected = false;               // was up at least once (a later drop restarts the tries)
char        lcdL1[17] = "", lcdL2[17] = "";        // current LCD text, mirrored to the web UI
bool        lcdPresent = false;                     // boot self-check: LCD answered on the I2C bus
uint32_t    infoUntil = 0;                          // hold the WiFi-info screen until this millis()

// ---------------------------- Boot diagnostics -------------------------------
// RTC_NOINIT memory survives every reset except a true power-on, so each boot can report why the
// PREVIOUS one ended and how far it got. The USB console cannot answer this: the ROM header is gone
// before the host enumerates, and opening the port resets the chip. bootMagic validates the vars,
// which are garbage on a cold boot.
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootSeq;      // consecutive boots since the last clean power-on
// Stages only advance, so the last one reached localises where a boot died:
//   1 setup entry (PHY init runs here)   2 I2C scanned        3 splash written    4 jingle done
//   5 boot blink done                    6 self-check done    7 in loop           8 startNetwork entered
//   9 settings read                     10 forceap consumed  11 WiFi.mode done   12 WiFi.begin issued
//  13 STA branch done                   14 startNetwork returned                 15 WiFi connected
RTC_NOINIT_ATTR uint32_t bootStage;    // last checkpoint the running boot reached
uint32_t prevStage = 0;                // bootStage as left by the previous boot
int      prevReset = 0;                // esp_reset_reason() for the reset that ended it

// How long the running boot has been alive, restamped at every checkpoint and every loop pass, so the
// next boot can say how long the previous one survived. RTC garbage after a power cut is the tell that
// power was actually lost rather than the chip resetting.
RTC_NOINIT_ATTR uint32_t bootUptime;
#define STAGE(n) do { bootStage = (n); bootUptime = millis(); } while (0)

// Boot log, served at /boot. Kept in RTC memory, never flash: writing it to NVS put a blob write in
// every boot, which is the exact fault this firmware is trying not to cause. A power cut wipes it,
// and that is itself the signal - an empty log means power went away rather than the chip resetting.
constexpr uint8_t BOOT_LOG_N = 16;
struct BootRec { uint8_t reason; uint8_t stage; uint16_t seq; uint32_t upMs; };
RTC_NOINIT_ATTR BootRec  bootRing[BOOT_LOG_N];
RTC_NOINIT_ATTR uint32_t bootTotal;
uint32_t phyInitMs  = 0;   // how long the early PHY bring-up took

// WiFi start is deferred out of setup() by this much. The association burst is the largest current
// step of the whole boot and it landed ~200 ms after the jingle, which is where the board was dying.
// Holding it off lets the supply and the bulk caps recover from the LCD and jingle load first.
constexpr uint32_t NET_START_DELAY_MS = 500;
uint32_t netStartAt = 0;               // deadline for the deferred startNetwork() (0 = already started)


// ---------------------------- Control Pilot ----------------------------------
// Drive the state-select MOSFETs. B = ST_B; C = ST_B+ST_C; D = ST_B+ST_D; A = all off.
void applyState(CpState s) {
  bool b, c, d;
  stateGates(s, b, c, d);            // pure logic from cp_logic.h; d (old State D) unused
  digitalWrite(PIN_ST_B, b ? HIGH : LOW);
  digitalWrite(PIN_ST_C, c ? HIGH : LOW);
  (void)d;                           // FAULT is driven by the injection gesture, not by CP state
}

// Measure the CP PWM duty (%) by timing the clamped SENSE node digitally.
// Returns -1 when no PWM is present (e.g. State A, or the EVSE holds a DC pilot).
float readCpDuty() {
#ifdef SIM_CP_PWM
  return (float)simDutyPct;          // simulated pilot (Wokwi); ramped in loop()
#else
  for (int i = 0; i < 3; i++) {                          // retry: WiFi/interrupt jitter can spoil a read
    unsigned long hi = pulseIn(PIN_SENSE, HIGH, 3000UL);   // > one 1 kHz period; short so no-pilot reads
    unsigned long lo = pulseIn(PIN_SENSE, LOW,  3000UL);   // don't stall the loop / starve the AP portal
    if (hi && lo) return 100.0f * hi / (hi + lo);
  }
  return -1.0f;
#endif
}
// dutyToAmps() and ppStatus() are in cp_logic.h.

// A live charger holds the pilot high: +12V in State A, PWM in B/C. With SENSE pulled down, an
// unplugged or dead line reads low. Sample briefly and report whether any high is present.
bool pilotPresent() {
  for (int i = 0; i < 40; i++) {          // ~4 ms
    if (digitalRead(PIN_SENSE) == HIGH) return true;
    delayMicroseconds(100);
  }
  return false;
}

// ---------------------------- Proximity --------------------------------------
float readPpVolts() {
  return analogRead(PIN_PP) * 3.3f / 4095.0f;
}

// ---------------------------- Buzzer -----------------------------------------
// Play a sequence of notes. freq 0 = rest. A passive transducer plays the pitch you drive,
// so these are real notes; pitched into the 0.8-1.6 kHz range to stay audible on the piezo.
void playMelody(const Note* m, int n) {
  stopSong();                               // a UI tone always wins over a running fun song
  for (int i = 0; i < n; i++) {
    if (m[i].freq > 0) ledcWriteTone(PIN_BUZZ, m[i].freq);
    else               ledcWriteTone(PIN_BUZZ, 0);
    delay(m[i].ms);
    ledcWriteTone(PIN_BUZZ, 0);
    delay(18);                              // brief gap so repeated notes are distinct
  }
}

// Power-on "Charge!" fanfare.
const Note STARTUP_JINGLE[] = {
  { 784, 110 },   // G5
  {1047, 110 },   // C6
  {1319, 110 },   // E6
  {1568, 240 },   // G6 (held)
  {1319, 110 },   // E6
  {1568, 380 },   // G6  ("Char-GE!")
};

// Per-mode signature played on each transition, plus a distinct error tone. Each has its own contour.
const Note TONE_READY[]   = { {880,200} };                                 // Ready: single tone
const Note TONE_SUSPEND[] = { {1047,130}, {784,230} };                     // Suspended: two-note fall
const Note TONE_CHARGE[]  = { {1047,110}, {1319,110}, {1568,300} };        // Charging: rising triad
const Note TONE_ERR[]     = { {440,110}, {294,300} };                      // Error: low fall (rejected)
const Note TONE_WIFI_OK[] = { {1568,90}, {2093,170} };                     // WiFi connected: short rising chime
const Note TONE_AP_ENTER[]= { {1047,90}, {1568,90}, {2093,220} };          // Enter setup AP: rising three-note
const Note TONE_AP_EXIT[] = { {2093,90}, {1568,90}, {1047,220} };          // Exit setup AP: falling three-note

// ---------------------------- Fun songs (web UI /fun) ------------------------
// Full melodies, stepped non-blocking from loop() (serviceSong) so the web server and buttons stay
// live for the whole song. Pitched an octave up for the piezo, like the UI tones.

// Happy Birthday, all four phrases (quarter = 500 ms).
const Note SONG_BIRTHDAY[] = {
  {784,375},{784,125},{880,500},{784,500},{1047,500},{988,1000},              // Happy birthday to you
  {784,375},{784,125},{880,500},{784,500},{1175,500},{1047,1000},             // Happy birthday to you
  {784,375},{784,125},{1568,500},{1319,500},{1047,500},{988,500},{880,750},   // Happy birthday dear ...
  {1397,375},{1397,125},{1319,500},{1047,500},{1175,500},{1047,1500},         // Happy birthday to you
};

// We Wish You a Merry Christmas: full verse plus both halves of the "Good tidings" chorus
// (quarter = 420 ms).
const Note SONG_XMAS[] = {
  {784,420},                                                     // We
  {1047,420},{1047,210},{1175,210},{1047,210},{988,210},         // wish you a merry
  {880,420},{880,420},{880,420},                                 // Christmas, we
  {1175,420},{1175,210},{1319,210},{1175,210},{1047,210},        // wish you a merry
  {988,420},{988,420},{988,420},                                 // Christmas, we
  {1319,420},{1319,210},{1397,210},{1319,210},{1175,210},        // wish you a merry
  {1047,420},{880,420},{784,210},{784,210},                      // Christmas, and a
  {880,420},{1175,420},{988,420},                                // happy new
  {1047,840},                                                    // year
  {784,420},                                                     // Good
  {1047,420},{1047,420},{1047,420},                              // tidings we
  {988,840},{988,420},                                           // bring, to
  {1047,420},{988,420},{880,420},                                // you and your
  {784,840},                                                     // kin
  {1175,420},{1319,420},                                         // Good ti-
  {1175,210},{1175,210},{1047,210},{1047,210},                   // dings for Christmas
  {1568,420},{784,420},{784,210},{784,210},                      // and a
  {880,420},{1175,420},{988,420},                                // happy new
  {1047,840},                                                    // year
};

// Pachelbel's Canon in D, single pass of the solo-violin melody, generated from a verified MIDI
// transcription (+1 octave for the piezo, quarter = 900 ms). Every section exactly once, no
// ground-bass intro/outro arpeggios, no section repeats: half-note theme, eighth-note variation,
// sixteenth-note runs, syncopated section, lyrical section with C naturals, high finale, held D.
const Note SONG_CANON[] = {
  {1480,1800},{1319,1800},{1175,1800},{1109,1800},{988,1800},{880,1800},  // theme
  {988,1800},{1109,1800},{1175,450},{1109,450},{1175,450},{740,450},      // eighth variation
  {880,900},{1109,900},{1175,900},{1480,900},{1760,1350},{1976,450},
  {1568,450},{1480,450},{1319,450},{1568,450},{1480,450},{1319,450},
  {1175,450},{1109,450},{988,450},{880,450},{784,450},{740,450},
  {659,900},{1109,900},{1175,450},{1480,225},{1568,225},{1760,450},       // sixteenth runs
  {1480,225},{1568,225},{1760,225},{880,225},{988,225},{1109,225},
  {1175,225},{1319,225},{1480,225},{1568,225},{1480,450},{1175,225},
  {1319,225},{1480,450},{740,225},{784,225},{880,225},{988,225},
  {880,225},{784,225},{880,225},{740,225},{784,225},{880,225},
  {784,450},{988,225},{880,225},{784,450},{740,225},{659,225},
  {740,225},{659,225},{587,225},{659,225},{740,225},{784,225},
  {880,225},{988,225},{784,450},{988,225},{880,225},{988,450},
  {1109,225},{1175,225},{1109,225},{988,225},{1109,225},{1175,225},
  {1319,225},{1480,225},{1568,225},{1319,225},{1480,450},{1175,225},      // syncopated
  {1109,225},{1175,450},{740,450},{880,450},{1109,225},{1175,225},
  {1319,450},{1109,450},{988,450},{1175,225},{1319,225},{1480,450},
  {1175,450},{1480,450},{1480,225},{1319,225},{1175,450},{1109,450},
  {988,450},{988,225},{880,225},{988,450},{1109,450},{1175,450},
  {1480,225},{1319,225},{1175,450},{1760,450},{1568,450},{1175,225},
  {1109,225},{988,450},{988,450},{880,450},{880,225},{784,225},
  {740,450},{659,450},{1175,1350},{1480,450},{1480,450},{1568,450},       // lyrical
  {1480,450},{1319,450},{1175,1350},{1175,450},{1175,450},{1319,450},
  {1175,450},{1109,450},{988,1800},{1175,1800},{1175,450},{1047,450},
  {988,450},{1047,450},{880,1800},{1175,900},{1480,450},{1760,450},
  {1760,450},{1976,450},{1760,450},{1568,450},{1480,1350},{1480,450},
  {1480,450},{1568,450},{1480,450},{1319,450},{1175,450},{1047,450},
  {988,450},{1047,450},{1175,1800},{1175,450},{1047,450},{988,450},
  {1047,450},{1109,1350},{1109,450},{2349,1800},{2217,900},{1976,1800},   // high finale
  {1760,1800},{1976,1800},{2217,1800},{1175,2700},                        // held final D6
};

// Non-blocking player state. songNotes != nullptr means a song is running.
const Note* songNotes  = nullptr;
int         songLen    = 0;
int         songIdx    = 0;
bool        songGap    = false;     // inside the 18 ms inter-note gap
uint32_t    songNextMs = 0;         // millis() at which the current note/gap ends

void stopSong() {
  if (!songNotes) return;
  songNotes = nullptr;
  ledcWriteTone(PIN_BUZZ, 0);
}

void startSong(const Note* m, int n, const char* name) {
  songNotes = m; songLen = n; songIdx = -1;
  songGap = true; songNextMs = millis();    // first serviceSong() call advances to note 0
  setLines(name, "Playing...");
}

// Advance the running song by at most one note/gap edge per call. Called every loop() pass.
void serviceSong() {
  if (!songNotes || (int32_t)(millis() - songNextMs) < 0) return;
  if (!songGap) {                           // note finished -> silent gap so repeated notes separate
    ledcWriteTone(PIN_BUZZ, 0);
    songGap = true; songNextMs = millis() + 18;
    return;
  }
  if (++songIdx >= songLen) { songNotes = nullptr; redraw(); return; }
  songGap = false;
  ledcWriteTone(PIN_BUZZ, songNotes[songIdx].freq > 0 ? songNotes[songIdx].freq : 0);
  songNextMs = millis() + songNotes[songIdx].ms;
}

// ---------------------------- Display ----------------------------------------
// setLines() writes both rows to the LCD (padded to 16) and caches them in lcdL1/lcdL2 so the web
// UI mirrors exactly what the panel shows.
// Write one 16-char LCD row; a line longer than 16 chars marquee-scrolls (one char per SCROLL_MS,
// looping with a 3-space gap). Scrolling advances only for rows redrawn each refresh (the Ready line).
void writeLcdRow(uint8_t row, const char* s) {
  static uint32_t startMs[2] = {0, 0};
  static char     prev[2][48] = {{0}, {0}};
  char* mirror = row ? lcdL2 : lcdL1;
  size_t n = strlen(s);
  if (strncmp(prev[row], s, sizeof(prev[row]) - 1) != 0) {   // text changed -> restart the marquee at the start
    strncpy(prev[row], s, sizeof(prev[row]) - 1); prev[row][sizeof(prev[row]) - 1] = 0;
    startMs[row] = millis();
  }
  char win[17];
  if (n <= 16) {
    snprintf(win, sizeof(win), "%-16.16s", s);
  } else {
    size_t period = n + 3;                                   // text + 3-space gap before it loops
    uint32_t elapsed = millis() - startMs[row];
    size_t off = (elapsed < 1000) ? 0 : ((elapsed - 1000) / SCROLL_MS) % period;  // hold the start 1s, then scroll
    for (size_t i = 0; i < 16; i++) { size_t idx = (off + i) % period; win[i] = idx < n ? s[idx] : ' '; }
    win[16] = 0;
  }
  memcpy(mirror, win, sizeof(win));                          // win is always NUL-terminated at [16]
  if (lcdPresent) { lcd.setCursor(0, row); lcd.print(win); }  // headless/muted: mirror still updates for the web UI
}
void setLines(const char* a, const char* b) {
  writeLcdRow(0, a);
  writeLcdRow(1, b);
}

// Sample the charger's PWM offer with debounce (updates the offering / offeredDuty / offeredAmps globals).
void pollOffer() {
  float d = readCpDuty();                              // -1 when no PWM is present
  if (d >= 0) { offering = true; offerMiss = 0; offeredDuty = d; offeredAmps = dutyToAmps(d); }
  else if (offering && ++offerMiss >= 3) { offering = false; offeredDuty = -1.0f; offeredAmps = -1.0f; }
}

void drawState() {
  if (faultActive) { setLines("* FAULT INJECT *", "pilot collapsed"); return; }   // held gesture: show it
  char b1[24], b2[48] = "";
  if (mode != MODE_READY) pollOffer();                 // refresh the debounced charger-offer state
  if (mode == MODE_CHARGE)                             // Mode 2 label follows the charger's offer:
    snprintf(b1, sizeof(b1), "%s", offering ? "Charging" : "Preparing");  // PWM present = Charging, else Preparing
  else
    snprintf(b1, sizeof(b1), "%s", MODE_DESC[mode]);   // Ready / Suspended
  if (mode != MODE_READY) {                            // Charging/Suspended: show the charger's offered current
    if (offeredAmps < 0) snprintf(b2, sizeof(b2), "Offered: --");
    else                 snprintf(b2, sizeof(b2), "Offered: %.1fA", offeredAmps);
  } else {                                             // Ready: WiFi / web-UI status on line 2 (scrolls if long)
    if (WiFi.status() == WL_CONNECTED)      snprintf(b2, sizeof(b2), "Web UI: %s", WiFi.localIP().toString().c_str());
    else if (!staSsid.length())             snprintf(b2, sizeof(b2), "No WiFi - hold MODE 5s to set up");
    else if (wifiGaveUp)                    snprintf(b2, sizeof(b2), "WiFi failed - hold MODE 5s to reconfigure");
    else                                    snprintf(b2, sizeof(b2), "WiFi connecting %u/%u...", wifiAttempt, WIFI_MAX_ATTEMPTS);
  }
  setLines(b1, b2);
}

void drawStats() {
  if (faultActive) { setLines("* FAULT INJECT *", "pilot collapsed"); return; }   // keep the fault visible on the stats view too
  pollOffer();                                         // shared debounce, same source as the main view
  float pp = readPpVolts();
  char b1[24], b2[24];
  if (offeredDuty < 0) snprintf(b1, sizeof(b1), "PWM: none");
  else                 snprintf(b1, sizeof(b1), "Duty %.0f%% %.1fA", offeredDuty, offeredAmps);
  if (offeredAmps > 0) snprintf(b2, sizeof(b2), "%.1fkW PP %s", offeredAmps * LINE_VOLTAGE / 1000.0f, ppStatus(pp));
  else                 snprintf(b2, sizeof(b2), "PP %s", ppStatus(pp));
  setLines(b1, b2);
}

// AP-screen second line: the join network and the address to browse to (scrolls to fit 16x2).
void apJoinLine(char* buf, size_t n) {
  snprintf(buf, n, "%s @ %s", AP_SSID, WiFi.softAPIP().toString().c_str());
}
void redraw() {
  if (apMode) { char l2[48]; apJoinLine(l2, sizeof(l2)); setLines("Setup: join AP", l2); return; }
  if (view == VIEW_STATE) drawState();
  else                    drawStats();
}

// ---------------------------- Actions (shared by panel + web) ----------------
// Enter a UI mode: drive the pilot, update the LCD, play the mode's tone. Each mode maps to the CP
// states directly. Charging is the combined B+C step: it handshakes in State B so the EVSE starts its
// PWM, then drops to State C to draw. Suspended holds State B (present, not drawing = SuspendedEV).
void enterMode(UiMode m) {
  if (faultActive) { faultActive = false; digitalWrite(PIN_FAULT, LOW); }   // a mode change clears any injected fault
  mode = m;
  view = VIEW_STATE;
  offering = false; offerMiss = 0; offeredAmps = -1.0f; offeredDuty = -1.0f;   // reset the offer debounce
  const Note* tone = TONE_READY; int toneLen = 1;   // Ready: single tone (default)
  switch (m) {
    case MODE_READY:                            // State A: idle home; clears the session
      applyState(CP_A);
      break;
    case MODE_CHARGE:                           // combined B+C: handshake in State B, then draw in State C
      applyState(CP_B);                         // present as connected so the EVSE starts its PWM
      redraw();                                 // Mode 2 label (Preparing until the charger offers PWM)
      delay(900);                               // let the charger react and offer the pilot PWM
      applyState(CP_C);                         // close the charge circuit: draw current
      tone = TONE_CHARGE; toneLen = 3;          // rising triad
      break;
    case MODE_SUSPEND:                          // State B while plugged: present, not drawing = SuspendedEV
      applyState(CP_B);
      tone = TONE_SUSPEND; toneLen = 2;         // two-note fall
      break;
  }
  redraw();
  playMelody(tone, toneLen);
  mqttPublishState();
  Serial.printf("Mode %s\n", MODE_DESC[mode]);
}
// Tried to start charging with no charger on the line: brief error, no mode change.
void showNoChargerError() {
  setLines("No charger", "Plug in first");
  infoUntil = millis() + 2000;
  playMelody(TONE_ERR, sizeof(TONE_ERR) / sizeof(TONE_ERR[0]));
}
// Tried to inject a fault while not actively charging (Ready or Preparing): the EVSE only reports a
// pilot fault once it is driving PWM, so a fault here does nothing. Brief error, no state change.
void showNotChargingError() {
  setLines("Charge first", "to inject fault");
  infoUntil = millis() + 2000;
  playMelody(TONE_ERR, sizeof(TONE_ERR) / sizeof(TONE_ERR[0]));
}
void doModePress() {
  if (faultActive) { setFault(false); return; }          // a MODE tap first clears a latched (MQTT) fault
  UiMode next = static_cast<UiMode>((mode + 1) % 3);      // Ready -> Charging -> Suspended -> Ready
  if (next == MODE_CHARGE && !pilotPresentNow) { showNoChargerError(); return; }  // charging needs a live charger
  enterMode(next);
}
void doStatsPress() {
  stopSong();                                                // using the tester ends the fun song
  if (mode == MODE_READY) { showNoChargerError(); return; }  // stats only mean something once in a session
  view = (view == VIEW_STATE) ? VIEW_STATS : VIEW_STATE;
  redraw();
  Serial.printf("View: %s\n", view == VIEW_STATS ? "STATS" : "STATE");
}

// ---------------------------- WiFi + web server ------------------------------
String authUser = "admin", authPass = "admin";    // web-UI login, changeable, defaults admin/admin

// Station-mode UI/control endpoints need auth. AP setup is left open (physical-access gated).
bool needAuth() {
  if (apMode) return false;
  if (server.authenticate(authUser.c_str(), authPass.c_str())) return false;
  server.requestAuthentication();
  return true;
}

// Flash writes and a live WiFi MAC do not mix on the Arduino core. Its prebuilt WiFi libraries are
// built with ESP_WIFI_IRAM_OPT and ESP_WIFI_RX_IRAM_OPT off (ESP-IDF enables both by default), so the
// driver's interrupt path executes from flash. A flash write disables the instruction cache; if the
// MAC raises an interrupt in that window the handler fetches from dead cache and faults, and the
// interrupt watchdog resets the chip. Every settings write here reboots anyway, so stop the radio
// first and the window never opens.
static void stopWifiForFlash() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  delay(50);
}


void handleRoot()   { if (needAuth()) return; server.send(200, "text/html", apMode ? PAGE_CONFIG : PAGE_MAIN); }
void handleSetup()  {
  if (needAuth()) return;
  if (!apMode) { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); return; }  // config only in AP mode
  server.send(200, "text/html", PAGE_CONFIG);
}
// Escape a user string for embedding inside a JSON string literal (quotes and backslashes).
String jsonEsc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) { char c = s[i]; if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o;
}
// Boot history since the supply last dropped. reason is esp_reset_reason_t (1 = power-on, 9 = brownout,
// 11 = USB host reset); stage is how far the boot BEFORE that entry got (see the bootStage note).
void handleBoot() {
  if (needAuth()) return;
  String j = "{\"total\":" + String(bootTotal) + ",\"seq\":" + String(bootSeq) +
              ",\"phyInitMs\":" + String(phyInitMs) + ",\"boots\":[";
  uint32_t n = bootTotal < BOOT_LOG_N ? bootTotal : BOOT_LOG_N;
  uint32_t first = bootTotal - n;
  for (uint32_t i = 0; i < n; i++) {
    const BootRec& r = bootRing[(first + i) % BOOT_LOG_N];
    if (i) j += ",";
    j += "{\"n\":" + String(first + i + 1) + ",\"reason\":" + String(r.reason) +
         ",\"stage\":" + String(r.stage) + ",\"seq\":" + String(r.seq) +
         ",\"prevUpMs\":" + String(r.upMs) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// Force n association bursts, 1.5 s apart. If the supply cannot carry the burst the board resets and
// the boot log names the reason; if it survives, /boot shows the same count it started with.
// write a shipped boot can do, without touching any user setting.

void handleStatus() {
  if (needAuth()) return;
  char j[140];
  snprintf(j, sizeof(j), "{\"l1\":\"%s\",\"l2\":\"%s\",\"ssid\":\"%s\"}", lcdL1, lcdL2, AP_SSID);
  server.send(200, "application/json", j);
}
void handlePress() {
  if (needAuth()) return;
  if (apMode) { server.send(200, "text/plain", "setup"); return; }   // tester controls inert in setup mode, like the panel buttons
  String b = server.arg("btn");
  if (b == "mode")       doModePress();
  else if (b == "stats") doStatsPress();
  server.send(200, "text/plain", "ok");
}
void handleFun() { if (needAuth()) return; server.send(200, "text/html", PAGE_FUN); }
// Play or stop a fun song by name ("hbd"/"xmas"/"canon"/"stop"); true if it matched. Shared by /play and MQTT.
bool playNamedSong(const String& s) {
  if      (s == "hbd")   startSong(SONG_BIRTHDAY, sizeof(SONG_BIRTHDAY) / sizeof(SONG_BIRTHDAY[0]), "Happy Birthday");
  else if (s == "xmas")  startSong(SONG_XMAS, sizeof(SONG_XMAS) / sizeof(SONG_XMAS[0]), "Merry Christmas");
  else if (s == "canon") startSong(SONG_CANON, sizeof(SONG_CANON) / sizeof(SONG_CANON[0]), "Canon in D");
  else if (s == "stop")  { stopSong(); redraw(); }
  else return false;
  return true;
}
void handlePlay() {
  if (needAuth()) return;
  playNamedSong(server.arg("song"));
  server.send(200, "text/plain", "ok");
}
void handleSave() {
  if (needAuth()) return;
  // Read every field BEFORE responding: server.arg() needs the live request. Then answer the browser,
  // drop the radio, and only then touch flash - a flash write with the WiFi MAC live is what resets
  // this chip (stopWifiForFlash).
  String ssid = server.arg("ssid"), wpass = server.arg("pass");
  String au = server.arg("auser"), ap = server.arg("apass");
  String mh = server.arg("mhost"); mh.trim();
  String mt = server.arg("mtopic"); mt.trim();
  String mu = server.arg("muser"), mpw = server.arg("mpass");
  long   mp = server.arg("mport").toInt();

  server.send(200, "text/html",
    "<meta http-equiv='refresh' content='6;url=/'>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='margin:0;min-height:100vh;display:flex;flex-direction:column;align-items:center;"
    "justify-content:center;gap:10px;font-family:-apple-system,Segoe UI,Roboto,sans-serif;color:#e5e7eb;"
    "background:#0b1220;padding:28px;text-align:center'>"
    "<div style='font-size:21px;font-weight:700;color:#22c55e'>Saved</div>"
    "<div style='color:#8ea0bd'>Connecting to WiFi&hellip;</div></body>");
  delay(250);                        // let the response reach the browser
  stopWifiForFlash();

  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  if (wpass.length()) prefs.putString("pass", wpass);          // blank = keep the current password
  prefs.end();
  if (au.length() || ap.length()) {
    prefs.begin("auth", false);
    if (au.length()) prefs.putString("user", au);
    if (ap.length()) prefs.putString("pass", ap);
    prefs.end();
  }
  prefs.begin("mqtt", false);
  prefs.putString("host", mh);                                 // blank broker = MQTT off
  prefs.putUShort("port", (mp > 0 && mp < 65536) ? (uint16_t)mp : 1883);
  prefs.putString("user", mu);
  if (mpw.length()) prefs.putString("pass", mpw);              // blank = keep the current broker password
  prefs.putString("topic", mt.length() ? mt : DEFAULT_TOPIC);
  prefs.end();

  saveRebootAt = millis() + 300;     // reboot from loop() so the NVS commit finishes first
  if (!saveRebootAt) saveRebootAt = 1;
}
// Set the one-shot forceap flag and reboot so the setup AP comes up via the clean boot path (a live
// STA->AP switch leaves the DHCP server broken). Shared by the 5 s MODE hold and the /enterap route.
void rebootIntoAP() {
  stopWifiForFlash();
  prefs.begin("sys", false); prefs.putBool("forceap", true); prefs.end();
  delay(300);
  ESP.restart();
}
void handleReset() {  // forget WiFi + auth + MQTT + WiFi-driver config, back to setup AP with defaults
  if (needAuth()) return;
  server.send(200, "text/plain", "ok");
  delay(200);                       // let the response flush before the radio goes down
  stopWifiForFlash();
  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  prefs.begin("auth", false); prefs.clear(); prefs.end();
  prefs.begin("mqtt", false); prefs.clear(); prefs.end();
  prefs.begin("nvs.net80211", false); prefs.clear(); prefs.end();   // also wipe the WiFi driver's own persisted config; a stale bssid/opmode there can wedge STA joins
  prefs.begin("sys", false); prefs.putBool("forceap", true); prefs.end();
  delay(300);
  ESP.restart();   // reopen the setup AP with defaults (matches the "restart in setup mode" prompt)
}
void handleEnterAP() {  // reboot into the setup AP (web equivalent of the 5 s MODE hold), keeping all settings
  if (needAuth()) return;
  char page[420];
  snprintf(page, sizeof(page),
    "<meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;"
    "background:#0b1220;color:#e5e7eb;padding:36px;max-width:30rem;margin:auto'>"
    "<h2 style='color:#22c55e'>Opening setup mode</h2><p>The tester is rebooting into the "
    "<b>%s</b> setup network. Join it and browse to <b>192.168.4.1</b> to configure WiFi.</p></body>", AP_SSID);
  server.send(200, "text/html", page);
  rebootIntoAP();
}

// ---------------------------- MQTT -------------------------------------------
// Optional remote control, configured on the setup page (blank broker = off). Subscribes to
// Modes on <topic>/mode (ready | charge [charging, preparing] | suspend [suspended]); songs on
// <topic>/song (hbd | xmas | canon | stop). fault engages pilot-fault injection. Publishes retained
// <topic>/state (ready|charging|suspended|faulted) and <topic>/availability (LWT online/offline).
WiFiClient   mqttSock;
PubSubClient mqtt(mqttSock);
String       mqttHost = "";
uint16_t     mqttPort = 1883;
String       mqttUser = "", mqttPass = "";
String       mqttTopic = "evtester";
uint32_t     mqttLastTry = 0;

void mqttPublishState() {
  static const char* NAMES[] = { "ready", "charging", "suspended" };
  if (mqtt.connected()) mqtt.publish((mqttTopic + "/state").c_str(), faultActive ? "faulted" : NAMES[mode], true);
}
void setFault(bool on) {   // engage/clear pilot-fault injection; reflect it on the LCD and publish the state
  if (faultActive == on) return;
  faultActive = on;
  digitalWrite(PIN_FAULT, on ? HIGH : LOW);
  redraw();   // view-aware: reflect the fault on whichever view is showing
  mqttPublishState();
}

// Serve the MQTT config the web panel shows so a user knows how to drive the tester over MQTT.
void handleMqttInfo() {
  if (needAuth()) return;
  char j[400];
  snprintf(j, sizeof(j), "{\"on\":%s,\"connected\":%s,\"host\":\"%s\",\"port\":%u,\"topic\":\"%s\"}",
           mqttHost.length() ? "true" : "false", mqtt.connected() ? "true" : "false",
           jsonEsc(mqttHost).c_str(), mqttPort, jsonEsc(mqttTopic).c_str());
  server.send(200, "application/json", j);
}
// Current non-secret settings so the setup form can pre-fill (you needn't re-enter configured WiFi).
void handleCfg() {
  if (needAuth()) return;
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", ""); String pw = prefs.getString("pass", "");
  prefs.end();
  char j[600];
  snprintf(j, sizeof(j), "{\"ssid\":\"%s\",\"haspw\":%s,\"auser\":\"%s\",\"mhost\":\"%s\",\"mport\":%u,\"muser\":\"%s\",\"mhaspw\":%s,\"mtopic\":\"%s\"}",
           jsonEsc(ssid).c_str(), pw.length() ? "true" : "false", jsonEsc(authUser).c_str(),
           jsonEsc(mqttHost).c_str(), mqttPort, jsonEsc(mqttUser).c_str(), mqttPass.length() ? "true" : "false", jsonEsc(mqttTopic).c_str());
  server.send(200, "application/json", j);
}
void handleExitAP() {   // leave setup mode without saving; forceap was consumed at boot, so a plain reboot exits
  if (needAuth()) return;
  server.send(200, "text/html", "<meta http-equiv='refresh' content='2;url=/'><body style='font-family:sans-serif;"
    "background:#0b1220;color:#e5e7eb;padding:36px'>Leaving setup, rebooting...</body>");
  delay(300);
  ESP.restart();
}

void mqttOnMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)tolower(payload[i]);
  msg.trim();
  Serial.printf("MQTT: %s '%s'\n", topic, msg.c_str());
  if (String(topic).endsWith("/song")) { playNamedSong(msg); return; }   // songs: hbd/xmas/canon/stop
  // modes on <topic>/mode:
  if (msg == "ready") enterMode(MODE_READY);
  else if (msg == "charge" || msg == "charging" || msg == "preparing") {
    if (pilotPresentNow) enterMode(MODE_CHARGE);   // same guard as the panel button
    else showNoChargerError();
  }
  else if (msg == "suspend" || msg == "suspended") enterMode(MODE_SUSPEND);
  else if (msg == "fault") {                    // inject a pilot fault (cleared by any other mode command)
    if (offering) setFault(true);               // only meaningful while the EVSE is actively charging (PWM present)
    else showNotChargingError();
  }
}

// Keep the broker session alive; reconnect with a 15 s backoff. The 1.5 s socket connect timeout
// caps how long a down broker can stall the loop per attempt.
void serviceMqtt() {
  if (!mqttHost.length() || apMode || WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (mqttLastTry && millis() - mqttLastTry < 15000) return;
  mqttLastTry = millis();
  char cid[24];
  snprintf(cid, sizeof(cid), "evtester-%06X", (unsigned)(ESP.getEfuseMac() >> 24) & 0xFFFFFF);
  String avail = mqttTopic + "/availability";
  if (!mqtt.connect(cid, mqttUser.length() ? mqttUser.c_str() : nullptr,
                    mqttUser.length() ? mqttPass.c_str() : nullptr,
                    avail.c_str(), 0, true, "offline")) {
    Serial.printf("MQTT: connect to %s:%u failed, state %d\n", mqttHost.c_str(), mqttPort, mqtt.state());
    return;
  }
  mqtt.publish(avail.c_str(), "online", true);
  mqtt.subscribe((mqttTopic + "/mode").c_str());     // mode commands
  mqtt.subscribe((mqttTopic + "/song").c_str());    // song commands
  mqttPublishState();
  Serial.printf("MQTT: connected to %s:%u, modes on %s/mode, songs on %s/song\n",
                mqttHost.c_str(), mqttPort, mqttTopic.c_str(), mqttTopic.c_str());
}

// Drive the WiFi join with bounded, spaced retries instead of the SDK's endless auto-reconnect (whose
// constant association churn is what occasionally wedges the driver into a watchdog reboot). Up to
// WIFI_MAX_ATTEMPTS tries, WIFI_ATTEMPT_MS apart; then give up, power the radio off, and let the Ready
// screen prompt a reconfigure. A drop after a successful join restarts the cycle.
void serviceWifi() {
  if (apMode || !staSsid.length() || wifiGaveUp) return;
  if (WiFi.status() == WL_CONNECTED) { wifiWasConnected = true; wifiAttempt = 0; return; }
  if (wifiAttempt == 0) {                                // was connected, link dropped -> restart the tries
    if (!wifiWasConnected) return;
    wifiWasConnected = false; wifiAttempt = 1; wifiAttemptMs = millis();
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    Serial.printf("WiFi: link dropped, retrying 1/%u\n", WIFI_MAX_ATTEMPTS);
    return;
  }
  if (millis() - wifiAttemptMs < WIFI_ATTEMPT_MS) return;   // this attempt still has time to connect
  if (wifiAttempt >= WIFI_MAX_ATTEMPTS) {                   // out of tries -> give up, radio off
    wifiGaveUp = true;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    Serial.printf("WiFi: gave up after %u attempts (last reason %u) - running as tester. Hold MODE 5s to reconfigure.\n",
                  WIFI_MAX_ATTEMPTS, wifiFailReason);
    if (!songNotes) redraw();
    return;
  }
  wifiAttempt++;
  WiFi.begin(staSsid.c_str(), staPass.c_str());            // re-issue the join for the next attempt
  wifiAttemptMs = millis();
  Serial.printf("WiFi: attempt %u/%u\n", wifiAttempt, WIFI_MAX_ATTEMPTS);
}

// Pre-Aug-2025 XIAO ESP32-S3 units have marginal RF at full TX power (weak/distorted signal, reason-2
// auth failures even with the antenna). Seeed's fix: cap max TX power to 15 dBm so the PA stays linear.
// Harmless on newer boards. Applied to both the STA join and the setup AP.
static void capWifiTxPower() {
  esp_wifi_set_max_tx_power(60);                 // 60 = 15 dBm (0.25 dBm units)
  int8_t txp = 0; esp_wifi_get_max_tx_power(&txp);
  Serial.printf("WiFi: max TX power capped at %.2f dBm\n", txp * 0.25f);
}

// Open setup AP (192.168.4.1, captive DNS) and show it on the LCD.
void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);                               // default 192.168.4.1 (matches the proven bare AP test)
  capWifiTxPower();                                   // same weak-RF workaround as the STA path
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("WiFi: setup AP %s  http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  showWifiInfo();
}

// WiFi is optional and never blocks boot. AP mode is entered only by holding MODE 5 s at runtime
// (sets the forceap flag and reboots here); otherwise join saved WiFi, else run offline as the tester.
void startNetwork() {
  STAGE(9);
  WiFi.persistent(false);   // never let the WiFi driver write NVS (AP or STA): NVS-flash-during-association faults the other core mid-compaction
  prefs.begin("auth", true);
  authUser = prefs.getString("user", "admin");
  authPass = prefs.getString("pass", "admin");
  prefs.end();

  prefs.begin("wifi", true);
  staSsid     = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  prefs.begin("mqtt", true);
  mqttHost  = prefs.getString("host", "");
  mqttPort  = prefs.getUShort("port", 1883);
  mqttUser  = prefs.getString("user", "");
  mqttPass  = prefs.getString("pass", "");
  mqttTopic = prefs.getString("topic", DEFAULT_TOPIC);
  prefs.end();
  if (mqttHost.length()) {
    mqttSock.setConnectionTimeout(1500);
    mqtt.setServer(mqttHost.c_str(), mqttPort);
    mqtt.setCallback(mqttOnMessage);
  }

  // AP is entered by holding MODE 5 s at runtime, which sets this one-shot flag and reboots to here.
  // Otherwise: join saved WiFi if configured, else run as the tester with WiFi off. No auto-AP fallback.
  STAGE(10);
  prefs.begin("sys", false);
  bool forceAP = prefs.getBool("forceap", false);
  if (forceAP) prefs.putBool("forceap", false);   // consume the request so the next boot is normal
  prefs.end();

  if (forceAP) {
    startAP();
    webUp = true;
  } else if (staSsid.length()) {
    apMode = false;
    staPass = pass;
    STAGE(11);
    WiFi.mode(WIFI_STA);   // no-op: the PHY was already brought up at the top of setup()
    capWifiTxPower();                               // pre-Aug-2025 XIAO ESP32-S3 weak-RF workaround
    WiFi.setSleep(false);                          // modem power-save off
    WiFi.setAutoReconnect(false);                  // we drive the retries (bounded) from serviceWifi()
    WiFi.setHostname("evtester");                     // unique DHCP name in the router client list
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {   // keep the last failure reason for the info screen
      wifiFailReason = info.wifi_sta_disconnected.reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    STAGE(12);
    WiFi.begin(staSsid.c_str(), staPass.c_str());  // non-blocking; connects in the background
    STAGE(13);
    wifiAttempt = 1; wifiAttemptMs = millis();
    webUp = true;
    Serial.printf("WiFi: joining %s (login %s), attempt 1/%u\n", staSsid.c_str(), authUser.c_str(), WIFI_MAX_ATTEMPTS);
  } else {
    WiFi.mode(WIFI_OFF);   // no WiFi configured and not forced -> run as tester. Hold MODE 5s to set up.
    Serial.println("WiFi: not configured - running as tester. Hold MODE 5s for the setup AP.");
  }

  // Start the web server only when the TCP/IP stack is up (STA or AP). With WiFi off the lwIP core
  // mutex is never created, so server.begin() would assert (xQueueSemaphoreTake null) and boot-loop.
  if (!webUp) return;

  server.on("/",       handleRoot);
  server.on("/setup",  handleSetup);
  server.on("/status", handleStatus);
  server.on("/boot",   handleBoot);
  server.on("/mqttinfo", handleMqttInfo);
  server.on("/cfg",    handleCfg);
  server.on("/exitap", HTTP_POST, handleExitAP);
  server.on("/press",  handlePress);
  server.on("/fun",    handleFun);
  server.on("/play",   handlePlay);
  server.on("/save",   HTTP_POST, handleSave);
  server.on("/reset",  handleReset);
  server.on("/enterap", HTTP_POST, handleEnterAP);   // web: hold both panel buttons 5 s -> reboot to setup AP
  server.onNotFound([]() {                       // AP captive-portal: bounce everything to setup
    if (apMode) { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
}

// Put WiFi status on the LCD for 4 s: the setup-AP join line, or the web-UI IP on connect. Called from
// startAP() and once when the station connects (the Ready line otherwise shows live status).
void showWifiInfo() {
  if (apMode)                             { char l2[48]; apJoinLine(l2, sizeof(l2)); setLines("Setup: join AP", l2); }
  else if (WiFi.status() == WL_CONNECTED) setLines("WiFi connected", WiFi.localIP().toString().c_str());
  else                                    setLines("WiFi", "connecting...");
  infoUntil = millis() + 4000;
}

// Scan the I2C bus and report every device on serial (diagnostic: confirms the LCD is on the bus).
// Returns the best-guess LCD address (prefer 0x27 / 0x3F); the address actually used is hardcoded LCD_ADDR.
uint8_t scanLcd() {
  Wire.begin(PIN_SDA, PIN_SCL);
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\n", a);
      if (a == 0x27 || a == 0x3F) found = a;   // known LCD backpack addresses win
      else if (!found)            found = a;    // otherwise take the first device seen
    }
  }
  if (!found) Serial.println("I2C: NO device found - check SDA/SCL wiring and that the LCD is plugged in");
  else        Serial.printf("LCD: using address 0x%02X\n", found);
  return found;
}

// Boot self-check: confirm the expected on-board components/connections responded. The one thing the
// hardware can actually sense is the I2C LCD (a missing LCD, unplugged J3 cable, or dead Q5/Q6 level
// shifter all show as no I2C ack). A failure is THROWN - serial message, the error tone, and an LED
// error blink - and mirrored to the web UI, but does not halt, so a deliberately headless unit still
// runs its web panel. Needs the buzzer (ledcAttach) and LED already set up. Returns true if all OK.
bool bootSelfCheck() {
  if (lcdPresent) { Serial.println("SELF-CHECK: OK - LCD present on I2C bus"); return true; }
  Serial.println("SELF-CHECK FAIL: LCD not on I2C bus (0x27/0x3F) - check the J3 cable and Q5/Q6 level shifter");
  setLines("ERR: LCD MISSING", "check J3 cable");                     // shows on the web UI mirror
  playMelody(TONE_ERR, sizeof(TONE_ERR) / sizeof(TONE_ERR[0]));       // audible fault tone
  for (int i = 0; i < 6; i++) { digitalWrite(LED_BUILTIN, LED_ON);  delay(120);
                                digitalWrite(LED_BUILTIN, LED_OFF); delay(120); }
  return false;
}

// ---------------------------- Setup / loop -----------------------------------
void setup() {
  Serial.begin(115200);

  // Read the boot diagnostics before anything else can reset us. A true power-on restarts the count;
  // any other reason means the previous boot died, and prevStage says where.
  prevReset = (int)esp_reset_reason();
  bool rtcValid = (bootMagic == 0xB007C0DEu);
  prevStage = rtcValid ? bootStage : 0;
  uint32_t prevUp = rtcValid ? bootUptime : 0;   // 0 = RTC was wiped, i.e. power really went away
  if (!rtcValid || prevReset == ESP_RST_POWERON) { bootMagic = 0xB007C0DEu; bootSeq = 0; bootTotal = 0; }
  bootSeq++;
  STAGE(1);

  bootRing[bootTotal % BOOT_LOG_N] = { (uint8_t)prevReset, (uint8_t)prevStage, (uint16_t)bootSeq, prevUp };
  bootTotal++;

  Serial.printf("BOOT #%lu (log %lu)  reset reason %d  prev stage %lu  prev uptime %lu ms\n",
                (unsigned long)bootSeq, (unsigned long)bootTotal, prevReset,
                (unsigned long)prevStage, (unsigned long)prevUp);

  // Bring the WiFi PHY up FIRST, before the LCD, buzzer, jingle and web server exist. PHY init is the
  // one call that can write flash during boot (it stores RF calibration when the stored copy is
  // missing or stale), and a flash write drops the instruction cache. The fault needs the other core
  // to be executing flash-resident code at that moment, so doing it here - when nothing else is
  // running - closes the window. Measured: it is the difference between a reproducible TG1WDT double
  // boot and none.
  {
    uint32_t tPhy = millis();
    WiFi.mode(WIFI_STA);
    phyInitMs = millis() - tPhy;
  }

  uint64_t chipId = ESP.getEfuseMac();   // stable per-device suffix so multiple units are distinct on one network
  snprintf(AP_SSID, sizeof(AP_SSID), "EVtivity-EV-Tester-%02X%02X",
           (uint8_t)(chipId >> 32), (uint8_t)(chipId >> 40));   // last two MAC octets - device-unique, not the constant OUI bytes
  snprintf(DEFAULT_TOPIC, sizeof(DEFAULT_TOPIC), "evtester-%02x%02x",
           (uint8_t)(chipId >> 32), (uint8_t)(chipId >> 40));   // same hex as the AP suffix, lowercased for the topic; keeps units distinct on one broker

  pinMode(PIN_ST_B, OUTPUT); pinMode(PIN_ST_C, OUTPUT);
  pinMode(PIN_FAULT, OUTPUT); digitalWrite(PIN_FAULT, LOW);   // fault injection off at boot (R21 also holds it low)
  applyState(CP_A);                       // State A. Not the first thing in setup() any more (PHY init
                                          // precedes it), which is safe: R2/R4 hold the Q1/Q2 gates low
                                          // in hardware, so the pilot is State A from power-up whatever
                                          // the firmware does. Do not rely on ordering here for safety.

  // Detect the LCD on the I2C bus first (boot self-check input), then bring it up only if it answered,
  // so a headless unit (no LCD) doesn't stall writing to a missing device. scanLcd() does Wire.begin.
  uint8_t la = scanLcd();
  STAGE(2);
  lcdPresent = (la != 0);
  if (la && la != LCD_ADDR)               // address is hardcoded to LCD_ADDR; just warn on a mismatch
    Serial.printf("NOTE: bus device 0x%02X != hardcoded LCD 0x%02X\n", la, LCD_ADDR);
  ledcAttach(PIN_BUZZ, BUZZ_FREQ, 10);    // buzzer up first so the fanfare can start with the banner
  ledcWriteTone(PIN_BUZZ, 0);

  // backlight() BEFORE init() sets the backlight bit on, so the init write sequence never blanks it
  // (the library defaults the bit to off, which is what caused the momentary darkness during init).
  if (lcdPresent) {
    lcd.backlight();
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0); lcd.print("EVtivity Tester");
    lcd.setCursor(0, 1); lcd.print("AC J1772 " FW_VERSION);
    // Cable-free readout: on a clean power-on the splash is unchanged. Any other reset reason means
    // the previous boot died, so line 2 reports it for the length of the fanfare (barrel power, no USB).
    if (prevReset != ESP_RST_POWERON && prevReset != ESP_RST_UNKNOWN) {
      char l2[17];
      snprintf(l2, sizeof(l2), "rst %d stg %lu b%lu", prevReset,
               (unsigned long)prevStage, (unsigned long)bootSeq);
      lcd.setCursor(0, 1); lcd.print("                ");
      lcd.setCursor(0, 1); lcd.print(l2);
    }
  }
  STAGE(3);
  playMelody(STARTUP_JINGLE, sizeof(STARTUP_JINGLE) / sizeof(STARTUP_JINGLE[0]));  // fanfare while the banner is up
  STAGE(4);

  pinMode(LED_BUILTIN, OUTPUT);                          // boot blink: 3 quick flashes
  for (int i = 0; i < 3; i++) { digitalWrite(LED_BUILTIN, LED_ON); delay(80);
                                digitalWrite(LED_BUILTIN, LED_OFF); delay(80); }
  STAGE(5);

  // Clamped pilot, read digitally. R20 (100k, on every board from v0.1.7) alone holds an unplugged line
  // low for charger-present detection - matches the SPICE-verified 100k with no internal-pulldown loading.
  // Pre-R20 (v0.1.1) boards are retired, so plain INPUT is now the correct default.
  pinMode(PIN_SENSE, INPUT);
  analogReadResolution(12);               // 0..4095
  analogSetPinAttenuation(PIN_PP, ADC_11db); // ~0..3.3 V range

  pinMode(PIN_BTN_MODE,  INPUT_PULLUP);
  pinMode(PIN_BTN_STATS, INPUT_PULLUP);

  if (lcdPresent) lcd.clear();            // the banner was up through the fanfare; clear for boot messages

  bootSelfCheck();      // throw an error (tone + LED + serial + web) if a component/connection is missing
  STAGE(6);
  netStartAt = millis() + NET_START_DELAY_MS;   // startNetwork() runs from loop(); see NET_START_DELAY_MS
  Serial.printf("Network start deferred %lu ms\n", (unsigned long)NET_START_DELAY_MS);
  if ((int32_t)(millis() - infoUntil) > 0) redraw();   // keep the setup-AP info screen if startAP put one up
}

void loop() {
  bootUptime = millis();         // how far this boot got in time, for the next boot to report
  if (bootStage < 7) STAGE(7);   // stages only ever advance, so later marks are not undone here
  if (netStartAt && (int32_t)(millis() - netStartAt) >= 0) {   // deferred WiFi start (NET_START_DELAY_MS)
    netStartAt = 0;
    STAGE(8);
    startNetwork();              // join saved WiFi in the background, or open the setup AP
    STAGE(14);
  }
  if (apMode) dnsServer.processNextRequest();
  if (webUp) server.handleClient();        // serve the web UI / config portal (skipped when WiFi is off)
  if (saveRebootAt && (int32_t)(millis() - saveRebootAt) >= 0) ESP.restart();   // deferred reboot after a settings save
  serviceWifi();                           // bounded join retries, then give up (no-op in AP / once connected)
  serviceSong();                           // step a running fun song (no-op when idle)
  serviceMqtt();                           // broker keepalive + commands (no-op when off)

  // heartbeat: a brief LED flash so you can see the XIAO is alive on any power source
  static uint32_t hbMs = 0; static bool hbOn = false;
  if (!hbOn && millis() - hbMs > 1400) { digitalWrite(LED_BUILTIN, LED_ON);  hbOn = true;  hbMs = millis(); }
  else if (hbOn && millis() - hbMs > 60) { digitalWrite(LED_BUILTIN, LED_OFF); hbOn = false; hbMs = millis(); }

#ifdef SIM_CP_PWM
  static uint32_t simT = 0;                              // ramp 10..85% so offered amps sweeps
  if (millis() - simT > 800) { simT = millis(); simDutyPct += 5; if (simDutyPct > 85) simDutyPct = 10; }
#endif

  bool sLow = (digitalRead(PIN_BTN_STATS) == LOW);
  bool mLow = (digitalRead(PIN_BTN_MODE) == LOW);
  static bool faultRejectShown = false;
  static bool physFaultHeld = false;                   // fault came from holding both buttons (momentary)
  if (!(sLow && mLow)) faultRejectShown = false;       // reset the once-per-gesture "No charger" reject

  // Pilot-fault injection: hold BOTH buttons. Only while the EVSE is actively charging (offering PWM) does
  // driving FAULT high (Q7 collapses the positive pilot half) make it report Faulted; release recovers. In
  // Ready or Preparing the EVSE ignores the collapsed pilot, so reject with a prompt instead of doing
  // nothing. Consume both buttons so it doesn't also toggle view/mode.
  if (apMode) {
    // Tester controls are inert during WiFi setup (a tap must not drive the pilot behind the "Setup"
    // screen). The only gesture: hold any button 5 s to leave setup and reboot to normal operation.
    static uint32_t apBtnMs = 0;
    if (sLow || mLow) {
      if (apBtnMs == 0) apBtnMs = millis();
      else if (millis() - apBtnMs >= 5000) {
        setLines("Exiting setup", "please wait...");
        playMelody(TONE_AP_EXIT, sizeof(TONE_AP_EXIT) / sizeof(TONE_AP_EXIT[0]));   // distinct falling beep
        delay(250);
        ESP.restart();                       // forceap flag was consumed at boot -> reboots to STA/tester
      }
    } else apBtnMs = 0;
  } else if (sLow && mLow) {
    if (offering) {
      setFault(true); physFaultHeld = true;                                                    // inject (EVSE is charging)
    } else if (!faultActive && !faultRejectShown) {   // (the fault itself collapses the pilot - don't then "reject")
      faultRejectShown = true;
      if (pilotPresentNow) showNotChargingError();                                             // present but not charging yet
      else                 showNoChargerError();                                               // no charger at all
    }
    statsDown = true;                                  // block the STATS toggle
    modeDown = true; modeHeldFired = true;             // block the MODE tap + hold
  } else {
    if (physFaultHeld) { physFaultHeld = false; setFault(false); }   // the held-button fault is momentary: recover on
                                                                    // release (an MQTT-set fault stays until a mode cmd)

    // STATS button: tap toggles the stats view.
    if (sLow && !statsDown && millis() - statsMs > 30) { statsDown = true; statsMs = millis(); doStatsPress(); }
    else if (!sLow && statsDown) statsDown = false;

    // MODE button: tap cycles the mode (Ready->Charging->Suspended); hold 5 s reboots into the setup AP.
    if (mLow && !modeDown) { modeDown = true; modeDownMs = millis(); modeHeldFired = false; }
    else if (mLow && modeDown && !modeHeldFired && millis() - modeDownMs >= 5000) {
      modeHeldFired = true;
      setLines("Entering setup", "please wait...");
      playMelody(TONE_AP_ENTER, sizeof(TONE_AP_ENTER) / sizeof(TONE_AP_ENTER[0]));   // distinct rising beep
      rebootIntoAP();
    } else if (!mLow && modeDown) {
      modeDown = false;
      if (!modeHeldFired && millis() - modeDownMs >= 40) doModePress();
    }
  }

  // Announce the WiFi join once (and again after a reconnect): IP on serial and on the LCD.
  static bool wifiAnnounced = false;
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    if (!wifiAnnounced) {
      wifiAnnounced = true; wifiFailReason = 0;
      STAGE(15);
      Serial.printf("WiFi: connected, RSSI %d dBm, web UI at http://%s/\n", WiFi.RSSI(), WiFi.localIP().toString().c_str());
      playMelody(TONE_WIFI_OK, sizeof(TONE_WIFI_OK) / sizeof(TONE_WIFI_OK[0]));   // success chime
      showWifiInfo();                      // put the IP on the LCD without needing the button hold
    }
  } else wifiAnnounced = false;

  // Track the charger pilot. With no pilot there is no session (you cannot charge nothing), so a physical
  // unplug from Charging/Suspended resets to Ready. Starting Charging is manual (see doModePress).
  static uint32_t pilotMs = 0;
  if (millis() - pilotMs > 200) {
    pilotMs = millis();
    pilotPresentNow = pilotPresent();
    if (!pilotPresentNow && mode != MODE_READY && !faultActive) enterMode(MODE_READY);   // unplugged -> Ready (fault injection collapses the pilot, so don't treat it as an unplug)
  }

  if (!songNotes && (int32_t)(millis() - infoUntil) > 0 && millis() - lastRefresh > REFRESH_MS) {  // info holds
    lastRefresh = millis();               // 4 s; a running song keeps its title on the LCD
    redraw();                             // refresh live duty / amps / proximity
  }
}
