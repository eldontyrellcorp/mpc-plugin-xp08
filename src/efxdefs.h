// efxdefs.h - the 40 EFX types' parameters: for each type, what each of the patch's 12 EFX
// parameter values is (name, how it is shown, how it converts to milliseconds, hertz, dB...).
//
// Read from the JV-1080's own EFX edit screens in its program ROM (its owner's dump): for each
// type, three pages of four fields, each naming the parameter number it edits, a display format
// and the format's list or table. The time and rate scales are the firmware's tables, reduced
// here to their rules (steps per range); the phaser's manual frequency and the gate time are not
// in those tables and use a rule of this plugin's (marked below). No ROM data is in this file.
#ifndef XP_EFXDEFS_H
#define XP_EFXDEFS_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace efx {

enum Kind : uint8_t {
    K_NONE,      // not used by this type
    K_RAW,       // 0..127
    K_RAW15,     // 0..15
    K_PLUS1,     // 0..4, shown 1..5
    K_PHASE,     // 0..90, x2 degrees
    K_PAN,       // 0..127: L64..63R
    K_BAL_DE,    // 0..100: dry / effect balance D100:0E .. D0:100E
    K_BAL_AB,    // 0..100: A100:0B .. A0:100B
    K_FBK,       // 0..98: -98..+98 % in steps of 2
    K_FINE,      // 0..100: -50..+50 cents
    K_SIGNED,    // 0..2a: -a..+a (dB, semitones)
    K_LIST,      // a list (arg: L_*)
    K_TABLE,     // a time or rate scale (arg: T_*)
};

enum List : uint8_t { L_LOWFREQ, L_HIFREQ, L_DAMP, L_FREQ, L_Q, L_FBMODE, L_PHASE, L_SPEED, L_RATIO, L_REVTYPE,
                      L_GATETYPE, L_FILTER3, L_FILTER2, L_POSTGAIN, L_AMP, L_COUNT };
enum Table : uint8_t { T_PREDELAY, T_DELAY500, T_DELAY1000, T_TAP, T_RATE, T_MANUAL, T_GATE };

struct Field { const char* name; Kind kind; uint8_t arg; };
struct TypeDef { const char* ctrl[2]; Field f[12]; };   // ctrl: the two parameters the EFX controllers move

constexpr int kFreqs[17] = {200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000};

inline int listSize(int l)
{
    static const int n[L_COUNT] = {2, 2, 18, 17, 5, 2, 2, 2, 4, 6, 4, 3, 2, 4, 4};
    return l < L_COUNT ? n[l] : 1;
}

inline const char* listText(int l, int i)
{
    static const char* const lowF[] = {"200 Hz", "400 Hz"}, *hiF[] = {"4000 Hz", "8000 Hz"};
    static const char* const q[] = {"0.5", "1.0", "2.0", "4.0", "9.0"}, *fb[] = {"NORMAL", "CROSS"}, *ph[] = {"NORMAL", "INVERT"};
    static const char* const sp[] = {"SLOW", "FAST"}, *ra[] = {"1.5:1", "2.0:1", "4.0:1", "100:1"};
    static const char* const rv[] = {"ROOM1", "ROOM2", "STAGE1", "STAGE2", "HALL1", "HALL2"};
    static const char* const gt[] = {"NORMAL", "REVERSE", "SWEEP1", "SWEEP2"}, *f3[] = {"OFF", "LPF", "HPF"}, *f2[] = {"LPF", "BPF"};
    static const char* const pg[] = {"0 dB", "+6 dB", "+12 dB", "+18 dB"}, *amp[] = {"SMALL", "BUILT-IN", "2-STACK", "3-STACK"};
    static const char* const* lists[L_COUNT] = {lowF, hiF, nullptr, nullptr, q, fb, ph, sp, ra, rv, gt, f3, f2, pg, amp};
    if (i < 0 || i >= listSize(l)) return "";
    if (l == L_DAMP || l == L_FREQ) return nullptr;   // numbers: see listHz
    return lists[l][i];
}

// L_DAMP / L_FREQ: Hz (0 = BYPASS)
inline int listHz(int l, int i) { return (l == L_DAMP && i >= 17) ? 0 : kFreqs[i < 0 ? 0 : (i > 16 ? 16 : i)]; }

inline int tableSize(int t)
{
    static const int n[] = {126, 127, 121, 126, 126, 127, 100};
    return n[t];
}

// The note values of the tap delays' top ten steps (116..125), in quarter notes, at the patch's tempo
constexpr float kTapNotes[10] = {0.25f, 1.0f / 3, 0.375f, 0.5f, 2.0f / 3, 0.75f, 1.0f, 4.0f / 3, 1.5f, 2.0f};
constexpr const char* kTapNoteNames[10] = {"1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2"};

// a step of a scale -> ms (times) or Hz (rates)
inline float tableValue(int t, int v, float bpm = 120)
{
    switch (t)
    {
        case T_PREDELAY:   // 0.1 ms steps to 5, 0.5 to 10, 1 to 50, 2 to 100 ms
            return v <= 50 ? v * 0.1f : v <= 60 ? 5 + (v - 50) * 0.5f : v <= 100 ? 10.0f + (v - 60) : 50.0f + (v - 100) * 2;
        case T_DELAY500:   // 0.1 ms steps to 5, 0.5 to 10, 1 to 40, 10 to 300, 20 to 500 ms
            return v <= 50 ? v * 0.1f : v <= 60 ? 5 + (v - 50) * 0.5f : v <= 90 ? 10.0f + (v - 60) : v <= 116 ? 40.0f + (v - 90) * 10 : 300.0f + (v - 116) * 20;
        case T_DELAY1000:  // 200..600 ms in 5 ms steps, then to 1000 in 10 ms
            return v <= 80 ? 200.0f + v * 5 : 600.0f + (v - 80) * 10;
        case T_TAP:        // 200..550 ms in 5 ms steps, then to 1000 in 10 ms; 116..125: note values
            if (v >= 116) return kTapNotes[(v - 116) > 9 ? 9 : (v - 116)] * 60000.0f / bpm;
            return v <= 70 ? 200.0f + v * 5 : 550.0f + (v - 70) * 10;
        case T_RATE:       // 0.05 Hz steps to 5, 0.1 to 7, 0.5 to 10 Hz
            return v <= 99 ? 0.05f + v * 0.05f : v <= 119 ? 5.0f + (v - 99) * 0.1f : 7.0f + (v - 119) * 0.5f;
        case T_MANUAL:     // (this plugin's rule) 100 Hz .. 8 kHz, evenly in octaves
            return 100.0f * std::pow(80.0f, (float)v / 126.0f);
        case T_GATE:       // (this plugin's rule) 5..500 ms in 5 ms steps
            return 5.0f + v * 5.0f;
        default: return 0;
    }
}

// the highest value a field takes
inline int fieldMax(const Field& f)
{
    switch (f.kind)
    {
        case K_RAW: return 127;
        case K_RAW15: return 15;
        case K_PLUS1: return 4;
        case K_PHASE: return 90;
        case K_PAN: return 127;
        case K_BAL_DE: case K_BAL_AB: case K_FINE: return 100;
        case K_FBK: return 98;
        case K_SIGNED: return 2 * f.arg;
        case K_LIST: return listSize(f.arg) - 1;
        case K_TABLE: return tableSize(f.arg) - 1;
        default: return 0;
    }
}

// what the screen shows
inline std::string fieldText(const Field& f, int v, float bpm = 120)
{
    char b[32];
    if (v > fieldMax(f)) v = fieldMax(f);
    switch (f.kind)
    {
        case K_NONE: return "";
        case K_RAW: case K_RAW15: std::snprintf(b, sizeof b, "%d", v); break;
        case K_PLUS1: std::snprintf(b, sizeof b, "%d", v + 1); break;
        case K_PHASE: std::snprintf(b, sizeof b, "%d deg", v * 2); break;
        case K_PAN: if (v == 64) return "0"; std::snprintf(b, sizeof b, v < 64 ? "L%d" : "%dR", v < 64 ? 64 - v : v - 64); break;
        case K_BAL_DE: std::snprintf(b, sizeof b, "D%d:%dE", 100 - v, v); break;
        case K_BAL_AB: std::snprintf(b, sizeof b, "A%d:%dB", 100 - v, v); break;
        case K_FBK: std::snprintf(b, sizeof b, "%+d%%", (v - 49) * 2); break;
        case K_FINE: std::snprintf(b, sizeof b, "%+d", v - 50); break;
        case K_SIGNED: std::snprintf(b, sizeof b, "%+d", v - f.arg); break;
        case K_LIST:
            if (const char* s = listText(f.arg, v)) return s;
            if (int hz = listHz(f.arg, v)) std::snprintf(b, sizeof b, "%d Hz", hz); else return "BYPASS";
            break;
        case K_TABLE:
        {
            if (f.arg == T_TAP && v >= 116) return kTapNoteNames[v - 116];
            float x = tableValue(f.arg, v, bpm);
            if (f.arg == T_RATE) std::snprintf(b, sizeof b, "%.2f Hz", x);
            else if (f.arg == T_MANUAL) std::snprintf(b, sizeof b, "%d Hz", (int)std::lround(x));
            else std::snprintf(b, sizeof b, x < 10 ? "%.1f ms" : "%.0f ms", x);
            break;
        }
    }
    return b;
}

// The 40 types, in the patch's EFX type order; f[k] is the patch's EFX parameter k
constexpr TypeDef kTypes[40] = {
    {{"Level", ""}, {{"LowFreq", K_LIST, L_LOWFREQ},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Freq", K_LIST, L_HIFREQ},
      {"Hi Gain", K_SIGNED, 15},
      {"P1 Freq", K_LIST, L_DAMP},
      {"P1 Q", K_LIST, L_Q},
      {"P1 Gain", K_SIGNED, 15},
      {"P2 Freq", K_LIST, L_DAMP},
      {"P2 Q", K_LIST, L_Q},
      {"P2 Gain", K_SIGNED, 15},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0}}},
    {{"Drive", "Pan"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Amp Type", K_LIST, L_AMP},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Drive", "Pan"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Amp Type", K_LIST, L_AMP},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Manual", "Rate"}, {{"Manual", K_TABLE, T_MANUAL},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Resonance", K_RAW, 0},
      {"Mix", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Level"}, {{"Band 1", K_SIGNED, 15},
      {"Band 2", K_SIGNED, 15},
      {"Band 3", K_SIGNED, 15},
      {"Band 4", K_SIGNED, 15},
      {"Band 5", K_SIGNED, 15},
      {"Band 6", K_SIGNED, 15},
      {"Band 7", K_SIGNED, 15},
      {"Band 8", K_SIGNED, 15},
      {"Width", K_PLUS1, 0},
      {"Pan", K_PAN, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0}}},
    {{"Sens", "Mix"}, {{"Sens", K_RAW, 0},
      {"Mix", K_RAW, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Rate", "Manual"}, {{"Filter", K_LIST, L_FILTER2},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Sens", K_RAW, 0},
      {"Manual", K_RAW, 0},
      {"Peak", K_RAW, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Speed", "Level"}, {{"Hi Slow", K_TABLE, T_RATE},
      {"Low Slow", K_TABLE, T_RATE},
      {"Hi Fast", K_TABLE, T_RATE},
      {"Low Fast", K_TABLE, T_RATE},
      {"Speed", K_LIST, L_SPEED},
      {"Hi Accel", K_RAW15, 0},
      {"Low Accel", K_RAW15, 0},
      {"Hi Level", K_RAW, 0},
      {"Low Level", K_RAW, 0},
      {"Separation", K_RAW, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Level"}, {{"Sustain", K_RAW, 0},
      {"Attack", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Post Gain", K_LIST, L_POSTGAIN},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Level"}, {{"Thresh", K_RAW, 0},
      {"Release", K_RAW, 0},
      {"Ratio", K_LIST, L_RATIO},
      {"Pan", K_PAN, 0},
      {"Gain", K_LIST, L_POSTGAIN},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Rate", "Balance"}, {{"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Dly Dev", K_RAW, 0},
      {"Dpt Dev", K_SIGNED, 20},
      {"Pan Dev", K_RAW, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"TrmRate", "Balance"}, {{"Pre Dly", K_TABLE, T_PREDELAY},
      {"ChoRate", K_TABLE, T_RATE},
      {"Cho Dpt", K_RAW, 0},
      {"TrmRate", K_TABLE, T_RATE},
      {"Trm Sep", K_RAW, 0},
      {"Phase", K_PHASE, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Rate", "Balance"}, {{"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Phase", K_PHASE, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Rate", "Balance"}, {{"Filter", K_LIST, L_FILTER3},
      {"Cutoff", K_LIST, L_FREQ},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Phase", K_PHASE, 0},
      {nullptr, K_NONE, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0}}},
    {{"Rate", "Fbk"}, {{"Filter", K_LIST, L_FILTER3},
      {"Cutoff", K_LIST, L_FREQ},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Phase", K_PHASE, 0},
      {"Fbk", K_FBK, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0}}},
    {{"StpRate", "Fbk"}, {{"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Fbk", K_FBK, 0},
      {"Step Rate", K_TABLE, T_RATE},
      {"Phase", K_PHASE, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Fbk", "Balance"}, {{"Mode", K_LIST, L_FBMODE},
      {"Delay L", K_TABLE, T_DELAY500},
      {"Delay R", K_TABLE, T_DELAY500},
      {"Phase L", K_LIST, L_PHASE},
      {"Phase R", K_LIST, L_PHASE},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0}}},
    {{"Rate", "Balance"}, {{"Mode", K_LIST, L_FBMODE},
      {"Delay L", K_TABLE, T_DELAY500},
      {"Delay R", K_TABLE, T_DELAY500},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Phase", K_PHASE, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0}}},
    {{"Fbk", "Balance"}, {{"Delay L", K_TABLE, T_TAP},
      {"Delay R", K_TABLE, T_TAP},
      {"Delay C", K_TABLE, T_TAP},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Level L", K_RAW, 0},
      {"Level R", K_RAW, 0},
      {"Level C", K_RAW, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0}}},
    {{"Fbk", "Balance"}, {{"Delay 1", K_TABLE, T_TAP},
      {"Delay 2", K_TABLE, T_TAP},
      {"Delay 3", K_TABLE, T_TAP},
      {"Delay 4", K_TABLE, T_TAP},
      {"Level 1", K_RAW, 0},
      {"Level 2", K_RAW, 0},
      {"Level 3", K_RAW, 0},
      {"Level 4", K_RAW, 0},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0}}},
    {{"Delay", "Fbk"}, {{"Delay", K_TABLE, T_DELAY1000},
      {"Feedback", K_FBK, 0},
      {"Accel", K_RAW15, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Pan", K_PAN, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"CoarseA", "CoarseB"}, {{"Mode", K_PLUS1, 0},
      {"CoarseA", K_SIGNED, 24},
      {"CoarseB", K_SIGNED, 24},
      {"Fine A", K_FINE, 0},
      {"Fine B", K_FINE, 0},
      {"PreDlyA", K_TABLE, T_DELAY500},
      {"PreDlyB", K_TABLE, T_DELAY500},
      {"Pan A", K_PAN, 0},
      {"Pan B", K_PAN, 0},
      {"Lvl Bal", K_BAL_AB, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0}}},
    {{"Coarse", "Fbk"}, {{"Mode", K_PLUS1, 0},
      {"Coarse", K_SIGNED, 24},
      {"Fine", K_FINE, 0},
      {"Pre Dly", K_TABLE, T_DELAY500},
      {"Feedback", K_FBK, 0},
      {"Pan", K_PAN, 0},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Time", "Balance"}, {{"Type", K_LIST, L_REVTYPE},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Time", K_RAW, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Balance", "Level"}, {{"Type", K_LIST, L_GATETYPE},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Gate Time", K_TABLE, T_GATE},
      {"Low Gain", K_SIGNED, 15},
      {"Hi Gain", K_SIGNED, 15},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Balance"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Balance"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Fbk", K_FBK, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Balance"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Balance"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Balance"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Fbk", K_FBK, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Pan", "Balance"}, {{"Drive", K_RAW, 0},
      {"Pan", K_PAN, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Sens", "Balance"}, {{"Sens", K_RAW, 0},
      {"Mix", K_RAW, 0},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Sens", "Balance"}, {{"Sens", K_RAW, 0},
      {"Mix", K_RAW, 0},
      {"Pre Dly", K_TABLE, T_PREDELAY},
      {"Rate", K_TABLE, T_RATE},
      {"Depth", K_RAW, 0},
      {"Fbk", K_FBK, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Sens", "Balance"}, {{"Sens", K_RAW, 0},
      {"Mix", K_RAW, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {nullptr, K_NONE, 0},
      {"Balance", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Cho Bal", "Dly Bal"}, {{"Cho Dly", K_TABLE, T_PREDELAY},
      {"ChoRate", K_TABLE, T_RATE},
      {"Cho Dpt", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {"Cho Bal", K_BAL_DE, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Dly Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Dly Bal", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Flg Bal", "Dly Bal"}, {{"Flg Dly", K_TABLE, T_PREDELAY},
      {"FlgRate", K_TABLE, T_RATE},
      {"Flg Dpt", K_RAW, 0},
      {"Flg Fbk", K_FBK, 0},
      {"Flg Bal", K_BAL_DE, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Dly Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Dly Bal", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Cho Bal", "Flg Bal"}, {{"Cho Dly", K_TABLE, T_PREDELAY},
      {"ChoRate", K_TABLE, T_RATE},
      {"Cho Dpt", K_RAW, 0},
      {"Cho Bal", K_BAL_DE, 0},
      {"Flg Dly", K_TABLE, T_PREDELAY},
      {"FlgRate", K_TABLE, T_RATE},
      {"Flg Dpt", K_RAW, 0},
      {"Flg Fbk", K_FBK, 0},
      {"Flg Bal", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Cho Bal", "Dly Bal"}, {{"Cho Dly", K_TABLE, T_PREDELAY},
      {"ChoRate", K_TABLE, T_RATE},
      {"Cho Dpt", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {"Cho Bal", K_BAL_DE, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Dly Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Dly Bal", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Flg Bal", "Dly Bal"}, {{"Flg Dly", K_TABLE, T_PREDELAY},
      {"FlgRate", K_TABLE, T_RATE},
      {"Flg Dpt", K_RAW, 0},
      {"Flg Fbk", K_FBK, 0},
      {"Flg Bal", K_BAL_DE, 0},
      {"Delay", K_TABLE, T_DELAY500},
      {"Dly Fbk", K_FBK, 0},
      {"HF Damp", K_LIST, L_DAMP},
      {"Dly Bal", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
    {{"Cho Bal", "Flg Bal"}, {{"Cho Dly", K_TABLE, T_PREDELAY},
      {"ChoRate", K_TABLE, T_RATE},
      {"Cho Dpt", K_RAW, 0},
      {"Cho Bal", K_BAL_DE, 0},
      {"Flg Dly", K_TABLE, T_PREDELAY},
      {"FlgRate", K_TABLE, T_RATE},
      {"Flg Dpt", K_RAW, 0},
      {"Flg Fbk", K_FBK, 0},
      {"Flg Bal", K_BAL_DE, 0},
      {"Level", K_RAW, 0},
      {nullptr, K_NONE, 0},
      {nullptr, K_NONE, 0}}},
};

// Each type's initial values, set when the EFX type is changed on the EFX EDIT page (as the JV-1080
// loads a type's settings when it is picked): per parameter, the value the boards' patches use most
// for that type; for the types they do not use, middling values. A balance the patches mostly leave
// all dry starts at D70:30E instead, so that a newly picked type is heard.
constexpr uint8_t kDefaults[40][12] = {
    {0, 15, 0, 15, 7, 1, 15, 7, 1, 15, 127, 0},
    {127, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0, 0},
    {127, 64, 3, 15, 15, 127, 0, 0, 0, 0, 0, 0},
    {30, 19, 64, 0, 127, 64, 127, 0, 0, 0, 0, 0},
    {15, 15, 15, 15, 15, 15, 15, 30, 0, 64, 127, 0},
    {127, 64, 15, 15, 127, 0, 0, 0, 0, 0, 0, 0},
    {0, 19, 64, 0, 64, 100, 127, 0, 0, 0, 0, 0},
    {21, 0, 121, 121, 0, 10, 10, 127, 127, 127, 127, 0},
    {127, 64, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0},
    {64, 32, 2, 64, 1, 15, 15, 127, 0, 0, 0, 0},
    {5, 9, 20, 0, 20, 20, 50, 127, 0, 0, 0, 0},
    {0, 9, 50, 39, 127, 90, 50, 127, 0, 0, 0, 0},
    {10, 9, 20, 90, 15, 15, 50, 127, 0, 0, 0, 0},
    {2, 7, 10, 9, 127, 90, 0, 15, 15, 50, 127, 0},
    {2, 7, 10, 0, 50, 90, 89, 15, 15, 50, 127, 0},
    {5, 9, 50, 89, 49, 90, 15, 15, 50, 127, 0, 0},
    {0, 126, 126, 0, 0, 59, 17, 15, 15, 30, 127, 0},
    {0, 126, 126, 59, 17, 4, 20, 90, 15, 15, 50, 127},
    {10, 60, 115, 59, 17, 127, 127, 127, 15, 15, 50, 127},
    {115, 90, 60, 10, 127, 127, 127, 127, 59, 17, 50, 127},
    {120, 66, 10, 17, 64, 15, 15, 50, 127, 0, 0, 0},
    {0, 24, 24, 50, 50, 0, 0, 0, 127, 50, 50, 127},
    {4, 24, 50, 126, 59, 64, 15, 15, 50, 127, 0, 0},
    {5, 10, 100, 17, 15, 15, 50, 127, 0, 0, 0, 0},
    {1, 0, 99, 15, 15, 50, 127, 0, 0, 0, 0, 0},
    {64, 64, 20, 10, 64, 0, 50, 127, 0, 0, 0, 0},
    {127, 64, 10, 1, 84, 75, 50, 60, 0, 0, 0, 0},
    {64, 64, 105, 69, 17, 50, 127, 0, 0, 0, 0, 0},
    {127, 64, 10, 9, 77, 0, 50, 42, 0, 0, 0, 0},
    {64, 64, 20, 10, 64, 69, 50, 127, 0, 0, 0, 0},
    {127, 64, 15, 98, 16, 50, 127, 0, 0, 0, 0, 0},
    {127, 64, 10, 9, 47, 0, 50, 127, 0, 0, 0, 0},
    {127, 68, 10, 0, 40, 73, 50, 127, 0, 0, 0, 0},
    {127, 50, 111, 59, 17, 0, 50, 127, 0, 0, 0, 0},
    {10, 9, 107, 0, 50, 126, 75, 15, 50, 127, 0, 0},
    {2, 2, 40, 49, 50, 126, 69, 14, 50, 127, 0, 0},
    {20, 10, 64, 50, 20, 10, 64, 69, 50, 127, 0, 0},
    {75, 9, 20, 0, 50, 119, 59, 17, 50, 127, 0, 0},
    {20, 10, 64, 69, 50, 105, 69, 17, 50, 127, 0, 0},
    {10, 0, 20, 50, 10, 4, 21, 89, 50, 127, 0, 0}
};

} // namespace efx

#endif
