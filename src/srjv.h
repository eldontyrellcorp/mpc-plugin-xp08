// srjv.h - reads Roland SR-JV80 expansion board dumps: descrambling, the DPCM wave data, the
// sample and multisample tables and the JV-1080/2080 patch table.
//
// Format, as documented by the community (no Roland code or data is part of this file):
//   * hackyourlife/srscramble: address and data lines swapped on the board's ASIC, the ID block
//   * giulioz's roland_decoder gist: the 8-bit descrambling permutation, the DPCM decoding
//   * PythonBlue/SRJV FORMAT.md: block headers, sample / multisample tables; Import2080.py:
//     the bit-packed patch records (JV-2080 SysEx order, each parameter in its own bit width)
// This is an independent implementation from those descriptions.
//
// The JV-1080's internal wave ROMs use the same block layout and DPCM coding, with their own
// address-line order (measured on dumps: see loadInternal) and the boards' first data-line order.
//
// A card is 8 MB: eight 1 MB blocks, each a 1 KB header, 31 KB of DPCM exponents (one nibble per
// 16 samples) and 992 KB of one-byte deltas. Samples are mono, at the rate in the header (32 kHz).
#ifndef XP_SRJV_H
#define XP_SRJV_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace srjv {

constexpr size_t kCardSize = 8u << 20;
constexpr int kCommonBytes = 74;   // JV-2080 patch common (SysEx 00 00 .. 00 49)
constexpr int kToneBytes = 129;    // one tone (SysEx 00 .. 80)
constexpr int kPackedPatch = 401;  // a patch record in the card's JV-1080 table

enum LoopType : uint8_t { LoopForward = 0, LoopPingPong = 1, LoopOff = 2, LoopReverse = 4, LoopReverseOff = 6 };

struct Sample
{
    uint32_t start = 0, loop = 0, end = 0;   // addresses in the card = indexes into Card::pcm
    uint8_t volume = 127;
    uint8_t loopType = LoopOff;
    uint8_t rootKey = 60;
    float fineSemis = 0.0f;                  // the sample's fine tune, in semitones
    float loopFineSemis = 0.0f;              // the loop's own fine tune (+0x10), in semitones
};

struct Multisample
{
    std::string name;
    uint8_t hiKey[16] = {};
    uint16_t sample[16] = {};
    int count = 0;                           // zones in use
    // the zone for `key` (0..127)
    const uint16_t* zoneFor(int key) const
    {
        for (int i = 0; i < count; ++i) if (key <= hiKey[i]) return &sample[i];
        return count ? &sample[count - 1] : nullptr;
    }
};

// A patch as the JV-2080's SysEx lays it out: index these with the offsets in jvparams.h.
struct Patch
{
    uint8_t common[kCommonBytes] = {};
    uint8_t tone[4][kToneBytes] = {};
    std::string name() const;
};

struct Card
{
    std::string file;          // the dump's file name (in roms/)
    std::string id;            // the card's ASCII name from its header (bytes 0x20..0x2F)
    std::string date;          // 0x30..0x39
    uint32_t sampleRate = 32000;
    int groupId = -1;          // the wave group id its own patches use for its waves
    bool jv80 = false;         // its patches came from the JV-80 table (a board for the JV-80/880)
    std::vector<Sample> samples;
    std::vector<Multisample> waves;
    std::vector<Patch> patches;
    std::vector<int16_t> pcm;  // the whole card decoded, one sample per card address
    int intB = -1;             // the JV-1080's internal waves: waves[0..intB) are INT-A, from intB on INT-B
    std::string error;         // why it did not load (empty: loaded)
    bool ok() const { return error.empty(); }
    std::string label() const; // short name for the screen
};

// Is this file an SR-JV80 dump? Reads only a few bytes. `name` gets the card's id.
bool probe(const std::string& path, std::string* name);

// Loads and decodes a whole card (allocates ~16 MB; seconds on a slow CPU: not on the audio thread).
// A board made for the JV-80 names no wave group id; `jv80GroupId` (1..127) gives it one, and its
// patches' tones on its waves that id, so that patches on other boards can use its waves too.
// 0: none (its own patches then find their waves through the slot they play from).
std::unique_ptr<Card> load(const std::string& path, int jv80GroupId = 0);

// ---- The JV-1080's own waves (the XP-50/60/80 have the same): its program ROM (1 MB, the wave
// lists and the multisample and sample tables) and its four 2 MB wave ROMs (three "INT-A" chips,
// one "INT-B"), as dumped from the instrument's chips.
enum class InternalPart { None, Program, WaveA, WaveB, Jv880Program, Jv880Wave };
InternalPart internalPart(const std::string& path);   // reads a few bytes (the whole file for a program ROM)
// `waveA`: the three INT-A chips in order (sorted file names). The card has no patches; its
// waves are INT-A 1..255 then INT-B 1..193 (Card::intB).
std::unique_ptr<Card> loadInternal(const std::string& program, const std::vector<std::string>& waveA,
                                   const std::string& waveB);

// ---- The JV-80's own waves, from a JV-880 (the same 4 MB set): its program ROM (256 KB, "rom2": the
// multisample and sample tables) and its two 2 MB wave ROMs ("Roland JV80 Internal"). These are the
// waves the boards' JV-80 patches name as INT. `waves`: the two chips in order (sorted file names).
// The card's waves are the JV-80's INT waves in number order (129).
std::unique_ptr<Card> loadJv880(const std::string& program, const std::vector<std::string>& waves);

// For tests and tools: an image in the board's on-chip order, and the same in dump order.
std::vector<uint8_t> descramble(const std::vector<uint8_t>& dump, int dataVariant);
std::vector<uint8_t> scramble(const std::vector<uint8_t>& plain, int dataVariant);
// Decode/encode one 401-byte record (the bit packing of the JV-1080 patch table).
Patch unpackPatch(const uint8_t* packed);
void packPatch(const Patch& p, uint8_t* packed);

} // namespace srjv

#endif
