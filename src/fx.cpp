// fx.cpp - see fx.h.
#include "fx.h"
#include "efxdefs.h"
#include "jvparams.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xp {

namespace {
constexpr float kTwoPi = 6.28318530718f;
inline float fastTanh(float x) { if (x > 3) return 1; if (x < -3) return -1; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
inline float sinApprox(float ph)   // ph in [0, 1)
{
    float x = ph * kTwoPi - 3.14159265f;          // -pi..pi
    float y = 1.27323954f * x - 0.405284735f * x * std::fabs(x);
    return -(0.225f * (y * std::fabs(y) - y) + y);
}
}

// ------------------------------------------------------------------ DelayLine
void DelayLine::alloc(size_t minLen)
{
    size_t n = 1;
    while (n < minLen + 2) n <<= 1;
    buf.assign(n, 0.0f);
    mask = (uint32_t)n - 1;
    w = 0;
}
void DelayLine::clear() { std::fill(buf.begin(), buf.end(), 0.0f); }

// ------------------------------------------------------------------ ModDelay
void ModDelay::alloc(float sampleRate)
{
    sr = sampleRate;
    l.alloc((size_t)(sr * 0.14f));
    r.alloc((size_t)(sr * 0.14f));
}
void ModDelay::clear() { l.clear(); r.clear(); fbL = fbR = 0; }
void ModDelay::set(float rateHz, float baseMs, float depthMs, float fb)
{
    rate = rateHz / sr;
    base = baseMs * 0.001f * sr;
    depth = depthMs * 0.001f * sr;
    feedback = fb;
}
void ModDelay::process(float inL, float inR, float* outL, float* outR)
{
    phase += rate;
    if (phase >= 1) phase -= 1;
    float m1 = sinApprox(phase), ph2 = phase + 0.25f;
    float m2 = sinApprox(ph2 >= 1 ? ph2 - 1 : ph2);
    l.push(inL + fbL * feedback);
    r.push(inR + fbR * feedback);
    float a = l.tap(base + depth * (1 + m1) + 2), b = r.tap(base + depth * (1 + m2) + 2);
    fbL = a; fbR = b;
    *outL = a; *outR = b;
}

// ------------------------------------------------------------------ Reverb
void Reverb::alloc(float sampleRate)
{
    sr = sampleRate;
    for (DelayLine& d : lines) d.alloc((size_t)(sr * 0.12f));
    pre.alloc((size_t)(sr * 0.12f));   // pre-delays to 100 ms
    dl.alloc((size_t)(sr * 1.3f));
    dr.alloc((size_t)(sr * 1.3f));
}
void Reverb::clear()
{
    for (DelayLine& d : lines) d.clear();
    pre.clear(); dl.clear(); dr.clear();
    std::memset(lp, 0, sizeof(lp));
}
void Reverb::set(int type, float time01, float hfHz, float feedback01)
{
    // line lengths (ms) for ROOM1, ROOM2, STAGE1, STAGE2, HALL1, HALL2: mutually prime-ish sets
    static const float sizes[6] = {0.45f, 0.6f, 0.75f, 0.85f, 1.0f, 1.15f};
    static const float base[8] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.3f};
    damp = hfHz <= 0 ? 0.0f : std::exp(-kTwoPi * hfHz / sr);
    if (type >= 6)
    {
        mode = type == 6 ? 1 : 2;
        delay = (10.0f + time01 * 990.0f) * 0.001f * sr;     // TIME as the delay, 10 ms .. 1 s
        fb = feedback01 * 0.9f;
        return;
    }
    mode = 0;
    float s = sizes[type < 0 ? 0 : type];
    float rt60 = 0.3f * std::pow(30.0f, time01) * (0.6f + 0.4f * s);   // 0.3 .. ~9 s
    preDelay = (type >= 4 ? 0.018f : 0.008f) * sr;
    for (int i = 0; i < 8; ++i)
    {
        len[i] = (int)(base[i] * s * 0.001f * sr);
        g[i] = std::pow(10.0f, -3.0f * (float)len[i] / (rt60 * sr));
    }
}
void Reverb::process(float in, float* outL, float* outR)
{
    if (mode != 0)
    {
        float a = dl.tap(delay), b = dr.tap(mode == 2 ? delay * 0.5f : delay * 1.01f);
        if (mode == 1) { dl.push(in + a * fb); dr.push(in + b * fb); }
        else { dl.push(in + b * fb); dr.push(a); }            // ping-pong
        *outL = a; *outR = b;
        return;
    }
    pre.push(in);
    float x = pre.tap(preDelay + 1);
    float o[8];
    float sum = 0;
    for (int i = 0; i < 8; ++i)
    {
        o[i] = lines[i].at((uint32_t)len[i]);
        lp[i] = o[i] + (lp[i] - o[i]) * damp;     // HF damping in the loop
        sum += lp[i];
    }
    sum *= 0.25f;                                  // Householder: o - 2/N * sum
    for (int i = 0; i < 8; ++i) lines[i].push((lp[i] - sum) * g[i] + (i & 1 ? -x : x) * 0.35f);
    *outL = (o[0] - o[2] + o[4] - o[6]) * 0.5f;
    *outR = (o[1] - o[3] + o[5] - o[7]) * 0.5f;
}

// ------------------------------------------------------------------ EFX: delays and reverbs
void EfxPro::alloc(float sampleRate)
{
    sr_ = sampleRate;
    for (Mod* m : {&modA_, &modB_}) { m->l.alloc((size_t)(sr_ * 0.13f)); m->r.alloc((size_t)(sr_ * 0.13f)); }
    echo_.d.alloc((size_t)(sr_ * 0.55f));
    rotDelay_.alloc((size_t)(sr_ * 0.01f));
    pitchLine_.alloc((size_t)(sr_ * 0.65f));   // pre-delay to 500 ms and the shifter's window
    hexa_.d.alloc((size_t)(sr_ * 0.13f));
    for (DelayLine& d : d_) d.alloc((size_t)(sr_ * 2.1f));   // taps to 1 s, note values to 2 s
    verb_.alloc(sr_);
    pre_.alloc((size_t)(sr_ * 0.65f));                       // gate reverb: pre-delay + gate time
}

void EfxPro::clear()
{
    for (DelayLine& d : d_) d.clear();
    verb_.clear();
    pre_.clear();
    for (Mod* m : {&modA_, &modB_}) { m->l.clear(); m->r.clear(); m->fL.z = m->fR.z = 0; }
    echo_.d.clear(); echo_.damp.z = 0;
    for (Biquad& b : eqL_) b.z1 = b.z2 = 0;
    for (Biquad& b : eqR_) b.z1 = b.z2 = 0;
    for (Biquad& b : spec_) b.z1 = b.z2 = 0;
    wahL_ = Svf(); wahR_ = Svf(); wahEnv_ = 0;
    rotDelay_.clear(); rotSplit_.z = 0;
    cmpEnv_ = 0;
    pitchLine_.clear(); pitchY_ = 0;
    hexa_.d.clear();
    std::memset(phaser_.ap, 0, sizeof(phaser_.ap)); phaser_.fbz = 0;
    drive_.cab.z = drive_.low.z = enhL_.z = enhR_.z = 0;
    fbL_ = fbR_ = 0;
    dampL_.z = dampR_.z = lowL_.z = lowR_.z = highL_.z = highR_.z = 0;
    glide_ = glideTarget_;
}

void EfxPro::set(int type, const uint8_t* p, float bpm)
{
    using namespace efx;
    const TypeDef& td = kTypes[type];
    auto raw = [&](int k) { return std::min<int>(p[k], fieldMax(td.f[k])); };
    auto ms = [&](int k) { return tableValue(td.f[k].arg, raw(k), bpm); };
    auto fbk = [&](int k) { return (float)((raw(k) - 49) * 2) / 100.0f; };
    auto db = [&](int k) { return std::pow(10.0f, (float)(raw(k) - td.f[k].arg) / 20.0f); };
    auto pan = [&](int k) { return ((float)raw(k) - 64.0f) / 64.0f; };
    auto damp = [&](int k) { int hz = listHz(L_DAMP, raw(k)); damp_ = hz > 0; dampL_.a = dampR_.a = coef((float)hz, sr_); };
    // the common stage: EQ shelves, balance, level (each type has them at its own places)
    lowGain_ = highGain_ = 1; dry_ = 0; wet_ = 1; level_out_ = 1;
    for (int k = 0; k < 12; ++k)
    {
        const Field& f = td.f[k];
        if (!f.name) continue;
        if (!std::strcmp(f.name, "Low Gain")) lowGain_ = db(k);
        else if (!std::strcmp(f.name, "Hi Gain")) highGain_ = db(k);
        else if (f.kind == K_BAL_DE && !std::strcmp(f.name, "Balance")) { wet_ = raw(k) / 100.0f; dry_ = 1 - wet_; }
    }
    for (int k = 11; k >= 0; --k)   // the output level: the last "Level" (TRIPLE-TAP's Level L/R/C come before it)
        if (td.f[k].name && !std::strcmp(td.f[k].name, "Level")) { level_out_ = raw(k) / 127.0f; break; }
    lowL_.a = lowR_.a = coef(200.0f, sr_);
    highL_.a = highR_.a = coef(4000.0f, sr_);
    bool newType = type != type_;
    type_ = type;
    switch (type)
    {
        case 16:   // STEREO-DELAY: Mode, Delay L, Delay R, Phase L, Phase R, Fbk, HF Damp
        case 17:   // MODULATION-DELAY: Mode, Delay L, Delay R, Fbk, HF Damp, Rate, Depth, Phase
        {
            cross_ = raw(0) == 1;
            time_[0] = msToSamples(ms(1));
            time_[1] = msToSamples(ms(2));
            if (type == 16)
            {
                phaseL_ = raw(3) ? -1.0f : 1.0f; phaseR_ = raw(4) ? -1.0f : 1.0f;
                fb_ = fbk(5); damp(6);
                lfoDepth_ = 0;
            }
            else
            {
                phaseL_ = phaseR_ = 1;
                fb_ = fbk(3); damp(4);
                lfoRate_ = tableValue(T_RATE, raw(5)) / sr_;
                lfoDepth_ = msToSamples(raw(6) / 127.0f * 10.0f);       // depth: to +-10 ms (this plugin's scale)
                lfoPhase_ = raw(7) * 2.0f / 360.0f;
            }
            break;
        }
        case 18:   // TRIPLE-TAP: Delay L, R, C, Fbk, HF Damp, Level L, R, C
            for (int k = 0; k < 3; ++k) { time_[k] = msToSamples(ms(k)); level_[k] = raw(5 + k) / 100.0f; }
            pan_[0] = -1; pan_[1] = 1; pan_[2] = 0;
            fb_ = fbk(3); damp(4);
            break;
        case 19:   // QUADRUPLE-TAP: Delay 1-4, Level 1-4, Fbk, HF Damp (taps 1 / 3 left, 2 / 4 right)
            for (int k = 0; k < 4; ++k) { time_[k] = msToSamples(ms(k)); level_[k] = raw(4 + k) / 127.0f; }
            pan_[0] = -1; pan_[1] = 1; pan_[2] = -0.5f; pan_[3] = 0.5f;
            fb_ = fbk(8); damp(9);
            break;
        case 20:   // TIME-CONTROL: Delay, Feedback, Accel, HF Damp, Pan
            glideTarget_ = msToSamples(ms(0));
            if (newType) glide_ = glideTarget_;
            fb_ = fbk(1);
            glideRate_ = std::pow(2.0f, (float)raw(2) - 15.0f) * 64.0f;   // how fast the time follows: Accel 0..15
            damp(3);
            outPan_ = pan(4);
            break;
        case 23:   // REVERB: Type, Pre Dly, Time, HF Damp
        {
            int hz = listHz(L_DAMP, raw(3));
            verb_.set(raw(0), raw(2) / 127.0f, (float)hz, 0.0f);
            verb_.setPreDelay(ms(1));
            break;
        }
        case 24:   // GATE-REVERB: Type, Pre Dly, Gate Time
            gateType_ = raw(0);
            preDelay_ = msToSamples(ms(1));
            gateLen_ = msToSamples(ms(2));
            break;
        case 0: case 4: case 6: case 7: case 8: case 9: case 21: case 22: setStage4(type, p, bpm); break;
        default: setStage3(type, p, bpm); break;
    }
    if (type == 0) lowGain_ = highGain_ = 1;   // STEREO-EQ: its shelves are its own filters
    if (newType) clear();
}

// ---- stage 4
void EfxPro::Biquad::peak(float hz, float q, float db, float sr)
{
    float A = std::pow(10.0f, db / 40.0f), w = 6.2831853f * std::min(hz, sr * 0.45f) / sr, al = std::sin(w) / (2 * q), cs = std::cos(w);
    float a0 = 1 + al / A;
    b0 = (1 + al * A) / a0; b1 = -2 * cs / a0; b2 = (1 - al * A) / a0; a1 = -2 * cs / a0; a2 = (1 - al / A) / a0;
}
void EfxPro::Biquad::lowShelf(float hz, float db, float sr)
{
    float A = std::pow(10.0f, db / 40.0f), w = 6.2831853f * hz / sr, cs = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2.0f), sq = 2 * std::sqrt(A) * al;
    float a0 = (A + 1) + (A - 1) * cs + sq;
    b0 = A * ((A + 1) - (A - 1) * cs + sq) / a0; b1 = 2 * A * ((A - 1) - (A + 1) * cs) / a0; b2 = A * ((A + 1) - (A - 1) * cs - sq) / a0;
    a1 = -2 * ((A - 1) + (A + 1) * cs) / a0; a2 = ((A + 1) + (A - 1) * cs - sq) / a0;
}
void EfxPro::Biquad::highShelf(float hz, float db, float sr)
{
    float A = std::pow(10.0f, db / 40.0f), w = 6.2831853f * std::min(hz, sr * 0.45f) / sr, cs = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2.0f), sq = 2 * std::sqrt(A) * al;
    float a0 = (A + 1) - (A - 1) * cs + sq;
    b0 = A * ((A + 1) + (A - 1) * cs + sq) / a0; b1 = -2 * A * ((A - 1) + (A + 1) * cs) / a0; b2 = A * ((A + 1) + (A - 1) * cs - sq) / a0;
    a1 = 2 * ((A - 1) - (A + 1) * cs) / a0; a2 = ((A + 1) - (A - 1) * cs - sq) / a0;
}

float EfxPro::Shifter::tap(const DelayLine& d)
{
    // two taps half a window apart, each fading in and out as it sweeps. Triangular fades sum to 1, so
    // the output never exceeds the input (sine fades sum to 1.41 on a steady sound: with feedback, the
    // FBK pitch shifter's loop then grew)
    float p2 = ph + 0.5f; if (p2 >= 1) p2 -= 1;
    float a = d.tap(pre + 1 + ph * window), b = d.tap(pre + 1 + p2 * window);
    return a * (1.0f - std::fabs(2.0f * ph - 1.0f)) + b * (1.0f - std::fabs(2.0f * p2 - 1.0f));
}

// Stage 4's settings. The scales the ROM does not give (marked "scale") are this plugin's.
void EfxPro::setStage4(int type, const uint8_t* p, float bpm)
{
    (void)bpm;
    using namespace efx;
    const TypeDef& td = kTypes[type];
    auto raw = [&](int k) { return std::min<int>(p[k], fieldMax(td.f[k])); };
    auto db = [&](int k) { return (float)(raw(k) - td.f[k].arg); };
    auto pan = [&](int k) { return ((float)raw(k) - 64.0f) / 64.0f; };
    static const float qs[5] = {0.5f, 1.0f, 2.0f, 4.0f, 9.0f}, post[4] = {1.0f, 2.0f, 4.0f, 8.0f};   // post gain 0/+6/+12/+18 dB
    switch (type)
    {
        case 0:   // STEREO-EQ: Low Freq, Low Gain, Hi Freq, Hi Gain, P1 Freq, Q, Gain, P2 Freq, Q, Gain
            for (Biquad* b : {eqL_, eqR_})
            {
                b[0].lowShelf(raw(0) ? 400.0f : 200.0f, db(1), sr_);
                b[1].highShelf(raw(2) ? 8000.0f : 4000.0f, db(3), sr_);
                b[2].peak((float)kFreqs[std::min(raw(4), 16)], qs[raw(5)], db(6), sr_);
                b[3].peak((float)kFreqs[std::min(raw(7), 16)], qs[raw(8)], db(9), sr_);
            }
            eqCount_ = 4;
            break;
        case 4:   // SPECTRUM: Band 1-8, Width, Pan (bands at 250 Hz .. 8 kHz, Width 1..5 as Q 4..0.5: scale)
        {
            static const float bands[8] = {250, 500, 1000, 1250, 2000, 3150, 4000, 8000};
            static const float widths[5] = {4.0f, 2.0f, 1.4f, 1.0f, 0.5f};
            for (int k = 0; k < 8; ++k) spec_[k].peak(bands[k], widths[raw(8)], db(k), sr_);
            specCount_ = 8;
            monoPan_ = pan(9);
            break;
        }
        case 6:   // AUTO-WAH: Filter, Rate, Depth, Sens, Manual, Peak (Manual 100 Hz .. 6 kHz; depth +-2 octaves: scale)
            wahBpf_ = raw(0) == 1;
            wahRate_ = tableValue(T_RATE, raw(1)) / sr_;
            wahDepth_ = raw(2) / 127.0f * 2.0f;
            wahSens_ = raw(3) / 127.0f * 4.0f;
            wahBase_ = 100.0f * std::pow(60.0f, raw(4) / 127.0f);
            wahK_ = 2.0f - 1.9f * raw(5) / 127.0f;
            break;
        case 7:   // ROTARY: Hi Slow, Low Slow, Hi Fast, Low Fast, Speed, Hi Accel, Low Accel, Hi Level, Low Level, Separation
        {
            bool fast = raw(4) == 1;
            rotHornTo_ = tableValue(T_RATE, raw(fast ? 2 : 0));
            rotDrumTo_ = tableValue(T_RATE, raw(fast ? 3 : 1));
            rotHornAcc_ = std::pow(2.0f, (float)raw(5) - 15.0f) * 8.0f / sr_;   // Accel 0..15: how fast a rotor changes speed (scale)
            rotDrumAcc_ = std::pow(2.0f, (float)raw(6) - 15.0f) * 8.0f / sr_;
            rotHornLvl_ = raw(7) / 127.0f; rotDrumLvl_ = raw(8) / 127.0f;
            rotSep_ = raw(9) / 127.0f;
            rotSplit_.a = coef(800.0f, sr_);
            if (rotHornHz_ == 0) { rotHornHz_ = rotHornTo_; rotDrumHz_ = rotDrumTo_; }
            break;
        }
        case 8:   // COMPRESSOR: Sustain, Attack, Pan, Post Gain (sustain: threshold to -40 dB, ratio to 8:1: scale)
        {
            float s = raw(0) / 127.0f;
            cmpThresh_ = std::pow(10.0f, -40.0f * s / 20.0f);
            cmpRatio_ = 1.0f + 7.0f * s;
            cmpAtt_ = 1.0f - std::exp(-1.0f / (sr_ * 0.0001f * std::pow(1000.0f, raw(1) / 127.0f)));   // 0.1 .. 100 ms
            cmpRel_ = 1.0f - std::exp(-1.0f / (sr_ * 0.25f));
            monoPan_ = pan(2);
            cmpPost_ = post[raw(3)];
            break;
        }
        case 9:   // LIMITER: Thresh, Release, Ratio, Pan, Post Gain (threshold -60..0 dB, release 5 ms .. 1 s: scale)
        {
            static const float ratios[4] = {1.5f, 2.0f, 4.0f, 100.0f};
            cmpThresh_ = std::pow(10.0f, (-60.0f + 60.0f * raw(0) / 127.0f) / 20.0f);
            cmpRel_ = 1.0f - std::exp(-1.0f / (sr_ * 0.005f * std::pow(200.0f, raw(1) / 127.0f)));
            cmpRatio_ = ratios[raw(2)];
            cmpAtt_ = 1.0f - std::exp(-1.0f / (sr_ * 0.0005f));
            monoPan_ = pan(3);
            cmpPost_ = post[raw(4)];
            break;
        }
        case 21:  // 2VOICE-PITCH: Mode, Coarse A, B, Fine A, B, Pre Dly A, B, Pan A, B, Level Balance A:B
        case 22:  // FBK-PITCH: Mode, Coarse, Fine, Pre Dly, Feedback, Pan
        {
            static const float windowsMs[5] = {15, 25, 40, 60, 80};   // Mode 1..5: quicker .. smoother (scale)
            float win = sr_ * 0.001f * windowsMs[raw(0)];
            bool two = type == 21;
            shA_.ratio = std::exp2(((float)(raw(1) - 24) + (float)(raw(two ? 3 : 2) - 50) / 100.0f) / 12.0f);
            shA_.window = win;
            shA_.pre = msToSamples(tableValue(T_DELAY500, raw(two ? 5 : 3)));
            panA_ = pan(two ? 7 : 5);
            if (two)
            {
                shB_.ratio = std::exp2(((float)(raw(2) - 24) + (float)(raw(4) - 50) / 100.0f) / 12.0f);
                shB_.window = win;
                shB_.pre = msToSamples(tableValue(T_DELAY500, raw(6)));
                panB_ = pan(8);
                lvlB_ = raw(9) / 100.0f; lvlA_ = 1 - lvlB_;
                pitchFb_ = 0;
            }
            else { lvlA_ = 1; lvlB_ = 0; pitchFb_ = (float)((raw(4) - 49) * 2) / 100.0f; }
            break;
        }
        default: break;
    }
}

// Stage 3's settings. The ROM gives the parameters, their scales and units; how hard a Depth or a
// Drive acts is this plugin's choice (marked "scale").
void EfxPro::setStage3(int type, const uint8_t* p, float bpm)
{
    using namespace efx;
    const TypeDef& td = kTypes[type];
    auto raw = [&](int k) { return std::min<int>(p[k], fieldMax(td.f[k])); };
    auto ms = [&](int k) { return tableValue(td.f[k].arg, raw(k), bpm); };
    auto hz = [&](int k) { return tableValue(T_RATE, raw(k)); };
    auto fbk = [&](int k) { return (float)((raw(k) - 49) * 2) / 100.0f; };
    auto pan = [&](int k) { return ((float)raw(k) - 64.0f) / 64.0f; };
    auto bal = [&](int k) { return raw(k) / 100.0f; };
    auto setDrive = [&](int dk, int pk, bool hard, int amp) {
        drive_.hard = hard;
        drive_.gain = 1.0f + raw(dk) / 127.0f * (hard ? 60.0f : 14.0f);                 // scale
        drive_.norm = 1.0f / std::tanh(std::min(drive_.gain * 0.5f, 3.0f)) * 0.5f;
        drive_.pan = pan(pk);
        static const float cabHz[4] = {2500, 4000, 5500, 7000};                          // SMALL .. 3-STACK: scale
        drive_.cab.a = coef(cabHz[amp < 0 ? 1 : amp], sr_);
        drive_.lowCut = amp == 0; drive_.low.a = coef(150.0f, sr_);
    };
    auto setMod = [&](Mod& m, int preK, int rateK, int depthK, int phaseK, int fbK, bool flanger) {
        m.base = msToSamples(ms(preK)) + 1.0f;
        m.rate = hz(rateK) / sr_;
        m.depth = msToSamples(raw(depthK) / 127.0f * (flanger ? 4.0f : 5.0f));        // scale: to 5 ms (chorus) / 4 ms (flanger)
        m.offset = phaseK >= 0 ? raw(phaseK) * 2.0f / 360.0f : 0.25f;
        m.fb = fbK >= 0 ? fbk(fbK) * 0.95f : 0.0f;
        m.step = 0; m.filter = 0;
    };
    auto setEcho = [&](int timeK, int fbK, int dampK) {
        echo_.time = msToSamples(ms(timeK));
        echo_.fb = fbk(fbK);
        int h = listHz(L_DAMP, raw(dampK));
        echo_.damped = h > 0; echo_.damp.a = coef((float)h, sr_);
    };
    first_ = second_ = 0; parallel_ = false; balA_ = balB_ = 1;
    switch (type)
    {
        case 1: case 2:   // OVERDRIVE / DISTORTION: Drive, Pan, Amp Type
            setDrive(0, 1, type == 2, raw(2));
            first_ = type;
            break;
        case 3:           // PHASER: Manual, Rate, Depth, Res, Mix, Pan
            phaser_.manual = tableValue(T_MANUAL, raw(0));
            phaser_.rate = hz(1) / sr_;
            phaser_.depth = raw(2) / 127.0f * 2.0f;                                        // scale: +-2 octaves
            phaser_.res = raw(3) / 127.0f * 0.85f;
            phaser_.mix = raw(4) / 127.0f;
            phaser_.pan = pan(5);
            break;
        case 5:           // ENHANCER: Sens, Mix
            enhSens_ = raw(0) / 127.0f; enhMix_ = raw(1) / 127.0f;
            enhL_.a = enhR_.a = coef(1500.0f, sr_);
            break;
        case 10:          // HEXA-CHORUS: Pre Dly, Rate, Depth, Dly Dev, Dpt Dev, Pan Dev
        {
            float pre = msToSamples(ms(0)) + 1.0f, dev = msToSamples(raw(3) / 127.0f * 8.0f);   // scale: to 8 ms apart
            float d = msToSamples(raw(2) / 127.0f * 5.0f), dd = (raw(4) - 20) / 20.0f, pd = raw(5) / 20.0f;
            for (int k = 0; k < 6; ++k)
            {
                float u = k / 5.0f - 0.5f;   // -0.5 .. +0.5 across the voices
                hexa_.base[k] = pre + dev * (u + 0.5f);
                hexa_.depth[k] = d * std::max(0.0f, 1.0f + dd * u);
                hexa_.pan[k] = std::max(-1.0f, std::min(1.0f, (k & 1 ? 1.0f : -1.0f) * (0.2f + 0.8f * std::min(1.0f, pd)) * (0.5f + std::fabs(u))));
            }
            hexa_.rate = hz(1) / sr_;
            break;
        }
        case 11:          // TREMOLO-CHORUS: Pre Dly, Cho Rate, Cho Depth, Trem Rate, Trem Sep, Phase
            setMod(modA_, 0, 1, 2, 5, -1, false);
            trmRate_ = hz(3) / sr_; trmSep_ = raw(4) / 127.0f;
            first_ = 4;
            break;
        case 12:          // SPACE-D: Pre Dly, Rate, Depth, Phase (two-phase: the right voice moves against the left)
            setMod(modA_, 0, 1, 2, 3, -1, false);
            modA_.offset += 0.5f;
            first_ = 4;
            break;
        case 13:          // STEREO-CHORUS: Filter, Cutoff, Pre Dly, Rate, Depth, Phase
        case 14:          // STEREO-FLANGER: the same, and Fbk
        {
            setMod(modA_, 2, 3, 4, 5, type == 14 ? 6 : -1, type == 14);
            modA_.filter = raw(0);
            modA_.fL.a = modA_.fR.a = coef((float)kFreqs[std::min(raw(1), 16)], sr_);
            first_ = type == 13 ? 4 : 5;
            break;
        }
        case 15:          // STEP-FLANGER: Pre Dly, Rate, Depth, Fbk, Step Rate, Phase
            setMod(modA_, 0, 1, 2, 5, 3, true);
            modA_.step = tableValue(T_RATE, raw(4)) / sr_;
            first_ = 5;
            break;
        case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33:
        {
            // OD / DS / EH, then CHORUS / FLANGER / DELAY
            int a = type <= 27 ? 1 : type <= 30 ? 2 : 3, b = (type - 25) % 3;
            if (a < 3) setDrive(0, 1, a == 2, 2);
            else { enhSens_ = raw(0) / 127.0f; enhMix_ = raw(1) / 127.0f; enhL_.a = enhR_.a = coef(1500.0f, sr_); }
            first_ = a;
            if (b == 0) { setMod(modB_, 2, 3, 4, -1, -1, false); second_ = 4; }
            else if (b == 1) { setMod(modB_, 2, 3, 4, -1, 5, true); second_ = 5; }
            else { setEcho(2, 3, 4); second_ = 6; }
            break;
        }
        case 34: case 37: // CHORUS > or / DELAY: Cho Dly, Rate, Depth, -, Cho Bal, Delay, Fbk, HF Damp, Dly Bal
            setMod(modA_, 0, 1, 2, -1, -1, false); balA_ = bal(4);
            setEcho(5, 6, 7); balB_ = bal(8);
            first_ = 4; second_ = 6; parallel_ = type == 37;
            break;
        case 35: case 38: // FLANGER > or / DELAY: Flg Dly, Rate, Depth, Fbk, Flg Bal, Delay, Fbk, HF Damp, Dly Bal
            setMod(modA_, 0, 1, 2, -1, 3, true); balA_ = bal(4);
            setEcho(5, 6, 7); balB_ = bal(8);
            first_ = 5; second_ = 6; parallel_ = type == 38;
            break;
        case 36: case 39: // CHORUS > or / FLANGER: Cho Dly, Rate, Depth, Cho Bal, Flg Dly, Rate, Depth, Fbk, Flg Bal
            setMod(modA_, 0, 1, 2, -1, -1, false); balA_ = bal(3);
            setMod(modB_, 4, 5, 6, -1, 7, true); balB_ = bal(8);
            first_ = 4; second_ = 5; parallel_ = type == 39;
            break;
        default: break;
    }
}

float EfxPro::Drive::process(float x)
{
    float y = x * gain;
    y = hard ? std::max(-1.0f, std::min(1.0f, y * 0.7f)) * 0.8f + 0.2f * std::tanh(y) : std::tanh(y);
    y *= norm;
    y = cab.lp(y);
    if (lowCut) y -= low.lp(y);
    return y;
}

void EfxPro::Mod::process(float inL, float inR, float& oL, float& oR)
{
    ph += rate; if (ph >= 1) ph -= 1;
    float p2 = ph + offset; p2 -= std::floor(p2);
    float mL = std::sin(6.2831853f * ph), mR = std::sin(6.2831853f * p2);
    if (step > 0) { stepPh += step; if (stepPh >= 1) { stepPh -= 1; heldL = mL; heldR = mR; } mL = heldL; mR = heldR; }
    if (filter == 1) { inL = fL.lp(inL); inR = fR.lp(inR); }
    else if (filter == 2) { inL -= fL.lp(inL); inR -= fR.lp(inR); }
    float a = l.tap(base + depth * (0.5f + 0.5f * mL)), b = r.tap(base + depth * (0.5f + 0.5f * mR));
    l.push(inL + fb * a);
    r.push(inR + fb * b);
    oL = a; oR = b;
}

float EfxPro::Echo::process(float x)
{
    float y = d.tap(time);
    d.push(x + fb * (damped ? damp.lp(y) : y));
    return y;
}

float EfxPro::Phaser::process(float x, float sr)
{
    if ((count++ & 15) == 0)   // the sweep: the allpass frequency every 16 samples
    {
        ph += rate * 16; if (ph >= 1) ph -= 1;
        float f = std::min(manual * std::exp2(depth * std::sin(6.2831853f * ph)), sr * 0.45f);
        float t = std::tan(3.14159265f * f / sr);
        coefA = (t - 1) / (t + 1);
    }
    float y = x + fbz * res;
    for (float& s : ap) { float o = coefA * y + s; s = y - coefA * o; y = o; }
    fbz = y;
    return x + mix * y;
}

void EfxPro::Hexa::process(float x, float& oL, float& oR)
{
    ph += rate; if (ph >= 1) ph -= 1;
    d.push(x);
    oL = oR = 0;
    for (int k = 0; k < 6; ++k)
    {
        float m = std::sin(6.2831853f * (ph + k / 6.0f));
        float v = d.tap(base[k] + depth[k] * (0.5f + 0.5f * m));
        oL += v * std::min(1.0f, 1.0f - pan[k]);
        oR += v * std::min(1.0f, 1.0f + pan[k]);
    }
    oL *= 0.4f; oR *= 0.4f;
}

// one stage of the two-stage types (and the single drive / chorus / flanger types)
void EfxPro::runStage(int kind, Mod& m, float& l, float& r)
{
    switch (kind)
    {
        case 1: case 2:
        {
            float y = drive_.process(0.5f * (l + r));
            l = y * std::min(1.0f, 1.0f - drive_.pan); r = y * std::min(1.0f, 1.0f + drive_.pan);
            break;
        }
        case 3:
        {
            float hl = l - enhL_.lp(l), hr = r - enhR_.lp(r);   // the top, driven: new harmonics
            l += enhMix_ * std::tanh(hl * (1 + 12 * enhSens_)) * 0.5f;
            r += enhMix_ * std::tanh(hr * (1 + 12 * enhSens_)) * 0.5f;
            break;
        }
        case 4: case 5: { float a, b; m.process(l, r, a, b); l = a; r = b; break; }
        case 6: { float y = echo_.process(0.5f * (l + r)); l = r = y; break; }
        default: break;
    }
}

void EfxPro::stageA(float& l, float& r)
{
    float dl = l, dr = r;
    runStage(first_, modA_, l, r);
    if (first_ >= 4 && second_) { l = dl * (1 - balA_) + l * balA_; r = dr * (1 - balA_) + r * balA_; }   // its own balance
}

void EfxPro::stageB(float& l, float& r)
{
    float dl = l, dr = r;
    runStage(second_, modB_, l, r);   // the second stage's chorus / flanger is modB_ (a delay uses echo_)
    if (first_ >= 4) { l = dl * (1 - balB_) + l * balB_; r = dr * (1 - balB_) + r * balB_; }
}

void EfxPro::process(float* L, float* R, int n)
{
    for (int i = 0; i < n; ++i)
    {
        float inL = L[i], inR = R[i], wl = 0, wr = 0;
        switch (type_)
        {
            case 16:
            case 17:
            {
                float mL = 0, mR = 0;
                if (lfoDepth_ > 0)
                {
                    lfo_ += lfoRate_; if (lfo_ >= 1) lfo_ -= 1;
                    float p2 = lfo_ + lfoPhase_; if (p2 >= 1) p2 -= 1;
                    mL = lfoDepth_ * (0.5f + 0.5f * std::sin(6.2831853f * lfo_));
                    mR = lfoDepth_ * (0.5f + 0.5f * std::sin(6.2831853f * p2));
                }
                float a = d_[0].tap(time_[0] + mL), b = d_[1].tap(time_[1] + mR);
                float fa = damp_ ? dampL_.lp(a) : a, fbb = damp_ ? dampR_.lp(b) : b;
                d_[0].push(inL + fb_ * (cross_ ? fbb : fa));
                d_[1].push(inR + fb_ * (cross_ ? fa : fbb));
                wl = a * phaseL_; wr = b * phaseR_;
                break;
            }
            case 18:
            case 19:
            {
                int taps = type_ == 18 ? 3 : 4;
                for (int k = 0; k < taps; ++k)
                {
                    float x = d_[0].tap(time_[k]) * level_[k];
                    wl += x * std::min(1.0f, 1.0f - pan_[k]);
                    wr += x * std::min(1.0f, 1.0f + pan_[k]);
                }
                float back = d_[0].tap(time_[type_ == 18 ? 2 : 0]);   // fed back: the centre tap / tap 1
                if (damp_) back = dampL_.lp(back);
                d_[0].push(0.5f * (inL + inR) + fb_ * back);
                break;
            }
            case 20:
            {
                glide_ += (glideTarget_ - glide_) * std::min(1.0f, glideRate_ / sr_);
                float a = d_[0].tap(glide_);
                d_[0].push(0.5f * (inL + inR) + fb_ * (damp_ ? dampL_.lp(a) : a));
                wl = a * std::min(1.0f, 1.0f - outPan_);
                wr = a * std::min(1.0f, 1.0f + outPan_);
                break;
            }
            case 23:
                verb_.process(0.5f * (inL + inR), &wl, &wr);
                break;
            case 24:
            {
                // gate reverb: 16 reflections across the gate time, after the pre-delay;
                // NORMAL even, REVERSE rising, SWEEP1 / SWEEP2 moving left to right / right to left
                pre_.push(0.5f * (inL + inR));
                for (int k = 0; k < 16; ++k)
                {
                    float pos = (k + 0.5f) / 16.0f;
                    float x = pre_.tap(preDelay_ + gateLen_ * pos * (0.93f + 0.07f * (float)((k * 7) % 5) / 4.0f));
                    float g = gateType_ == 1 ? 0.25f + 0.75f * pos : 0.8f - 0.2f * pos;
                    float pn = gateType_ == 2 ? pos * 2 - 1 : gateType_ == 3 ? 1 - pos * 2 : (k & 1 ? 0.6f : -0.6f);
                    x *= g * 0.22f;
                    wl += x * std::min(1.0f, 1.0f - pn);
                    wr += x * std::min(1.0f, 1.0f + pn);
                }
                break;
            }
            case 0:
                wl = inL; wr = inR;
                for (int k = 0; k < eqCount_; ++k) { wl = eqL_[k].process(wl); wr = eqR_[k].process(wr); }
                break;
            case 4:
            {
                float y = 0.5f * (inL + inR);
                for (int k = 0; k < specCount_; ++k) y = spec_[k].process(y);
                wl = y * std::min(1.0f, 1.0f - monoPan_); wr = y * std::min(1.0f, 1.0f + monoPan_);
                break;
            }
            case 6:
            {
                float a = std::fabs(inL) + std::fabs(inR);
                wahEnv_ += (a - wahEnv_) * (a > wahEnv_ ? 0.01f : 0.0005f);
                wahPh_ += wahRate_; if (wahPh_ >= 1) wahPh_ -= 1;
                float f = wahBase_ * std::exp2(wahDepth_ * std::sin(6.2831853f * wahPh_) + wahSens_ * std::min(wahEnv_, 1.0f));
                float g = std::tan(3.14159265f * std::min(f, sr_ * 0.45f) / sr_);
                float lp, bp;
                wahL_.run(inL, g, wahK_, lp, bp); wl = wahBpf_ ? bp * 2 : lp;
                wahR_.run(inR, g, wahK_, lp, bp); wr = wahBpf_ ? bp * 2 : lp;
                break;
            }
            case 7:
            {
                rotHornHz_ += (rotHornTo_ - rotHornHz_) * rotHornAcc_;
                rotDrumHz_ += (rotDrumTo_ - rotDrumHz_) * rotDrumAcc_;
                rotHorn_ += rotHornHz_ / sr_; rotHorn_ -= std::floor(rotHorn_);
                rotDrum_ += rotDrumHz_ / sr_; rotDrum_ -= std::floor(rotDrum_);
                float x = 0.5f * (inL + inR), lo = rotSplit_.lp(x), hi = x - lo;
                float hs = std::sin(6.2831853f * rotHorn_), ds = std::sin(6.2831853f * rotDrum_);
                rotDelay_.push(hi);
                float horn = rotDelay_.tap(1 + sr_ * 0.0003f * (1 + hs)) * rotHornLvl_;   // the horn's doppler
                float drum = lo * rotDrumLvl_;
                wl = horn * (0.7f + 0.3f * rotSep_ * hs) + drum * (0.8f + 0.2f * rotSep_ * ds);
                wr = horn * (0.7f - 0.3f * rotSep_ * hs) + drum * (0.8f - 0.2f * rotSep_ * ds);
                break;
            }
            case 8:
            case 9:
            {
                float x = 0.5f * (inL + inR), a = std::fabs(x);
                cmpEnv_ += (a - cmpEnv_) * (a > cmpEnv_ ? cmpAtt_ : cmpRel_);
                float gain = 1;
                if (cmpEnv_ > cmpThresh_) gain = std::pow(cmpEnv_ / cmpThresh_, 1.0f / cmpRatio_ - 1.0f);
                float y = x * gain * cmpPost_;
                wl = y * std::min(1.0f, 1.0f - monoPan_); wr = y * std::min(1.0f, 1.0f + monoPan_);
                break;
            }
            case 21:
            case 22:
            {
                float x = 0.5f * (inL + inR);
                pitchLine_.push(x + pitchFb_ * pitchY_);
                float a = shA_.tap(pitchLine_);
                shA_.advance();
                pitchY_ = a;
                wl = a * lvlA_ * std::min(1.0f, 1.0f - panA_); wr = a * lvlA_ * std::min(1.0f, 1.0f + panA_);
                if (lvlB_ > 0)
                {
                    float b = shB_.tap(pitchLine_);
                    shB_.advance();
                    wl += b * lvlB_ * std::min(1.0f, 1.0f - panB_); wr += b * lvlB_ * std::min(1.0f, 1.0f + panB_);
                }
                break;
            }
            case 3:
            {
                float y = phaser_.process(0.5f * (inL + inR), sr_);
                wl = y * std::min(1.0f, 1.0f - phaser_.pan); wr = y * std::min(1.0f, 1.0f + phaser_.pan);
                break;
            }
            case 5: wl = inL; wr = inR; runStage(3, modA_, wl, wr); break;
            case 10: hexa_.process(0.5f * (inL + inR), wl, wr); break;
            case 11:
            {
                runStage(4, modA_, wl = inL, wr = inR);
                trmPh_ += trmRate_; if (trmPh_ >= 1) trmPh_ -= 1;
                float t2 = trmPh_ + 0.5f * trmSep_; t2 -= std::floor(t2);
                wl *= 0.5f + 0.5f * std::sin(6.2831853f * trmPh_);
                wr *= 0.5f + 0.5f * std::sin(6.2831853f * t2);
                break;
            }
            default:
                if (second_ == 0) { wl = inL; wr = inR; runStage(first_, modA_, wl, wr); break; }
                if (parallel_)
                {
                    float al = inL, ar = inR, bl = inL, br = inR;
                    stageA(al, ar); stageB(bl, br);
                    wl = 0.5f * (al + bl); wr = 0.5f * (ar + br);
                }
                else { wl = inL; wr = inR; stageA(wl, wr); stageB(wl, wr); }
                break;
        }
        // the common stage: shelves on the effect sound, then the balance and the level
        if (lowGain_ != 1) { wl += (lowGain_ - 1) * lowL_.lp(wl); wr += (lowGain_ - 1) * lowR_.lp(wr); }
        if (highGain_ != 1) { wl += (highGain_ - 1) * (wl - highL_.lp(wl)); wr += (highGain_ - 1) * (wr - highR_.lp(wr)); }
        L[i] = (inL * dry_ + wl * wet_) * level_out_;
        R[i] = (inR * dry_ + wr * wet_) * level_out_;
    }
}

// ------------------------------------------------------------------ EFX
void Efx::alloc(float sampleRate)
{
    sr_ = sampleRate;
    pro_.alloc(sr_);
    mod_.alloc(sr_);
    dl_.alloc((size_t)(sr_ * 1.1f));
    dr_.alloc((size_t)(sr_ * 1.1f));
    verb_.alloc(sr_);
}
void Efx::clear()
{
    mod_.clear(); dl_.clear(); dr_.clear(); verb_.clear();
    std::memset(apL_, 0, sizeof(apL_)); std::memset(apR_, 0, sizeof(apR_));
    wahEnv_ = wahL1_ = wahL2_ = wahR1_ = wahR2_ = compEnv_ = enhL_ = enhR_ = toneL_ = toneR_ = 0;
}
void Efx::setType(int type, const uint8_t* p, float bpm)
{
    type_ = type;
    if (EfxPro::handles(type)) { pro_.set(type, p, bpm); pro_.clear(); a_ = b_ = None; return; }
    static const Kind single[25] = {None, Drive, Dist, Phaser, None, Enhance, AutoWah, Rotary, Comp, Comp,
                                    Chorus, Chorus, Chorus, Chorus, Flanger, Flanger,
                                    Delay, Delay, Delay, Delay, Delay, None, None, Verb, Verb};
    static const Kind chainA[15] = {Drive, Drive, Drive, Dist, Dist, Dist, Enhance, Enhance, Enhance, Chorus, Flanger, Chorus, Chorus, Flanger, Chorus};
    static const Kind chainB[15] = {Chorus, Flanger, Delay, Chorus, Flanger, Delay, Chorus, Flanger, Delay, Delay, Delay, Flanger, Delay, Delay, Flanger};
    parallel_ = false;
    if (type >= 0 && type < 25) { a_ = single[type]; b_ = None; }
    else if (type >= 25 && type < 40) { a_ = chainA[type - 25]; b_ = chainB[type - 25]; parallel_ = type >= 37; }
    else { a_ = b_ = None; }
    setParams(type, p);
    if (a_ == Flanger || b_ == Flanger) mod_.set(0.25f, 1.0f, 2.0f, 0.6f);
    else mod_.set(0.6f, 7.0f, 3.0f, 0.1f);
    verb_.set(type == 24 ? 0 : 2, type == 24 ? 0.15f : 0.45f, 6000.0f, 0.0f);
    clear();
}
void Efx::setParams(int type, const uint8_t* p, float bpm)
{
    if (EfxPro::handles(type)) { pro_.set(type, p, bpm); return; }
    drive_ = (type == 1 || type == 2 || (type >= 25 && type <= 30)) ? p[0] / 127.0f : 0.5f;
}
void Efx::stage(Kind k, float& l, float& r)
{
    switch (k)
    {
        case Drive:
        case Dist:
        {
            float g = 1.0f + drive_ * (k == Dist ? 40.0f : 12.0f);
            float norm = 1.0f / fastTanh(g * 0.5f);
            l = fastTanh(l * g) * norm * 0.5f;
            r = fastTanh(r * g) * norm * 0.5f;
            toneL_ += (l - toneL_) * 0.35f; toneR_ += (r - toneR_) * 0.35f;   // the amp's top end
            l = toneL_; r = toneR_;
            break;
        }
        case Phaser:
        {
            phPhase_ += 0.4f / sr_;
            if (phPhase_ >= 1) phPhase_ -= 1;
            float c = 0.3f + 0.6f * (0.5f + 0.5f * sinApprox(phPhase_));
            float a = (1 - c) / (1 + c);
            float xl = l, xr = r;
            for (int i = 0; i < 4; ++i)
            {
                float yl = -a * xl + apL_[i]; apL_[i] = xl + a * yl; xl = yl;
                float yr = -a * xr + apR_[i]; apR_[i] = xr + a * yr; xr = yr;
            }
            l = 0.5f * (l + xl); r = 0.5f * (r + xr);
            break;
        }
        case AutoWah:
        {
            float in = std::fabs(l) + std::fabs(r);
            wahEnv_ += (in - wahEnv_) * (in > wahEnv_ ? 0.01f : 0.0005f);
            float f = 300.0f + 2500.0f * std::fmin(wahEnv_ * 3, 1.0f);
            float g = 2 * std::sin(3.14159f * f / sr_), q = 0.25f;
            wahL2_ += g * wahL1_; float hl = l - wahL2_ - q * wahL1_; wahL1_ += g * hl;
            wahR2_ += g * wahR1_; float hr = r - wahR2_ - q * wahR1_; wahR1_ += g * hr;
            l = wahL1_ * 1.5f; r = wahR1_ * 1.5f;
            break;
        }
        case Rotary:
        {
            rotPhase_ += 0.8f / sr_; if (rotPhase_ >= 1) rotPhase_ -= 1;     // slow horn
            rotPhase2_ += 0.65f / sr_; if (rotPhase2_ >= 1) rotPhase2_ -= 1; // slow drum
            float h = sinApprox(rotPhase_), d = sinApprox(rotPhase2_);
            float m = 0.5f * (l + r);
            float o1, o2;
            mod_.set(0.0f, 1.0f, 0.0f, 0.0f);
            mod_.l.push(m); mod_.r.push(m);
            o1 = mod_.l.tap(2 + 0.0006f * sr_ * (1 + h));
            o2 = mod_.r.tap(2 + 0.0012f * sr_ * (1 + d));
            l = o1 * (0.75f + 0.25f * h) + o2 * (0.8f + 0.2f * d) * 0.6f;
            r = o1 * (0.75f - 0.25f * h) + o2 * (0.8f - 0.2f * d) * 0.6f;
            break;
        }
        case Comp:
        {
            float in = std::fmax(std::fabs(l), std::fabs(r));
            compEnv_ += (in - compEnv_) * (in > compEnv_ ? 0.02f : 0.0002f);
            float th = 0.25f, gain = compEnv_ > th ? std::sqrt(th / compEnv_) : 1.0f;
            l *= gain * 1.6f; r *= gain * 1.6f;
            break;
        }
        case Enhance:
        {
            enhL_ += (l - enhL_) * 0.2f; enhR_ += (r - enhR_) * 0.2f;   // the top: input minus a lowpass
            l += fastTanh((l - enhL_) * 3.0f) * 0.25f;
            r += fastTanh((r - enhR_) * 3.0f) * 0.25f;
            break;
        }
        case Chorus:
        case Flanger:
        {
            float a, b;
            mod_.process(l, r, &a, &b);
            l = 0.6f * l + 0.6f * a; r = 0.6f * r + 0.6f * b;
            break;
        }
        case Delay:
        {
            float a = dl_.tap(0.30f * sr_), b = dr_.tap(0.45f * sr_);
            dl_.push(l + a * 0.35f); dr_.push(r + b * 0.35f);
            l += 0.45f * a; r += 0.45f * b;
            break;
        }
        case Verb:
        {
            float a, b;
            verb_.process(0.5f * (l + r), &a, &b);
            l += 0.5f * a; r += 0.5f * b;
            break;
        }
        case None:
        default: break;
    }
}
void Efx::process(float* L, float* R, int n)
{
    if (EfxPro::handles(type_)) { pro_.process(L, R, n); return; }
    if (a_ == None && b_ == None) return;
    for (int i = 0; i < n; ++i)
    {
        float l = L[i], r = R[i];
        if (parallel_)
        {
            float l2 = l, r2 = r;
            stage(a_, l, r);
            stage(b_, l2, r2);
            l = 0.5f * (l + l2); r = 0.5f * (r + r2);
        }
        else
        {
            stage(a_, l, r);
            if (b_ != None) stage(b_, l, r);
        }
        L[i] = l; R[i] = r;
    }
}

// ------------------------------------------------------------------ Fx
void Fx::prepare(float sampleRate)
{
    sr_ = sampleRate;
    efxType_ = -1;
    efx_.alloc(sr_);
    chorus_.alloc(sr_);
    reverb_.alloc(sr_);
}

void Fx::setPatch(const uint8_t* c)
{
    int type = c[jv::C_EFX_TYPE];
    float bpm = (float)std::max(20, std::min(250, c[jv::C_TEMPO_HI] * 16 + c[jv::C_TEMPO_LO]));   // the patch's tempo
    if (type != efxType_) { efx_.setType(type, c + jv::C_EFX_PARAM, bpm); efxType_ = type; }
    else efx_.setParams(type, c + jv::C_EFX_PARAM, bpm);
    efxMix_ = c[jv::C_EFX_MIX_SEND] / 127.0f;
    efxCho_ = c[jv::C_EFX_CHORUS_SEND] / 127.0f;
    efxRev_ = c[jv::C_EFX_REVERB_SEND] / 127.0f;

    float rate = 0.05f * std::pow(2.0f, c[jv::C_CHORUS_RATE] / 127.0f * 7.6f);
    int pd = c[jv::C_CHORUS_PREDELAY];
    float preMs = pd < 50 ? pd * 0.1f : 5.0f + (pd - 50) * 1.24f;
    chorus_.set(rate, preMs + 1.0f, c[jv::C_CHORUS_DEPTH] / 127.0f * 4.0f, c[jv::C_CHORUS_FEEDBACK] / 127.0f * 0.9f);
    choLevel_ = c[jv::C_CHORUS_LEVEL] / 127.0f;
    int out = c[jv::C_CHORUS_OUTPUT];
    choToMix_ = out == 1 ? 0.0f : 1.0f;
    choToRev_ = out == 0 ? 0.0f : 1.0f;

    type = c[jv::C_REVERB_TYPE] & 7;
    int damp = c[jv::C_REVERB_HF_DAMP];
    reverb_.set(type, c[jv::C_REVERB_TIME] / 127.0f, damp < 17 ? (float)jv::kHfDamp[damp] : 0.0f, c[jv::C_REVERB_FEEDBACK] / 127.0f);
    revLevel_ = c[jv::C_REVERB_LEVEL] / 127.0f;
}

void Fx::process(const float* dryL, const float* dryR, float* efxL, float* efxR, const float* choSend, const float* revSend,
                 float* outL, float* outR, int n, bool efxOn, bool chorusOn, bool reverbOn)
{
    if (efxOn) efx_.process(efxL, efxR, n);
    for (int i = 0; i < n; ++i)
    {
        float el = efxL[i], er = efxR[i];
        float em = 0.5f * (el + er);
        float l = dryL[i] + el * efxMix_, r = dryR[i] + er * efxMix_;
        float cin = choSend[i] + em * efxCho_;
        float rin = revSend[i] + em * efxRev_;
        if (chorusOn)
        {
            float a, b;
            chorus_.process(cin, cin, &a, &b);
            a *= choLevel_; b *= choLevel_;
            l += a * choToMix_; r += b * choToMix_;
            rin += 0.5f * (a + b) * choToRev_;
        }
        if (reverbOn)
        {
            float a, b;
            reverb_.process(rin, &a, &b);
            l += a * revLevel_; r += b * revLevel_;
        }
        outL[i] = l;
        outR[i] = r;
    }
}

} // namespace xp
