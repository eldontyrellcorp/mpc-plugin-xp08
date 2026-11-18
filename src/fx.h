// fx.h - the XP-80's effects section: EFX (insert, 40 types), chorus and reverb (sends).
//
//   tones (MIX)  ----------------------------------------------+--> out
//   tones (EFX)  --> EFX --> mix send -------------------------+
//                         \-> chorus send / reverb send
//   tone/EFX chorus sends --> CHORUS --> MIX and/or reverb ----+
//   tone/EFX/chorus reverb sends --> REVERB -------------------+
//
// Chorus and reverb follow the patch's settings (level, rate, depth, pre-delay, feedback,
// output; reverb type, level, time, HF damp, delay feedback). The EFX's twelve parameters are
// decoded for all 40 types (efxdefs.h), and EfxPro plays every type with all its parameters.
// (Efx's older family sounds, from before, are no longer reached: Efx hands every type to EfxPro.)
#ifndef XP_FX_H
#define XP_FX_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace xp {

struct DelayLine
{
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;
    void alloc(size_t minLen);
    void clear();
    inline void push(float x) { buf[w & mask] = x; ++w; }
    inline float tap(float delaySamples) const   // fractional, at least 1
    {
        float d = delaySamples < 1.0f ? 1.0f : delaySamples;
        uint32_t i = (uint32_t)d;
        float fr = d - (float)i;
        float a = buf[(w - i) & mask], b = buf[(w - i - 1) & mask];
        return a + (b - a) * fr;
    }
    inline float at(uint32_t d) const { return buf[(w - d) & mask]; }
};

struct ModDelay                            // chorus / flanger, stereo
{
    DelayLine l, r;
    float phase = 0, rate = 0.5f, base = 0, depth = 0, feedback = 0, fbL = 0, fbR = 0, sr = 44100;
    void alloc(float sampleRate);
    void clear();
    void set(float rateHz, float baseMs, float depthMs, float fb);
    void process(float inL, float inR, float* outL, float* outR);
};

struct Reverb                              // 8-line FDN; or a stereo / ping-pong delay
{
    DelayLine lines[8], pre;
    float lp[8] = {}, g[8] = {}, damp = 0.3f, sr = 44100;
    int len[8] = {};
    int mode = 0;                          // 0 room/hall, 1 delay, 2 pan delay
    DelayLine dl, dr;
    float delay = 10000, fb = 0.3f, preDelay = 0;
    void alloc(float sampleRate);
    void clear();
    void set(int type, float time01, float hfHz, float feedback01);
    void setPreDelay(float ms) { preDelay = ms * 0.001f * sr; }
    void process(float in, float* outL, float* outR);
};

// The EFX types that follow all of their parameters (efxdefs.h): the delays (STEREO, MODULATION,
// TRIPLE-TAP, QUADRUPLE-TAP, TIME-CONTROL), the reverbs (REVERB, GATE-REVERB), overdrive,
// distortion, phaser, enhancer, the choruses (HEXA, TREMOLO, SPACE-D, STEREO), the flangers
// (STEREO, STEP) and the fifteen two-stage types (A>B in series, A/B in parallel, each stage with
// its balance), and STEREO-EQ, SPECTRUM, AUTO-WAH, ROTARY, COMPRESSOR, LIMITER and the two pitch
// shifters. Stereo in, stereo out; each ends in the types' common stage: low and high EQ
// shelves, dry / effect balance, level.
class EfxPro
{
public:
    static bool handles(int type) { return type >= 0 && type < 40; }   // all 40 (stage 4 done)
    void alloc(float sampleRate);
    void clear();
    void set(int type, const uint8_t* params, float bpm);
    void process(float* L, float* R, int n);
private:
    struct OnePole { float a = 0, z = 0; float lp(float x) { z += a * (x - z); return z; } };
    static float coef(float hz, float sr) { return hz <= 0 ? 1.0f : 1.0f - std::exp(-6.2831853f * hz / sr); }
    float msToSamples(float ms) const { return ms * 0.001f * sr_; }
    int type_ = -1;
    float sr_ = 44100;
    DelayLine d_[4];                       // ST/MOD: L, R; TAP: one line (d_[0]); TIME-CTRL: d_[0]
    float time_[4] = {}, level_[4] = {}, pan_[4] = {};   // tap times (samples), levels, pans (-1..+1)
    float fb_ = 0, fbL_ = 0, fbR_ = 0;
    bool cross_ = false;
    float phaseL_ = 1, phaseR_ = 1;
    OnePole dampL_, dampR_;
    bool damp_ = false;
    // modulation (MOD-DELAY)
    float lfo_ = 0, lfoRate_ = 0, lfoDepth_ = 0, lfoPhase_ = 0;
    // TIME-CONTROL: the delay glides to its new time
    float glide_ = 0, glideTarget_ = 0, glideRate_ = 0, outPan_ = 0;
    // reverbs
    Reverb verb_;
    DelayLine pre_;
    float preDelay_ = 0;
    int gateType_ = 0;
    float gateLen_ = 0;
    // the common stage
    OnePole lowL_, lowR_, highL_, highR_;
    float lowGain_ = 1, highGain_ = 1, dry_ = 1, wet_ = 0, level_out_ = 1;

    // ---- stage 3's blocks
    struct Drive                           // overdrive / distortion through an amp's speaker
    {
        float gain = 1, norm = 1, pan = 0;
        bool hard = false;
        OnePole cab, low;                  // the cabinet's top end; a low cut (the small amp's)
        bool lowCut = false;
        float process(float x);
    };
    struct Mod                             // chorus / flanger: a stereo modulated delay
    {
        DelayLine l, r;
        float base = 0, depth = 0, rate = 0, ph = 0, offset = 0.25f, fb = 0;
        float step = 0, stepPh = 0, heldL = 0, heldR = 0;   // the step flanger's held LFO
        int filter = 0;                    // on the effect's input: 0 off, 1 LPF, 2 HPF
        OnePole fL, fR;
        void process(float inL, float inR, float& oL, float& oR);
    };
    struct Echo                            // a mono delay with feedback and HF damp (the two-stage types)
    {
        DelayLine d;
        float time = 0, fb = 0;
        OnePole damp;
        bool damped = false;
        float process(float x);
    };
    struct Phaser
    {
        float ap[8] = {}, fbz = 0, manual = 1000, depth = 0, rate = 0, ph = 0, res = 0, mix = 0, pan = 0, coefA = 0;
        int count = 0;
        float process(float x, float sr);
    };
    struct Hexa                            // six chorus voices on one line
    {
        DelayLine d;
        float base[6] = {}, depth[6] = {}, pan[6] = {}, rate = 0, ph = 0;
        void process(float x, float& oL, float& oR);
    };
    Drive drive_;
    Mod modA_, modB_;
    Echo echo_;
    Phaser phaser_;
    Hexa hexa_;
    float enhSens_ = 0, enhMix_ = 0;
    OnePole enhL_, enhR_;
    float trmRate_ = 0, trmPh_ = 0, trmSep_ = 0;          // TREMOLO-CHORUS
    float balA_ = 1, balB_ = 1;                           // the two-stage types' stage balances (effect share)
    int first_ = 0, second_ = 0;                          // their stages: 1 OD 2 DS 3 EH 4 CHO 5 FL 6 DLY
    bool parallel_ = false;
    void stageA(float& l, float& r);
    void stageB(float& l, float& r);
    void runStage(int kind, Mod& m, float& l, float& r);
    void setStage3(int type, const uint8_t* params, float bpm);

    // ---- stage 4's blocks
    struct Biquad                          // RBJ cookbook filters, transposed direct form II
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void peak(float hz, float q, float db, float sr);
        void lowShelf(float hz, float db, float sr);
        void highShelf(float hz, float db, float sr);
    };
    struct Svf                             // the auto-wah's filter (topology-preserving state variable)
    {
        float ic1 = 0, ic2 = 0;
        void run(float x, float g, float k, float& lp, float& bp)
        {
            float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; lp = v2; bp = v1;
        }
    };
    struct Shifter                         // a pitch shifter: two taps sweeping a delay line, crossfaded
    {
        float ratio = 1, window = 1000, pre = 0, ph = 0;
        float tap(const DelayLine& d);
        void advance() { ph += (1.0f - ratio) / window; ph -= std::floor(ph); }
    };
    Biquad eqL_[4], eqR_[4], spec_[8];
    int eqCount_ = 0, specCount_ = 0;
    float monoPan_ = 0;                    // the mono types' output pan
    Svf wahL_, wahR_;
    float wahBase_ = 1000, wahDepth_ = 0, wahSens_ = 0, wahK_ = 1, wahEnv_ = 0, wahRate_ = 0, wahPh_ = 0;
    bool wahBpf_ = false;
    // rotary: the horn (above the crossover) and the drum (below), each with its speed gliding to its target
    float rotHorn_ = 0, rotDrum_ = 0, rotHornHz_ = 0, rotDrumHz_ = 0, rotHornTo_ = 0, rotDrumTo_ = 0;
    float rotHornAcc_ = 0, rotDrumAcc_ = 0, rotHornLvl_ = 1, rotDrumLvl_ = 1, rotSep_ = 1;
    OnePole rotSplit_;
    DelayLine rotDelay_;
    // compressor / limiter
    float cmpThresh_ = 1, cmpRatio_ = 1, cmpAtt_ = 0, cmpRel_ = 0, cmpEnv_ = 0, cmpPost_ = 1;
    // pitch shifters
    DelayLine pitchLine_;
    Shifter shA_, shB_;
    float pitchFb_ = 0, pitchY_ = 0, panA_ = 0, panB_ = 0, lvlA_ = 1, lvlB_ = 0;
    void setStage4(int type, const uint8_t* params, float bpm);
};

class Efx
{
public:
    void alloc(float sampleRate);
    void clear();
    void setType(int type, const uint8_t* params, float bpm = 120);   // clears the effect's state
    void setParams(int type, const uint8_t* params, float bpm = 120); // the same type, new values: no clearing
    void process(float* L, float* R, int n);   // in place
private:
    enum Kind { None, Drive, Dist, Phaser, AutoWah, Rotary, Comp, Enhance, Chorus, Flanger, Delay, Verb };
    void stage(Kind k, float& l, float& r);
    Kind a_ = None, b_ = None;
    bool parallel_ = false;
    float sr_ = 44100, drive_ = 0.5f;
    // states
    float apL_[4] = {}, apR_[4] = {}, phPhase_ = 0;
    float wahEnv_ = 0, wahL1_ = 0, wahL2_ = 0, wahR1_ = 0, wahR2_ = 0;
    float rotPhase_ = 0, rotPhase2_ = 0;
    float compEnv_ = 0;
    float enhL_ = 0, enhR_ = 0, toneL_ = 0, toneR_ = 0;
    ModDelay mod_;
    DelayLine dl_, dr_;
    Reverb verb_;
    EfxPro pro_;
    int type_ = -1;
};

class Fx
{
public:
    void prepare(float sampleRate);
    void setPatch(const uint8_t* common);      // from a patch's common block (srjv::Patch::common)
    // the buses of one block; out = dry + EFX + chorus + reverb
    void process(const float* dryL, const float* dryR, float* efxL, float* efxR, const float* choSend, const float* revSend,
                 float* outL, float* outR, int n, bool efxOn, bool chorusOn, bool reverbOn);
private:
    float sr_ = 44100;
    Efx efx_;
    ModDelay chorus_;
    Reverb reverb_;
    float efxMix_ = 1, efxCho_ = 0, efxRev_ = 0;
    float choLevel_ = 0, choToRev_ = 0, choToMix_ = 1;
    float revLevel_ = 0;
    int efxType_ = -1;                         // a new type clears the EFX; edits of the same type do not
};

} // namespace xp

#endif
