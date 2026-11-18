// engine.h - an XP-80 style patch engine, playing SR-JV80 cards (srjv.h).
//
// The XP-80's patch architecture (the JV-1080's): a patch is up to four tones. Each tone is a
//
//   WG (a multisample of a card, pitch envelope, 2 LFOs, keyfollow, random pitch, bend)
//     -> TVF (LPF / BPF / HPF / PKG, cutoff envelope, keyfollow, LFO)
//     -> TVA (level, bias, velocity, envelope, LFO, pan with keyfollow / random / alternate / LFO)
//     -> output: MIX (dry) or EFX, with chorus and reverb sends
//
// and responds to note range, velocity range, the matrix control (3 controllers x 4
// destinations), hold, bend and the patch's solo / portamento settings. Every sounding tone is
// one voice of the polyphony, as on the XP-80 (64 there; adjustable here for the MPC's CPU).
//
// Not modelled (they play as structure 1, all tones side by side): the tone structures 2-10
// (filter chaining, ring modulation, booster), FXM, analog feel, stretch tuning; the tone delay
// mode PLAYMATE plays as NORMAL, and TAP-SYNC follows the patch's tempo (there is no tap).
#ifndef XP_ENGINE_H
#define XP_ENGINE_H

#include "srjv.h"
#include "fx.h"

#include <cstdint>

namespace xp {

constexpr int kMaxVoices = 64;
constexpr int kCtl = 32;                 // control rate: envelopes, LFOs, filter coefficients

struct Env                               // a JV envelope: times T1..T4, levels L0..L4, sustain at L3
{
    float level = 0, target = 0, step = 0;
    float left = 0;                      // frames left in this stage (a stage lasts its time, even a flat one)
    float times[4] = {}, levels[5] = {};
    int stage = 0;                       // 0..2 to L1..L3, 3 sustain, 4 release to L4, 5 done
    void start(const float t[4], const float l[5], float sr);
    void release(float sr);
    float tick(int frames, float sr);    // advance one control block
    bool done() const { return stage >= 5; }
private:
    void go(int s, float sr);
};

struct Lfo
{
    float phase = 0, value = 0, held = 0, fade = 0, rate = 1, delay = 0, fadeTime = 0, t = 0, offset = 0;
    int wave = 0, fadeMode = 0;
    uint32_t rng = 1;
    bool released = false;
    const uint8_t* p = nullptr;           // the tone's LFO parameters: read on every tick, so edits act at once
    int rateRaw = -1, delayRaw = -1, fadeRaw = -1;   // what rate, delay and fadeTime were computed from
    void start(const uint8_t* p, bool keySync, uint32_t seed);
    void follow();                        // rate, wave, offset, delay and fade from the parameters
    float tick(float dt, float rateMul);
};

struct Svf
{
    float ic1 = 0, ic2 = 0, g = 0, k = 2, a1 = 1, a2 = 0, a3 = 0;
    void set(float hz, float reso, float sr);
    inline float lp(float x, float* bp, float* hp)
    {
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        *bp = v1;
        *hp = x - k * v1 - v2;
        return v2;
    }
};

// The LADDER filter model: a 4-pole low-pass (24 dB / octave) of four one-pole stages with feedback,
// solved without a one-sample delay in the loop (topology-preserving transform), its feedback
// saturating. Resonance to self-oscillation (k = 4); the bass it takes away partly made up.
struct Ladder
{
    float s[4] = {}, G = 0, k = 0, comp = 1;
    void set(float hz, float reso01, float sr);
    float process(float x);
};

struct Voice
{
    bool active = false, released = false, held = false, delayed = false;
    int note = 60, startNote = 60, key = 60, vel = 100, tone = 0, slot = 0;
    uint32_t age = 0;
    const srjv::Card* card = nullptr;
    const srjv::Patch* patch = nullptr;
    const srjv::Sample* smp = nullptr;
    const uint8_t* tp = nullptr;          // the tone's parameters
    double pos = 0;
    int dir = 1;
    float baseSemis = 0, glideFrom = 0, glide = 0, glideStep = 0, randCents = 0, gainLin = 1;
    float panBase = 0;
    float delayLeft = 0;                  // seconds still to wait; < 0: waiting for the note-off
    float delayAfterOff = 0;              // KEY-OFF modes: the wait that starts at the note-off
    uint8_t delayMode = 0;                // the tone delay mode (jvparams.h)
    bool releaseAtSustain = false;        // the key went up before the tone sounded: play it through, then release
    bool inLoop = false;                  // a forward loop reached: its own fine tune applies from then on
    Env penv, fenv, aenv;
    Lfo lfo1, lfo2;
    Svf f, f2;                            // f2: the 24 dB models' second stage
    Ladder lad;
    int ftype = 1;
    float ampPrev = 0, ampNow = 0, panL = 0.7f, panR = 0.7f;
    float srcRatio = 1;
    float stepNow = 0;
};

struct Settings                           // the plugin's own controls (offsets to the patch)
{
    int polyphony = 32;
    int octave = 0;                       // -3..+3
    bool toneOn[4] = {true, true, true, true};
    float cutoff = 0, reso = 0, attack = 0, release = 0;   // -1..+1
    bool efx = true, chorus = true, reverb = true;
    float hostTempo = 120;
    int filterModel = 0;                  // 0 XP-80 (2-pole, 12 dB), 1 24 dB (two stages), 2 LADDER (4-pole low-pass)
    float filterDrive = 0;                // 0..1: saturation into the filter                // the MPC's tempo (for patches whose clock source is the system)
};

class Engine
{
public:
    void prepare(float sampleRate);
    // the cards in slots A-D (null = empty); voices of a slot whose card changed stop at once
    void setCards(const srjv::Card* const cards[4]);
    void setPatch(int slot, int index);   // index into the card's patch table; -1 = none
    // The plugin's edit buffer: the patch to play, owned by the caller and edited in place while
    // it plays (`p` stays valid until replaced). Its waves are looked up as from `slot`'s board.
    // From then on setCards no longer picks patches by index.
    void setPatchData(int slot, const srjv::Patch* p);
    void refreshFx() { if (patch_) fx_.setPatch(patch_->common); }   // after an edit of the effects
    // stop the voices that play `p` (before the caller overwrites it)
    void stopVoicesOf(const srjv::Patch* p);
    // the JV-1080's own waves (srjv::loadInternal), for tones on INT-A / INT-B; null = none
    void setInternal(const srjv::Card* c);
    // the JV-80's own waves (srjv::loadJv880), for the JV-80 patches' INT tones; null = none
    void setInternal80(const srjv::Card* c);
    const srjv::Patch* patch() const { return patch_; }

    void noteOn(int note, int vel);
    void noteOff(int note);
    void controller(int cc, int value);
    void pitchBend(int value14);          // 0..16383
    void pressure(int value);
    void allNotesOff();
    void allSoundOff();
    void resetControllers();               // CC121, and at prepare

    void render(float* L, float* R, int frames);

    Settings settings;
    int activeVoices() const;

private:
    void startTone(int t, int note, int vel, int alt);
    Voice* allocate();
    void renderVoice(Voice& v, int frames);
    void controlBlock(Voice& v, int frames);
    float ctrlValue(int which) const;     // matrix controllers 1..3, 0..1
    const srjv::Card* cardForTone(const uint8_t* tp, int* waveNo) const;   // waveNo: index into its waves

    float sr_ = 44100.0f;
    const srjv::Card* cards_[4] = {};
    const srjv::Card* internal_ = nullptr;
    const srjv::Card* internal80_ = nullptr;
    const srjv::Card* patchCard_ = nullptr;
    int patchSlot_ = -1, patchIndex_ = -1;
    bool external_ = false;               // the patch comes from setPatchData
    const srjv::Patch* patch_ = nullptr;
    Voice voices_[kMaxVoices];
    uint32_t age_ = 0, rng_ = 12345;
    bool sustain_ = false;
    float bend_ = 0;                      // -1..+1
    uint8_t cc_[128] = {};
    float pressure_ = 0;
    int altCount_ = 0;
    int lastNote_ = -1;
    bool keys_[128] = {};
    int heldOrder_[128] = {}, heldCount_ = 0;
    // buses for one control block
    float dryL_[kCtl], dryR_[kCtl], efxL_[kCtl], efxR_[kCtl], cho_[kCtl], rev_[kCtl];
    Fx fx_;
};

} // namespace xp

#endif
