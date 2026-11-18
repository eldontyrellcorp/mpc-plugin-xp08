// engine.cpp - see engine.h. Parameter ranges are the JV-1080 / XP-80's (jvparams.h); how a
// value maps to seconds, hertz or octaves is this engine's approximation of the hardware.
#include "engine.h"
#include "jvparams.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xp {

namespace {

constexpr float kPi = 3.14159265f;

// Envelope (and LFO delay / fade, portamento) times, 0..127 -> seconds. Not measured on hardware:
// fitted to what the boards' patches need. A pizzicato's decay (Short Pizz T3 27: about 0.1 s;
// Pizzicato 1 T3 45: about 0.3 s) and the hold of the tempo patches' drum hits (T2 50, sized to
// 0.22-0.31 s kicks) give about 0.1 s at 27 and 0.4 s at 50; the same step (x1.063) on to 127,
// about 44 s. (The first curve, 0.05 s at 50 and 10 s at 127, cut pizzicati and hits to a click.)
inline float envTime(int v) { return jv::envSeconds(v); }
// The tone delay keeps the first curve (its values reach 125: 5 s there, not 40)
inline float toneDelayTime(int v) { return v <= 0 ? 0.0f : 0.0015f * std::exp((float)v * 0.0696f); }
inline float lfoRate(int v) { return 0.05f * std::exp2((float)v / 127.0f * 10.0f); }             // 0.05 .. ~51 Hz
inline float sgn63(int v) { return ((float)v - 63.0f) / 63.0f; }      // 0..126, 63 = 0 -> -1..+1
inline float pan64(int v) { return ((float)v - 64.0f) / 63.0f; }      // 0..127, 64 = centre -> -1..+1
inline float veloSens(int v) { return ((float)v - 50.0f) / 50.0f; }   // 0..125, 50 = 0 -> -1..+1.5
inline float semisToRatio(float s) { return std::exp2(s * (1.0f / 12.0f)); }

// Pitch LFO depth (-1..+1, i.e. -63..+63) -> semitones. Not measured on hardware: about 3 cents a
// step for the small depths the patches use for vibrato and drift (1..8), growing to an octave at
// 63 for the effect patches. (Linear +-4 semitones made a random LFO at depth 5 wobble +-25 cents.)
inline float pitchLfoSemis(float x)
{
    float a = std::fabs(x);
    float s = 2.0f * a + 10.0f * a * a * a;
    return x < 0 ? -s : s;
}

inline uint32_t xorshift(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float rnd(uint32_t& s) { return (float)(xorshift(s) & 0xFFFFFF) / 8388608.0f - 1.0f; }   // -1..+1

// velocity response: curve 0..6 bends it, sens -1..+1 says how much it matters
inline float velocityGain(int vel, int curve, float sens)
{
    static const float exps[7] = {1.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f};
    float v = std::pow((float)vel / 127.0f, exps[curve < 0 || curve > 6 ? 0 : curve]);
    return std::max(0.0f, sens >= 0 ? 1.0f - std::min(sens, 1.0f) * (1.0f - v) + std::max(0.0f, sens - 1.0f) * (v - 0.5f)
                                    : 1.0f + sens * v);
}

} // namespace

// ------------------------------------------------------------------ envelope
void Env::start(const float t[4], const float l[5], float sr)
{
    std::memcpy(times, t, sizeof(times));
    std::memcpy(levels, l, sizeof(levels));
    level = levels[0];
    go(0, sr);
}

void Env::go(int s, float sr)
{
    stage = s;
    if (s >= 5) { step = 0; return; }
    if (s == 3) { step = 0; target = level; return; }
    target = s < 3 ? levels[s + 1] : levels[4];
    left = std::max(1.0f, (s < 3 ? times[s] : times[3]) * sr);
    step = (target - level) / left;
}

void Env::release(float sr) { if (stage < 4) go(4, sr); }

float Env::tick(int frames, float sr)
{
    if (stage == 3 || stage >= 5) return level;
    // a stage ends when its time is up, not when the level meets the target: from L1 to an equal L2
    // the level holds for T2 (ending at the level would skip every hold)
    level += step * (float)frames;
    left -= (float)frames;
    if (left <= 0)
    {
        level = target;
        go(stage == 4 ? 5 : stage + 1, sr);
    }
    return level;
}

// ------------------------------------------------------------------ LFO
void Lfo::follow()
{
    static const float offsets[5] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    wave = p[jv::LFO_WAVE] & 7;
    offset = offsets[std::min<int>(p[jv::LFO_OFFSET], 4)];
    fadeMode = p[jv::LFO_FADE_MODE] & 3;
    if (p[jv::LFO_RATE] != rateRaw) { rateRaw = p[jv::LFO_RATE]; rate = lfoRate(rateRaw); }
    if (p[jv::LFO_DELAY] != delayRaw) { delayRaw = p[jv::LFO_DELAY]; delay = envTime(delayRaw); }
    if (p[jv::LFO_FADE_TIME] != fadeRaw) { fadeRaw = p[jv::LFO_FADE_TIME]; fadeTime = envTime(fadeRaw); }
}

void Lfo::start(const uint8_t* params, bool keySync, uint32_t seed)
{
    p = params;
    rateRaw = delayRaw = fadeRaw = -1;
    follow();
    rng = seed | 1;
    phase = (keySync || p[jv::LFO_KEY_SYNC]) ? 0.0f : (float)(xorshift(rng) & 0xFFFF) / 65536.0f;
    t = 0;
    released = false;
    held = rnd(rng);
    value = 0;
}

float Lfo::tick(float dt, float rateMul)
{
    follow();
    float prev = phase;
    phase += rate * rateMul * dt;
    bool wrapped = phase >= 1.0f;
    phase -= std::floor(phase);
    t += dt;
    float w;
    switch (wave)
    {
        case 0: w = phase < 0.5f ? 4 * phase - 1 : 3 - 4 * phase; break;              // TRI
        case 1: w = std::sin(2 * kPi * phase); break;                                  // SIN
        case 2: w = 1 - 2 * phase; break;                                              // SAW (down)
        case 3: w = phase < 0.5f ? 1.0f : -1.0f; break;                                // SQR
        case 4: { float x = phase < 0.5f ? 4 * phase - 1 : 3 - 4 * phase;             // TRP
                  w = std::max(-1.0f, std::min(1.0f, 2 * x)); break; }
        case 5: if (wrapped || prev > phase) held = rnd(rng); w = held; break;          // S&H
        case 6: if (wrapped || prev > phase) held = rnd(rng);                          // RND (smoothed)
                value += (held - value) * std::min(1.0f, rate * dt * 4); w = value; break;
        default: value += rnd(rng) * rate * dt * 2; value = std::max(-1.0f, std::min(1.0f, value)); w = value; break; // CHS
    }
    // fade: ON-IN / OFF-IN rise after the delay, ON-OUT / OFF-OUT fall after it
    float amp = 1;
    bool fromNoteOff = fadeMode >= 2;
    if (fromNoteOff && !released) amp = fadeMode == 2 ? 0.0f : 1.0f;
    else
    {
        float tt = t - delay;
        bool in = (fadeMode & 1) == 0;
        if (tt < 0) amp = in ? 0.0f : 1.0f;
        else if (fadeTime > 0) amp = in ? std::min(1.0f, tt / fadeTime) : std::max(0.0f, 1.0f - tt / fadeTime);
        else amp = in ? 1.0f : 0.0f;
    }
    return amp * (w * (1.0f - 0.5f * std::fabs(offset)) + 0.5f * offset);
}

// ------------------------------------------------------------------ filter
inline float softClip(float x) { if (x > 3) return 1; if (x < -3) return -1; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }

void Ladder::set(float hz, float reso01, float sr)
{
    hz = std::max(16.0f, std::min(hz, sr * 0.45f));
    float g = std::tan(kPi * hz / sr);
    G = g / (1.0f + g);
    reso01 = std::max(0.0f, std::min(reso01, 1.0f));
    k = 4.1f * reso01;                    // 127: past 4, it sings on its own
    comp = 1.0f + 0.5f * k;               // half of the level the feedback takes from the bass
}

float Ladder::process(float x)
{
    // each stage: y = G u + (1 - G) s. Four in a row, with u = x - k y4, solved for y4 at once
    float G2 = G * G, G4 = G2 * G2, one = 1.0f - G;
    float sig = one * (G2 * G * s[0] + G2 * s[1] + G * s[2] + s[3]);
    float y4 = (G4 * x * comp + sig) / (1.0f + k * G4);
    float u = softClip(x * comp - k * y4);   // the saturating feedback path
    for (float& st : s)
    {
        float v = (u - st) * G;
        float y = v + st;
        st = y + v;
        u = y;
    }
    return u;
}

void Svf::set(float hz, float reso, float sr)
{
    hz = std::max(16.0f, std::min(hz, sr * 0.45f));
    g = std::tan(kPi * hz / sr);
    k = 2.0f - 1.96f * std::max(0.0f, std::min(reso, 1.0f));
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

// ------------------------------------------------------------------ engine
void Engine::resetControllers()
{
    std::memset(cc_, 0, sizeof(cc_));
    cc_[7] = 100;     // the power-on values: volume 100, expression 127, pan centre
    cc_[10] = 64;
    cc_[11] = 127;
    bend_ = 0;
    pressure_ = 0;
}

void Engine::prepare(float sampleRate)
{
    sr_ = sampleRate;
    resetControllers();
    fx_.prepare(sr_);
    if (patch_) fx_.setPatch(patch_->common);
    allSoundOff();
}

void Engine::setCards(const srjv::Card* const cards[4])
{
    for (int s = 0; s < 4; ++s)
    {
        if (cards[s] == cards_[s]) continue;
        for (Voice& v : voices_) if (v.active && (v.card == cards_[s] || v.slot == s)) v.active = false;
        if (patchSlot_ == s) { patch_ = nullptr; patchCard_ = nullptr; }
        cards_[s] = cards[s];
    }
    if (!external_ && patchSlot_ >= 0 && !patch_) setPatch(patchSlot_, patchIndex_);
    if (external_ && patch_ && patchSlot_ >= 0) patchCard_ = cards_[patchSlot_];
}

void Engine::setPatchData(int slot, const srjv::Patch* p)
{
    external_ = true;
    patchSlot_ = slot;
    patchIndex_ = -1;
    patchCard_ = (slot >= 0 && slot < 4) ? cards_[slot] : nullptr;
    patch_ = p;
    if (patch_) fx_.setPatch(patch_->common);
}

void Engine::stopVoicesOf(const srjv::Patch* p)
{
    for (Voice& v : voices_) if (v.active && v.patch == p) v.active = false;
}

void Engine::setPatch(int slot, int index)
{
    patchSlot_ = slot;
    patchIndex_ = index;
    const srjv::Card* c = (slot >= 0 && slot < 4) ? cards_[slot] : nullptr;
    const srjv::Patch* p = (c && index >= 0 && index < (int)c->patches.size()) ? &c->patches[(size_t)index] : nullptr;
    if (p == patch_) return;
    patch_ = p;
    patchCard_ = c;
    if (patch_) fx_.setPatch(patch_->common);
}

void Engine::setInternal(const srjv::Card* c)
{
    if (c == internal_) return;
    for (Voice& v : voices_) if (v.active && v.card == internal_) v.active = false;
    internal_ = c;
}

void Engine::setInternal80(const srjv::Card* c)
{
    if (c == internal80_) return;
    for (Voice& v : voices_) if (v.active && v.card == internal80_) v.active = false;
    internal80_ = c;
}

const srjv::Card* Engine::cardForTone(const uint8_t* tp, int* waveNo) const
{
    *waveNo = tp[jv::T_WAVE_HI] << 4 | tp[jv::T_WAVE_LO];
    int id = tp[jv::T_WAVE_GROUP_ID];
    if (tp[jv::T_WAVE_GROUP] == 0 && id == 0)         // the JV-80's INT waves (a JV-80 patch's INT tones)
        return internal80_ && *waveNo < (int)internal80_->waves.size() ? internal80_ : nullptr;
    if (tp[jv::T_WAVE_GROUP] == 0)                     // INT: group id 1 = INT-A, 2 = INT-B (from a JV-1080's ROMs)
    {
        if (!internal_ || internal_->intB < 0 || (id != 1 && id != 2)) return nullptr;
        int n = id == 1 ? internal_->intB : (int)internal_->waves.size() - internal_->intB;
        if (*waveNo >= n) return nullptr;
        if (id == 2) *waveNo += internal_->intB;
        return internal_;
    }
    if (tp[jv::T_WAVE_GROUP] != 2) return nullptr;     // PCM-card waves are not on SR-JV80 boards
    for (const srjv::Card* c : cards_) if (c && c->groupId == id) return c;
    if (patchCard_ && (patchCard_->groupId == id || patchCard_->groupId < 0)) return patchCard_;
    return nullptr;
}

int Engine::activeVoices() const
{
    int n = 0;
    for (const Voice& v : voices_) n += v.active ? 1 : 0;
    return n;
}

Voice* Engine::allocate()
{
    int poly = std::max(1, std::min(settings.polyphony, kMaxVoices));
    int used = 0;
    Voice* free = nullptr;
    for (Voice& v : voices_) { if (v.active) ++used; else if (!free) free = &v; }
    if (used < poly && free) return free;
    Voice* best = nullptr;     // steal: the oldest released voice, else the oldest
    for (Voice& v : voices_)
    {
        if (!v.active) continue;
        if (!best || (v.released && !best->released) || (v.released == best->released && v.age < best->age)) best = &v;
    }
    return best;
}

float Engine::ctrlValue(int which) const
{
    if (which == 0) return cc_[1] / 127.0f;           // controller 1: the modulation lever
    if (!patch_) return 0;
    int src = patch_->common[which == 1 ? jv::C_CTRL_SRC2 : jv::C_CTRL_SRC3];
    switch (src)
    {
        case 1: return pressure_;                      // SYS-CTRL1 (here: aftertouch, which the MPC's pads send)
        case 2: return cc_[4] / 127.0f;                // SYS-CTRL2 (here: the foot controller, CC4)
        case 3: return cc_[1] / 127.0f;                // MODULATION
        case 4: return cc_[2] / 127.0f;                // BREATH
        case 5: return cc_[4] / 127.0f;                // FOOT
        case 6: return cc_[7] / 127.0f;                // VOLUME
        case 7: return cc_[10] / 127.0f;               // PAN
        case 8: return cc_[11] / 127.0f;               // EXPRESSION
        case 9: return std::fabs(bend_);               // BENDER
        case 10: return pressure_;                     // AFTERTOUCH
        default: return 0;
    }
}

void Engine::noteOn(int note, int vel)
{
    if (!patch_ || vel <= 0) { noteOff(note); return; }
    const uint8_t* c = patch_->common;
    bool solo = c[jv::C_KEY_ASSIGN] == 1;
    bool anyHeld = heldCount_ > 0;
    if (!keys_[note]) { keys_[note] = true; heldOrder_[heldCount_++ < 128 ? heldCount_ - 1 : 127] = note; }

    if (solo)
    {
        bool legato = c[jv::C_SOLO_LEGATO] && anyHeld;
        bool retuned = false;
        for (Voice& v : voices_)
        {
            if (!v.active || v.patch != patch_) continue;
            if (legato && !v.released)
            {
                // legato: the sounding tones move to the new note without restarting
                v.glideFrom = (float)(v.note - note) + v.glide;
                v.glide = v.glideFrom;
                float t = envTime(c[jv::C_PORTA_TIME]) * 0.5f;
                v.glideStep = (c[jv::C_PORTA_SW] && t > 0) ? v.glide / (t * sr_ / kCtl) : v.glide;
                v.note = note;
                retuned = true;
            }
            else if (!v.released) { v.released = true; v.aenv.release(sr_); v.fenv.release(sr_); v.penv.release(sr_); }
        }
        if (retuned) { lastNote_ = note; return; }
    }

    int alt = (altCount_++ & 1) ? -1 : 1;
    for (int t = 0; t < 4; ++t) startTone(t, note, vel, alt);
    lastNote_ = note;
}

void Engine::startTone(int t, int note, int vel, int alt)
{
    const srjv::Patch& p = *patch_;
    const uint8_t* c = p.common;
    const uint8_t* tp = p.tone[t];
    if (!tp[jv::T_SWITCH] || !settings.toneOn[t]) return;
    int key = note + 12 * ((int)c[jv::C_OCTAVE] - 3 + settings.octave);
    if (key < 0 || key > 127) return;
    if (note < tp[jv::T_KEY_LO] || note > tp[jv::T_KEY_HI]) return;

    float xfadeGain = 1;
    if (c[jv::C_VELO_RANGE_SW])
    {
        int lo = std::max(1, (int)tp[jv::T_VELO_LO]), hi = std::max(lo, (int)tp[jv::T_VELO_HI]), xf = tp[jv::T_VELO_XFADE];
        if (vel < lo) { if (xf == 0 || vel < lo - xf) return; xfadeGain = (float)(vel - (lo - xf)) / (float)xf; }
        if (vel > hi) { if (xf == 0 || vel > hi + xf) return; xfadeGain = (float)((hi + xf) - vel) / (float)xf; }
    }

    int waveNo = 0;
    const srjv::Card* card = cardForTone(tp, &waveNo);
    if (!card || waveNo >= (int)card->waves.size()) return;
    const uint16_t* zone = card->waves[(size_t)waveNo].zoneFor(key);
    if (!zone || *zone >= card->samples.size()) return;
    const srjv::Sample* smp = &card->samples[*zone];
    if (smp->end <= smp->start) return;

    Voice* vp = allocate();
    if (!vp) return;
    Voice& v = *vp;
    v = Voice();
    v.active = true;
    v.age = ++age_;
    v.note = note;
    v.startNote = note;
    v.key = key;
    v.vel = vel;
    v.tone = t;
    v.slot = patchSlot_;
    v.card = card;
    v.patch = patch_;
    v.smp = smp;
    v.tp = tp;
    v.srcRatio = (float)card->sampleRate / sr_;
    bool reverse = smp->loopType == srjv::LoopReverse || smp->loopType == srjv::LoopReverseOff;
    v.pos = reverse ? (double)smp->end : (double)smp->start;
    v.dir = reverse ? -1 : 1;

    // pitch: keyfollow around C4, then the sample's own root and tuning
    float kf = jv::kKeyfollow[tp[jv::T_PITCH_KF] & 15] / 100.0f;
    float keyF = 60.0f + (float)(key - 60) * kf;
    v.baseSemis = keyF - (float)smp->rootKey + smp->fineSemis + (float)((int)tp[jv::T_COARSE] - 48) +
                  (float)((int)tp[jv::T_FINE] - 50) / 100.0f;
    int rp = std::min<int>(tp[jv::T_RANDOM_PITCH], 30);
    v.randCents = jv::kRandomPitch[rp] * rnd(rng_) / 100.0f;

    // portamento from the last note
    if (c[jv::C_PORTA_SW] && lastNote_ >= 0 && lastNote_ != note && (c[jv::C_PORTA_MODE] == 0 || heldCount_ > 1))
    {
        float tm = envTime(c[jv::C_PORTA_TIME]) * 0.5f;
        v.glide = (float)(lastNote_ - note);
        v.glideStep = tm > 0 ? v.glide / (tm * sr_ / kCtl) : v.glide;
    }

    // level: tone, wave gain, sample volume, patch, velocity, bias, crossfade
    static const float waveGain[4] = {0.5f, 1.0f, 2.0f, 4.0f};
    float g = tp[jv::T_LEVEL] / 127.0f * waveGain[tp[jv::T_WAVE_GAIN] & 3] * (smp->volume / 127.0f) * (c[jv::C_LEVEL] / 127.0f);
    g *= velocityGain(vel, tp[jv::T_AENV_CURVE], veloSens(tp[jv::T_AENV_VELO]));
    {
        int dir = tp[jv::T_BIAS_DIR], point = tp[jv::T_BIAS_POINT];
        float amount = ((float)std::min<int>(tp[jv::T_BIAS_LEVEL], 14) - 7.0f) / 7.0f;   // -1..+1
        float oct = (float)(key - point) / 12.0f;
        bool applies = (dir == 0 && oct < 0) || (dir == 1 && oct > 0) || dir == 2 || dir == 3;
        if (applies && amount != 0) g *= std::max(0.0f, 1.0f + amount * std::fabs(oct) * 0.5f);
    }
    v.gainLin = g * xfadeGain;

    // pan: tone, keyfollow, random, alternate, patch
    float pan = pan64(tp[jv::T_PAN]) + pan64(c[jv::C_PAN]);
    pan += jv::kTimeSens[std::min<int>(tp[jv::T_PAN_KF], 14)] / 100.0f * (float)(key - 60) / 64.0f;
    pan += (tp[jv::T_RANDOM_PAN] / 63.0f) * rnd(rng_);
    pan += pan64(tp[jv::T_ALT_PAN]) * (float)alt;
    v.panBase = pan;

    // envelopes; times follow velocity (T1, T4) and key
    float keyT = (float)(key - 60) / 12.0f;
    auto times = [&](uint8_t t1, uint8_t velT1, uint8_t velT4, uint8_t timeKf, float* out) {
        float kfT = jv::kTimeSens[tp[timeKf] & 15] / 100.0f;
        float scale = std::exp2(-kfT * keyT);
        float velAmt = (float)(vel - 64) / 63.0f;
        for (int i = 0; i < 4; ++i) out[i] = envTime(tp[t1 + i]) * scale;
        out[0] *= std::exp2(-jv::kTimeSens[tp[velT1] & 15] / 100.0f * velAmt * 2.0f);
        out[3] *= std::exp2(-jv::kTimeSens[tp[velT4] & 15] / 100.0f * velAmt * 2.0f);
    };
    float t4[4], l5[5];
    times(jv::T_PENV_T1, jv::T_PENV_VELO_T1, jv::T_PENV_VELO_T4, jv::T_PENV_TIME_KF, t4);
    l5[0] = 0;
    for (int i = 0; i < 4; ++i) l5[i + 1] = sgn63(tp[jv::T_PENV_L1 + i]);
    v.penv.start(t4, l5, sr_);

    times(jv::T_FENV_T1, jv::T_FENV_VELO_T1, jv::T_FENV_VELO_T4, jv::T_FENV_TIME_KF, t4);
    l5[0] = 0;
    for (int i = 0; i < 4; ++i) l5[i + 1] = tp[jv::T_FENV_L1 + i] / 127.0f;
    v.fenv.start(t4, l5, sr_);

    times(jv::T_AENV_T1, jv::T_AENV_VELO_T1, jv::T_AENV_VELO_T4, jv::T_AENV_TIME_KF, t4);
    t4[0] *= std::exp2(settings.attack * 4.0f);
    t4[3] *= std::exp2(settings.release * 4.0f);
    l5[0] = 0; l5[4] = 0;
    for (int i = 0; i < 3; ++i) l5[i + 1] = tp[jv::T_AENV_L1 + i] / 127.0f;
    v.aenv.start(t4, l5, sr_);

    uint32_t seed = xorshift(rng_);
    v.lfo1.start(tp + jv::T_LFO1, false, seed);
    v.lfo2.start(tp + jv::T_LFO2, false, seed * 7919u);

    v.ftype = std::min<int>(tp[jv::T_FILTER_TYPE], 4);

    // tone delay. Modes (the boards use all seven): 0 NORMAL, 1 HOLD, 2 PLAYMATE (played as NORMAL),
    // 3 CLOCK-SYNC and 4 TAP-SYNC (the time in clock ticks, 24 a quarter note, at the patch's tempo;
    // no tap here: TAP-SYNC follows the same tempo), 5 KEY-OFF-NORMAL, 6 KEY-OFF-DECAY.
    int dm = std::min<int>(tp[jv::T_DELAY_MODE], 6);
    float wait = toneDelayTime(tp[jv::T_DELAY_TIME]);
    if (dm == 3 || dm == 4)
    {
        float bpm = c[jv::C_CLOCK_SRC] == 1 ? settings.hostTempo : (float)(c[jv::C_TEMPO_HI] * 16 + c[jv::C_TEMPO_LO]);
        bpm = std::max(20.0f, std::min(bpm, 250.0f));
        wait = (float)tp[jv::T_DELAY_TIME] / 24.0f * 60.0f / bpm;
    }
    v.delayMode = (uint8_t)dm;
    if (dm == 5 || dm == 6) { v.delayed = true; v.delayLeft = -1; v.delayAfterOff = wait; }
    else { v.delayed = wait > 0; v.delayLeft = wait; }
    controlBlock(v, 0);
    v.ampPrev = v.ampNow;
}

void Engine::noteOff(int note)
{
    if (keys_[note])
    {
        keys_[note] = false;
        int j = 0;
        for (int i = 0; i < heldCount_; ++i) if (heldOrder_[i] != note) heldOrder_[j++] = heldOrder_[i];
        heldCount_ = j;
    }
    bool solo = patch_ && patch_->common[jv::C_KEY_ASSIGN] == 1;
    for (Voice& v : voices_)
    {
        if (!v.active || v.note != note || v.released) continue;
        if (v.delayed)
        {
            if (v.delayMode == 5 || v.delayMode == 6)       // KEY-OFF: the wait starts now
            {
                if (v.delayLeft >= 0) continue;
                v.delayLeft = v.delayAfterOff;
                if (v.delayLeft <= 0) { v.delayed = false; v.delayLeft = 0; }
                if (v.delayMode == 6)                       // its envelope ran while the key was down: the release is heard
                {
                    v.released = true;
                    v.aenv.release(sr_); v.fenv.release(sr_); v.penv.release(sr_);
                    v.lfo1.released = v.lfo2.released = true;
                }
                else v.releaseAtSustain = true;             // NORMAL envelope from the start, then its release
                continue;
            }
            if (v.delayMode == 1) { v.active = false; continue; }   // HOLD: up before the tone sounded
            v.releaseAtSustain = true;                      // NORMAL, PLAYMATE, CLOCK / TAP-SYNC: it still sounds
            continue;
        }
        if (sustain_ && v.tp[jv::T_HOLD_SW]) { v.held = true; continue; }
        v.released = true;
        v.aenv.release(sr_); v.fenv.release(sr_); v.penv.release(sr_);
        v.lfo1.released = v.lfo2.released = true;
    }
    // solo: back to the last key still held
    if (solo && heldCount_ > 0 && patch_ && note == lastNote_)
    {
        int back = heldOrder_[heldCount_ - 1];
        bool sounding = false;
        for (Voice& v : voices_) if (v.active && !v.released && v.note == back) sounding = true;
        if (!sounding) { int keep = heldCount_; heldCount_ = 0; noteOn(back, 100); heldCount_ = keep; }
    }
}

void Engine::controller(int cc, int value)
{
    cc &= 127;
    cc_[cc] = (uint8_t)(value & 127);
    if (cc == 64)
    {
        sustain_ = value >= 64;
        if (!sustain_)
            for (Voice& v : voices_)
                if (v.active && v.held) { v.held = false; v.released = true; v.aenv.release(sr_); v.fenv.release(sr_); v.penv.release(sr_); }
    }
    else if (cc == 120) allSoundOff();
    else if (cc == 121) { resetControllers(); sustain_ = false; }
    else if (cc == 123) allNotesOff();
}

void Engine::pitchBend(int value14) { bend_ = std::max(-1.0f, std::min(1.0f, ((float)value14 - 8192.0f) / 8191.0f)); }
void Engine::pressure(int value) { pressure_ = (float)(value & 127) / 127.0f; }

void Engine::allNotesOff()
{
    for (int n = 0; n < 128; ++n) if (keys_[n]) noteOff(n);
    sustain_ = false;
    for (Voice& v : voices_)
        if (v.active && !v.released) { v.held = false; v.released = true; v.aenv.release(sr_); v.fenv.release(sr_); v.penv.release(sr_); }
}

void Engine::allSoundOff()
{
    for (Voice& v : voices_) v.active = false;
    std::memset(keys_, 0, sizeof(keys_));
    heldCount_ = 0;
    sustain_ = false;
}

// Envelopes, LFOs, matrix, pitch, cutoff, level and pan for the next `frames` frames.
void Engine::controlBlock(Voice& v, int frames)
{
    const uint8_t* tp = v.tp;
    const uint8_t* c = v.patch->common;
    float dt = (float)frames / sr_;

    // matrix control: 3 controllers x 4 destinations
    float mod[jv::D_COUNT] = {};
    for (int k = 0; k < 3; ++k)
    {
        float val = ctrlValue(k);
        if (val == 0) continue;
        for (int e = 0; e < 4; ++e)
        {
            int dest = tp[jv::T_MATRIX + k * 8 + e * 2];
            if (dest > 0 && dest < jv::D_COUNT) mod[dest] += val * sgn63(tp[jv::T_MATRIX + k * 8 + e * 2 + 1]);
        }
    }
    float l1 = v.lfo1.tick(dt, std::exp2(mod[jv::D_L1_RATE] * 3.0f));
    float l2 = v.lfo2.tick(dt, std::exp2(mod[jv::D_L2_RATE] * 3.0f));
    float pe = v.penv.tick(frames, sr_), fe = v.fenv.tick(frames, sr_), ae = v.aenv.tick(frames, sr_);

    // pitch (semitones)
    float penvDepth = (float)((int)tp[jv::T_PENV_DEPTH] - 12) * velocityGain(v.vel, 0, veloSens(tp[jv::T_PENV_VELO]));
    if (v.glide != 0)
    {
        v.glide -= v.glideStep * (frames > 0 ? (float)frames / kCtl : 0.0f);
        if ((v.glideStep > 0 && v.glide < 0) || (v.glideStep < 0 && v.glide > 0)) v.glide = 0;
    }
    float bendSemis = 0;
    if (tp[jv::T_BEND_SW]) bendSemis = bend_ * (float)(bend_ >= 0 ? c[jv::C_BEND_UP] : c[jv::C_BEND_DOWN]);
    float semis = v.baseSemis + v.glide + v.randCents +
                  pe * penvDepth + bendSemis + mod[jv::D_PITCH] * 12.0f +
                  l1 * pitchLfoSemis(sgn63(tp[jv::T_PITCH_LFO1]) + mod[jv::D_L1_PITCH]) +
                  l2 * pitchLfoSemis(sgn63(tp[jv::T_PITCH_LFO2]) + mod[jv::D_L2_PITCH]);
    // a forward loop's own fine tune, once the voice has reached the loop. Sticky: after a wrap the
    // position lands a fraction before the loop start (on the sample equal to the end), and going
    // by the position alone switched the tuning off for a block at every pass: a click
    if (v.smp->loopType == srjv::LoopForward && !v.inLoop && v.pos >= (double)v.smp->loop) v.inLoop = true;
    if (v.inLoop) semis += v.smp->loopFineSemis;
    // a legato note moves the voice to another note: its distance to the note it started on
    semis += (float)(v.note - v.startNote) * (jv::kKeyfollow[tp[jv::T_PITCH_KF] & 15] / 100.0f);
    v.stepNow = v.srcRatio * semisToRatio(semis);

    // filter
    if (v.ftype != 0)
    {
        float oct = (float)tp[jv::T_CUTOFF] / 127.0f * 10.2f + settings.cutoff * 4.0f;
        oct += jv::kKeyfollow[tp[jv::T_CUTOFF_KF] & 15] / 100.0f * (float)(v.key - 60) / 12.0f;
        oct += fe * sgn63(tp[jv::T_FENV_DEPTH]) * 7.0f * velocityGain(v.vel, tp[jv::T_FENV_CURVE], veloSens(tp[jv::T_FENV_VELO]));
        oct += l1 * (sgn63(tp[jv::T_FILTER_LFO1]) + mod[jv::D_L1_FILTER]) * 2.0f;
        oct += l2 * (sgn63(tp[jv::T_FILTER_LFO2]) + mod[jv::D_L2_FILTER]) * 2.0f;
        oct += mod[jv::D_CUTOFF] * 6.0f;
        float reso = tp[jv::T_RESONANCE] / 127.0f + settings.reso * 0.5f + mod[jv::D_RESO] * 0.6f;
        float hz = 20.0f * std::exp2(oct);
        v.f.set(hz, reso, sr_);
        if (settings.filterModel >= 1) v.f2.set(hz, 0.3f, sr_);        // the second stage: no resonance of its own (Q 0.7)
        if (settings.filterModel == 2 && v.ftype == 1) v.lad.set(hz, std::max(0.0f, std::min(reso, 1.0f)), sr_);
    }

    // level
    float a = ae * ae * v.gainLin;
    a *= 1.0f + 0.5f * l1 * (sgn63(tp[jv::T_AMP_LFO1]) + mod[jv::D_L1_AMP]);
    a *= 1.0f + 0.5f * l2 * (sgn63(tp[jv::T_AMP_LFO2]) + mod[jv::D_L2_AMP]);
    a *= std::max(0.0f, 1.0f + mod[jv::D_LEVEL]);
    if (tp[jv::T_VOLUME_SW]) { float vol = cc_[7] / 127.0f, ex = cc_[11] / 127.0f; a *= vol * vol * ex * ex; }
    v.ampNow = std::max(0.0f, a);

    // pan, constant power
    float pan = v.panBase + mod[jv::D_PAN] + l1 * (sgn63(tp[jv::T_PAN_LFO1]) + mod[jv::D_L1_PAN]) +
                l2 * (sgn63(tp[jv::T_PAN_LFO2]) + mod[jv::D_L2_PAN]);
    if (tp[jv::T_PAN_SW]) pan += ((float)cc_[10] - 64.0f) / 63.0f;
    pan = std::max(-1.0f, std::min(1.0f, pan));
    float ang = (pan + 1.0f) * (kPi * 0.25f);
    v.panL = std::cos(ang);
    v.panR = std::sin(ang);

    if (v.aenv.done()) v.active = false;
}

void Engine::renderVoice(Voice& v, int n)
{
    const srjv::Sample& s = *v.smp;
    const int16_t* pcm = v.card->pcm.data();
    const double start = s.start, end = s.end, loop = s.loop;
    const bool looped = s.loopType == srjv::LoopForward || s.loopType == srjv::LoopPingPong;
    const bool ping = s.loopType == srjv::LoopPingPong;
    const double loopLen = end + 1 - loop;
    // Ping-pong, as DPCM hardware plays it: running backwards it keeps adding the deltas, so the
    // backward pass is the waveform reversed and inverted. The boards cut these loops so that the
    // decoded value is 0 at `end` and at `loop - 1`: the two turns are point reflections through
    // those zeros, smooth both ways. (Reversed without the inversion, each turn folds the wave.)
    const double pingLow = loop > start ? loop - 1 : loop;
    const float step = v.stepNow;
    const int tone = v.tp[jv::T_OUTPUT];
    float* outL = tone == 1 ? efxL_ : dryL_;
    float* outR = tone == 1 ? efxR_ : dryR_;
    const float send = v.tp[jv::T_SEND_LEVEL] / 127.0f;
    const float choS = v.tp[jv::T_CHORUS_SEND] / 127.0f, revS = v.tp[jv::T_REVERB_SEND] / 127.0f;
    const float aStep = (v.ampNow - v.ampPrev) / (float)n;
    float amp = v.ampPrev;
    const float scale = 1.0f / 32768.0f;
    const int model = settings.filterModel;
    const float drive = settings.filterDrive;
    const float driveIn = 1.0f + 7.0f * drive, driveOut = 1.0f / (1.0f + 1.5f * drive);   // into the clipper; the level back
    for (int i = 0; i < n; ++i)
    {
        int idx = (int)v.pos;
        float fr = (float)(v.pos - (double)idx);
        int nx = idx + 1;
        if (nx > (int)end) nx = looped && !ping ? (int)loop : (int)end;
        float x = ((float)pcm[idx] + ((float)pcm[nx] - (float)pcm[idx]) * fr) * scale;
        if (ping && v.dir < 0) x = -x;

        if (drive > 0) x = softClip(x * driveIn) * driveOut;              // Filter Drive
        float y = x;
        if (v.ftype != 0)
        {
            if (model == 2 && v.ftype == 1) y = v.lad.process(x);           // LADDER low-pass
            else
            {
                float bp, hp;
                float lp = v.f.lp(x, &bp, &hp);
                switch (v.ftype)
                {
                    case 1: y = lp; break;
                    case 2: y = bp * v.f.k; break;
                    case 3: y = hp; break;
                    default: y = x + bp * v.f.k * 3.0f; break;     // PKG: a peak at the cutoff
                }
                if (model >= 1 && v.ftype <= 3)                    // 24 dB: the same through a second stage
                {
                    float bp2, hp2;
                    float lp2 = v.f2.lp(y, &bp2, &hp2);
                    y = v.ftype == 1 ? lp2 : v.ftype == 3 ? hp2 : bp2 * v.f2.k;
                }
            }
        }
        amp += aStep;
        y *= amp;
        float l = y * v.panL, r = y * v.panR;
        outL[i] += l * (tone == 1 ? 1.0f : send);
        outR[i] += r * (tone == 1 ? 1.0f : send);
        if (tone != 1) { cho_[i] += (l + r) * 0.5f * choS; rev_[i] += (l + r) * 0.5f * revS; }

        v.pos += step * (double)v.dir;
        if (v.dir > 0 && v.pos > end)
        {
            if (!looped) { v.active = false; break; }
            if (ping) { v.pos = end - (v.pos - end); v.dir = -1; }
            // forward: between end and end + 1 the voice still plays from the end towards the loop
            // start (nx above); it wraps once past end + 1, landing inside the loop. (Wrapping at
            // `end` and moving up to the loop start skipped up to a sample at every pass: a click.)
            else if (v.pos >= end + 1) v.pos -= loopLen;
        }
        else if (v.dir < 0 && v.pos < (ping ? pingLow : start))
        {
            if (ping) { v.pos = pingLow + (pingLow - v.pos); v.dir = 1; }
            else { v.active = false; break; }
        }
    }
    v.ampPrev = v.ampNow;
}

void Engine::render(float* L, float* R, int frames)
{
    int done = 0;
    while (done < frames)
    {
        int n = std::min(kCtl, frames - done);
        std::memset(dryL_, 0, sizeof(float) * (size_t)n);
        std::memset(dryR_, 0, sizeof(float) * (size_t)n);
        std::memset(efxL_, 0, sizeof(float) * (size_t)n);
        std::memset(efxR_, 0, sizeof(float) * (size_t)n);
        std::memset(cho_, 0, sizeof(float) * (size_t)n);
        std::memset(rev_, 0, sizeof(float) * (size_t)n);
        for (Voice& v : voices_)
        {
            if (!v.active) continue;
            if (v.delayed)
            {
                // waiting: the envelopes and LFOs start with the tone (KEY-OFF-DECAY's run all along)
                if (v.delayMode == 6) controlBlock(v, n);
                if (v.delayLeft > 0 && (v.delayLeft -= (float)n / sr_) <= 0) { v.delayed = false; v.delayLeft = 0; }
                continue;
            }
            controlBlock(v, n);
            if (v.releaseAtSustain && !v.released && v.aenv.stage >= 3)
            {
                v.released = true;
                v.aenv.release(sr_); v.fenv.release(sr_); v.penv.release(sr_);
                v.lfo1.released = v.lfo2.released = true;
            }
            if (!v.active) continue;
            renderVoice(v, n);
        }
        fx_.process(dryL_, dryR_, efxL_, efxR_, cho_, rev_, L + done, R + done, n,
                    settings.efx, settings.chorus, settings.reverb);
        done += n;
    }
}

} // namespace xp
