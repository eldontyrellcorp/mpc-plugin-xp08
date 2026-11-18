// editparams.h - the patch edit parameters of XP-08: one row per parameter, as the XP-80's edit
// pages show them. Each row says where the value lives in the patch (srjv::Patch, offsets of
// jvparams.h), its range in the boards' encoding and how the screen shows it.
//
// Tone rows edit the tone chosen with TONE SELECT (parameter "Edit Tone"), as on the XP-80: one
// set of controls, four tones. Common rows edit the patch common. The plugin keeps every one of
// these parameters in step with the edit buffer (the patch being played).
//
// Only parameters the engine plays are here. FXM, structures / booster, analog feel, stretch
// tuning, the EFX types' own parameters (but Drive) and the patch name are not editable.
//
// The ORDER IS PERMANENT (it is the plugin's parameter order): append new rows at the end.
#ifndef XP_EDITPARAMS_H
#define XP_EDITPARAMS_H

#include "jvparams.h"

#include <cstdint>

namespace edit {

enum Fmt : uint8_t {
    Raw,        // the value
    Signed63,   // 0..126, 63 = 0: "-63".."+63"
    Pan,        // 0..127, 64 = centre: "L64".."0".."R63"
    Coarse,     // 0..96: -48..+48
    Fine,       // 0..100: -50..+50
    PDepth,     // 0..24: -12..+12
    Velo,       // 0..125, 50 = 0: -100..+150 %
    Keyf,       // 0..15: the keyfollow table, %
    TimeS,      // 0..14: the time / pan keyfollow table, %
    RndPitch,   // 0..30: cents
    Damp,       // 0..17: Hz, 17 = BYPASS
    Note,       // 0..127: C-1..G9
    Octave,     // 0..6: -3..+3
    PreDelay,   // chorus pre-delay: ms
    Choice,     // the row's choices
    WaveGroup,  // virtual: the wave group (and id) of the tone
    WaveNo,     // virtual: the wave number 1..255 (two nibbles in the patch)
};

struct Def
{
    const char* id;
    const char* name;      // on screen, about 12 characters
    bool tone;             // a tone parameter (else common)
    uint8_t at;            // offset in the tone or the common
    uint8_t max;           // raw range 0..max (WaveNo: 1..max)
    Fmt fmt;
    const char* choices;   // Choice / WaveGroup: '|' separated
};

constexpr const char* kSrc = "OFF|SYS-CTRL1|SYS-CTRL2|MODULATION|BREATH|FOOT|VOLUME|PAN|EXPRESSION|BENDER|AFTERTOUCH";
constexpr const char* kDest = "OFF|PITCH|CUTOFF|RESONANCE|LEVEL|PAN|(6)|(7)|(8)|LFO1 PITCH|LFO2 PITCH|LFO1 TVF|LFO2 TVF|"
                              "LFO1 TVA|LFO2 TVA|LFO1 PAN|LFO2 PAN|LFO1 RATE|LFO2 RATE";
constexpr const char* kLfoWave = "TRI|SIN|SAW|SQR|TRP|S&H|RND|CHS";
constexpr const char* kCurve = "1|2|3|4|5|6|7";
constexpr const char* kOffset = "-100|-50|0|+50|+100";
constexpr const char* kFade = "ON-IN|ON-OUT|OFF-IN|OFF-OUT";
constexpr const char* kOffOn = "OFF|ON";
constexpr const char* kEfx =
    "STEREO-EQ|OVERDRIVE|DISTORTION|PHASER|SPECTRUM|ENHANCER|AUTO-WAH|ROTARY|COMPRESSOR|LIMITER|"
    "HEXA-CHORUS|TREM-CHORUS|SPACE-D|ST-CHORUS|ST-FLANGER|STEP-FLANGER|ST-DELAY|MOD-DELAY|3TAP-DELAY|4TAP-DELAY|"
    "TIMECTRL-DLY|2V-PITCH|FBK-PITCH|REVERB|GATE-REVERB|OD>CHORUS|OD>FLANGER|OD>DELAY|DS>CHORUS|DS>FLANGER|"
    "DS>DELAY|EH>CHORUS|EH>FLANGER|EH>DELAY|CHO>DELAY|FL>DELAY|CHO>FLANGER|CHO/DELAY|FL/DELAY|CHO/FLANGER";

#define T(id, name, at, max, fmt) {id, name, true, (uint8_t)(at), max, fmt, nullptr}
#define TC(id, name, at, list) {id, name, true, (uint8_t)(at), 0, Choice, list}
#define C(id, name, at, max, fmt) {id, name, false, (uint8_t)(at), max, fmt, nullptr}
#define CC(id, name, at, list) {id, name, false, (uint8_t)(at), 0, Choice, list}

constexpr Def kDefs[] = {
    // ---- patch common
    C("c_level", "Patch Level", jv::C_LEVEL, 127, Raw),
    C("c_pan", "Patch Pan", jv::C_PAN, 127, Pan),
    C("c_octave", "Patch Oct", jv::C_OCTAVE, 6, Octave),
    C("c_bend_up", "Bend Up", jv::C_BEND_UP, 12, Raw),
    C("c_bend_down", "Bend Down", jv::C_BEND_DOWN, 48, Raw),
    CC("c_key_assign", "Key Assign", jv::C_KEY_ASSIGN, "POLY|SOLO"),
    CC("c_legato", "Solo Legato", jv::C_SOLO_LEGATO, kOffOn),
    CC("c_porta_sw", "Portamento", jv::C_PORTA_SW, kOffOn),
    CC("c_porta_mode", "Porta Mode", jv::C_PORTA_MODE, "NORMAL|LEGATO"),
    C("c_porta_time", "Porta Time", jv::C_PORTA_TIME, 127, Raw),
    CC("c_velo_range", "Velo Range", jv::C_VELO_RANGE_SW, kOffOn),
    CC("c_ctrl_src2", "Ctrl 2 Src", jv::C_CTRL_SRC2, kSrc),
    CC("c_ctrl_src3", "Ctrl 3 Src", jv::C_CTRL_SRC3, kSrc),
    // effects
    CC("c_efx_type", "EFX Type", jv::C_EFX_TYPE, kEfx),
    C("c_efx_drive", "EFX Drive", jv::C_EFX_PARAM, 127, Raw),
    C("c_efx_mix", "EFX Out Lvl", jv::C_EFX_MIX_SEND, 127, Raw),
    C("c_efx_cho", "EFX>Chorus", jv::C_EFX_CHORUS_SEND, 127, Raw),
    C("c_efx_rev", "EFX>Reverb", jv::C_EFX_REVERB_SEND, 127, Raw),
    C("c_cho_level", "Chorus Lvl", jv::C_CHORUS_LEVEL, 127, Raw),
    C("c_cho_rate", "Chorus Rate", jv::C_CHORUS_RATE, 127, Raw),
    C("c_cho_depth", "Chorus Dpth", jv::C_CHORUS_DEPTH, 127, Raw),
    C("c_cho_pre", "Chorus PreD", jv::C_CHORUS_PREDELAY, 125, PreDelay),
    C("c_cho_fb", "Chorus Fbk", jv::C_CHORUS_FEEDBACK, 127, Raw),
    CC("c_cho_out", "Chorus Out", jv::C_CHORUS_OUTPUT, "MIX|REV|MIX+REV"),
    CC("c_rev_type", "Reverb Type", jv::C_REVERB_TYPE, "ROOM1|ROOM2|STAGE1|STAGE2|HALL1|HALL2|DELAY|PAN-DELAY"),
    C("c_rev_level", "Reverb Lvl", jv::C_REVERB_LEVEL, 127, Raw),
    C("c_rev_time", "Reverb Time", jv::C_REVERB_TIME, 127, Raw),
    C("c_rev_damp", "HF Damp", jv::C_REVERB_HF_DAMP, 17, Damp),
    C("c_rev_fb", "Delay Fbk", jv::C_REVERB_FEEDBACK, 127, Raw),

    // ---- tone: wave
    {"t_wave_group", "Wave Group", true, jv::T_WAVE_GROUP, 0, WaveGroup, "INT-A|INT-B|JV80 INT|PCM|EXP-A|EXP-B|EXP-C|EXP-D"},
    T("t_wave", "Wave", jv::T_WAVE_HI, 255, WaveNo),
    TC("t_wave_gain", "Wave Gain", jv::T_WAVE_GAIN, "-6dB|0dB|+6dB|+12dB"),
    T("t_key_lo", "Key Low", jv::T_KEY_LO, 127, Note),
    T("t_key_hi", "Key High", jv::T_KEY_HI, 127, Note),
    T("t_velo_lo", "Velo Low", jv::T_VELO_LO, 127, Raw),
    T("t_velo_hi", "Velo High", jv::T_VELO_HI, 127, Raw),
    T("t_velo_xfade", "Velo Xfade", jv::T_VELO_XFADE, 127, Raw),
    TC("t_delay_mode", "Delay Mode", jv::T_DELAY_MODE, "NORMAL|HOLD|PLAYMATE|CLOCK-SYNC|TAP-SYNC|KEY-OFF-NRM|KEY-OFF-DCY"),
    T("t_delay_time", "Delay Time", jv::T_DELAY_TIME, 127, Raw),
    TC("t_hold_sw", "Hold Sw", jv::T_HOLD_SW, kOffOn),
    TC("t_bend_sw", "Bend Sw", jv::T_BEND_SW, kOffOn),
    TC("t_volume_sw", "Volume Sw", jv::T_VOLUME_SW, kOffOn),
    TC("t_pan_sw", "Pan Sw", jv::T_PAN_SW, kOffOn),
    // pitch
    T("t_coarse", "Coarse Tune", jv::T_COARSE, 96, Coarse),
    T("t_fine", "Fine Tune", jv::T_FINE, 100, Fine),
    T("t_random_pitch", "Rnd Pitch", jv::T_RANDOM_PITCH, 30, RndPitch),
    T("t_pitch_kf", "Pitch KF", jv::T_PITCH_KF, 15, Keyf),
    T("t_penv_depth", "P-Env Depth", jv::T_PENV_DEPTH, 24, PDepth),
    T("t_penv_velo", "P-Env Velo", jv::T_PENV_VELO, 125, Velo),
    T("t_penv_vt1", "P-Env T1 Vel", jv::T_PENV_VELO_T1, 14, TimeS),
    T("t_penv_vt4", "P-Env T4 Vel", jv::T_PENV_VELO_T4, 14, TimeS),
    T("t_penv_tkf", "P-Env T KF", jv::T_PENV_TIME_KF, 14, TimeS),
    T("t_penv_t1", "P-Env T1", jv::T_PENV_T1 + 0, 127, Raw),
    T("t_penv_t2", "P-Env T2", jv::T_PENV_T1 + 1, 127, Raw),
    T("t_penv_t3", "P-Env T3", jv::T_PENV_T1 + 2, 127, Raw),
    T("t_penv_t4", "P-Env T4", jv::T_PENV_T1 + 3, 127, Raw),
    T("t_penv_l1", "P-Env L1", jv::T_PENV_L1 + 0, 126, Signed63),
    T("t_penv_l2", "P-Env L2", jv::T_PENV_L1 + 1, 126, Signed63),
    T("t_penv_l3", "P-Env L3", jv::T_PENV_L1 + 2, 126, Signed63),
    T("t_penv_l4", "P-Env L4", jv::T_PENV_L1 + 3, 126, Signed63),
    // TVF
    TC("t_filter", "Filter Type", jv::T_FILTER_TYPE, "OFF|LPF|BPF|HPF|PKG"),
    T("t_cutoff", "TVF Cutoff", jv::T_CUTOFF, 127, Raw),
    T("t_reso", "TVF Reso", jv::T_RESONANCE, 127, Raw),
    T("t_cutoff_kf", "Cutoff KF", jv::T_CUTOFF_KF, 15, Keyf),
    T("t_fenv_depth", "F-Env Depth", jv::T_FENV_DEPTH, 126, Signed63),
    TC("t_fenv_curve", "F-Env Curve", jv::T_FENV_CURVE, kCurve),
    T("t_fenv_velo", "F-Env Velo", jv::T_FENV_VELO, 125, Velo),
    T("t_fenv_vt1", "F-Env T1 Vel", jv::T_FENV_VELO_T1, 14, TimeS),
    T("t_fenv_vt4", "F-Env T4 Vel", jv::T_FENV_VELO_T4, 14, TimeS),
    T("t_fenv_tkf", "F-Env T KF", jv::T_FENV_TIME_KF, 14, TimeS),
    T("t_fenv_t1", "F-Env T1", jv::T_FENV_T1 + 0, 127, Raw),
    T("t_fenv_t2", "F-Env T2", jv::T_FENV_T1 + 1, 127, Raw),
    T("t_fenv_t3", "F-Env T3", jv::T_FENV_T1 + 2, 127, Raw),
    T("t_fenv_t4", "F-Env T4", jv::T_FENV_T1 + 3, 127, Raw),
    T("t_fenv_l1", "F-Env L1", jv::T_FENV_L1 + 0, 127, Raw),
    T("t_fenv_l2", "F-Env L2", jv::T_FENV_L1 + 1, 127, Raw),
    T("t_fenv_l3", "F-Env L3", jv::T_FENV_L1 + 2, 127, Raw),
    T("t_fenv_l4", "F-Env L4", jv::T_FENV_L1 + 3, 127, Raw),
    // TVA
    T("t_level", "Tone Level", jv::T_LEVEL, 127, Raw),
    TC("t_bias_dir", "Bias Dir", jv::T_BIAS_DIR, "LOWER|UPPER|LOW&UP|ALL"),
    T("t_bias_point", "Bias Point", jv::T_BIAS_POINT, 127, Note),
    T("t_bias_level", "Bias Level", jv::T_BIAS_LEVEL, 14, TimeS),
    TC("t_aenv_curve", "A-Env Curve", jv::T_AENV_CURVE, kCurve),
    T("t_aenv_velo", "A-Env Velo", jv::T_AENV_VELO, 125, Velo),
    T("t_aenv_vt1", "A-Env T1 Vel", jv::T_AENV_VELO_T1, 14, TimeS),
    T("t_aenv_vt4", "A-Env T4 Vel", jv::T_AENV_VELO_T4, 14, TimeS),
    T("t_aenv_tkf", "A-Env T KF", jv::T_AENV_TIME_KF, 14, TimeS),
    T("t_aenv_t1", "A-Env T1", jv::T_AENV_T1 + 0, 127, Raw),
    T("t_aenv_t2", "A-Env T2", jv::T_AENV_T1 + 1, 127, Raw),
    T("t_aenv_t3", "A-Env T3", jv::T_AENV_T1 + 2, 127, Raw),
    T("t_aenv_t4", "A-Env T4", jv::T_AENV_T1 + 3, 127, Raw),
    T("t_aenv_l1", "A-Env L1", jv::T_AENV_L1 + 0, 127, Raw),
    T("t_aenv_l2", "A-Env L2", jv::T_AENV_L1 + 1, 127, Raw),
    T("t_aenv_l3", "A-Env L3", jv::T_AENV_L1 + 2, 127, Raw),
    // pan and output
    T("t_pan", "Tone Pan", jv::T_PAN, 127, Pan),
    T("t_pan_kf", "Pan KF", jv::T_PAN_KF, 14, TimeS),
    T("t_random_pan", "Random Pan", jv::T_RANDOM_PAN, 63, Raw),
    T("t_alt_pan", "Alt Pan", jv::T_ALT_PAN, 127, Pan),
    TC("t_output", "Output", jv::T_OUTPUT, "MIX|EFX"),
    T("t_send", "Output Lvl", jv::T_SEND_LEVEL, 127, Raw),
    T("t_cho_send", "Chorus Send", jv::T_CHORUS_SEND, 127, Raw),
    T("t_rev_send", "Reverb Send", jv::T_REVERB_SEND, 127, Raw),
    // LFO 1
    TC("t_l1_wave", "LFO1 Wave", jv::T_LFO1 + jv::LFO_WAVE, kLfoWave),
    T("t_l1_rate", "LFO1 Rate", jv::T_LFO1 + jv::LFO_RATE, 127, Raw),
    TC("t_l1_offset", "LFO1 Offset", jv::T_LFO1 + jv::LFO_OFFSET, kOffset),
    TC("t_l1_sync", "LFO1 KeySync", jv::T_LFO1 + jv::LFO_KEY_SYNC, kOffOn),
    T("t_l1_delay", "LFO1 Delay", jv::T_LFO1 + jv::LFO_DELAY, 127, Raw),
    TC("t_l1_fade_mode", "LFO1 Fade", jv::T_LFO1 + jv::LFO_FADE_MODE, kFade),
    T("t_l1_fade_time", "LFO1 FadeT", jv::T_LFO1 + jv::LFO_FADE_TIME, 127, Raw),
    T("t_l1_pitch", "LFO1 Pitch", jv::T_PITCH_LFO1, 126, Signed63),
    T("t_l1_tvf", "LFO1 TVF", jv::T_FILTER_LFO1, 126, Signed63),
    T("t_l1_tva", "LFO1 TVA", jv::T_AMP_LFO1, 126, Signed63),
    T("t_l1_pan", "LFO1 Pan", jv::T_PAN_LFO1, 126, Signed63),
    // LFO 2
    TC("t_l2_wave", "LFO2 Wave", jv::T_LFO2 + jv::LFO_WAVE, kLfoWave),
    T("t_l2_rate", "LFO2 Rate", jv::T_LFO2 + jv::LFO_RATE, 127, Raw),
    TC("t_l2_offset", "LFO2 Offset", jv::T_LFO2 + jv::LFO_OFFSET, kOffset),
    TC("t_l2_sync", "LFO2 KeySync", jv::T_LFO2 + jv::LFO_KEY_SYNC, kOffOn),
    T("t_l2_delay", "LFO2 Delay", jv::T_LFO2 + jv::LFO_DELAY, 127, Raw),
    TC("t_l2_fade_mode", "LFO2 Fade", jv::T_LFO2 + jv::LFO_FADE_MODE, kFade),
    T("t_l2_fade_time", "LFO2 FadeT", jv::T_LFO2 + jv::LFO_FADE_TIME, 127, Raw),
    T("t_l2_pitch", "LFO2 Pitch", jv::T_PITCH_LFO2, 126, Signed63),
    T("t_l2_tvf", "LFO2 TVF", jv::T_FILTER_LFO2, 126, Signed63),
    T("t_l2_tva", "LFO2 TVA", jv::T_AMP_LFO2, 126, Signed63),
    T("t_l2_pan", "LFO2 Pan", jv::T_PAN_LFO2, 126, Signed63),
    // matrix control: controller 1 (modulation), 2 and 3 (Ctrl 2 / 3 Src), four destinations each
    TC("t_m1_d1", "Ctl1 Dest1", jv::T_MATRIX + 0, kDest), T("t_m1_s1", "Ctl1 Sens1", jv::T_MATRIX + 1, 126, Signed63),
    TC("t_m1_d2", "Ctl1 Dest2", jv::T_MATRIX + 2, kDest), T("t_m1_s2", "Ctl1 Sens2", jv::T_MATRIX + 3, 126, Signed63),
    TC("t_m1_d3", "Ctl1 Dest3", jv::T_MATRIX + 4, kDest), T("t_m1_s3", "Ctl1 Sens3", jv::T_MATRIX + 5, 126, Signed63),
    TC("t_m1_d4", "Ctl1 Dest4", jv::T_MATRIX + 6, kDest), T("t_m1_s4", "Ctl1 Sens4", jv::T_MATRIX + 7, 126, Signed63),
    TC("t_m2_d1", "Ctl2 Dest1", jv::T_MATRIX + 8, kDest), T("t_m2_s1", "Ctl2 Sens1", jv::T_MATRIX + 9, 126, Signed63),
    TC("t_m2_d2", "Ctl2 Dest2", jv::T_MATRIX + 10, kDest), T("t_m2_s2", "Ctl2 Sens2", jv::T_MATRIX + 11, 126, Signed63),
    TC("t_m2_d3", "Ctl2 Dest3", jv::T_MATRIX + 12, kDest), T("t_m2_s3", "Ctl2 Sens3", jv::T_MATRIX + 13, 126, Signed63),
    TC("t_m2_d4", "Ctl2 Dest4", jv::T_MATRIX + 14, kDest), T("t_m2_s4", "Ctl2 Sens4", jv::T_MATRIX + 15, 126, Signed63),
    TC("t_m3_d1", "Ctl3 Dest1", jv::T_MATRIX + 16, kDest), T("t_m3_s1", "Ctl3 Sens1", jv::T_MATRIX + 17, 126, Signed63),
    TC("t_m3_d2", "Ctl3 Dest2", jv::T_MATRIX + 18, kDest), T("t_m3_s2", "Ctl3 Sens2", jv::T_MATRIX + 19, 126, Signed63),
    TC("t_m3_d3", "Ctl3 Dest3", jv::T_MATRIX + 20, kDest), T("t_m3_s3", "Ctl3 Sens3", jv::T_MATRIX + 21, 126, Signed63),
    TC("t_m3_d4", "Ctl3 Dest4", jv::T_MATRIX + 22, kDest), T("t_m3_s4", "Ctl3 Sens4", jv::T_MATRIX + 23, 126, Signed63),
};

#undef T
#undef TC
#undef C
#undef CC

constexpr int kCount = (int)(sizeof(kDefs) / sizeof(Def));

// The wave group choices (WaveGroup rows): what the tone's group and id bytes mean
enum Group { G_INT_A, G_INT_B, G_JV80_INT, G_PCM, G_EXP_A };   // EXP-A..D = G_EXP_A + slot

} // namespace edit

#endif
