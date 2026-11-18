// jvparams.h - the JV-1080 / JV-2080 / XP-80 patch parameters: their places in a patch's
// SysEx layout (srjv::Patch), and the bit widths the SR-JV80 patch table packs them in.
//
// Offsets follow the JV-1080 / JV-2080 MIDI implementation (Patch Common 00 00 xx, Patch Tone
// 00 1x xx). VALUES are as the SR-JV80 boards' JV-1080 tables store them, measured over the
// ~3,800 patches of 18 boards (they differ from the SysEx data): signed +-63 parameters are
// 0..126 with 63 = 0, coarse tune 0..96 (48 = 0), fine tune 0..100 (50 = 0), velocity
// sensitivities 0..125 (50 = 0, i.e. -100..+150 %), pans 0..127 (64 = centre). JV-80 patches
// are converted into this encoding when a board is loaded.
#ifndef XP_JVPARAMS_H
#define XP_JVPARAMS_H

#include <cmath>
#include <cstdint>

namespace jv {

// Envelope (and LFO delay / fade, portamento) times, 0..127 -> seconds: see engine.cpp for where the
// curve comes from. Here so that the screen shows the times the engine plays.
inline float envSeconds(int v) { return v <= 0 ? 0.0f : 0.1f * std::exp((float)(v - 27) * 0.061f); }

// ---------------------------------------------------------------- patch common (74 bytes)
enum Common : uint8_t {
    C_NAME = 0x00,          // 12 ASCII characters
    C_EFX_TYPE = 0x0C,      // 0..39
    C_EFX_PARAM = 0x0D,     // 12 parameters, their meaning depends on the type
    C_EFX_OUTPUT = 0x19,    // 0 MIX, 1 OUTPUT-A, 2 OUTPUT-B (all to the main pair here)
    C_EFX_MIX_SEND = 0x1A, C_EFX_CHORUS_SEND = 0x1B, C_EFX_REVERB_SEND = 0x1C,
    C_EFX_CTRL_SRC1 = 0x1D, C_EFX_CTRL_DEPTH1 = 0x1E,      // depths 0..126, 63 = 0
    C_EFX_CTRL_SRC2 = 0x1F, C_EFX_CTRL_DEPTH2 = 0x20,
    C_CHORUS_LEVEL = 0x21, C_CHORUS_RATE = 0x22, C_CHORUS_DEPTH = 0x23, C_CHORUS_PREDELAY = 0x24,
    C_CHORUS_FEEDBACK = 0x25, C_CHORUS_OUTPUT = 0x26,       // 0 MIX, 1 REV, 2 MIX+REV
    C_REVERB_TYPE = 0x27,   // 0 ROOM1 1 ROOM2 2 STAGE1 3 STAGE2 4 HALL1 5 HALL2 6 DELAY 7 PAN-DELAY
    C_REVERB_LEVEL = 0x28, C_REVERB_TIME = 0x29,
    C_REVERB_HF_DAMP = 0x2A, // 0..17: 200 Hz .. 8 kHz, 17 = BYPASS
    C_REVERB_FEEDBACK = 0x2B,
    C_TEMPO_HI = 0x2C, C_TEMPO_LO = 0x2D,  // 20..250 BPM in two nibbles
    C_LEVEL = 0x2E, C_PAN = 0x2F,          // pan 0..127, 64 centre
    C_ANALOG_FEEL = 0x30,
    C_BEND_UP = 0x31,       // 0..12 semitones
    C_BEND_DOWN = 0x32,     // 0..48 semitones
    C_KEY_ASSIGN = 0x33,    // 0 POLY, 1 SOLO
    C_SOLO_LEGATO = 0x34,
    C_PORTA_SW = 0x35, C_PORTA_MODE = 0x36, C_PORTA_TYPE = 0x37, C_PORTA_START = 0x38, C_PORTA_TIME = 0x39,
    C_CTRL_SRC2 = 0x3A, C_CTRL_SRC3 = 0x3B,
    C_EFX_HOLD = 0x3C, C_CTRL1_HOLD = 0x3D, C_CTRL2_HOLD = 0x3E, C_CTRL3_HOLD = 0x3F,
    C_VELO_RANGE_SW = 0x40,
    C_OCTAVE = 0x41,        // 0..6 = -3..+3
    C_STRETCH = 0x42, C_PRIORITY = 0x43,
    C_STRUCT12 = 0x44, C_BOOST12 = 0x45, C_STRUCT34 = 0x46, C_BOOST34 = 0x47,
    C_CLOCK_SRC = 0x48,     // JV-2080 on
    C_CATEGORY = 0x49,      // JV-2080 on
};

// ---------------------------------------------------------------- one tone (129 bytes)
enum Tone : uint8_t {
    T_SWITCH = 0x00,
    T_WAVE_GROUP = 0x01,    // 0 INT, 1 PCM card, 2 EXP board
    T_WAVE_GROUP_ID = 0x02,
    T_WAVE_HI = 0x03, T_WAVE_LO = 0x04,    // wave number 0..254 in two nibbles
    T_WAVE_GAIN = 0x05,     // 0 -6 dB, 1 0 dB, 2 +6 dB, 3 +12 dB
    T_FXM_SW = 0x06, T_FXM_COLOR = 0x07, T_FXM_DEPTH = 0x08,
    T_DELAY_MODE = 0x09, T_DELAY_TIME = 0x0A,
    T_VELO_XFADE = 0x0B, T_VELO_LO = 0x0C, T_VELO_HI = 0x0D, T_KEY_LO = 0x0E, T_KEY_HI = 0x0F,
    T_REDAMPER_SW = 0x10, T_VOLUME_SW = 0x11, T_HOLD_SW = 0x12, T_BEND_SW = 0x13, T_PAN_SW = 0x14,
    T_MATRIX = 0x15,        // 3 controllers x 4 (destination 0..18, Dest; depth 0..126, 63 = 0)
    T_LFO1 = 0x2D, T_LFO2 = 0x35,   // + LFO_* below
    T_COARSE = 0x3D,        // 0..96 = -48..+48
    T_FINE = 0x3E,          // 0..100 = -50..+50 cents
    T_RANDOM_PITCH = 0x3F,  // 0..30 (table)
    T_PITCH_KF = 0x40,      // 0..15 (table kKeyfollow)
    T_PENV_DEPTH = 0x41,    // 0..24 = -12..+12 semitones
    T_PENV_VELO = 0x42,     // velocity sensitivities: 0..125, 50 = 0 (-100..+150 %)
    T_PENV_VELO_T1 = 0x43, T_PENV_VELO_T4 = 0x44, T_PENV_TIME_KF = 0x45,
    T_PENV_T1 = 0x46,       // T1..T4, then L1..L4
    T_PENV_L1 = 0x4A,       // 0..126, 63 = 0
    T_PITCH_LFO1 = 0x4E, T_PITCH_LFO2 = 0x4F,   // LFO depths 0..126, 63 = 0
    T_FILTER_TYPE = 0x50,   // 0 OFF 1 LPF 2 BPF 3 HPF 4 PKG
    T_CUTOFF = 0x51, T_CUTOFF_KF = 0x52, T_RESONANCE = 0x53, T_RESO_VELO = 0x54,
    T_FENV_DEPTH = 0x55,    // 0..126, 63 = 0
    T_FENV_CURVE = 0x56, T_FENV_VELO = 0x57, T_FENV_VELO_T1 = 0x58, T_FENV_VELO_T4 = 0x59, T_FENV_TIME_KF = 0x5A,
    T_FENV_T1 = 0x5B,       // T1..T4, then L1..L4 (0..127)
    T_FENV_L1 = 0x5F,
    T_FILTER_LFO1 = 0x63, T_FILTER_LFO2 = 0x64,
    T_LEVEL = 0x65, T_BIAS_DIR = 0x66, T_BIAS_POINT = 0x67, T_BIAS_LEVEL = 0x68,
    T_AENV_CURVE = 0x69, T_AENV_VELO = 0x6A, T_AENV_VELO_T1 = 0x6B, T_AENV_VELO_T4 = 0x6C, T_AENV_TIME_KF = 0x6D,
    T_AENV_T1 = 0x6E,       // T1..T4, then L1..L3 (0..127)
    T_AENV_L1 = 0x72,
    T_AMP_LFO1 = 0x75, T_AMP_LFO2 = 0x76,
    T_PAN = 0x77,           // 0..127, 64 = centre
    T_PAN_KF = 0x78,        // 0..14 (kTimeSens)
    T_RANDOM_PAN = 0x79, T_ALT_PAN = 0x7A, T_PAN_LFO1 = 0x7B, T_PAN_LFO2 = 0x7C,
    T_OUTPUT = 0x7D,        // 0 MIX, 1 EFX, 2 OUTPUT-A, 3 OUTPUT-B
    T_SEND_LEVEL = 0x7E, T_CHORUS_SEND = 0x7F, T_REVERB_SEND = 0x80,
};

enum Lfo : uint8_t {        // offsets from T_LFO1 / T_LFO2
    LFO_WAVE = 0,           // 0 TRI 1 SIN 2 SAW 3 SQR 4 TRP 5 S&H 6 RND 7 CHS
    LFO_KEY_SYNC = 1, LFO_RATE = 2,
    LFO_OFFSET = 3,         // 0..4 = -100, -50, 0, +50, +100
    LFO_DELAY = 4,
    LFO_FADE_MODE = 5,      // 0 ON-IN 1 ON-OUT 2 OFF-IN 3 OFF-OUT
    LFO_FADE_TIME = 6, LFO_EXT_SYNC = 7,
};

// Matrix control destinations (5 bits, 0..18 on the boards). 6..8 are destinations of the
// later JV generation that this engine does not model; the rest is inferred from how the
// boards' patches use each controller (the modulation lever's most used one is 9: vibrato).
enum Dest : uint8_t { D_OFF, D_PITCH, D_CUTOFF, D_RESO, D_LEVEL, D_PAN, D_X6, D_X7, D_X8, D_L1_PITCH, D_L2_PITCH,
                      D_L1_FILTER, D_L2_FILTER, D_L1_AMP, D_L2_AMP, D_L1_PAN, D_L2_PAN, D_L1_RATE, D_L2_RATE, D_COUNT };

// Pitch and cutoff keyfollow, in percent (4 bits)
constexpr int kKeyfollow[16] = {-100, -70, -50, -30, -10, 0, 10, 20, 30, 40, 50, 70, 100, 120, 150, 200};
// Envelope time keyfollow, velocity-time sensitivity and pan keyfollow, in percent (0..14)
constexpr int kTimeSens[16] = {-100, -70, -50, -40, -30, -20, -10, 0, 10, 20, 30, 40, 50, 70, 100, 100};
// Random pitch depth, in cents (0..30)
constexpr int kRandomPitch[31] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100,
                                  200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200};
// Reverb HF damp, Hz (17 = bypass)
constexpr int kHfDamp[18] = {200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 0};

// ---------------------------------------------------------------- the SR-JV80 bit packing
// The order and widths the patch table stores parameters in: {SysEx offset, bits}. A record is
// the common list, 12 zero bits, then four times the tone list and 8 zero bits: 401 bytes.
struct Field { uint8_t at; uint8_t bits; };

constexpr Field kCommonFields[] = {
    {0x00, 7}, {0x01, 7}, {0x02, 7}, {0x03, 7}, {0x04, 7}, {0x05, 7}, {0x06, 7}, {0x07, 7}, {0x08, 7}, {0x09, 7}, {0x0A, 7}, {0x0B, 7},
    {0x0C, 6},
    {0x0D, 7}, {0x0E, 7}, {0x0F, 7}, {0x10, 7}, {0x11, 7}, {0x12, 7}, {0x13, 7}, {0x14, 7}, {0x15, 7}, {0x16, 7}, {0x17, 7}, {0x18, 7},
    {0x19, 2}, {0x1A, 7}, {0x1B, 7}, {0x1C, 7}, {0x1D, 4}, {0x1E, 7}, {0x1F, 4}, {0x20, 7},
    {0x21, 7}, {0x22, 7}, {0x23, 7}, {0x24, 7}, {0x25, 7}, {0x26, 2},
    {0x27, 3}, {0x28, 7}, {0x29, 7}, {0x2A, 5}, {0x2B, 7},
    {0x2D, 4}, {0x2C, 4},                       // the tempo's nibbles, low one first
    {0x2E, 7}, {0x2F, 7}, {0x30, 7}, {0x31, 4}, {0x32, 6},
    {0x33, 1}, {0x34, 1}, {0x35, 1}, {0x36, 1}, {0x37, 1}, {0x38, 1}, {0x39, 7},
    {0x3A, 4}, {0x3B, 4}, {0x3C, 2}, {0x3D, 2}, {0x3E, 2}, {0x3F, 2},
    {0x40, 1}, {0x41, 3}, {0x42, 2}, {0x43, 1},
    {0x44, 4}, {0x45, 2}, {0x46, 4}, {0x47, 2},
    {0x48, 1}, {0x49, 7},
};
constexpr int kCommonPad = 12;

constexpr Field kToneFields[] = {
    {0x00, 1}, {0x01, 2}, {0x02, 7}, {0x04, 4}, {0x03, 4},   // the wave number's nibbles, low one first
    {0x05, 2}, {0x06, 1}, {0x07, 2}, {0x08, 4}, {0x09, 3}, {0x0A, 7},
    {0x0B, 7}, {0x0C, 7}, {0x0D, 7}, {0x0E, 7}, {0x0F, 7},
    {0x10, 1}, {0x11, 1}, {0x12, 1}, {0x13, 1}, {0x14, 2},
    {0x15, 5}, {0x16, 7}, {0x17, 5}, {0x18, 7}, {0x19, 5}, {0x1A, 7}, {0x1B, 5}, {0x1C, 7},
    {0x1D, 5}, {0x1E, 7}, {0x1F, 5}, {0x20, 7}, {0x21, 5}, {0x22, 7}, {0x23, 5}, {0x24, 7},
    {0x25, 5}, {0x26, 7}, {0x27, 5}, {0x28, 7}, {0x29, 5}, {0x2A, 7}, {0x2B, 5}, {0x2C, 7},
    {0x2D, 3}, {0x2E, 1}, {0x2F, 7}, {0x30, 3}, {0x31, 7}, {0x32, 2}, {0x33, 7}, {0x34, 2},
    {0x35, 3}, {0x36, 1}, {0x37, 7}, {0x38, 3}, {0x39, 7}, {0x3A, 2}, {0x3B, 7}, {0x3C, 2},
    {0x3D, 7}, {0x3E, 7}, {0x3F, 5}, {0x40, 4}, {0x41, 5}, {0x42, 7}, {0x43, 4}, {0x44, 4}, {0x45, 4},
    {0x46, 7}, {0x47, 7}, {0x48, 7}, {0x49, 7}, {0x4A, 7}, {0x4B, 7}, {0x4C, 7}, {0x4D, 7}, {0x4E, 7}, {0x4F, 7},
    {0x50, 3}, {0x51, 7}, {0x52, 4}, {0x53, 7}, {0x54, 7}, {0x55, 7}, {0x56, 3}, {0x57, 7}, {0x58, 4}, {0x59, 4}, {0x5A, 4},
    {0x5B, 7}, {0x5C, 7}, {0x5D, 7}, {0x5E, 7}, {0x5F, 7}, {0x60, 7}, {0x61, 7}, {0x62, 7}, {0x63, 7}, {0x64, 7},
    {0x65, 7}, {0x66, 2}, {0x67, 7}, {0x68, 4}, {0x69, 3}, {0x6A, 7}, {0x6B, 4}, {0x6C, 4}, {0x6D, 4},
    {0x6E, 7}, {0x6F, 7}, {0x70, 7}, {0x71, 7}, {0x72, 7}, {0x73, 7}, {0x74, 7}, {0x75, 7}, {0x76, 7}, {0x77, 7},
    {0x78, 4}, {0x79, 6}, {0x7A, 7}, {0x7B, 7}, {0x7C, 7},
    {0x7D, 2}, {0x7E, 7}, {0x7F, 7}, {0x80, 7},
};
constexpr int kTonePad = 8;

constexpr int bitsOf(const Field* f, int n) { int s = 0; for (int i = 0; i < n; ++i) s += f[i].bits; return s; }
constexpr int kCommonFieldCount = sizeof(kCommonFields) / sizeof(Field);
constexpr int kToneFieldCount = sizeof(kToneFields) / sizeof(Field);
static_assert(kCommonFieldCount == 74 && kToneFieldCount == 129, "every SysEx byte once");
static_assert(bitsOf(kCommonFields, kCommonFieldCount) + kCommonPad + 4 * (bitsOf(kToneFields, kToneFieldCount) + kTonePad) == 401 * 8,
              "a packed patch is 401 bytes");

} // namespace jv

#endif
