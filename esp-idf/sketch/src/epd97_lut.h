// -----------------------------------------------------------------------------
// The two short waveform tables, one per COG revision.
//
// The panel's own waveform lives in its OTP and is what EPD_SLOW runs. These
// replace it, written to register 0xB0 after the 0x44/0x45/0xA7 sequence (which
// is what loads the OTP waveform into the very registers being replaced) and
// before the DC/DC soft start.
//
// The two revisions take different table formats at the same register:
//
//   driver 8   180 bytes = 5 tables x  9 groups x 4 bytes, frame counts
//              packed two to a byte as nibbles
//   driver B   400 bytes = 5 tables x 10 groups x 8 bytes, one whole byte
//              per frame count
//
// A level byte is two bits per phase with phase 0 in the top bits:
// 0 = ground, 1 = VDH, 2 = VDL, 3 = VDHR.
//
// Table index is (red bit << 1) | bw bit, so 0 = paper, 1 = ink, 2 = red,
// 3 = unreachable, 4 = VCOM.
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "epd97_config.h"

#define LUT8_BYTES  180u
#define LUTB_BYTES  400u

// --- driver 8 ----------------------------------------------------------------
// 103 frames at 32.0 ms - 3.3 s - against the panel's own ~51 s.
//
// Group 0 is a balanced shake, group 1 the chopped drive, the remaining seven
// one frame at ground each. Those seven are not optional: a group left at zero
// is not an empty group, it runs about four seconds.
//
// Two findings hold the shape up. The drive has to be chopped, not lengthened:
// short pushes with a rest at ground beat one long run at the rail by a factor
// of four or five on redness while making the ink darker, because a long run
// drags the slower red pigment along behind the black and the rests let it
// relax back. And the shake has to be balanced - as much VDH as VDL, so it adds
// no net charge whatever a pixel holds; without that the ground goes pink
// within a handful of updates.
static const uint8_t k_lut8_base[LUT8_BYTES] = {
  /* table 0, paper (-> white) */
  0x66, 0x33, 0x33, 0x02,
  0x88, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 1, ink (-> black) */
  0x99, 0x33, 0x33, 0x02,
  0x44, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 2, red - overwritten by epd_build_lut8() when the frame has red */
  0x99, 0x33, 0x33, 0x02,
  0x44, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 3, unreachable */
  0x99, 0x33, 0x33, 0x02,
  0x44, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  /* table 4, VCOM */
  0x00, 0x33, 0x33, 0x02,
  0x00, 0x31, 0x31, 0x09,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
  0x00, 0x10, 0x00, 0x01,
};

// Build the 180-byte table for this frame.
//
// with_red = the frame carries red, OR the frame before it did. Both matter: a
// frame that FOLLOWS a red one still has to clear the red pigment out or the
// old picture stays visible under the new one.
//
// clearing = there is red to remove but none to make. Every group then goes to
// the erase, because spending them on a red push is wasted and starving the
// erase is what leaves the old picture legible in the white after it.
//
// Red on this film is not a third direction. Both coloured pigments carry the
// same charge and both rise on a positive field; they differ only in threshold,
// red's being the lower one. On the full rail VDH black comes up with the red
// and has to be pulled back out afterwards - and the pull that removes black
// removes red with it, because the two differ in speed by about 2.5, not the
// ten to one the literature suggests. On the reduced rail VDHR black never
// comes up at all, so there is nothing to take back out.
static inline void epd_build_lut8(uint8_t* out, bool with_red, bool clearing)
{
  memcpy(out, k_lut8_base, LUT8_BYTES);
  if (!with_red) {
    return;
  }

  const uint8_t white = out[EPD_8_PAP_TABLE * 9 * 4 + 4];   // 0x88
  const uint8_t black = out[1 * 9 * 4 + 4];                 // 0x44

  // Erase to white, then hold the reduced positive rail. The two swing stages
  // the published three-pigment waveforms carry - activation and separation -
  // both rock on VDH, the rail that brings black up. On a red pixel that is
  // exactly what must not happen, so they are left out and the red table sees
  // only the negative rail and then the reduced positive one.
  const struct { uint8_t level; uint8_t groups; uint8_t rep; } stage[2] = {
    { white,
      clearing ? (uint8_t)EPD_8_RED_CLEAR_GROUPS : (uint8_t)EPD_8_RED_ERASE_GROUPS,
      clearing ? (uint8_t)EPD_8_RED_CLEAR_REP    : (uint8_t)EPD_8_RED_ERASE_REP },
    { (uint8_t)EPD_8_RED_LEVEL,
      clearing ? (uint8_t)0 : (uint8_t)EPD_8_RED_GROUPS,
      (uint8_t)EPD_8_RED_REP },
  };

  // Start at group 2. Group 0 is the shake and group 1 the main drive that
  // makes black black and white white; writing the red stages over group 1
  // replaces nine repeats with one, which is grey blacks and red landing on an
  // undriven pixel.
  unsigned g = 2u;
  for (unsigned st = 0; st < 2u; st++) {
    for (unsigned k = 0; k < stage[st].groups && g < 9u; k++, g++) {
      // Every table gets the timing, not just red's: a group lasts as long as
      // the group says, and a table left at one frame of ground ends it there
      // whatever the others asked for. Ink and paper are held at their own
      // targets throughout - ground is not neutral over stages this long, a
      // black pixel relaxes to grey there and a white one drifts.
      for (unsigned tb = 0; tb < 5u; tb++) {
        uint8_t* q = &out[(tb * 9u + g) * 4u];
        if (tb == (unsigned)EPD_8_RED_TABLE)      q[0] = stage[st].level;
        else if (tb == (unsigned)EPD_8_PAP_TABLE) q[0] = white;
        else if (tb == 4u)                        q[0] = 0x00;   // VCOM
        else                                      q[0] = black;
        // The vendor's own cleaning waveform for this COG works in 480 ms
        // swings, and that is the timescale the pigments respond on. An erase
        // written with the black drive's 32 ms phases does not clear anything.
        q[1] = (uint8_t)EPD_8_RED_FRAMES;
        q[2] = (uint8_t)EPD_8_RED_FRAMES;
        q[3] = stage[st].rep;
      }
    }
  }
}

// --- driver B ----------------------------------------------------------------
// 3460 ms against 33400 ms for the panel's own waveform, measured with a camera
// against a conditioned film: ink at L* 15.1 with a* +1.88, paper at L* 86.9,
// contrast 71.8.
//
// Two things here are not what the 084 does. Tables 0 and 1 take one half of
// the pixels each - driving either alone leaves a clean checkerboard - so they
// get opposite drives rather than the 084's "one white table, three black". And
// the plain shake has to be split: a shake writes the same level into every
// table, which swamps that division and pushes the whole panel one way.
static inline void put_group_b(uint8_t* q, uint8_t level, uint8_t on,
                               uint8_t off, uint8_t rep)
{
  q[0] = level;
  q[1] = on; q[2] = off; q[3] = (uint8_t)EPD_B_PH2;
  q[4] = on; q[5] = off; q[6] = (uint8_t)EPD_B_PH2;
  q[7] = rep;
}

// Clear before setting: a pixel on its way to red may be coming from black, and
// driving it straight at red leaves what was there showing through. One group,
// not three - of the nine red has, every one spent clearing is one not spent
// pulling.
static inline uint8_t red_level_b(unsigned g)
{
  const unsigned rg = (g >= (unsigned)EPD_B_SHAKE_GROUPS)
                        ? (g - (unsigned)EPD_B_SHAKE_GROUPS) : 0u;
  return (rg < (unsigned)EPD_B_RED_WHITE_GROUPS)
           ? (uint8_t)EPD_B_RED_WHITE_LEVEL
           : (uint8_t)EPD_B_RED_LEVEL;
}

// Turn whole tables (bit t of `tables` = table t) into idle groups - ground for
// one frame, the same filler the tables already use - so their pixels get no
// drive and keep whatever is on the film. VCOM (table 4) is already at ground.
//   EPD_FAST_KEEP_RED  idles 2 and 3: red stays as a slow refresh put it.
//   EPD_FAST_INK       idles 0, 2 and 3: only the ink table drives.
// Works on either layout: `groups` per table, `bytes` per group. Driver 8 idle
// groups need their frame-count bytes too, since a group of all zeros runs
// about four seconds rather than not at all.
static inline void epd_idle_tables(uint8_t* out, unsigned groups, unsigned bytes, unsigned tables)
{
  const uint8_t* idle = &out[(4u * groups + groups - 1u) * bytes];   // VCOM's last group
  for (unsigned t = 0u; t <= 3u; t++) {
    if (!(tables & (1u << t))) continue;
    for (unsigned g = 0; g < groups; g++) {
      memcpy(&out[(t * groups + g) * bytes], idle, bytes);
    }
  }
}

// EPD_FAST_INK: the ink table's shake groups idle too. The shake rocks a pixel
// both ways, which is a visible flicker on lines that are already black.
static inline void epd_idle_shake(uint8_t* out, unsigned groups, unsigned bytes,
                                  unsigned table, unsigned shake_groups)
{
  const uint8_t* idle = &out[(4u * groups + groups - 1u) * bytes];
  for (unsigned g = 0; g < shake_groups; g++) {
    memcpy(&out[(table * groups + g) * bytes], idle, bytes);
  }
}

static inline void epd_build_lutb(uint8_t* out)
{
  for (unsigned t = 0; t < 5u; t++) {
    for (unsigned g = 0; g < 10u; g++) {
      uint8_t* q = &out[(t * 10u + g) * 8u];

      if (t == 4u) {                              // VCOM stays at ground
        put_group_b(q, 0x00, 1, 1, 1);
        continue;
      }

      if (g < (unsigned)EPD_B_SHAKE_GROUPS) {
        // The split shake. 0x66 is VDH,VDL,VDH,VDL and 0x99 the reverse, so
        // each half is balanced over the group and cannot accumulate, but at
        // any instant the two are being pushed apart rather than together.
        uint8_t shk = 0x00;
        if (t == (unsigned)EPD_B_INK_TABLE)      shk = (uint8_t)EPD_B_INK_SHAKE;
        else if (t == (unsigned)EPD_B_PAP_TABLE) shk = (uint8_t)EPD_B_PAP_SHAKE;
        else if (t == (unsigned)EPD_B_RED_TABLE) shk = (uint8_t)EPD_B_RED_SHAKE;
        put_group_b(q, shk, EPD_B_SHAKE_F, EPD_B_SHAKE_F, EPD_B_SHAKE_REP);
        continue;
      }

      if (g < (unsigned)(EPD_B_SHAKE_GROUPS + EPD_B_DRIVE_GROUPS)) {
        uint8_t lvl, on = EPD_B_DRIVE_ON, off = EPD_B_DRIVE_OFF;
        if (t == (unsigned)EPD_B_INK_TABLE) {
          lvl = (uint8_t)EPD_B_INK_LEVEL;
        } else if (t == (unsigned)EPD_B_RED_TABLE) {
          lvl = red_level_b(g);
        } else if (t == (unsigned)EPD_B_PAP_TABLE) {
          // The paper keeps its own rest: full duty on both costs paper
          // brightness, 45/1 everywhere dropped it from L* 76.7 to 67.1.
          lvl = (uint8_t)EPD_B_WHITE_LEVEL;
          on  = EPD_B_PAP_ON;
          off = EPD_B_PAP_OFF;
        } else {
          lvl = 0x00;
        }
        put_group_b(q, lvl, on, off, EPD_B_DRIVE_REP);
        continue;
      }

      if (t == (unsigned)EPD_B_RED_TABLE &&
          g < (unsigned)(EPD_B_SHAKE_GROUPS + EPD_B_RED_GROUPS)) {
        // Red keeps driving where black and white have finished. Those groups
        // sit at ground for every table otherwise, so red - the slow pigment -
        // is taking time nothing else is using.
        put_group_b(q, red_level_b(g), EPD_B_DRIVE_ON, EPD_B_DRIVE_OFF,
                    EPD_B_DRIVE_REP);
        continue;
      }

      put_group_b(q, 0x00, 1, 1, 1);
    }
  }
}
