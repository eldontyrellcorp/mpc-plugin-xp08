// srjv.cpp - see srjv.h.
#include "srjv.h"
#include "jvparams.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>

namespace srjv {

namespace {

// Descrambled address bit k comes from dump address bit kAddrFrom[k] (bits 19 and up stay).
constexpr int kAddrFrom[19] = {2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14};
// The two readings of the data lines found in the community's notes (the one as the other's
// inverse): descrambled bit k comes from dump bit kDataFrom[variant][k]. load() tries both.
constexpr int kDataFrom[2][8] = {{2, 0, 4, 5, 7, 6, 3, 1}, {1, 7, 0, 6, 2, 3, 5, 4}};

uint32_t plainAddr(uint32_t dumpAddr)
{
    uint32_t a = dumpAddr & ~0x7FFFFu;
    for (int k = 0; k < 19; ++k) a |= ((dumpAddr >> kAddrFrom[k]) & 1u) << k;
    return a;
}

uint32_t dumpAddr(uint32_t plain)
{
    uint32_t a = plain & ~0x7FFFFu;
    for (int k = 0; k < 19; ++k) a |= ((plain >> k) & 1u) << kAddrFrom[k];
    return a;
}

struct DataTables { uint8_t plain[2][256], dump[2][256]; };
const DataTables& dataTables()
{
    static const DataTables t = [] {
        DataTables d{};
        for (int v = 0; v < 2; ++v)
            for (int x = 0; x < 256; ++x)
            {
                uint8_t p = 0;
                for (int k = 0; k < 8; ++k) p |= (uint8_t)(((x >> kDataFrom[v][k]) & 1) << k);
                d.plain[v][x] = p;
                d.dump[v][p] = (uint8_t)x;
            }
        return d;
    }();
    return t;
}

uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

std::string ascii(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; ++i) s += printable(p[i]) ? (char)p[i] : ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

std::string baseName(const std::string& path)
{
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// How the bytes 0x20..0x39 (the card's name and date) read: higher is more like text. A date
// "YYYY-MM-DD" or "YYYY/MM/DD" at 0x30 counts most. `read(addr)` gives a descrambled byte.
template <class F> int textScore(F read)
{
    int score = 0;
    for (uint32_t a = 0x20; a < 0x3A; ++a) score += printable(read(a)) ? 1 : 0;
    int digits = 0;
    for (uint32_t a = 0x30; a < 0x3A; ++a) { uint8_t c = read(a); digits += (c >= '0' && c <= '9') ? 1 : 0; }
    return score + 2 * digits;
}

// -1: the file is already descrambled; 0, 1: the data variant; -2: not a card.
template <class F> int detectVariant(F rawAt)
{
    const DataTables& t = dataTables();
    int best = -2, bestScore = 18;   // a card's header must read as text
    int s = textScore([&](uint32_t a) { return rawAt(a); });
    if (s > bestScore) { best = -1; bestScore = s; }
    for (int v = 0; v < 2; ++v)
    {
        int sv = textScore([&](uint32_t a) { return t.plain[v][rawAt(dumpAddr(a))]; });
        if (sv > bestScore) { best = v; bestScore = sv; }
    }
    return best;
}

class BitReader
{
public:
    explicit BitReader(const uint8_t* p) : p_(p) {}
    uint32_t take(int bits)
    {
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i, ++pos_) v |= (uint32_t)((p_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
        return v;
    }
    void skip(int bits) { pos_ += (size_t)bits; }
private:
    const uint8_t* p_;
    size_t pos_ = 0;
};

class BitWriter
{
public:
    explicit BitWriter(uint8_t* p) : p_(p) {}
    void put(uint32_t v, int bits)
    {
        for (int i = 0; i < bits; ++i, ++pos_) if ((v >> i) & 1) p_[pos_ >> 3] |= (uint8_t)(1 << (pos_ & 7));
    }
    void skip(int bits) { pos_ += (size_t)bits; }
private:
    uint8_t* p_;
    size_t pos_ = 0;
};

// One 18-byte sample table entry; `base` is where its address space starts in Card::pcm. The fine
// tune is the correction to apply on playback (the sample sounds that much below its root key),
// on the boards and in the JV-1080's own tables alike.
Sample readSample(const uint8_t* e, uint32_t base, size_t size)
{
    Sample s;
    s.volume = e[0];
    s.start = be24(e + 1) + base;
    s.loop = be24(e + 4) + base;
    s.end = be24(e + 7) + base;
    s.loopType = e[0x0C];
    s.rootKey = e[0x0D] & 0x7F;
    s.fineSemis = ((int)be16(e + 0x0E) - 1024) / 1024.0f;
    s.loopFineSemis = ((int)be16(e + 0x10) - 1024) / 1024.0f;
    if (s.end >= size || s.start > s.end) { s.start = s.end = s.loop = 0; s.volume = 0; }
    if (s.loop < s.start || s.loop > s.end) s.loop = s.start;
    return s;
}

// One 60-byte multisample entry: name, 16 zone top keys, 16 sample numbers (+ `first`).
Multisample readWave(const uint8_t* e, uint32_t first, uint32_t nSamples)
{
    Multisample m;
    m.name = ascii(e, 12);
    for (int z = 0; z < 16; ++z)
    {
        m.hiKey[z] = e[12 + z];
        m.sample[z] = (uint16_t)(first + std::min<uint32_t>(be16(e + 28 + 2 * z), nSamples - 1));
    }
    m.count = 16;
    for (int z = 0; z < 16; ++z) if (m.hiKey[z] >= 127) { m.count = z + 1; break; }
    return m;
}

// The DPCM waves, decoded once: one 16-bit value per address of `rom`.
void decodePcm(Card& card, const std::vector<uint8_t>& rom)
{
    const size_t size = rom.size();
    card.pcm.assign(size, 0);
    for (const Sample& s : card.samples)
    {
        if (s.volume == 0 && s.end == 0) continue;
        uint32_t acc = 0;
        for (uint32_t a = s.start; a <= s.end && a < size; ++a)
        {
            uint8_t expByte = rom[((a & 0xFFFFFu) >> 5) | (a & 0xF00000u)];
            int shift = (a & 0x10) ? (expByte >> 4) : (expByte & 0x0F);
            int32_t d = (int8_t)rom[a];
            acc += ((uint32_t)d << shift) << 14;     // wraps as the hardware's adder does
            card.pcm[a] = (int16_t)((int32_t)acc >> 16);
        }
    }
}

// ---- The JV-1080's internal waves ------------------------------------------------------------
// Address lines of the wave ROM chips: on-chip address bit k comes from dump address bit
// kIntAddrFrom[k] (21 bits, 2 MB). Found from the dumps: neighbouring samples correlate, so
// each bit's partner is the one that makes them neighbours; the chips' block headers ("INT100A",
// the date, 32 kHz at 0x40) then read where a board's do, and the samples play at their root keys.
constexpr int kIntAddrFrom[21] = {0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16, 9, 6, 8, 14, 17, 11, 15, 18, 19, 20};
constexpr int kIntDataVariant = 0;
constexpr size_t kIntChip = 2u << 20;
constexpr int kIntWaves[2] = {255, 193};   // INT-A, INT-B: the JV-1080's 448 waveforms

struct ProgramTables
{
    uint32_t waveAt[2] = {}, nWaves[2] = {}, sampleAt[2] = {}, nSamples[2] = {};   // [0] INT-A chips, [1] INT-B chip
    uint32_t listAt[2] = {};    // the wave lists: N 16-bit multisample numbers, then N bytes (1: in the INT-B chip's table)
};

std::vector<uint8_t> readFile(const std::string& path, size_t maxSize)
{
    std::vector<uint8_t> v;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0 && (size_t)size <= maxSize)
        {
            v.resize((size_t)size);
            if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
        }
        std::fclose(f);
    }
    return v;
}

// Finds the tables in the program ROM: each multisample table follows an 8-byte header (wave
// count, sample count, the address of its sample table, which follows the waves).
bool findTables(const std::vector<uint8_t>& p, ProgramTables& t)
{
    struct Found { uint32_t waveAt, nW, sampleAt, nS, maxEnd; };
    std::vector<Found> found;
    for (uint32_t h = 0; h + 8 <= p.size() && found.size() < 2; ++h)
    {
        uint32_t nW = be16(&p[h]), nS = be16(&p[h + 2]), ptr = be32(&p[h + 4]);
        if (nW < 16 || nW > 1024 || nS < 16 || nS > 4096) continue;
        uint32_t sampleAt = h + 8 + nW * 60;
        if ((ptr & 0xFFFFF) != (sampleAt & 0xFFFFF) || sampleAt + nS * 18 > p.size()) continue;
        uint32_t named = 0;
        for (uint32_t w = 0; w < nW; ++w)
        {
            const uint8_t* e = &p[h + 8 + w * 60];
            bool ok = true;
            for (int i = 0; i < 12; ++i) ok = ok && printable(e[i]);
            named += ok ? 1 : 0;
        }
        if (named * 10 < nW * 9) continue;
        uint32_t maxEnd = 0;
        for (uint32_t s = 0; s < nS; ++s) maxEnd = std::max(maxEnd, be24(&p[sampleAt + s * 18 + 7]));
        found.push_back({h + 8, nW, sampleAt, nS, maxEnd});
        h = sampleAt + nS * 18 - 1;
    }
    if (found.size() != 2) return false;
    // INT-A spans three chips, INT-B one
    if (found[0].maxEnd < found[1].maxEnd) std::swap(found[0], found[1]);
    if (found[0].maxEnd >= 3 * kIntChip || found[1].maxEnd >= kIntChip) return false;
    for (int b = 0; b < 2; ++b)
    {
        t.waveAt[b] = found[(size_t)b].waveAt; t.nWaves[b] = found[(size_t)b].nW;
        t.sampleAt[b] = found[(size_t)b].sampleAt; t.nSamples[b] = found[(size_t)b].nS;
    }
    // the wave lists: INT-A's (255 entries), then INT-B's (193) at the next even address
    auto listOk = [&](uint32_t at, int n) {
        if (at + (uint32_t)n * 3 > p.size()) return false;
        std::vector<bool> seen(t.nWaves[0] + t.nWaves[1], false);
        int distinct = 0;
        for (int i = 0; i < n; ++i)
        {
            uint32_t v = be16(&p[at + 2 * i]), f = p[at + 2 * n + i];
            if (f > 1 || v >= t.nWaves[f]) return false;
            uint32_t k = v + (f ? t.nWaves[0] : 0);
            if (!seen[k]) { seen[k] = true; ++distinct; }
        }
        return distinct * 10 >= n * 9;
    };
    for (uint32_t at = 0; at + 6 < p.size(); at += 2)
    {
        if (!listOk(at, kIntWaves[0])) continue;
        uint32_t b = (at + 3 * kIntWaves[0] + 1) & ~1u;
        if (!listOk(b, kIntWaves[1])) continue;
        t.listAt[0] = at; t.listAt[1] = b;
        return true;
    }
    return false;
}

bool plausibleName(const Patch& p)
{
    int ok = 0;
    for (int i = 0; i < 12; ++i) ok += printable(p.common[i]) ? 1 : 0;
    return ok == 12;
}

// Plausible values where the JV-1080 format is strict: a real table passes for most patches.
bool plausiblePatch(const Patch& p)
{
    if (!plausibleName(p) || p.common[jv::C_EFX_TYPE] > 39 || p.common[jv::C_REVERB_TYPE] > 7 ||
        p.common[jv::C_BEND_UP] > 12 || p.common[jv::C_BEND_DOWN] > 48 || p.common[jv::C_OCTAVE] > 6 ||
        p.common[jv::C_STRUCT12] > 9 || p.common[jv::C_STRUCT34] > 9)
        return false;
    for (int t = 0; t < 4; ++t)
        if (p.tone[t][jv::T_WAVE_GROUP] > 2 || p.tone[t][jv::T_FILTER_TYPE] > 4) return false;
    return true;
}

// ---- JV-80 patch records (the boards made for the JV-80/880 have only these) -----------------
// A 362-byte record: the JV-80 SysEx patch (common 34 bytes, tones 115 bytes, JV-880 manual's
// map), each parameter in its own bit width, some reordered, signed values stored as value-64.
// {SysEx offset, bits, signed}; offset 0xFF = zero padding.
struct F80 { uint8_t at, bits; bool sgn; };
constexpr uint8_t PAD = 0xFF;
constexpr F80 kJv80Common[] = {
    {0x00, 8, 0}, {0x01, 8, 0}, {0x02, 8, 0}, {0x03, 8, 0}, {0x04, 8, 0}, {0x05, 8, 0}, {0x06, 8, 0}, {0x07, 8, 0},
    {0x08, 8, 0}, {0x09, 8, 0}, {0x0A, 8, 0}, {0x0B, 8, 0},
    {0x0D, 3, 0}, {0x11, 2, 0}, {PAD, 2, 0}, {0x0C, 1, 0},
    {0x0E, 8, 0}, {0x0F, 8, 0}, {0x10, 8, 0}, {0x12, 8, 0}, {0x13, 8, 0}, {0x14, 7, 0}, {0x16, 1, 0}, {0x15, 8, 0},
    {0x17, 8, 0}, {0x18, 8, 0}, {0x19, 8, 0}, {0x1A, 8, 1},
    {0x1B, 4, 0}, {0x1F, 1, 0}, {0x1E, 1, 0}, {0x1D, 1, 0}, {0x1C, 1, 0}, {0x21, 7, 0}, {0x20, 1, 0},
};
constexpr F80 kJv80Tone[] = {
    {0x00, 2, 0}, {0x05, 4, 0}, {0x04, 1, 0}, {0x03, 1, 0}, {0x02, 4, 0}, {0x01, 4, 0}, {PAD, 8, 0}, {0x06, 8, 0}, {0x07, 8, 0},
    {0x0A, 4, 0}, {0x0C, 4, 0}, {0x0E, 4, 0}, {0x10, 4, 0}, {0x0B, 8, 1}, {0x0D, 8, 1}, {0x0F, 8, 1}, {0x11, 8, 1},
    {0x12, 4, 0}, {0x14, 4, 0}, {0x16, 4, 0}, {0x18, 4, 0}, {0x13, 8, 1}, {0x15, 8, 1}, {0x17, 8, 1}, {0x19, 8, 1},
    {0x1A, 4, 0}, {0x1C, 4, 0}, {0x1E, 4, 0}, {0x20, 4, 0}, {0x1B, 8, 1}, {0x1D, 8, 1}, {0x1F, 8, 1}, {0x21, 8, 1},
    {0x22, 3, 0}, {0x23, 3, 0}, {0x24, 1, 0}, {PAD, 1, 0}, {0x25, 7, 0}, {PAD, 1, 0}, {0x27, 4, 0}, {0x26, 4, 0}, {0x29, 7, 0}, {0x28, 1, 0},
    {0x2D, 3, 0}, {0x2E, 3, 0}, {0x2F, 1, 0}, {PAD, 1, 0}, {0x30, 7, 0}, {PAD, 1, 0}, {0x32, 4, 0}, {0x31, 4, 0}, {0x34, 7, 0}, {0x33, 1, 0},
    {0x2A, 8, 1}, {0x2B, 8, 1}, {0x2C, 8, 1}, {0x35, 8, 1}, {0x36, 8, 1}, {0x37, 8, 1},
    {0x38, 8, 1}, {0x39, 8, 1}, {0x3A, 4, 0}, {0x60, 4, 0}, {0x3B, 4, 0}, {0x3F, 4, 0}, {0x3C, 8, 1}, {0x3D, 4, 0}, {0x3E, 4, 0},
    {0x40, 8, 1}, {0x41, 8, 0}, {0x42, 8, 1}, {0x43, 8, 0}, {0x44, 8, 1}, {0x45, 8, 0}, {0x46, 8, 1}, {0x47, 8, 0}, {0x48, 8, 1},
    {0x4A, 8, 0}, {0x4B, 7, 0}, {0x4C, 1, 0}, {0x4D, 4, 0}, {0x52, 4, 0}, {0x4E, 3, 0}, {0x49, 2, 0}, {PAD, 3, 0},
    {0x4F, 8, 1}, {0x50, 4, 0}, {0x51, 4, 0}, {0x53, 7, 1}, {PAD, 1, 0},
    {0x54, 8, 0}, {0x55, 8, 0}, {0x56, 8, 0}, {0x57, 8, 0}, {0x58, 8, 0}, {0x59, 8, 0}, {0x5A, 8, 0}, {0x5B, 8, 0},
    {0x5C, 8, 0}, {0x5F, 4, 0}, {0x5E, 4, 0}, {0x62, 4, 0}, {0x63, 4, 0}, {0x5D, 4, 0}, {0x68, 4, 0}, {0x64, 3, 0}, {0x61, 2, 0},
    {PAD, 1, 0}, {0x08, 1, 0}, {0x09, 1, 0}, {0x65, 8, 1}, {0x66, 4, 0}, {0x67, 4, 0},
    {0x69, 8, 0}, {0x6A, 8, 0}, {0x6B, 8, 0}, {0x6C, 8, 0}, {0x6D, 8, 0}, {0x6E, 8, 0}, {0x6F, 8, 0},
    {0x70, 8, 0}, {0x71, 8, 0}, {0x72, 8, 0},
};
constexpr int kJv80Record = 362;
constexpr int bits80(const F80* f, int n) { int s = 0; for (int i = 0; i < n; ++i) s += f[i].bits; return s; }
static_assert(bits80(kJv80Common, sizeof(kJv80Common) / sizeof(F80)) == 26 * 8, "JV-80 common: 26 bytes");
static_assert(bits80(kJv80Tone, sizeof(kJv80Tone) / sizeof(F80)) == 84 * 8, "JV-80 tone: 84 bytes");

template <size_t N> void read80(BitReader& r, const F80 (&fields)[N], uint8_t* out)
{
    for (const F80& f : fields)
    {
        uint32_t v = r.take(f.bits);
        if (f.at == PAD) continue;
        if (f.sgn) v = (v + 64) & ((1u << f.bits) - 1);   // stored as value - 64
        out[f.at] = (uint8_t)v;
    }
}

// The JV-80's cutoff scale is not the JV-1080's: the same number is a brighter filter on a JV-80.
// Over the boards' patches, JV-80 low-pass cutoffs have a median of 32 against 78 for the JV-1080
// tables, and 84 JV-80 tones sit below 40 with no filter envelope (5 on the JV-1080 boards), which
// on the JV-1080's scale would be all but silent. Filter-off tones read 127 on both, so the field
// itself decodes right. The JV-1080 value of a JV-80 cutoff: the deciles of the two families'
// cutoffs matched, low-pass and high-pass tones apart (high-pass ones: median 13 against 44); a
// statistical fit, not a measured curve, straight lines between the points.
double jv80Cutoff(double c, bool highPass)
{
    static const double lpFrom[] = {0, 5, 8, 14, 20, 32, 47, 76, 116, 127}, lpTo[] = {20, 30, 52, 61, 70, 78, 87, 99, 121, 127};
    static const double hpFrom[] = {0, 1, 5, 9, 13, 23, 39, 55, 94, 127}, hpTo[] = {0, 10, 23, 33, 44, 60, 74, 85, 99, 127};
    const double* from = highPass ? hpFrom : lpFrom;
    const double* to = highPass ? hpTo : lpTo;
    c = std::max(0.0, std::min(c, 127.0));
    for (int i = 1; i < 10; ++i)
        if (c <= from[i]) return to[i - 1] + (to[i] - to[i - 1]) * (c - from[i - 1]) / (from[i] - from[i - 1]);
    return 127;
}

// A JV-80 amount that moves the cutoff (filter envelope depth, a matrix depth to the cutoff),
// -63..+63 in JV-80 cutoff steps, as JV-1080 cutoff steps from this tone's cutoff, then in the
// engine's units for that amount (`octPerStep`: what one step of it moves, in octaves).
uint8_t jv80CutoffAmount(int raw80, int cutoff80, bool highPass, double octPerStep)
{
    int d = raw80 - 64;                                        // stored as value + 64
    double steps = jv80Cutoff(cutoff80 + d, highPass) - jv80Cutoff(cutoff80, highPass);
    double v = steps * (10.2 / 127.0) / octPerStep;            // a JV-1080 cutoff step is 10.2 / 127 octave
    return (uint8_t)std::max(0, std::min(126, (int)std::lround(63 + v)));
}

int nearest(const int* table, int n, int value)
{
    int best = 0;
    for (int i = 1; i < n; ++i) if (std::abs(table[i] - value) < std::abs(table[best] - value)) best = i;
    return best;
}

// A JV-80 patch as the JV-1080 layout the engine plays (what a JV-1080 does with these boards).
Patch convertJv80(const uint8_t* c80, const uint8_t (*t80)[0x73])
{
    static const int random80[16] = {0, 5, 10, 20, 30, 40, 50, 70, 100, 200, 300, 400, 500, 600, 800, 1200};
    // JV-80 values -> the boards' JV-1080 encoding (jvparams.h): sense 1..127 (64 = 0) -> 0..126 (63 = 0)
    auto sense = [](uint8_t v) -> uint8_t { return (uint8_t)std::max(0, std::min(126, (int)v - 1)); };
    auto velo = [](uint8_t v) -> uint8_t { return (uint8_t)std::max(0, std::min(125, (int)std::lround(50 + ((int)v - 64) * 50.0 / 63.0))); };
    static const uint8_t dest80[13] = {jv::D_OFF, jv::D_PITCH, jv::D_CUTOFF, jv::D_RESO, jv::D_LEVEL, jv::D_L1_PITCH, jv::D_L2_PITCH,
                                       jv::D_L1_FILTER, jv::D_L2_FILTER, jv::D_L1_AMP, jv::D_L2_AMP, jv::D_L1_RATE, jv::D_L2_RATE};
    Patch p{};
    uint8_t* c = p.common;
    std::memcpy(c, c80, 12);
    c[jv::C_EFX_TYPE] = 0;                       // the JV-80 has no EFX
    c[jv::C_EFX_MIX_SEND] = 127;
    c[jv::C_CHORUS_LEVEL] = c80[0x12]; c[jv::C_CHORUS_DEPTH] = c80[0x13]; c[jv::C_CHORUS_RATE] = c80[0x14];
    c[jv::C_CHORUS_FEEDBACK] = c80[0x15]; c[jv::C_CHORUS_OUTPUT] = c80[0x16] ? 1 : 0;
    static const uint8_t chorusPre[3] = {20, 40, 70};
    c[jv::C_CHORUS_PREDELAY] = chorusPre[std::min<int>(c80[0x11], 2)];
    c[jv::C_REVERB_TYPE] = c80[0x0D] & 7; c[jv::C_REVERB_LEVEL] = c80[0x0E]; c[jv::C_REVERB_TIME] = c80[0x0F];
    c[jv::C_REVERB_FEEDBACK] = c80[0x10]; c[jv::C_REVERB_HF_DAMP] = 17;
    c[jv::C_TEMPO_HI] = 7; c[jv::C_TEMPO_LO] = 8;
    c[jv::C_ANALOG_FEEL] = c80[0x17]; c[jv::C_LEVEL] = c80[0x18]; c[jv::C_PAN] = c80[0x19];
    c[jv::C_BEND_DOWN] = (uint8_t)std::max(0, std::min(48, 64 - (int)c80[0x1A]));
    c[jv::C_BEND_UP] = std::min<uint8_t>(c80[0x1B], 12);
    c[jv::C_KEY_ASSIGN] = c80[0x1C] & 1; c[jv::C_SOLO_LEGATO] = c80[0x1D] & 1; c[jv::C_PORTA_SW] = c80[0x1E] & 1;
    c[jv::C_PORTA_MODE] = c80[0x1F] ? 0 : 1;     // JV-80: 0 LEGATO 1 NORMAL; here 0 NORMAL 1 LEGATO
    c[jv::C_PORTA_TYPE] = c80[0x20] & 1; c[jv::C_PORTA_TIME] = c80[0x21];
    c[jv::C_CTRL_SRC2] = 10;                     // the JV-80's aftertouch matrix
    c[jv::C_CTRL_SRC3] = 8;                      // and its expression matrix
    c[jv::C_VELO_RANGE_SW] = c80[0x0C] & 1;
    c[jv::C_OCTAVE] = 3;

    for (int t = 0; t < 4; ++t)
    {
        const uint8_t* s = t80[t];
        uint8_t* d = p.tone[t];
        d[jv::T_SWITCH] = s[0x03] & 1;
        // JV-80 wave groups: 0 INT, 1 EXP (this board), 2 PCM card
        d[jv::T_WAVE_GROUP] = s[0x00] == 1 ? 2 : (s[0x00] == 0 ? 0 : 1);
        d[jv::T_WAVE_GROUP_ID] = 0;              // "the board itself" (Card::groupId stays -1)
        d[jv::T_WAVE_HI] = s[0x01] & 15; d[jv::T_WAVE_LO] = s[0x02] & 15;
        d[jv::T_WAVE_GAIN] = 1;
        d[jv::T_FXM_SW] = s[0x04] & 1; d[jv::T_FXM_DEPTH] = s[0x05] & 15;
        d[jv::T_VELO_LO] = std::max<uint8_t>(1, s[0x06]); d[jv::T_VELO_HI] = std::max<uint8_t>(1, s[0x07]);
        d[jv::T_KEY_LO] = 0; d[jv::T_KEY_HI] = 127;
        d[jv::T_VOLUME_SW] = s[0x08] & 1; d[jv::T_HOLD_SW] = s[0x09] & 1; d[jv::T_BEND_SW] = 1; d[jv::T_PAN_SW] = 1;
        for (int k = 0; k < 3; ++k)              // modulation, aftertouch, expression
            for (int e = 0; e < 4; ++e)
            {
                int dest = s[0x0A + k * 8 + e * 2];
                d[jv::T_MATRIX + k * 8 + e * 2] = dest < 13 ? dest80[dest] : 0;
                d[jv::T_MATRIX + k * 8 + e * 2 + 1] = dest == 2      // to the cutoff: in JV-80 cutoff steps
                    ? jv80CutoffAmount(s[0x0B + k * 8 + e * 2], s[0x4A], s[0x49] == 2, 6.0 / 63.0)
                    : sense(s[0x0B + k * 8 + e * 2]);
            }
        for (int l = 0; l < 2; ++l)
        {
            const uint8_t* ls = s + (l ? 0x2D : 0x22);
            uint8_t* ld = d + (l ? jv::T_LFO2 : jv::T_LFO1);
            static const uint8_t wave80[6] = {0, 1, 2, 3, 5, 6};   // RND1 -> S&H, RND2 -> RND
            ld[jv::LFO_WAVE] = wave80[std::min<int>(ls[0], 5)];
            ld[jv::LFO_OFFSET] = std::min<uint8_t>(ls[1], 4);
            ld[jv::LFO_KEY_SYNC] = ls[2] & 1;
            ld[jv::LFO_RATE] = ls[3];
            int delay = ls[4] << 4 | ls[5];
            bool keyOff = delay >= 128;
            ld[jv::LFO_DELAY] = keyOff ? 0 : (uint8_t)delay;
            ld[jv::LFO_FADE_MODE] = (uint8_t)((keyOff ? 2 : 0) + (ls[6] & 1));
            ld[jv::LFO_FADE_TIME] = ls[7];
            // pitch depth: -63..+63 on the JV-80 as on the JV-1080 (JV-80 manual), so one to one
            d[l ? jv::T_PITCH_LFO2 : jv::T_PITCH_LFO1] = sense(ls[8]);
            d[l ? jv::T_FILTER_LFO2 : jv::T_FILTER_LFO1] = sense(ls[9]);
            d[l ? jv::T_AMP_LFO2 : jv::T_AMP_LFO1] = sense(ls[10]);
            d[l ? jv::T_PAN_LFO2 : jv::T_PAN_LFO1] = 63;
        }
        d[jv::T_COARSE] = (uint8_t)std::max(0, std::min(96, (int)s[0x38] - 16));    // 16..112 -> 0..96
        d[jv::T_FINE] = (uint8_t)std::max(0, std::min(100, (int)s[0x39] - 14));     // 14..114 -> 0..100
        d[jv::T_RANDOM_PITCH] = (uint8_t)nearest(jv::kRandomPitch, 31, random80[s[0x3A] & 15]);
        d[jv::T_PITCH_KF] = s[0x3B] & 15;
        d[jv::T_PENV_VELO] = velo(s[0x3C]);
        d[jv::T_PENV_VELO_T1] = std::min<uint8_t>(s[0x3D], 14); d[jv::T_PENV_VELO_T4] = std::min<uint8_t>(s[0x3E], 14);
        d[jv::T_PENV_TIME_KF] = std::min<uint8_t>(s[0x3F], 14);
        d[jv::T_PENV_DEPTH] = (uint8_t)std::max(0, std::min(24, (int)s[0x40] - 52));
        for (int i = 0; i < 4; ++i) { d[jv::T_PENV_T1 + i] = s[0x41 + 2 * i]; d[jv::T_PENV_L1 + i] = sense(s[0x42 + 2 * i]); }
        static const uint8_t mode80[3] = {0, 1, 3};   // OFF, LPF, HPF
        d[jv::T_FILTER_TYPE] = mode80[std::min<int>(s[0x49], 2)];
        d[jv::T_CUTOFF] = (uint8_t)std::lround(jv80Cutoff(s[0x4A], s[0x49] == 2)); d[jv::T_RESONANCE] = s[0x4B]; d[jv::T_CUTOFF_KF] = s[0x4D] & 15;
        d[jv::T_FENV_CURVE] = std::min<uint8_t>(s[0x4E], 6); d[jv::T_FENV_VELO] = velo(s[0x4F]);
        d[jv::T_FENV_VELO_T1] = std::min<uint8_t>(s[0x50], 14); d[jv::T_FENV_VELO_T4] = std::min<uint8_t>(s[0x51], 14);
        d[jv::T_FENV_TIME_KF] = std::min<uint8_t>(s[0x52], 14); d[jv::T_FENV_DEPTH] = jv80CutoffAmount(s[0x53], s[0x4A], s[0x49] == 2, 7.0 / 63.0);
        for (int i = 0; i < 4; ++i) { d[jv::T_FENV_T1 + i] = s[0x54 + 2 * i]; d[jv::T_FENV_L1 + i] = s[0x55 + 2 * i]; }
        d[jv::T_LEVEL] = s[0x5C];
        d[jv::T_BIAS_LEVEL] = 7;                 // level keyfollow: not modelled
        int pan = s[0x5E] << 4 | s[0x5F];
        d[jv::T_PAN] = (uint8_t)(pan >= 128 ? 64 : pan);
        d[jv::T_RANDOM_PAN] = pan >= 128 ? 63 : 0;
        d[jv::T_PAN_KF] = std::min<uint8_t>(s[0x60], 14);                          // the same 15-step list
        d[jv::T_ALT_PAN] = 64;
        int delay = s[0x62] << 4 | s[0x63];
        d[jv::T_DELAY_MODE] = delay >= 128 ? 3 : std::min<uint8_t>(s[0x61], 2);
        d[jv::T_DELAY_TIME] = delay >= 128 ? 0 : (uint8_t)delay;
        d[jv::T_AENV_CURVE] = std::min<uint8_t>(s[0x64], 6); d[jv::T_AENV_VELO] = velo(s[0x65]);
        d[jv::T_AENV_VELO_T1] = std::min<uint8_t>(s[0x66], 14); d[jv::T_AENV_VELO_T4] = std::min<uint8_t>(s[0x67], 14);
        d[jv::T_AENV_TIME_KF] = std::min<uint8_t>(s[0x68], 14);
        for (int i = 0; i < 4; ++i) d[jv::T_AENV_T1 + i] = s[0x69 + 2 * i];
        for (int i = 0; i < 3; ++i) d[jv::T_AENV_L1 + i] = s[0x6A + 2 * i];
        d[jv::T_OUTPUT] = 0;
        d[jv::T_SEND_LEVEL] = s[0x70]; d[jv::T_REVERB_SEND] = s[0x71]; d[jv::T_CHORUS_SEND] = s[0x72];
    }
    return p;
}

Patch unpackJv80(const uint8_t* packed)
{
    uint8_t c80[0x22] = {};
    uint8_t t80[4][0x73] = {};
    BitReader r(packed);
    read80(r, kJv80Common, c80);
    for (int t = 0; t < 4; ++t) read80(r, kJv80Tone, t80[t]);
    return convertJv80(c80, t80);
}

} // namespace

std::string Patch::name() const { return ascii(common, 12); }

std::string Card::label() const
{
    std::string s = id.empty() ? file : id;
    return s;
}

std::vector<uint8_t> descramble(const std::vector<uint8_t>& dump, int v)
{
    if (v < 0) return dump;
    const DataTables& t = dataTables();
    std::vector<uint8_t> out(dump.size());
    for (uint32_t i = 0; i < dump.size(); ++i)
    {
        uint32_t a = plainAddr(i);
        if (a < out.size()) out[a] = t.plain[v][dump[i]];
    }
    return out;
}

std::vector<uint8_t> scramble(const std::vector<uint8_t>& plain, int v)
{
    if (v < 0) return plain;
    const DataTables& t = dataTables();
    std::vector<uint8_t> out(plain.size());
    for (uint32_t a = 0; a < plain.size(); ++a)
    {
        uint32_t i = dumpAddr(a);
        if (i < out.size()) out[i] = t.dump[v][plain[a]];
    }
    return out;
}

Patch unpackPatch(const uint8_t* packed)
{
    Patch p;
    BitReader r(packed);
    for (const jv::Field& f : jv::kCommonFields) p.common[f.at] = (uint8_t)r.take(f.bits);
    r.skip(jv::kCommonPad);
    for (int t = 0; t < 4; ++t)
    {
        for (const jv::Field& f : jv::kToneFields) p.tone[t][f.at] = (uint8_t)r.take(f.bits);
        r.skip(jv::kTonePad);
    }
    return p;
}

void packPatch(const Patch& p, uint8_t* packed)
{
    std::memset(packed, 0, kPackedPatch);
    BitWriter w(packed);
    for (const jv::Field& f : jv::kCommonFields) w.put(p.common[f.at], f.bits);
    w.skip(jv::kCommonPad);
    for (int t = 0; t < 4; ++t)
    {
        for (const jv::Field& f : jv::kToneFields) w.put(p.tone[t][f.at], f.bits);
        w.skip(jv::kTonePad);
    }
}

bool probe(const std::string& path, std::string* name)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    bool ok = false;
    if (size >= (1L << 20) && size <= (long)kCardSize && size % (1L << 20) == 0)
    {
        auto rawAt = [&](uint32_t a) -> uint8_t {
            if ((long)a >= size || std::fseek(f, (long)a, SEEK_SET) != 0) return 0;
            int c = std::fgetc(f);
            return c < 0 ? 0 : (uint8_t)c;
        };
        int v = detectVariant(rawAt);
        if (v != -2)
        {
            ok = true;
            if (name)
            {
                const DataTables& t = dataTables();
                uint8_t id[16];
                for (uint32_t i = 0; i < 16; ++i) id[i] = v < 0 ? rawAt(0x20 + i) : t.plain[v][rawAt(dumpAddr(0x20 + i))];
                *name = ascii(id, 16);
            }
        }
    }
    std::fclose(f);
    return ok;
}

std::unique_ptr<Card> load(const std::string& path, int jv80GroupId)
{
    auto card = std::make_unique<Card>();
    card->file = baseName(path);
    std::vector<uint8_t> raw;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0 && size <= (long)kCardSize)
        {
            raw.resize((size_t)size);
            if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
        }
        std::fclose(f);
    }
    if (raw.size() < (1u << 20)) { card->error = "not readable, or not an 8 MB card dump"; return card; }

    int v = detectVariant([&](uint32_t a) -> uint8_t { return a < raw.size() ? raw[a] : 0; });
    if (v == -2) { card->error = "not an SR-JV80 dump (its header does not read)"; return card; }
    std::vector<uint8_t> rom = descramble(raw, v);
    raw.clear();
    raw.shrink_to_fit();
    const size_t size = rom.size();
    const uint8_t* h = rom.data();

    card->id = ascii(h + 0x20, 16);
    card->date = ascii(h + 0x30, 10);
    uint32_t rate = be16(h + 0x40) * 100;
    card->sampleRate = (rate >= 8000 && rate <= 48000) ? rate : 32000;

    // ---- samples
    const uint32_t nSamples = be16(h + 0x60), nWaves = be16(h + 0x62);
    const uint32_t sampleTable = be32(h + 0x80) & 0xFFFFFF, waveTable = be32(h + 0x84) & 0xFFFFFF;
    if (nSamples == 0 || nSamples > 4096 || sampleTable + nSamples * 18 > size)
    { card->error = "the sample table is not where the header says"; return card; }
    for (uint32_t i = 0; i < nSamples; ++i) card->samples.push_back(readSample(h + sampleTable + i * 18, 0, size));
    if (nWaves > 0 && nWaves <= 1024 && waveTable + nWaves * 60 <= size)
        for (uint32_t i = 0; i < nWaves; ++i) card->waves.push_back(readWave(h + waveTable + i * 60, 0, nSamples));

    decodePcm(*card, rom);

    // ---- the JV-1080 patch table: the block header whose table decodes as JV-1080 patches
    int bestBlock = -1, bestGood = 0;
    for (uint32_t b = 0; b < size >> 20; ++b)
    {
        const uint8_t* bh = h + (b << 20);
        uint32_t n = be16(bh + 0x66), at = be32(bh + 0x8C) & 0xFFFFFF;
        if (n == 0 || n > 256 || at + n * kPackedPatch > size) continue;
        int good = 0;
        for (uint32_t i = 0; i < n; ++i) good += plausiblePatch(unpackPatch(h + at + i * kPackedPatch)) ? 1 : 0;
        if (good * 10 >= (int)n * 9 && good > bestGood) { bestGood = good; bestBlock = (int)b; }
    }
    if (bestBlock >= 0)
    {
        const uint8_t* bh = h + ((uint32_t)bestBlock << 20);
        uint32_t n = be16(bh + 0x66), at = be32(bh + 0x8C) & 0xFFFFFF;
        for (uint32_t i = 0; i < n; ++i) card->patches.push_back(unpackPatch(h + at + i * kPackedPatch));
    }

    // a board made for the JV-80 has only its JV-80 table (block 0): convert those patches
    if (card->patches.empty())
    {
        uint32_t n = be16(h + 0x66), at = be32(h + 0x8C) & 0xFFFFFF;
        if (h[0x53] == 0x02 && n > 0 && n <= 256 && at + n * kJv80Record <= size)
        {
            std::vector<Patch> conv;
            int good = 0;
            for (uint32_t i = 0; i < n; ++i)
            {
                conv.push_back(unpackJv80(h + at + i * kJv80Record));
                good += plausibleName(conv.back()) ? 1 : 0;
            }
            if (good * 10 >= (int)n * 9) { card->patches = std::move(conv); card->jv80 = true; }
        }
    }

    // the group id this card's own patches give its waves
    std::map<int, int> ids;
    if (!card->jv80)   // a JV-80 board's patches name no id: their waves are the board's own
    for (const Patch& p : card->patches)
        for (int t = 0; t < 4; ++t)
            if (p.tone[t][jv::T_WAVE_GROUP] == 2) ids[p.tone[t][jv::T_WAVE_GROUP_ID]]++;
    int most = 0;
    for (auto& kv : ids) if (kv.second > most) { most = kv.second; card->groupId = kv.first; }
    if (card->jv80 && jv80GroupId > 0 && jv80GroupId < 128)
    {
        card->groupId = jv80GroupId;
        for (Patch& p : card->patches)
            for (int t = 0; t < 4; ++t)
                if (p.tone[t][jv::T_WAVE_GROUP] == 2) p.tone[t][jv::T_WAVE_GROUP_ID] = (uint8_t)jv80GroupId;
    }

    if (card->waves.empty()) card->error = "no multisample table";
    return card;
}

namespace {
// The JV-880's program ROM: byte 0 the number of multisamples, bytes 2-3 where the sample table
// starts, the multisamples (60 bytes each) from byte 4, the sample table (18 bytes each) after them.
bool jv880Tables(const std::vector<uint8_t>& p, uint32_t* nWaves, uint32_t* sampleAt)
{
    if (p.size() < 0x1000) return false;
    uint32_t n = p[0], at = be16(&p[2]);
    if (n < 16 || at != 4 + n * 60 || at + 18 * 16 > p.size()) return false;
    uint32_t named = 0;
    for (uint32_t w = 0; w < n; ++w)
    {
        bool ok = true;
        for (int i = 0; i < 12; ++i) ok = ok && printable(p[4 + w * 60 + i]);
        named += ok ? 1 : 0;
    }
    if (named * 10 < n * 9) return false;
    *nWaves = n;
    *sampleAt = at;
    return true;
}
} // namespace

InternalPart internalPart(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return InternalPart::None;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    uint8_t head[32] = {};
    std::fseek(f, 0, SEEK_SET);
    bool read = std::fread(head, 1, sizeof head, f) == sizeof head;
    std::fclose(f);
    if (!read) return InternalPart::None;
    if ((size_t)size == kIntChip && std::memcmp(head, "RolandJV1080", 12) == 0 && std::memcmp(head + 16, "Intwave", 7) == 0)
        return head[31] == 'B' ? InternalPart::WaveB : InternalPart::WaveA;
    if ((size_t)size == kIntChip && std::memcmp(head, "Roland JV80", 11) == 0 && std::memcmp(head + 16, "Internal", 8) == 0)
        return InternalPart::Jv880Wave;
    if (size == (256L << 10))
    {
        uint32_t n = 0, at = 0;
        if (jv880Tables(readFile(path, 256u << 10), &n, &at)) return InternalPart::Jv880Program;
    }
    if (size == (1L << 20))
    {
        ProgramTables t;
        if (findTables(readFile(path, 1u << 20), t)) return InternalPart::Program;
    }
    return InternalPart::None;
}

std::unique_ptr<Card> loadJv880(const std::string& program, const std::vector<std::string>& waves)
{
    auto card = std::make_unique<Card>();
    card->file = baseName(program);
    card->id = "JV-880 INT";
    std::vector<uint8_t> p = readFile(program, 256u << 10);
    uint32_t nWaves = 0, sampleAt = 0;
    if (!jv880Tables(p, &nWaves, &sampleAt)) { card->error = "the JV-880 program ROM's wave tables were not found"; return card; }
    if (waves.size() != 2) { card->error = "needs the JV-880's two wave ROMs"; return card; }
    // the chips: wired as the SR-JV80 boards are (address lines, first data-line order); checked
    // against the waves decoded by other means: the same steps, sample for sample
    std::vector<uint8_t> rom(2 * kIntChip, 0);
    const DataTables& dt = dataTables();
    for (int c = 0; c < 2; ++c)
    {
        std::vector<uint8_t> raw = readFile(waves[(size_t)c], kIntChip);
        if (raw.size() != kIntChip) { card->error = baseName(waves[(size_t)c]) + ": not a 2 MB wave ROM"; return card; }
        uint8_t* out = rom.data() + (size_t)c * kIntChip;
        for (uint32_t i = 0; i < kIntChip; ++i) out[plainAddr(i)] = dt.plain[0][raw[i]];
    }
    card->date = ascii(rom.data() + 0x30, 10);
    uint32_t rate = be16(rom.data() + 0x40) * 100;
    card->sampleRate = (rate >= 8000 && rate <= 48000) ? rate : 32000;
    // the sample table runs to the first entry that is not one
    for (uint32_t k = 0; k < 2048; ++k)
    {
        size_t e = sampleAt + k * 18;
        if (e + 18 > p.size() || p[e] > 127 || be24(&p[e + 7]) >= rom.size() || be24(&p[e + 1]) > be24(&p[e + 7])) break;
        card->samples.push_back(readSample(&p[e], 0, rom.size()));
    }
    if (card->samples.size() < 16) { card->error = "the JV-880's sample table is too short"; return card; }
    for (uint32_t w = 0; w < nWaves; ++w) card->waves.push_back(readWave(&p[4 + w * 60], 0, (uint32_t)card->samples.size()));
    decodePcm(*card, rom);
    return card;
}

std::unique_ptr<Card> loadInternal(const std::string& program, const std::vector<std::string>& waveA,
                                   const std::string& waveB)
{
    auto card = std::make_unique<Card>();
    card->file = baseName(program);
    card->id = "JV-1080 INT";
    std::vector<uint8_t> p = readFile(program, 1u << 20);
    ProgramTables t;
    if (p.empty() || !findTables(p, t)) { card->error = "the program ROM's wave tables were not found"; return card; }
    if (waveA.size() != 3 || waveB.empty()) { card->error = "needs the three INT-A wave ROMs and the INT-B one"; return card; }

    // the image: INT-A chips at 0, 2, 4 MB, the INT-B chip at 6 MB, each in its on-chip order
    std::vector<uint8_t> rom(4 * kIntChip, 0);
    const DataTables& dt = dataTables();
    std::vector<uint32_t> plain(kIntChip);
    for (uint32_t i = 0; i < kIntChip; ++i)
    {
        uint32_t a = 0;
        for (int k = 0; k < 21; ++k) a |= ((i >> kIntAddrFrom[k]) & 1u) << k;
        plain[i] = a;
    }
    for (int c = 0; c < 4; ++c)
    {
        const std::string& path = c < 3 ? waveA[(size_t)c] : waveB;
        std::vector<uint8_t> raw = readFile(path, kIntChip);
        if (raw.size() != kIntChip) { card->error = baseName(path) + ": not a 2 MB wave ROM"; return card; }
        uint8_t* out = rom.data() + (size_t)c * kIntChip;
        for (uint32_t i = 0; i < kIntChip; ++i) out[plain[i]] = dt.plain[kIntDataVariant][raw[i]];
    }
    card->date = ascii(rom.data() + 0x30, 10);
    uint32_t rate = be16(rom.data() + 0x40) * 100;
    card->sampleRate = (rate >= 8000 && rate <= 48000) ? rate : 32000;

    // samples: INT-A's then INT-B's (addresses moved to the INT-B chip)
    for (int b = 0; b < 2; ++b)
        for (uint32_t i = 0; i < t.nSamples[b]; ++i)
            card->samples.push_back(readSample(&p[t.sampleAt[b] + i * 18], b ? 3 * kIntChip : 0, rom.size()));
    // waves in wave-number order: INT-A 1..255, INT-B 1..193, each from the table its list names
    for (int l = 0; l < 2; ++l)
    {
        if (l == 1) card->intB = (int)card->waves.size();
        for (int i = 0; i < kIntWaves[l]; ++i)
        {
            uint32_t v = be16(&p[t.listAt[l] + 2 * i]), f = p[t.listAt[l] + 2 * kIntWaves[l] + i];
            card->waves.push_back(readWave(&p[t.waveAt[f] + v * 60], f ? t.nSamples[0] : 0, t.nSamples[f]));
        }
    }
    decodePcm(*card, rom);
    return card;
}

} // namespace srjv
