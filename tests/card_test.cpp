// card_test.cpp - builds an SR-JV80 image with known contents (our own samples and patches, in
// the format srjv.h reads), scrambles it, loads it back with srjv::load and checks everything,
// then plays it through the engine and measures the pitch and level.
//
//   tests/run.sh   (or: card_test <out.bin> to also keep the image, e.g. for roms/)
#include "srjv.h"
#include "jvparams.h"
#include "engine.h"
#include "fx.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { std::printf("PASS  "); } else { std::printf("FAIL  "); ++failures; } std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

namespace {

void put16(std::vector<uint8_t>& r, uint32_t at, uint32_t v) { r[at] = (uint8_t)(v >> 8); r[at + 1] = (uint8_t)v; }
void put24(std::vector<uint8_t>& r, uint32_t at, uint32_t v) { r[at] = (uint8_t)(v >> 16); r[at + 1] = (uint8_t)(v >> 8); r[at + 2] = (uint8_t)v; }
void put32(std::vector<uint8_t>& r, uint32_t at, uint32_t v) { put16(r, at, v >> 16); put16(r, at + 2, v & 0xFFFF); }

struct Src { std::vector<float> x; int root; bool looped; uint32_t loopFrom; bool ping = false; int fine = 1024; };

// DPCM: one signed byte per sample, a shift per 16 samples; the decoder sums (byte << shift) << 14.
void encode(std::vector<uint8_t>& rom, uint32_t at, const std::vector<float>& x)
{
    uint32_t acc = 0;
    for (size_t f = 0; f < x.size(); f += 16)
    {
        // the smallest shift that fits this frame's steps (plus the error carried in)
        int64_t worst = 0;
        int64_t prev = (int64_t)(int32_t)acc;
        for (size_t i = f; i < std::min(x.size(), f + 16); ++i)
        {
            int64_t t = (int64_t)std::llround(x[i] * 2147483647.0);
            worst = std::max<int64_t>(worst, std::llabs(t - prev));
            prev = t;
        }
        int e = 0;
        while (e < 15 && (worst + (int64_t(1) << (14 + e))) >> (14 + e) > 120) ++e;
        uint32_t a = at + (uint32_t)f;
        uint32_t ex = ((a & 0xFFFFFu) >> 5) | (a & 0xF00000u);
        if (a & 0x10) rom[ex] = (uint8_t)((rom[ex] & 0x0F) | e << 4); else rom[ex] = (uint8_t)((rom[ex] & 0xF0) | e);
        for (size_t i = f; i < std::min(x.size(), f + 16); ++i)
        {
            int64_t t = (int64_t)std::llround(x[i] * 2147483647.0);
            int64_t d = t - (int64_t)(int32_t)acc;
            int64_t q = (d + (d >= 0 ? 1 : -1) * (int64_t(1) << (13 + e))) / (int64_t(1) << (14 + e));
            q = std::max<int64_t>(-128, std::min<int64_t>(127, q));
            rom[at + i] = (uint8_t)(int8_t)q;
            acc += ((uint32_t)(int32_t)q << e) << 14;
        }
    }
}

srjv::Patch makePatch(const char* name, int efx)
{
    srjv::Patch p{};
    for (int i = 0; i < 12; ++i) p.common[i] = (uint8_t)(name[i] ? name[i] : ' ');
    for (int i = (int)std::strlen(name); i < 12; ++i) p.common[i] = ' ';
    p.common[jv::C_EFX_TYPE] = (uint8_t)efx;
    p.common[jv::C_EFX_MIX_SEND] = 127;
    p.common[jv::C_CHORUS_LEVEL] = 0;
    p.common[jv::C_REVERB_TYPE] = 4; p.common[jv::C_REVERB_LEVEL] = 0; p.common[jv::C_REVERB_TIME] = 80;
    p.common[jv::C_TEMPO_HI] = 7; p.common[jv::C_TEMPO_LO] = 8;
    p.common[jv::C_LEVEL] = 127; p.common[jv::C_PAN] = 64;
    p.common[jv::C_BEND_UP] = 2; p.common[jv::C_BEND_DOWN] = 2;
    p.common[jv::C_OCTAVE] = 3; p.common[jv::C_PORTA_TIME] = 20;
    p.common[jv::C_CATEGORY] = 5;
    return p;
}

void setTone(srjv::Patch& p, int t, int wave, int group = 2, int id = 42)
{
    uint8_t* tp = p.tone[t];
    std::memset(tp, 0, srjv::kToneBytes);
    tp[jv::T_SWITCH] = 1;
    tp[jv::T_WAVE_GROUP] = (uint8_t)group; tp[jv::T_WAVE_GROUP_ID] = (uint8_t)id;
    tp[jv::T_WAVE_HI] = (uint8_t)(wave >> 4); tp[jv::T_WAVE_LO] = (uint8_t)(wave & 15);
    tp[jv::T_WAVE_GAIN] = 1;
    tp[jv::T_VELO_LO] = 1; tp[jv::T_VELO_HI] = 127; tp[jv::T_KEY_LO] = 0; tp[jv::T_KEY_HI] = 127;
    for (int m = 0; m < 12; ++m) tp[jv::T_MATRIX + 2 * m + 1] = 63;
    for (int l : {jv::T_LFO1, jv::T_LFO2}) { tp[l + jv::LFO_RATE] = 60; tp[l + jv::LFO_OFFSET] = 2; }
    tp[jv::T_COARSE] = 48; tp[jv::T_FINE] = 50; tp[jv::T_PITCH_KF] = 12; tp[jv::T_PENV_DEPTH] = 12;
    tp[jv::T_PENV_VELO] = 50;
    for (int i = 0; i < 4; ++i) tp[jv::T_PENV_L1 + i] = 63;
    tp[jv::T_PITCH_LFO1] = tp[jv::T_PITCH_LFO2] = 63;
    tp[jv::T_FILTER_TYPE] = 0; tp[jv::T_CUTOFF] = 127; tp[jv::T_CUTOFF_KF] = 5; tp[jv::T_FENV_DEPTH] = 63; tp[jv::T_FENV_VELO] = 50;
    tp[jv::T_FILTER_LFO1] = tp[jv::T_FILTER_LFO2] = 63;
    tp[jv::T_LEVEL] = 127; tp[jv::T_BIAS_LEVEL] = 7; tp[jv::T_AENV_VELO] = 50; tp[jv::T_AENV_TIME_KF] = 7;
    tp[jv::T_AENV_VELO_T1] = 7; tp[jv::T_AENV_VELO_T4] = 7; tp[jv::T_FENV_TIME_KF] = 7; tp[jv::T_PENV_TIME_KF] = 7;
    tp[jv::T_AENV_T1] = 0; tp[jv::T_AENV_T1 + 1] = 0; tp[jv::T_AENV_T1 + 2] = 0; tp[jv::T_AENV_T1 + 3] = 20;
    tp[jv::T_AENV_L1] = 127; tp[jv::T_AENV_L1 + 1] = 127; tp[jv::T_AENV_L1 + 2] = 127;
    tp[jv::T_AMP_LFO1] = tp[jv::T_AMP_LFO2] = 63;
    tp[jv::T_PAN] = 64; tp[jv::T_PAN_KF] = 7; tp[jv::T_ALT_PAN] = 64; tp[jv::T_PAN_LFO1] = tp[jv::T_PAN_LFO2] = 63;
    tp[jv::T_VOLUME_SW] = 1;   // as on the boards: so the test also covers the MIDI volume / expression defaults
    tp[jv::T_OUTPUT] = 0; tp[jv::T_SEND_LEVEL] = 127;
}

std::vector<uint8_t> buildCard(std::vector<Src>& srcs, std::vector<srjv::Patch>& patches)
{
    std::vector<uint8_t> rom(srjv::kCardSize, 0);
    std::memcpy(&rom[0x20], "SR-JV80-TEST    ", 16);
    std::memcpy(&rom[0x30], "2026-11-15", 10);
    put16(rom, 0x40, 320);                      // 32 kHz
    // samples from 32 KB on, each on a 32-byte boundary
    const double sr = 32000.0;
    srcs.clear();
    auto tone = [&](double hz, int root, double secs, int shape) {
        Src s; s.root = root; s.looped = true;
        size_t n = (size_t)(sr * secs);
        for (size_t i = 0; i < n; ++i)
        {
            double ph = std::fmod(hz * (double)i / sr, 1.0);
            s.x.push_back(shape == 0 ? (float)(0.5 * std::sin(2 * M_PI * ph)) : (float)(0.4 * (2 * ph - 1)));
        }
        s.loopFrom = (uint32_t)(n - (size_t)std::lround(sr / hz * std::floor(hz * secs / 2)));   // whole cycles
        srcs.push_back(s);
    };
    tone(440.0, 69, 0.5, 0);
    tone(130.8128, 48, 0.6, 1);
    tone(523.2511, 72, 0.4, 1);
    {
        Src s; s.root = 60; s.looped = false; s.loopFrom = 0;
        uint32_t r = 1;
        for (int i = 0; i < 8000; ++i) { r = r * 1664525u + 1013904223u; s.x.push_back((float)((int32_t)r / 2147483648.0 * 0.7 * std::exp(-i / 1500.0))); }
        srcs.push_back(s);
    }
    {
        // ping-pong as the boards cut it: 500 Hz (64 samples a cycle at 32 kHz), the loop from a
        // zero crossing (loop - 1) to a zero crossing (end); 500 Hz is MIDI 71 + 21.3 cents
        Src s; s.root = 71; s.looped = true; s.ping = true; s.fine = 1024 - 218; s.loopFrom = 3201;
        for (int i = 0; i <= 6400; ++i) s.x.push_back((float)(0.5 * std::sin(2 * M_PI * i / 64.0)));
        srcs.push_back(s);
    }
    uint32_t at = 0x8000;
    const uint32_t sampleTable = 0x70000, waveTable = 0x71000;
    for (size_t i = 0; i < srcs.size(); ++i)
    {
        Src& s = srcs[i];
        encode(rom, at, s.x);
        uint32_t e = sampleTable + (uint32_t)i * 18;
        rom[e] = 127;
        put24(rom, e + 1, at);
        put24(rom, e + 4, at + s.loopFrom);
        put24(rom, e + 7, at + (uint32_t)s.x.size() - 1);
        rom[e + 0x0C] = s.ping ? srjv::LoopPingPong : s.looped ? srjv::LoopForward : srjv::LoopOff;
        rom[e + 0x0D] = (uint8_t)s.root;
        put16(rom, e + 0x0E, (uint32_t)s.fine);
        put16(rom, e + 0x10, 1024);
        at = (at + (uint32_t)s.x.size() + 31) & ~31u;
    }
    put16(rom, 0x60, (uint32_t)srcs.size());
    put32(rom, 0x80, sampleTable);
    // multisamples: SINE (1 zone), SAW (2 zones), HIT (1 zone)
    struct W { const char* name; std::vector<std::pair<int, int>> zones; };
    std::vector<W> waves = {{"SINE TEST", {{127, 0}}}, {"SAW TEST", {{59, 1}, {127, 2}}}, {"HIT TEST", {{127, 3}}}, {"PING TEST", {{127, 4}}}};
    for (size_t w = 0; w < waves.size(); ++w)
    {
        uint32_t e = waveTable + (uint32_t)w * 60;
        std::memset(&rom[e], ' ', 12);
        std::memcpy(&rom[e], waves[w].name, std::strlen(waves[w].name));
        for (size_t z = 0; z < waves[w].zones.size(); ++z)
        {
            rom[e + 12 + z] = (uint8_t)waves[w].zones[z].first;
            put16(rom, e + 28 + 2 * (uint32_t)z, (uint32_t)waves[w].zones[z].second);
        }
    }
    put16(rom, 0x62, (uint32_t)waves.size());
    put32(rom, 0x84, waveTable);
    // block 0: no JV-80 patches; block 1: the JV-1080 table
    patches.clear();
    srjv::Patch p = makePatch("Sine Test", 0); setTone(p, 0, 0); patches.push_back(p);
    p = makePatch("Saw Split", 0); setTone(p, 0, 1); setTone(p, 1, 2); p.tone[1][jv::T_SWITCH] = 0; patches.push_back(p);
    p = makePatch("Drive Hit", 1); setTone(p, 0, 2); p.common[jv::C_EFX_PARAM] = 90; p.tone[0][jv::T_OUTPUT] = 1;
    p.common[jv::C_EFX_PARAM + 1] = 64; p.common[jv::C_EFX_PARAM + 3] = 15; p.common[jv::C_EFX_PARAM + 4] = 15;   // OVERDRIVE: pan centre, EQ flat,
    p.common[jv::C_EFX_PARAM + 5] = 127; patches.push_back(p);                                                 // level full
    p = makePatch("Internal", 0); setTone(p, 0, 5, 0, 0); patches.push_back(p);   // an XP-80 internal wave: not on the board
    p = makePatch("Filter Sweep", 4); setTone(p, 0, 1); p.tone[0][jv::T_FILTER_TYPE] = 1; p.tone[0][jv::T_CUTOFF] = 40;
    p.tone[0][jv::T_RESONANCE] = 90; p.tone[0][jv::T_FENV_DEPTH] = 120; p.tone[0][jv::T_FENV_T1] = 60; p.tone[0][jv::T_FENV_L1] = 127;
    p.tone[0][jv::T_FENV_L1 + 1] = 60; p.tone[0][jv::T_FENV_L1 + 2] = 60; p.common[jv::C_CHORUS_LEVEL] = 100; p.common[jv::C_REVERB_LEVEL] = 90;
    p.tone[0][jv::T_CHORUS_SEND] = 100; p.tone[0][jv::T_REVERB_SEND] = 100; patches.push_back(p);
    p = makePatch("Ping Pong", 0); setTone(p, 0, 3); patches.push_back(p);
    const uint32_t patchTable = 0x1F0000;
    for (size_t i = 0; i < patches.size(); ++i) srjv::packPatch(patches[i], &rom[patchTable + (uint32_t)i * srjv::kPackedPatch]);
    put16(rom, 0x100066, (uint32_t)patches.size());
    put32(rom, 0x10008C, patchTable);
    rom[0x100053] = 0x05;
    return rom;
}

// ---- a JV-1080 internal ROM set of our own: a program ROM with the wave lists and the two tables,
// three INT-A chips and one INT-B chip, with the measured address and data line orders.
// A wave 1 = a 440 Hz sine placed in the second INT-A chip; B wave 1 = a C5 saw.
const int kIntAddr[21] = {0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16, 9, 6, 8, 14, 17, 11, 15, 18, 19, 20};
const int kIntData[8] = {2, 0, 4, 5, 7, 6, 3, 1};   // plain data bit k is dump bit kIntData[k]

std::vector<uint8_t> scrambleChip(const std::vector<uint8_t>& plain, char bank)
{
    std::vector<uint8_t> dump(plain.size());
    for (uint32_t i = 0; i < plain.size(); ++i)
    {
        uint32_t a = 0;
        for (int k = 0; k < 21; ++k) a |= ((i >> kIntAddr[k]) & 1u) << k;
        uint8_t p = plain[a], d = 0;
        for (int k = 0; k < 8; ++k) d |= (uint8_t)(((p >> k) & 1) << kIntData[k]);
        dump[i] = d;
    }
    // the chips' id as it reads in dump order (over plain 0x00..0x1F, which the tables never use)
    char id[33];
    std::snprintf(id, sizeof id, "RolandJV1080O.S.Intwave Ver1,00%c", bank);
    std::memcpy(dump.data(), id, 32);
    return dump;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& v)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(v.data(), 1, v.size(), f) == v.size();
    return std::fclose(f) == 0 && ok;
}

// writes <base>-prom.bin, -wave1..3.bin (INT-A), -wave4.bin (INT-B)
bool buildInternal(const std::string& base)
{
    const uint32_t chip = 2u << 20;
    std::vector<uint8_t> a(3 * chip, 0), b(chip, 0), prom(1u << 20, 0xFF);
    for (std::vector<uint8_t>* r : {&a, &b})
        for (uint32_t blk = 0; blk < r->size(); blk += 1u << 20)
        {
            std::memcpy(&(*r)[blk + 0x20], r == &a ? "INT100A" : "INT100B", 7);
            std::memcpy(&(*r)[blk + 0x30], "2026-11-15", 10);
            put16(*r, blk + 0x40, 320);
        }
    const double sr = 32000;
    std::vector<float> sine, saw;
    for (int i = 0; i < 16000; ++i) sine.push_back((float)(0.5 * std::sin(2 * M_PI * 440.0 * i / sr)));
    for (int i = 0; i < 16000; ++i) saw.push_back((float)(0.4 * (2 * std::fmod(523.2511 * i / sr, 1.0) - 1)));
    const uint32_t sineAt = chip + 0x8000, sawAt = 0x8000;
    encode(a, sineAt, sine);
    encode(b, sawAt, saw);
    // tables: A 200 waves / 16 samples, B 60 waves / 16 samples; wave 0 of each is the test wave
    struct T { uint32_t at, nW, nS; } tab[2] = {{0x71000, 200, 16}, {0x7A800, 60, 16}};
    for (int t = 0; t < 2; ++t)
    {
        uint32_t h = tab[t].at, waves = h + 8, samples = waves + tab[t].nW * 60;
        put16(prom, h, tab[t].nW);
        put16(prom, h + 2, tab[t].nS);
        put32(prom, h + 4, 0x0A000000u | samples);
        for (uint32_t w = 0; w < tab[t].nW; ++w)
        {
            uint32_t e = waves + w * 60;
            char name[13];
            std::snprintf(name, sizeof name, "%-12s", w ? "INT DUMMY" : (t ? "INT SAW" : "INT SINE"));
            std::memcpy(&prom[e], name, 12);
            prom[e + 12] = 127;
            for (int z = 1; z < 16; ++z) prom[e + 12 + z] = 0;
            for (int z = 0; z < 16; ++z) put16(prom, e + 28 + 2 * z, w ? 1 : 0);
        }
        for (uint32_t i = 0; i < tab[t].nS; ++i)
        {
            uint32_t e = samples + i * 18, from = i ? 0x7000 : (t ? sawAt : sineAt);
            uint32_t len = i ? 16 : 16000;
            std::memset(&prom[e], 0, 18);
            prom[e] = 127;
            put24(prom, e + 1, from);
            put24(prom, e + 4, from + len / 2);
            put24(prom, e + 7, from + len - 1);
            prom[e + 0x0C] = srjv::LoopForward;
            prom[e + 0x0D] = t ? 72 : 69;
            put16(prom, e + 0x0E, 1024);
            put16(prom, e + 0x10, 1024);
        }
    }
    // the wave lists: INT-A 1..200 = A waves, 201..255 = B waves; INT-B 1..60 = B waves, then A waves
    for (int i = 0; i < 255; ++i) { bool inB = i >= 200; put16(prom, 0x300 + 2 * i, inB ? i - 200 : i); prom[0x300 + 510 + i] = inB; }
    const uint32_t listB = (0x300 + 765 + 1) & ~1u;
    for (int i = 0; i < 193; ++i) { bool inB = i < 60; put16(prom, listB + 2 * i, inB ? i : i - 60); prom[listB + 386 + i] = inB; }
    bool ok = writeFile(base + "-prom.bin", prom);
    for (int c = 0; c < 3; ++c)
        ok = writeFile(base + "-wave" + std::to_string(c + 1) + ".bin",
                       scrambleChip(std::vector<uint8_t>(a.begin() + c * chip, a.begin() + (c + 1) * chip), 'A')) && ok;
    return writeFile(base + "-wave4.bin", scrambleChip(b, 'B')) && ok;
}

// ---- a JV-880 style set of our own: a 256 KB program ROM (16 multisamples, then the sample table)
// and two 2 MB wave ROMs wired as the boards are. JV-80 INT wave 1 = a 440 Hz sine in the second chip.
bool buildJv880(const std::string& base)
{
    const uint32_t chip = 2u << 20;
    std::vector<uint8_t> img(2 * chip, 0), prom(256u << 10, 0xFF);
    for (uint32_t blk = 0; blk < img.size(); blk += 1u << 20)
    {
        std::memcpy(&img[blk + 0x20], "INT100", 6);
        std::memcpy(&img[blk + 0x30], "2026-11-16", 10);
        put16(img, blk + 0x40, 320);
    }
    std::vector<float> sine;
    for (int i = 0; i < 16000; ++i) sine.push_back((float)(0.5 * std::sin(2 * M_PI * 440.0 * i / 32000.0)));
    const uint32_t at = chip + 0x8000;
    encode(img, at, sine);
    const uint32_t nW = 16, nS = 16, st = 4 + nW * 60;
    prom[0] = (uint8_t)nW; prom[1] = 0; put16(prom, 2, st);
    for (uint32_t w = 0; w < nW; ++w)
    {
        uint32_t e = 4 + w * 60;
        char name[13];
        std::snprintf(name, sizeof name, "%-12s", w ? "JV80 DUMMY" : "JV80 SINE");
        std::memcpy(&prom[e], name, 12);
        prom[e + 12] = 127;
        for (int z = 1; z < 16; ++z) prom[e + 12 + z] = 0;
        for (int z = 0; z < 16; ++z) put16(prom, e + 28 + 2 * z, w ? 1 : 0);
    }
    for (uint32_t i = 0; i < nS; ++i)
    {
        uint32_t e = st + i * 18, from = i ? 0x7000 : at, len = i ? 16 : 16000;
        std::memset(&prom[e], 0, 18);
        prom[e] = 127;
        put24(prom, e + 1, from); put24(prom, e + 4, from + len / 2); put24(prom, e + 7, from + len - 1);
        prom[e + 0x0C] = srjv::LoopForward; prom[e + 0x0D] = 69;
        put16(prom, e + 0x0E, 1024); put16(prom, e + 0x10, 1024);
    }
    bool ok = writeFile(base + "-prom.bin", prom);
    for (int c = 0; c < 2; ++c)
    {
        std::vector<uint8_t> dump = srjv::scramble(std::vector<uint8_t>(img.begin() + c * chip, img.begin() + (c + 1) * chip), 0);
        std::memcpy(dump.data(), "Roland JV80     Internal Ver1.00", 32);   // the chips' id, in dump order
        ok = writeFile(base + "-wave" + std::to_string(c + 1) + ".bin", dump) && ok;
    }
    return ok;
}

float pitchOf(const std::vector<float>& x, float sr)
{
    // rising zero crossings with interpolation, over the whole buffer
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0 && x[i] >= 0)
        {
            double t = (double)(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            ++n;
        }
    return n > 1 ? (float)((n - 1) * sr / (last - first)) : 0.0f;
}

float rmsDb(const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to && i < x.size(); ++i) s += (double)x[i] * x[i];
    return (float)(10 * std::log10(s / (double)(to - from) + 1e-20));
}

std::vector<float> play(xp::Engine& e, int note, int vel, float secs, float release, float sr)
{
    std::vector<float> L((size_t)(secs * sr)), R(L.size());
    e.noteOn(note, vel);
    size_t off = (size_t)(release * sr);
    for (size_t i = 0; i < L.size(); i += 100)
    {
        if (i <= off && off < i + 100) e.noteOff(note);
        e.render(&L[i], &R[i], (int)std::min<size_t>(100, L.size() - i));
    }
    return L;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<Src> srcs;
    std::vector<srjv::Patch> patches;
    std::vector<uint8_t> plain = buildCard(srcs, patches);
    std::string base = argc > 1 ? argv[1] : "build/tests/test-card";

    // ---- the bit packing round-trips, and the image scrambles and descrambles
    {
        uint8_t packed[srjv::kPackedPatch];
        srjv::packPatch(patches[4], packed);
        srjv::Patch back = srjv::unpackPatch(packed);
        CHECK(std::memcmp(&back, &patches[4], sizeof back) == 0, "a patch packs into 401 bytes and unpacks unchanged");
    }
    for (int v = 0; v < 2; ++v)
    {
        std::vector<uint8_t> dump = srjv::scramble(plain, v);
        CHECK(srjv::descramble(dump, v) == plain, "data variant %d: scramble then descramble gives the image back", v);
        CHECK(std::memcmp(&dump[0x20], "SR-JV80-TEST", 12) != 0, "data variant %d: the dump's header is unreadable before descrambling", v);
        std::string path = base + "-v" + std::to_string(v) + ".bin";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::printf("FAIL  cannot write %s\n", path.c_str()); return 1; }
        std::fwrite(dump.data(), 1, dump.size(), f);
        std::fclose(f);

        std::string id;
        CHECK(srjv::probe(path, &id) && id == "SR-JV80-TEST", "data variant %d: probe finds the card (\"%s\")", v, id.c_str());
        auto card = srjv::load(path);
        CHECK(card->ok(), "data variant %d: loads (%s)", v, card->ok() ? "ok" : card->error.c_str());
        if (!card->ok()) continue;
        CHECK(card->sampleRate == 32000 && card->date == "2026-11-15", "header: %u Hz, date %s", card->sampleRate, card->date.c_str());
        CHECK(card->samples.size() == 5 && card->waves.size() == 4, "%zu samples, %zu multisamples", card->samples.size(), card->waves.size());
        CHECK(card->waves.size() == 4 && card->waves[1].name == "SAW TEST" && card->waves[1].count == 2, "multisample 2 \"%s\", %d zones",
              card->waves.size() > 1 ? card->waves[1].name.c_str() : "?", card->waves.size() > 1 ? card->waves[1].count : 0);
        CHECK(card->patches.size() == patches.size(), "%zu patches in the JV-1080 table", card->patches.size());
        bool same = card->patches.size() == patches.size();
        for (size_t i = 0; same && i < patches.size(); ++i) same = std::memcmp(&card->patches[i], &patches[i], sizeof(srjv::Patch)) == 0;
        CHECK(same, "every patch parameter as written (\"%s\" ... \"%s\")", card->patches.empty() ? "" : card->patches[0].name().c_str(),
              card->patches.empty() ? "" : card->patches.back().name().c_str());
        CHECK(card->groupId == 42, "the board's wave group id from its patches: %d", card->groupId);
        // the DPCM decode against the source
        double err = 0, sig = 0;
        for (size_t i = 0; i < srcs.size(); ++i)
        {
            const srjv::Sample& s = card->samples[i];
            for (size_t k = 0; k < srcs[i].x.size(); ++k)
            {
                double a = card->pcm[s.start + k] / 32768.0, b = srcs[i].x[k];
                err += (a - b) * (a - b); sig += b * b;
            }
        }
        float snr = (float)(10 * std::log10(sig / (err + 1e-20)));
        CHECK(snr > 35, "the waves decode within %.1f dB of the source (DPCM)", snr);
    }

    // ---- the engine
    auto card = srjv::load(base + "-v0.bin");
    if (!card->ok()) { std::printf("FAIL  no card for the engine tests\n"); return 1; }
    const float sr = 44100;
    static xp::Engine e;
    e.prepare(sr);
    const srjv::Card* cards[4] = {card.get(), nullptr, nullptr, nullptr};
    e.setCards(cards);
    e.setPatch(0, 0);
    CHECK(e.patch() && e.patch()->name() == "Sine Test", "patch EXP-A 001 is \"%s\"", e.patch() ? e.patch()->name().c_str() : "-");
    std::vector<float> a4 = play(e, 69, 100, 1.0f, 0.8f, sr);
    std::vector<float> held(a4.begin() + 4410, a4.begin() + 30000);
    float hz = pitchOf(held, sr);
    CHECK(std::fabs(hz - 440.0f) < 1.0f, "A4 on the 440 Hz sample (32 kHz card, 44.1 kHz out) plays at %.2f Hz", hz);
    std::vector<float> a5 = play(e, 81, 100, 0.5f, 0.4f, sr);
    hz = pitchOf(std::vector<float>(a5.begin() + 4410, a5.begin() + 15000), sr);
    CHECK(std::fabs(hz - 880.0f) < 2.0f, "A5 plays at %.2f Hz (keyfollow +100)", hz);
    float on = rmsDb(a4, 4410, 30000), off = rmsDb(a4, (size_t)(0.95 * sr), a4.size());
    CHECK(on > -20 && off < on - 40, "it sounds (%.1f dB) and stops after the release (%.1f dB)", on, off);
    e.allSoundOff();

    // the multisample zones: C3 on the low saw (root C3), C5 on the high one (root C5)
    e.setPatch(0, 1);
    std::vector<float> c3 = play(e, 48, 100, 0.5f, 0.45f, sr);
    hz = pitchOf(std::vector<float>(c3.begin() + 2000, c3.begin() + 18000), sr);
    CHECK(std::fabs(hz - 130.81f) < 1.0f, "C3 on SAW TEST plays at %.2f Hz", hz);
    std::vector<float> c5 = play(e, 72, 100, 0.5f, 0.45f, sr);
    hz = pitchOf(std::vector<float>(c5.begin() + 2000, c5.begin() + 18000), sr);
    CHECK(std::fabs(hz - 523.25f) < 2.0f, "C5 plays the high zone at %.2f Hz", hz);
    e.allSoundOff();

    // a one-shot through the EFX (overdrive), a tone on an internal wave (silent), a filtered patch with chorus and reverb
    e.setPatch(0, 2);
    std::vector<float> hit = play(e, 60, 127, 0.6f, 0.5f, sr);
    CHECK(rmsDb(hit, 0, 4000) > -25 && e.activeVoices() == 0, "the one-shot plays (%.1f dB) and ends by itself", rmsDb(hit, 0, 4000));
    e.allSoundOff();
    e.setPatch(0, 3);
    std::vector<float> in = play(e, 60, 100, 0.3f, 0.2f, sr);
    CHECK(rmsDb(in, 0, in.size()) < -100, "a tone on an XP-80 internal wave stays silent (%.1f dB)", rmsDb(in, 0, in.size()));
    e.setPatch(0, 4);
    std::vector<float> sw = play(e, 48, 100, 2.0f, 1.0f, sr);
    bool finite = true;
    for (float x : sw) finite = finite && std::isfinite(x) && std::fabs(x) < 4.0f;
    CHECK(finite && rmsDb(sw, 2000, 40000) > -30, "the filter / chorus / reverb patch is finite and sounds (%.1f dB)", rmsDb(sw, 2000, 40000));
    CHECK(rmsDb(sw, (size_t)(1.6 * sr), sw.size()) > -90, "the reverb tail rings on after the release (%.1f dB)", rmsDb(sw, (size_t)(1.6 * sr), sw.size()));

    // ---- the JV-1080's internal waves (our own ROM set)
    {
        std::string ib = base + "-int";
        CHECK(buildInternal(ib), "wrote a JV-1080 style ROM set (%s-*.bin)", ib.c_str());
        std::vector<std::string> waveA = {ib + "-wave1.bin", ib + "-wave2.bin", ib + "-wave3.bin"};
        bool parts = srjv::internalPart(ib + "-prom.bin") == srjv::InternalPart::Program &&
                     srjv::internalPart(waveA[1]) == srjv::InternalPart::WaveA &&
                     srjv::internalPart(ib + "-wave4.bin") == srjv::InternalPart::WaveB &&
                     srjv::internalPart(base + "-v0.bin") == srjv::InternalPart::None;
        CHECK(parts, "internalPart tells the program ROM, the INT-A and INT-B chips and a board apart");
        std::string id;
        CHECK(!srjv::probe(waveA[0], &id), "a wave ROM is not listed as a board");
        auto in = srjv::loadInternal(ib + "-prom.bin", waveA, ib + "-wave4.bin");
        CHECK(in->ok(), "the internal waves load (%s)", in->ok() ? "ok" : in->error.c_str());
        if (in->ok())
        {
            CHECK(in->waves.size() == 448 && in->intB == 255, "%zu waves, INT-B from %d", in->waves.size(), in->intB);
            CHECK(in->waves[0].name == "INT SINE" && in->waves[200].name == "INT SAW" && in->waves[255].name == "INT SAW" &&
                  in->waves[255 + 60].name == "INT SINE", "the wave lists pick each wave's table");
            srjv::Card host;   // a board slot with patches only: every tone on the internal waves
            srjv::Patch p = makePatch("Int A Sine", 0); setTone(p, 0, 0, 0, 1); host.patches.push_back(p);
            p = makePatch("Int B Saw", 0); setTone(p, 0, 0, 0, 2); host.patches.push_back(p);
            const srjv::Card* hc[4] = {&host, nullptr, nullptr, nullptr};
            e.allSoundOff();
            e.setCards(hc);
            e.setPatch(0, 0);
            std::vector<float> none = play(e, 69, 100, 0.3f, 0.2f, sr);
            CHECK(rmsDb(none, 0, none.size()) < -100, "without the internal ROMs an INT tone is silent (%.1f dB)", rmsDb(none, 0, none.size()));
            e.setInternal(in.get());
            std::vector<float> x = play(e, 69, 100, 0.6f, 0.5f, sr);
            hz = pitchOf(std::vector<float>(x.begin() + 4410, x.begin() + 20000), sr);
            CHECK(std::fabs(hz - 440.0f) < 1.0f, "INT-A wave 1 (in the second chip) plays A4 at %.2f Hz", hz);
            e.allSoundOff();
            e.setPatch(0, 1);
            x = play(e, 72, 100, 0.6f, 0.5f, sr);
            hz = pitchOf(std::vector<float>(x.begin() + 4410, x.begin() + 20000), sr);
            CHECK(std::fabs(hz - 523.25f) < 2.0f, "INT-B wave 1 plays C5 at %.2f Hz", hz);
            e.allSoundOff();
            e.setInternal(nullptr);
        }
        e.setCards(cards);
    }

    // a forward loop: through its wraps (every 0.25 s here), the sine goes on without a kink
    {
        e.allSoundOff();
        e.setPatch(0, 0);
        std::vector<float> x = play(e, 70, 100, 1.5f, 1.4f, sr);    // B4: a non-integer step, wraps land between samples
        std::vector<float> held(x.begin() + 9000, x.begin() + 60000);
        double d2max = 0, amp = 0;
        for (size_t i = 1; i + 1 < held.size(); ++i)
        {
            d2max = std::max(d2max, (double)std::fabs(held[i + 1] - 2 * held[i] + held[i - 1]));
            amp = std::max(amp, (double)std::fabs(held[i]));
        }
        double w = 2 * M_PI * 466.16 / sr, ratio = d2max / (amp * w * w);
        CHECK(ratio < 2.0, "a forward loop's wraps are smooth (largest bend %.2f x a sine's)", ratio);
        e.allSoundOff();
    }

    // a ping-pong loop: back and forth through the turns, the sine goes on at its pitch, without a kink
    {
        e.allSoundOff();
        e.setPatch(0, 5);
        std::vector<float> x = play(e, 71, 100, 1.5f, 1.4f, sr);   // 0.2 s in, then about 7 turns
        std::vector<float> held(x.begin() + 9000, x.begin() + 60000);
        hz = pitchOf(held, sr);
        double d2max = 0, amp = 0;
        for (size_t i = 1; i + 1 < held.size(); ++i)
        {
            d2max = std::max(d2max, (double)std::fabs(held[i + 1] - 2 * held[i] + held[i - 1]));
            amp = std::max(amp, (double)std::fabs(held[i]));
        }
        double w = 2 * M_PI * 493.88 / sr, ratio = d2max / (amp * w * w);   // 1 for a clean sine
        CHECK(std::fabs(hz - 493.88f) < 1.0f, "a ping-pong loop with its fine tune plays B4 at %.2f Hz", hz);
        CHECK(ratio < 1.5, "its turns are smooth (largest bend %.2f x a sine's)", ratio);
        e.allSoundOff();
    }

    // tone delay: CLOCK-SYNC (24 ticks = a quarter note at the patch's tempo), HOLD, KEY-OFF-NORMAL
    {
        srjv::Card host;
        auto delayed = [&](const char* name, int mode, int time) {
            srjv::Patch p = makePatch(name, 0);
            setTone(p, 0, 0);                           // SINE TEST, on the board in slot A
            p.tone[0][jv::T_DELAY_MODE] = (uint8_t)mode;
            p.tone[0][jv::T_DELAY_TIME] = (uint8_t)time;
            // a hit, as these tones are: up at once, down to 0 over about 0.2 s while the key is held
            p.tone[0][jv::T_AENV_T1 + 1] = 60; p.tone[0][jv::T_AENV_T1 + 2] = 60;
            p.tone[0][jv::T_AENV_L1 + 1] = 100; p.tone[0][jv::T_AENV_L1 + 2] = 0;
            p.common[jv::C_TEMPO_HI] = 7; p.common[jv::C_TEMPO_LO] = 8;   // 120 BPM
            host.patches.push_back(p);
        };
        delayed("Clock 1 beat", 3, 24);
        delayed("Hold 1 s", 1, 99);                     // envTime(99) is about 1.5 s
        delayed("Key Off", 5, 0);
        const srjv::Card* hc[4] = {card.get(), &host, nullptr, nullptr};
        e.allSoundOff();
        e.setCards(hc);
        auto levels = [&](int patch, float upAt, float* before, float* after, float split) {
            e.allSoundOff();
            e.setPatch(1, patch);
            std::vector<float> x = play(e, 69, 100, 1.2f, upAt, sr);
            *before = rmsDb(x, 0, (size_t)(split * sr));
            *after = rmsDb(x, (size_t)((split + 0.05f) * sr), (size_t)((split + 0.2f) * sr));
        };
        float b0, a0;
        levels(0, 0.1f, &b0, &a0, 0.48f);               // a short tap: the delayed tone still comes, after one beat
        CHECK(b0 < -100 && a0 > -30, "CLOCK-SYNC 24 ticks at 120 BPM: silent for 0.5 s (%.0f dB), then sounds (%.0f dB), even after a short tap", b0, a0);
        levels(1, 0.3f, &b0, &a0, 1.0f);
        CHECK(b0 < -100 && a0 < -100, "HOLD: released before its delay, the tone never sounds (%.0f / %.0f dB)", b0, a0);
        levels(2, 0.4f, &b0, &a0, 0.38f);
        CHECK(b0 < -100 && a0 > -30, "KEY-OFF-NORMAL: silent while held (%.0f dB), sounds once the key is up (%.0f dB)", b0, a0);
        e.allSoundOff();
        e.setCards(cards);
    }

    // LFO rate: a square LFO on the TVA; its rate at the note's start, then changed while the note plays
    {
        srjv::Card host;
        srjv::Patch p = makePatch("LFO Rate", 0);
        setTone(p, 0, 0);
        uint8_t* l1 = p.tone[0] + jv::T_LFO1;
        l1[jv::LFO_WAVE] = 3; l1[jv::LFO_OFFSET] = 2; l1[jv::LFO_RATE] = 64; l1[jv::LFO_KEY_SYNC] = 1;
        p.tone[0][jv::T_AMP_LFO1] = 126;
        host.patches.push_back(p);
        const srjv::Card* hc[4] = {card.get(), &host, nullptr, nullptr};
        e.allSoundOff();
        e.setCards(hc);
        e.setPatch(1, 0);
        // the LFO's frequency from the level, 2 ms at a time: rising crossings of the mean
        auto lfoHz = [&](float secs) {
            int n = (int)(secs * sr), win = 88;
            std::vector<float> L((size_t)n), R((size_t)n), lv;
            for (int i = 0; i < n; i += 64) e.render(&L[(size_t)i], &R[(size_t)i], std::min(64, n - i));
            for (int i = 0; i + win <= n; i += win) { double q = 0; for (int k = 0; k < win; ++k) q += L[(size_t)(i + k)] * L[(size_t)(i + k)]; lv.push_back((float)q); }
            double mean = 0; for (float x : lv) mean += x; mean /= (double)lv.size();
            int first = -1, last = -1, count = 0;
            for (size_t i = 1; i < lv.size(); ++i) if (lv[i - 1] < mean && lv[i] >= mean) { if (first < 0) first = (int)i; last = (int)i; ++count; }
            return count > 1 ? (count - 1) / ((last - first) * win / sr) : 0.0f;
        };
        e.noteOn(69, 100);
        float slow = lfoHz(3.0f);
        host.patches[0].tone[0][jv::T_LFO1 + jv::LFO_RATE] = 90;   // an edit while the note is held
        lfoHz(0.3f);
        float fast = lfoHz(2.0f);
        e.noteOff(69);
        e.allSoundOff();
        e.setCards(cards);
        CHECK(std::fabs(slow - 1.64f) < 0.15f, "LFO rate 64 modulates at %.2f Hz (expected 1.64)", slow);
        CHECK(std::fabs(fast - 6.80f) < 0.5f, "the rate changed to 90 while the note plays: %.2f Hz at once (expected 6.8)", fast);
    }

    // ---- the JV-80's internal waves (our own JV-880 style set)
    {
        std::string jb = base + "-jv880";
        CHECK(buildJv880(jb), "wrote a JV-880 style ROM set (%s-*.bin)", jb.c_str());
        std::vector<std::string> chips = {jb + "-wave1.bin", jb + "-wave2.bin"};
        CHECK(srjv::internalPart(jb + "-prom.bin") == srjv::InternalPart::Jv880Program &&
              srjv::internalPart(chips[0]) == srjv::InternalPart::Jv880Wave, "internalPart tells the JV-880's program ROM and wave ROMs");
        auto in80 = srjv::loadJv880(jb + "-prom.bin", chips);
        CHECK(in80->ok() && in80->waves.size() == 16 && in80->waves[0].name == "JV80 SINE",
              "the JV-880 waves load (%s), %zu waves", in80->ok() ? "ok" : in80->error.c_str(), in80->waves.size());
        if (in80->ok())
        {
            srjv::Card host;
            srjv::Patch p = makePatch("JV80 Int", 0); setTone(p, 0, 0, 0, 0); host.patches.push_back(p);   // group INT, id 0: a JV-80 patch's
            const srjv::Card* hc[4] = {&host, nullptr, nullptr, nullptr};
            e.allSoundOff();
            e.setCards(hc);
            e.setPatch(0, 0);
            e.setInternal80(in80.get());
            std::vector<float> x = play(e, 69, 100, 0.6f, 0.5f, sr);
            hz = pitchOf(std::vector<float>(x.begin() + 4410, x.begin() + 20000), sr);
            CHECK(std::fabs(hz - 440.0f) < 1.0f, "a JV-80 INT tone plays JV-80 wave 1 (in the second chip) at %.2f Hz", hz);
            e.allSoundOff();
            e.setInternal80(nullptr);
            e.setCards(cards);
        }
    }

    // ---- the EFX types' own parameters (efxdefs.h): an impulse into the EFX, the echoes where they should be
    {
        static xp::Fx fx;
        fx.prepare(sr);
        auto run = [&](const uint8_t* common, bool leftOnly, std::vector<float>& L, std::vector<float>& R, float secs) {
            fx.setPatch(common);
            int n = (int)(secs * sr);
            L.assign((size_t)n, 0); R.assign((size_t)n, 0);
            std::vector<float> z(64, 0), el(64), er(64);
            for (int i = 0; i < n; i += 64)
            {
                int m = std::min(64, n - i);
                std::fill(el.begin(), el.end(), 0.0f); std::fill(er.begin(), er.end(), 0.0f);
                if (i == 0) { el[0] = 1; if (!leftOnly) er[0] = 1; }
                fx.process(z.data(), z.data(), el.data(), er.data(), z.data(), z.data(), &L[(size_t)i], &R[(size_t)i], m, true, false, false);
            }
        };
        auto peakAt = [&](const std::vector<float>& x, size_t from) {
            size_t best = from; for (size_t i = from; i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
            return (float)best / sr * 1000.0f;
        };
        uint8_t c[srjv::kCommonBytes] = {};
        c[jv::C_EFX_MIX_SEND] = 127; c[jv::C_TEMPO_HI] = 7; c[jv::C_TEMPO_LO] = 8;   // 120 BPM
        // STEREO-DELAY: Delay L 250 ms (111), R 300 ms (116), no feedback, HF damp bypass, all effect
        const uint8_t st[12] = {0, 111, 116, 0, 0, 49, 17, 15, 15, 100, 127, 0};
        c[jv::C_EFX_TYPE] = 16; std::memcpy(c + jv::C_EFX_PARAM, st, 12);
        std::vector<float> L, R;
        run(c, false, L, R, 0.5f);
        float l = peakAt(L, 10), r = peakAt(R, 10);
        CHECK(std::fabs(l - 250) < 1 && std::fabs(r - 300) < 1, "STEREO-DELAY: Delay L 250 ms / R 300 ms echo at %.1f / %.1f ms", l, r);
        // CROSS feedback: what goes in on the left comes back on the right
        const uint8_t cr[12] = {1, 111, 116, 0, 0, 98, 17, 15, 15, 100, 127, 0};
        std::memcpy(c + jv::C_EFX_PARAM, cr, 12);
        fx.setPatch(c);   // same type: new values
        run(c, true, L, R, 0.7f);
        r = peakAt(R, 10);
        CHECK(std::fabs(r - 550) < 2, "STEREO-DELAY CROSS: a left impulse comes back on the right at %.1f ms (250 + 300)", r);
        // QUADRUPLE-TAP: Delay 1 = a quarter note (122) at 120 BPM: 500 ms
        const uint8_t q4[12] = {122, 0, 0, 0, 127, 0, 0, 0, 49, 17, 100, 127};
        c[jv::C_EFX_TYPE] = 19; std::memcpy(c + jv::C_EFX_PARAM, q4, 12);
        run(c, false, L, R, 0.7f);
        l = peakAt(L, 10);
        CHECK(std::fabs(l - 500) < 1, "QUADRUPLE-TAP: Delay 1 = 1/4 at the patch's 120 BPM echoes at %.1f ms", l);
        // REVERB: a tail; with the balance all dry, the impulse only
        const uint8_t rv[12] = {4, 20, 90, 17, 15, 15, 100, 127, 0, 0, 0, 0};
        c[jv::C_EFX_TYPE] = 23; std::memcpy(c + jv::C_EFX_PARAM, rv, 12);
        run(c, false, L, R, 1.0f);
        float tail = rmsDb(L, (size_t)(0.2f * sr), (size_t)(0.6f * sr));
        c[jv::C_EFX_PARAM + 6] = 0;   // D100:0E
        run(c, false, L, R, 1.0f);
        float dry = rmsDb(L, (size_t)(0.2f * sr), (size_t)(0.6f * sr));
        CHECK(tail > -70 && dry < -120, "REVERB HALL1: a tail (%.0f dB); balance D100:0E: none (%.0f dB)", tail, dry);
    }

    // ---- stage 3: drive, flanger, phaser, the two-stage types
    {
        static xp::Fx fx;
        fx.prepare(sr);
        uint8_t c[srjv::kCommonBytes] = {};
        c[jv::C_EFX_MIX_SEND] = 127; c[jv::C_TEMPO_HI] = 7; c[jv::C_TEMPO_LO] = 8;
        auto through = [&](int type, std::initializer_list<int> params, const std::vector<float>& in, std::vector<float>& L, std::vector<float>& R) {
            c[jv::C_EFX_TYPE] = (uint8_t)type;
            std::memset(c + jv::C_EFX_PARAM, 0, 12);
            int k = 0; for (int v : params) c[jv::C_EFX_PARAM + k++] = (uint8_t)v;
            fx.setPatch(c);
            int n = (int)in.size();
            L.assign((size_t)n, 0); R.assign((size_t)n, 0);
            std::vector<float> z(64, 0), el(64), er(64);
            for (int i = 0; i < n; i += 64)
            {
                int m = std::min(64, n - i);
                for (int j = 0; j < m; ++j) el[j] = er[j] = in[(size_t)(i + j)];
                fx.process(z.data(), z.data(), el.data(), er.data(), z.data(), z.data(), &L[(size_t)i], &R[(size_t)i], m, true, false, false);
            }
        };
        std::vector<float> sine((size_t)sr), imp((size_t)(0.6f * sr), 0.0f), L, R, L2, R2;
        for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.3f * std::sin(2 * M_PI * 220.0 * i / sr);
        imp[0] = 1;
        // OVERDRIVE: more Drive, more harmonics (the sine's purity: its energy against its second difference)
        auto roughness = [](const std::vector<float>& x) { double a = 0, d = 0; for (size_t i = 1; i + 1 < x.size(); ++i) { a += x[i] * x[i]; double q = x[i + 1] - 2 * x[i] + x[i - 1]; d += q * q; } return d / (a + 1e-20); };
        through(1, {10, 64, 3, 15, 15, 127}, sine, L, R);
        double soft = roughness(L);
        through(1, {127, 64, 3, 15, 15, 127}, sine, L2, R2);
        double hard = roughness(L2);
        CHECK(hard > soft * 4, "OVERDRIVE: Drive 127 against 10: %.1fx the harmonics", hard / soft);
        // CHORUS>DELAY: chorus balance 0 (dry), delay 250 ms (111), delay balance all effect: an echo at 250 ms
        through(34, {0, 0, 0, 0, 0, 111, 49, 17, 100, 127}, imp, L, R);
        size_t best = 10; for (size_t i = 10; i < L.size(); ++i) if (std::fabs(L[i]) > std::fabs(L[best])) best = i;
        CHECK(std::fabs(best / sr * 1000 - 250) < 1, "CHORUS>DELAY: the delay stage echoes at %.1f ms", best / sr * 1000);
        // STEREO-FLANGER: feedback changes the sound
        std::vector<float> noise((size_t)sr); uint32_t q = 7;
        for (float& v : noise) { q = q * 1664525u + 1013904223u; v = (float)((int32_t)q / 2147483648.0 * 0.3); }
        through(14, {0, 16, 10, 40, 80, 45, 49, 15, 15, 50, 127}, noise, L, R);
        through(14, {0, 16, 10, 40, 80, 45, 95, 15, 15, 50, 127}, noise, L2, R2);
        double d = 0, e2 = 0; for (size_t i = 0; i < L.size(); ++i) { d += (L[i] - L2[i]) * (L[i] - L2[i]); e2 += L[i] * L[i]; }
        CHECK(10 * std::log10(d / e2) > -15, "STEREO-FLANGER: feedback 0 %% against +92 %% differs by %.0f dB", 10 * std::log10(d / e2));
        // PHASER: the Manual moves the notches
        through(3, {30, 20, 0, 80, 127, 64, 127}, noise, L, R);
        through(3, {90, 20, 0, 80, 127, 64, 127}, noise, L2, R2);
        d = 0; e2 = 0; for (size_t i = 0; i < L.size(); ++i) { d += (L[i] - L2[i]) * (L[i] - L2[i]); e2 += L[i] * L[i]; }
        CHECK(10 * std::log10(d / e2) > -15, "PHASER: Manual 30 against 90 differs by %.0f dB", 10 * std::log10(d / e2));
    }

    // ---- stage 4: EQ, pitch shifter, compressor, rotary, auto-wah, spectrum
    {
        static xp::Fx fx;
        fx.prepare(sr);
        uint8_t c[srjv::kCommonBytes] = {};
        c[jv::C_EFX_MIX_SEND] = 127; c[jv::C_TEMPO_HI] = 7; c[jv::C_TEMPO_LO] = 8;
        auto through = [&](int type, std::initializer_list<int> params, const std::vector<float>& in, std::vector<float>& L) {
            c[jv::C_EFX_TYPE] = (uint8_t)type;
            std::memset(c + jv::C_EFX_PARAM, 0, 12);
            int k = 0; for (int v : params) c[jv::C_EFX_PARAM + k++] = (uint8_t)v;
            fx.setPatch(c);
            int n = (int)in.size();
            L.assign((size_t)n, 0);
            std::vector<float> R((size_t)n), z(64, 0), el(64), er(64);
            for (int i = 0; i < n; i += 64)
            {
                int m = std::min(64, n - i);
                for (int j = 0; j < m; ++j) el[j] = er[j] = in[(size_t)(i + j)];
                fx.process(z.data(), z.data(), el.data(), er.data(), z.data(), z.data(), &L[(size_t)i], &R[(size_t)i], m, true, false, false);
            }
        };
        auto tone = [&](float hz, float amp, float secs) { std::vector<float> x((size_t)(secs * sr)); for (size_t i = 0; i < x.size(); ++i) x[i] = amp * (float)std::sin(2 * M_PI * hz * i / sr); return x; };
        std::vector<float> L, L2;
        // STEREO-EQ: P1 at 1 kHz (index 7), Q 1.0, +15 dB; everything else flat
        std::vector<float> k1 = tone(1000, 0.1f, 0.5f);
        through(0, {0, 15, 0, 15, 7, 1, 30, 7, 1, 15, 127}, k1, L);
        float gainDb = rmsDb(L, 10000, 20000) - rmsDb(k1, 10000, 20000);
        CHECK(std::fabs(gainDb - 15) < 1, "STEREO-EQ: a +15 dB peak at 1 kHz raises a 1 kHz sine by %.1f dB", gainDb);
        // 2VOICE-PITCH: voice A an octave up, all A, all effect
        std::vector<float> a220 = tone(220, 0.3f, 1.0f);
        through(21, {2, 36, 24, 50, 50, 0, 0, 64, 64, 0, 100, 127}, a220, L);
        // where the energy went: around 440 Hz (400-480) against the input's 220 (200-240). A
        // crossfading two-tap shifter puts sidebands at its window rate around the new pitch (with a
        // lone sine the two taps partly cancel), so measure the band, not one peak
        auto band = [&](int lo, int hi) {
            double e = 0;
            for (int f0 = lo; f0 <= hi; f0 += 2)
            {
                double re = 0, im = 0;
                for (size_t i = 8000; i < 40000; ++i) { double ph = 2 * M_PI * f0 * (double)i / sr; re += L[i] * std::cos(ph); im += L[i] * std::sin(ph); }
                e += re * re + im * im;
            }
            return e; };
        double up = band(400, 480), orig = band(200, 240);
        CHECK(up > orig * 100, "2VOICE-PITCH: Coarse A +12 moves a 220 Hz sine's energy to 400-480 Hz (%.0f dB above what is left at 220)", 10 * std::log10(up / (orig + 1e-20)));
        // COMPRESSOR: full sustain narrows the distance between a loud and a quiet input
        std::vector<float> loud = tone(220, 0.5f, 0.5f), soft = tone(220, 0.02f, 0.5f);
        through(8, {127, 20, 64, 0, 15, 15, 127}, loud, L);
        through(8, {127, 20, 64, 0, 15, 15, 127}, soft, L2);
        float inGap = rmsDb(loud, 10000, 20000) - rmsDb(soft, 10000, 20000), outGap = rmsDb(L, 10000, 20000) - rmsDb(L2, 10000, 20000);
        CHECK(outGap < inGap - 10, "COMPRESSOR: a %.0f dB gap between two inputs comes out %.0f dB", inGap, outGap);
        // ROTARY: FAST moves the level faster than SLOW (count the level's swings over 2 s)
        std::vector<float> h1 = tone(1500, 0.3f, 2.0f);
        auto swings = [&](const std::vector<float>& x) {
            std::vector<float> env; for (size_t i = 0; i + 441 <= x.size(); i += 441) { double q = 0; for (size_t j = i; j < i + 441; ++j) q += x[j] * x[j]; env.push_back((float)q); }
            double m = 0; for (float v : env) m += v; m /= env.size();
            int n = 0; for (size_t i = 1; i < env.size(); ++i) n += (env[i - 1] < m) != (env[i] < m);
            return n; };
        through(7, {20, 10, 120, 110, 0, 15, 15, 127, 127, 127, 127}, h1, L);
        through(7, {20, 10, 120, 110, 1, 15, 15, 127, 127, 127, 127}, h1, L2);
        int slow = swings(L), fast = swings(L2);
        CHECK(fast > slow * 2, "ROTARY: the horn swings %d times in 2 s on SLOW, %d on FAST", slow, fast);
        // AUTO-WAH and SPECTRUM: their settings change the sound
        std::vector<float> noise((size_t)sr); uint32_t q = 3;
        for (float& v : noise) { q = q * 1664525u + 1013904223u; v = (float)((int32_t)q / 2147483648.0 * 0.3); }
        auto differs = [&](const std::vector<float>& a, const std::vector<float>& b2) { double d = 0, e = 0; for (size_t i = 0; i < a.size(); ++i) { d += (a[i] - b2[i]) * (a[i] - b2[i]); e += a[i] * a[i]; } return 10 * std::log10(d / (e + 1e-20)); };
        through(6, {0, 20, 64, 0, 20, 100, 127}, noise, L);
        through(6, {0, 20, 64, 0, 100, 100, 127}, noise, L2);
        CHECK(differs(L, L2) > -15, "AUTO-WAH: Manual 20 against 100 differs by %.0f dB", differs(L, L2));
        through(4, {30, 15, 15, 15, 15, 15, 15, 0, 2, 64, 127}, noise, L);
        through(4, {0, 15, 15, 15, 15, 15, 15, 30, 2, 64, 127}, noise, L2);
        CHECK(differs(L, L2) > -15, "SPECTRUM: band 1 up against band 8 up differs by %.0f dB", differs(L, L2));
        bool finite = true; for (float v : L) finite = finite && std::isfinite(v);
        CHECK(finite, "stage 4's output stays finite");
    }

    // ---- the filter models: slopes, the LADDER's resonance, every model in the engine
    {
        auto gainAt = [&](int model, float fc, float f0) {   // a sine at f0 through the low-pass at fc, gain in dB
            xp::Svf a, b2; xp::Ladder lad;
            a.set(fc, 0.0f, sr); b2.set(fc, 0.3f, sr); lad.set(fc, 0.0f, sr);
            double in = 0, out = 0;
            for (int i = 0; i < 44100; ++i)
            {
                float x = (float)std::sin(2 * M_PI * f0 * i / sr), y, bp, hp;
                if (model == 2) y = lad.process(x);
                else { y = a.lp(x, &bp, &hp); if (model == 1) y = b2.lp(y, &bp, &hp); }
                if (i > 22050) { in += x * x; out += y * y; }
            }
            return 10 * std::log10(out / in);
        };
        double c12 = gainAt(0, 1000, 4000), c24 = gainAt(1, 1000, 4000), cl = gainAt(2, 1000, 4000);
        CHECK(c12 < -20 && c12 > -28 && c24 < -42 && cl < -42,
              "filter slopes two octaves above cutoff: XP-80 %.0f dB, 24dB %.0f dB, LADDER %.0f dB", c12, c24, cl);
        auto ring = [&](float reso) {   // a click, then how much is left 0.5 s later
            xp::Ladder lad; lad.set(800, reso, sr);
            double late = 0;
            for (int i = 0; i < 44100; ++i) { float y = lad.process(i == 0 ? 0.5f : 0.0f); if (i > 22050) late += y * y; }
            return 10 * std::log10(late / 22050 + 1e-30);
        };
        double sings = ring(1.0f), calm = ring(0.5f);
        CHECK(sings > -40 && calm < -150, "LADDER: resonance 127 sings on after a click (%.0f dB), 64 dies away (%.0f dB)", sings, calm);
        bool ok = true;
        for (int model = 0; model < 3; ++model)
        {
            e.allSoundOff();
            e.settings.filterModel = model;
            e.settings.filterDrive = 1.0f;
            e.setPatch(0, 4);             // "Filter Sweep": LPF, resonance 90
            std::vector<float> x = play(e, 48, 100, 1.0f, 0.8f, sr);
            bool finite = true; for (float v : x) finite = finite && std::isfinite(v) && std::fabs(v) < 4;
            float lv = rmsDb(x, 2000, 30000);
            ok = ok && finite && lv > -40;
            std::printf("INFO  filter model %d with full drive: %.1f dB\n", model, lv);
        }
        e.settings.filterModel = 0; e.settings.filterDrive = 0;
        CHECK(ok, "the three filter models with full drive sound and stay finite");
        e.allSoundOff();
    }

    // CPU: 32 voices on the saw patch for a second
    e.allSoundOff();
    e.setPatch(0, 1);
    e.settings.polyphony = 32;
    for (int n = 0; n < 32; ++n) e.noteOn(36 + n, 100);
    std::vector<float> L(44100), R(44100);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 44100; i += 128) e.render(&L[(size_t)i], &R[(size_t)i], std::min(128, 44100 - i));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("INFO  32 voices for 1 s took %.1f ms on this computer (%.1f%% of one core); %d voices active\n", ms, ms / 10.0, e.activeVoices());

    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
