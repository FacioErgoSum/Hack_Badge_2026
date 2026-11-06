// -----------------------------------------------------------------------------
// Pin map and tunables for the 9.7" Pervasive iTC panel on an ESP32-S3.
//
// Everything here is a #ifndef, so any of it can be overridden from
// platformio.ini build_flags without touching the file.
// -----------------------------------------------------------------------------
#pragma once

// --- Pin map -----------------------------------------------------------------
#ifndef EPD_PIN_DC
#define EPD_PIN_DC        10 
#endif
#ifndef EPD_PIN_MOSI
#define EPD_PIN_MOSI      47   // SDA. Bidirectional: the OTP is read back on it.
#endif
#ifndef EPD_PIN_SCK
#define EPD_PIN_SCK       21   // SCL
#endif
#ifndef EPD_PIN_CS_M
#define EPD_PIN_CS_M      13   // CS1, master COG (left half of the panel)
#endif
#ifndef EPD_PIN_CS_S
#define EPD_PIN_CS_S      3   // CS2, slave COG (right half)
#endif
#ifndef EPD_PIN_BUSY_M
#define EPD_PIN_BUSY_M    12
#endif
#ifndef EPD_PIN_BUSY_S
#define EPD_PIN_BUSY_S    8   // -1 if the slave BUSY is not wired
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST       11
#endif
#ifndef EPD_PIN_PWR
#define EPD_PIN_PWR       48   // VCC_EN
#endif
// Which level on VCC_EN powers the panel.
//
// 0 here: the board switches the panel's VCC with an SK2301AA1SHB, a P-channel
// MOSFET high-side switch driven straight off the GPIO. Pulling the gate LOW
// turns it on. Set to 1 for a plain active-HIGH enable input.
//
// Getting this backwards does not look like a power problem from the outside.
// The OTP read still succeeds - it runs during the window where the pin happens
// to be at the other level - and the refresh only falls over at the DC/DC soft
// start, as "booster never reported ready in 10 s".
//
// Note there is no pull-up on the gate, so between reset and epd_begin() the
// pin is high-Z and the switch is undefined. epd_begin() parks it before it
// drives it, but a 100k to 3V3 on the gate would close that window in hardware.
#ifndef EPD_PWR_ACTIVE_HIGH
#define EPD_PWR_ACTIVE_HIGH 0
#endif

// --- Bus ---------------------------------------------------------------------
#ifndef EPD_SPI_HOST_NUM
#define EPD_SPI_HOST_NUM  SPI2_HOST
#endif
// Bulk plane data only. Register writes and the OTP readback stay bitbanged:
// they are a few dozen bytes and the OTP wants a ~50 kHz clock.
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ        4000000u
#endif

// --- Timing ------------------------------------------------------------------
// Reset shape, from the vendor's b_reset(200,20,200,50,5) for the medium COG
// with margin added. Too short announces itself as cog_initial failing and the
// OTP reading back corrupt.
#ifndef EPD_T_RST_PWR
#define EPD_T_RST_PWR    800u   // settle after switching the panel supply on
#endif
#ifndef EPD_T_RST_HI
#define EPD_T_RST_HI      50u
#endif
#ifndef EPD_T_RST_LO
#define EPD_T_RST_LO     100u
#endif
#ifndef EPD_T_RST_SET
#define EPD_T_RST_SET    400u
#endif
// Second reset, after the OTP read.
#ifndef EPD_T_OTP_HI
#define EPD_T_OTP_HI      50u
#endif
#ifndef EPD_T_OTP_LO
#define EPD_T_OTP_LO     100u
#endif
#ifndef EPD_T_OTP_SET
#define EPD_T_OTP_SET    400u
#endif
// Minimum time the panel supply stays off between two COG cycles. The booster
// rails have to bleed down; starting a cycle on top of them stalls the update.
// 1500 ms measured good. At 800 ms cog_initial fails and the OTP reads corrupt.
#ifndef EPD_T_OFF_MIN
#define EPD_T_OFF_MIN   1500u
#endif
#ifndef EPD_T_DISCHARGE
#define EPD_T_DISCHARGE  300u   // held in reset, supply already cut
#endif
#ifndef EPD_T_PRE_UPD
#define EPD_T_PRE_UPD    660u   // settle in front of the update registers
#endif
#ifndef EPD_T_PRE_SS
#define EPD_T_PRE_SS      50u   // after the last 0xA7, before the soft start
#endif
// A refresh on the panel's own waveform is ~51 s on the 084 and ~34 s on the
// B1, so the ceiling has to be generous.
#ifndef EPD_IDLE_WAIT_MS
#define EPD_IDLE_WAIT_MS 180000u
#endif
// How long to keep watching for BUSY dropping again after the refresh reports
// done - some panels run a second waveform pass.
#ifndef EPD_SECOND_PASS_MS
#define EPD_SECOND_PASS_MS 1000u
#endif

// --- Session -----------------------------------------------------------------
// With the session held open across images the COG keeps its rails, its cached
// OTP and its latched waveform, so an image costs its plane data plus the
// refresh instead of a full power-up. Bounded rather than open-ended.
#ifndef EPD_SESSION_IMAGES
#define EPD_SESSION_IMAGES 20
#endif

// Whether a fast refresh may drive the third colour.
//
// 0, because the short waveform cannot make a good red. On the 084 the red
// table is barely reached in the time the short table runs at all; on the B1
// the refresh is a fixed ~1 s whatever the table asks for, and red is the slow
// pigment - it comes up pale and gets worse with every frame that half-drives
// it. Red is a slow-refresh colour on this film.
//
// With this off, a fast refresh draws red content as ink rather than dropping
// it, so the picture still reads - it is just monochrome until the next slow
// refresh. The red-clearing stages stay in play either way: a fast frame that
// follows a red one still spends its groups pulling the red pigment back out.
#ifndef EPD_FAST_ALLOW_RED
#define EPD_FAST_ALLOW_RED 0
#endif

// --- Fast refresh, driver 8 (E2969JS084) -------------------------------------
// Waveform bank on 0x45. On the 084 this picks between the OTP's temperature
// bands; the 9.7" default is 0x86, the 12" 0x8C.
#ifndef EPD_REG45_084
#define EPD_REG45_084 0x86
#endif
#ifndef EPD_REG45_B1
#define EPD_REG45_B1  0x84
#endif

// The red stages of the 180-byte table. Measured with a camera against the
// paper's own white balance; see README.
#ifndef EPD_8_RED_TABLE
#define EPD_8_RED_TABLE 2         // (red bit, bw bit) -> table index
#endif
#ifndef EPD_8_PAP_TABLE
#define EPD_8_PAP_TABLE 0
#endif
#ifndef EPD_8_RED_LEVEL
#define EPD_8_RED_LEVEL 0xFF      // VDHR in all four phases - the REDUCED rail
#endif
#ifndef EPD_8_RED_GROUPS
#define EPD_8_RED_GROUPS 6
#endif
#ifndef EPD_8_RED_REP
#define EPD_8_RED_REP 1
#endif
#ifndef EPD_8_RED_ERASE_GROUPS
#define EPD_8_RED_ERASE_GROUPS 1
#endif
#ifndef EPD_8_RED_ERASE_REP
#define EPD_8_RED_ERASE_REP 1
#endif
// A frame with no red that follows one with red: every group goes to erase,
// because there is nothing to make, only red to get rid of.
#ifndef EPD_8_RED_CLEAR_GROUPS
#define EPD_8_RED_CLEAR_GROUPS 7
#endif
#ifndef EPD_8_RED_CLEAR_REP
#define EPD_8_RED_CLEAR_REP 1
#endif
#ifndef EPD_8_RED_FRAMES
#define EPD_8_RED_FRAMES 0x66     // six frames a phase, ~190 ms
#endif

// --- Fast refresh, driver B (E2969JS0B1) -------------------------------------
// A different table format entirely: 5 tables x 10 groups x 8 bytes.
#ifndef EPD_B_INK_TABLE
#define EPD_B_INK_TABLE 1
#endif
#ifndef EPD_B_PAP_TABLE
#define EPD_B_PAP_TABLE 0
#endif
#ifndef EPD_B_RED_TABLE
#define EPD_B_RED_TABLE 2
#endif
#ifndef EPD_B_SHAKE_GROUPS
#define EPD_B_SHAKE_GROUPS 1
#endif
#ifndef EPD_B_SHAKE_F
#define EPD_B_SHAKE_F 10
#endif
#ifndef EPD_B_SHAKE_REP
#define EPD_B_SHAKE_REP 3
#endif
#ifndef EPD_B_INK_SHAKE
#define EPD_B_INK_SHAKE 0x66
#endif
#ifndef EPD_B_PAP_SHAKE
#define EPD_B_PAP_SHAKE 0x99
#endif
#ifndef EPD_B_RED_SHAKE
#define EPD_B_RED_SHAKE 0xEE
#endif
#ifndef EPD_B_DRIVE_GROUPS
#define EPD_B_DRIVE_GROUPS 4
#endif
#ifndef EPD_B_DRIVE_ON
#define EPD_B_DRIVE_ON 45
#endif
#ifndef EPD_B_DRIVE_OFF
#define EPD_B_DRIVE_OFF 1
#endif
#ifndef EPD_B_DRIVE_REP
#define EPD_B_DRIVE_REP 3
#endif
#ifndef EPD_B_INK_LEVEL
#define EPD_B_INK_LEVEL 0x44      // VDH darkens
#endif
#ifndef EPD_B_WHITE_LEVEL
#define EPD_B_WHITE_LEVEL 0xAA    // VDL on all four phases
#endif
#ifndef EPD_B_PAP_ON
#define EPD_B_PAP_ON 45
#endif
#ifndef EPD_B_PAP_OFF
#define EPD_B_PAP_OFF 3
#endif
#ifndef EPD_B_RED_LEVEL
#define EPD_B_RED_LEVEL 0xFF
#endif
#ifndef EPD_B_RED_GROUPS
#define EPD_B_RED_GROUPS 9
#endif
#ifndef EPD_B_RED_WHITE_GROUPS
#define EPD_B_RED_WHITE_GROUPS 1  // clear before setting
#endif
#ifndef EPD_B_RED_WHITE_LEVEL
#define EPD_B_RED_WHITE_LEVEL 0xAA
#endif
#ifndef EPD_B_PH2
#define EPD_B_PH2 1
#endif
// On driver B the refresh runs for a fixed ~1070 ms whatever the table asks
// for, so a single pass cannot carry enough dose to clear the previous frame.
// Three pulls of the already-latched waveform, no plane data resent.
#ifndef EPD_B_REFRESH_PULLS
#define EPD_B_REFRESH_PULLS 3
#endif

// --- Logging -----------------------------------------------------------------
#ifndef EPD_LOG
#define EPD_LOG 1        // one line per phase
#endif
#ifndef EPD_LOG_VERBOSE
#define EPD_LOG_VERBOSE 0
#endif
