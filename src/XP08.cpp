// XP-08 - an XP-80 style synthesizer for the MPC that plays Roland SR-JV80 expansion boards.
//
// Put SR-JV80 dumps (8 MB, as read from the boards) in the plugin folder's roms/ directory, and
// optionally a JV-1080's program and wave ROM dumps for the tones on its internal waves. As
// on the XP-80, four slots (A-D) hold boards; patch banks EXP-A..EXP-D are their patch tables
// (JV-1080 versions, up to 255 per board), played by the engine in engine.cpp.
//
// Signal flow (per voice, see engine.h): multisample -> TVF -> TVA -> pan -> MIX or EFX bus,
// chorus and reverb sends; then EFX -> chorus -> reverb (fx.h) -> Volume -> out.
//
// Editing, as on the XP-80: the patch picked is copied into an edit buffer, and that is what
// plays. The edit parameters (editparams.h) change the buffer in place: the common ones the
// patch common, the tone ones the tone chosen with TONE SELECT ("Edit Tone"). Picking another
// patch replaces the buffer (the XP-80 discards an edit it was not told to WRITE); the buffer
// is saved with the project. Whenever the buffer or the edit tone changes, the loader thread
// sets every edit parameter to what the buffer holds and tells the host.
//
// Threads: the constructor lists roms/ (reading a few bytes of each file). A loader thread
// decodes the boards chosen for the slots (8 MB each, a second or more on the MPC) and hands
// them to the audio thread through atomics; a board is freed only once the audio thread can no
// longer be using it. process() never allocates, locks or touches files. Edits from the host
// write single bytes of the buffer the audio thread plays (each byte store is atomic).

#include "mpc_plugin.h"
#include "mpc_resources.h"
#include "editparams.h"
#include "efxdefs.h"
#include "engine.h"
#include "jvparams.h"
#include "srjv.h"

#include <dirent.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kMaxCards = 32;      // cards listed from roms/
constexpr int kSlots = 4;
constexpr int kJv80GroupBase = 64; // the wave group ids given to JV-80 boards: 64 + their place in roms/

// Parameter order is permanent: append new ones at the end.
enum P {
    pVolume = 0,                   // output level; parameter 0 is nudged by the host on load
    pSlotA, pSlotB, pSlotC, pSlotD,// the board in each slot: 0 empty, n = n-th dump in roms/
    pBank,                         // EXP-A..D: the slot whose patches play
    pPatch,                        // 1..256
    pTone1, pTone2, pTone3, pTone4,// the XP-80's TONE SWITCH buttons: the patch's tone switches
    pOctave,                       // the OCTAVE buttons, -3..+3
    pCutoff, pReso, pAttack, pRelease, // quick edit, -63..+63 on top of the patch
    pEfx, pChorus, pReverb,        // effects on / off
    pPoly,                         // voices (the XP-80 has 64)
    pPgmChg,                       // MIDI program changes pick the patch
    pEditTone,                     // TONE SELECT: the tone the tone parameters edit
    pFirstEdit,                    // then edit::kDefs, in order
    pPatchDec = pFirstEdit + edit::kCount, pPatchInc,   // the DEC / INC buttons: one patch down / up
    pWaveDec, pWaveInc,            // one wave down / up for the tone being edited
    pToEfx1, pToEfx2, pToEfx3, pToEfx4,   // each tone's output, MIX or EFX (the EFX page's view of it)
    pEfxP1, pEfxP12 = pEfxP1 + 11,        // the EFX type's 12 parameters (efxdefs.h: what each one is)
    pEfxN1, pEfxN12 = pEfxN1 + 11,        // their names for the current type (display only: the knobs' captions)
    // the edited tone's amp and filter envelopes as ADSR, for the graphs: Attack = T1, Decay = T2
    // and T3 together (their ratio kept), Sustain = L3, Release = T4. Views of the T / L values.
    pAmpA, pAmpD, pAmpS, pAmpR,
    pFltA, pFltD, pFltS, pFltR,
    pFilterModel,                  // the filter: XP-80 (2-pole, 12 dB), 24 dB (two stages), LADDER (4-pole low-pass)
    pFilterDrive,                  // saturation into the filter, 0..127
    pCount
};

const char* const kBankNames[kSlots] = {"EXP-A", "EXP-B", "EXP-C", "EXP-D"};

struct CardFile { std::string path, file, id; };   // id: the name shown (the board's official name)

// The instruments' own ROMs found in roms/: a JV-1080's (all five, or none is used) for the INT
// waves of the JV-1080 patches, and a JV-880's (program ROM, two wave ROMs) for the JV-80 patches'
struct InternalFiles
{
    std::string program, waveB;
    std::vector<std::string> waveA;   // sorted by file name: chip 1, 2, 3
    bool complete() const { return !program.empty() && !waveB.empty() && waveA.size() == 3; }
    std::string program80;            // the last by file name (of rom2 1.0.0 and 1.0.1: the later version)
    std::vector<std::string> wave80;  // sorted by file name: chip 1, 2
    bool complete80() const { return !program80.empty() && wave80.size() == 2; }
};

// The board's official name from the code in its header (POP100, SFX101, DEM2_100...: the
// letters before the version say which board it is), as on the SR-JV80 boards' labels. Codes
// checked on dumps of the boards 01-10, 13-19 and the three Experience boards.
std::string officialName(const std::string& code)
{
    static const struct { const char* prefix; const char* name; } kBoards[] = {
        {"POP", "Pop"}, {"ORC", "Orchestral"}, {"PNO", "Piano"}, {"SYN", "Vintage Synth"}, {"ETH", "World"},
        {"DNC", "Dance"}, {"CAR", "Super Sound Set"}, {"GTO", "Keys of 60s & 70s"}, {"COL", "Session"},
        {"BSD", "Bass & Drum"}, {"CHR", "Vocal"}, {"ASI", "Asia"}, {"SFX", "Special FX"}, {"FLM", "Orchestral II"},
        {"CTR", "Country"}, {"LTN", "Latin"}, {"House", "House"},
        {"DEM2", "Experience II"}, {"DEM3", "Experience III"}, {"DEM", "Experience"},
    };
    for (const auto& b : kBoards)
        if (code.compare(0, std::strlen(b.prefix), b.prefix) == 0) return b.name;
    return {};
}

// A board we have no name for: its file name, without the extension, an "SR-JV80-nn" prefix and
// a checksum suffix (" - CS 0x...", " CS_0x...", " 0x..."), as dumps are often named.
std::string nameFromFile(const std::string& file)
{
    std::string s = file.substr(0, file.find_last_of('.'));
    for (const char* cut : {" - CS", " CS_", " CS ", " 0x"})
    {
        size_t at = s.find(cut);
        if (at != std::string::npos && at > 0) s.resize(at);
    }
    if (s.compare(0, 8, "SR-JV80-") == 0 || s.compare(0, 8, "SR-JV80_") == 0)
    {
        s.erase(0, 8);
        size_t sp = s.find_first_of(" _");
        if (sp != std::string::npos && sp <= 3) s.erase(0, sp + 1);
    }
    for (char& c : s) if (c == '_') c = ' ';
    return s;
}

std::string boardName(const std::string& code, const std::string& file)
{
    std::string n = officialName(code);
    if (n.empty()) n = nameFromFile(file);
    return n.empty() ? code : n;
}

std::vector<CardFile> listCards(const std::string& dir, InternalFiles& internal)
{
    std::vector<CardFile> out;
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names)
    {
        std::string id, path = dir + "/" + n;
        switch (srjv::internalPart(path))
        {
            case srjv::InternalPart::Program: internal.program = path; continue;
            case srjv::InternalPart::WaveA: internal.waveA.push_back(path); continue;
            case srjv::InternalPart::WaveB: internal.waveB = path; continue;
            case srjv::InternalPart::Jv880Program: internal.program80 = path; continue;
            case srjv::InternalPart::Jv880Wave: internal.wave80.push_back(path); continue;
            case srjv::InternalPart::None: break;
        }
        if (out.size() < (size_t)kMaxCards && srjv::probe(path, &id)) out.push_back({path, n, boardName(id, n)});
    }
    return out;
}

std::string clip(std::string s, size_t n) { if (s.size() > n) s.resize(n); return s; }

void put16(std::vector<uint8_t>& v, size_t n) { v.push_back((uint8_t)(n >> 8)); v.push_back((uint8_t)n); }

std::vector<std::string> split(const char* s)
{
    std::vector<std::string> out(1);
    for (; s && *s; ++s) { if (*s == '|') out.emplace_back(); else out.back() += *s; }
    return out;
}

inline void storeByte(uint8_t& b, int v) { __atomic_store_n(&b, (uint8_t)v, __ATOMIC_RELAXED); }

std::string noteName(int n)
{
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

std::string signedText(int v) { return v > 0 ? "+" + std::to_string(v) : std::to_string(v); }

} // namespace

class XP08 : public mpc::Plugin
{
public:
    XP08() : mpc::Plugin(makeInfo())
    {
        romDir_ = mpc::pluginFolder() + "/roms";
        files_ = listCards(romDir_, internalFiles_);

        std::vector<std::string> cards{"Empty"};
        for (int i = 0; i < kMaxCards; ++i)
            cards.push_back(i < (int)files_.size() ? clip(files_[(size_t)i].id, 20) : "-");

        addParam(mpc::floatParam("volume", "Volume", 0.0f, 1.0f, 0.8f));
        for (int s = 0; s < kSlots; ++s)
        {
            static const char* ids[kSlots] = {"slot_a", "slot_b", "slot_c", "slot_d"};
            static const char* names[kSlots] = {"Slot A", "Slot B", "Slot C", "Slot D"};
            addParam(mpc::choiceParam(ids[s], names[s], cards, s < (int)files_.size() ? s + 1 : 0));
        }
        addParam(mpc::choiceParam("bank", "Bank", {kBankNames[0], kBankNames[1], kBankNames[2], kBankNames[3]}, 0));
        addParam(mpc::intParam("patch", "Patch", 1, 256, 1));
        for (int t = 0; t < 4; ++t)
        {
            static const char* ids[4] = {"tone1", "tone2", "tone3", "tone4"};
            static const char* names[4] = {"Tone 1", "Tone 2", "Tone 3", "Tone 4"};
            addParam(mpc::boolParam(ids[t], names[t], true));
        }
        addParam(mpc::intParam("octave", "Octave", -3, 3, 0));
        addParam(mpc::intParam("cutoff", "Cutoff", -63, 63, 0));
        addParam(mpc::intParam("reso", "Resonance", -63, 63, 0));
        addParam(mpc::intParam("attack", "Attack", -63, 63, 0));
        addParam(mpc::intParam("release", "Release", -63, 63, 0));
        addParam(mpc::boolParam("efx", "EFX", true));
        addParam(mpc::boolParam("chorus", "Chorus", true));
        addParam(mpc::boolParam("reverb", "Reverb", true));
        addParam(mpc::intParam("poly", "Voices", 8, 64, 32));
        addParam(mpc::boolParam("pgmchg", "Prog Change", true));
        addParam(mpc::choiceParam("edit_tone", "Edit Tone", {"1", "2", "3", "4"}, 0));
        for (const edit::Def& d : edit::kDefs)
        {
            if (d.fmt == edit::Choice || d.fmt == edit::WaveGroup) addParam(mpc::choiceParam(d.id, d.name, split(d.choices), 0));
            else if (d.fmt == edit::WaveNo) addParam(mpc::intParam(d.id, d.name, 1, d.max, 1));
            else addParam(mpc::intParam(d.id, d.name, 0, d.max, 0));
        }

        // the knobs and the data wheel move a 256-value parameter in steps of about 2: these step by one
        addParam(mpc::boolParam("patch_dec", "Patch -", false));
        addParam(mpc::boolParam("patch_inc", "Patch +", false));
        addParam(mpc::boolParam("wave_dec", "Wave -", false));
        addParam(mpc::boolParam("wave_inc", "Wave +", false));
        for (int t = 0; t < 4; ++t)
        {
            static const char* ids[4] = {"tone1_efx", "tone2_efx", "tone3_efx", "tone4_efx"};
            static const char* names[4] = {"Tone 1 > EFX", "Tone 2 > EFX", "Tone 3 > EFX", "Tone 4 > EFX"};
            addParam(mpc::choiceParam(ids[t], names[t], {"MIX", "EFX"}, 0));
        }
        for (int k = 0; k < 12; ++k)
        {
            static char ids[12][16], names[12][16];
            std::snprintf(ids[k], sizeof ids[k], "efx_p%d", k + 1);
            std::snprintf(names[k], sizeof names[k], "EFX %d", k + 1);
            addParam(mpc::intParam(ids[k], names[k], 0, 127, 0));
        }
        for (int k = 0; k < 12; ++k)
        {
            static char ids[12][16], names[12][16];
            std::snprintf(ids[k], sizeof ids[k], "efx_n%d", k + 1);
            std::snprintf(names[k], sizeof names[k], "EFX %d Name", k + 1);
            mpc::Param n = mpc::intParam(ids[k], names[k], 0, 1, 0);
            n.automatable = false;
            addParam(n);
        }
        for (int k = 0; k < 8; ++k)
        {
            static const char* ids[8] = {"amp_a", "amp_d", "amp_s", "amp_r", "flt_a", "flt_d", "flt_s", "flt_r"};
            static const char* names[8] = {"Amp Attack", "Amp Decay", "Amp Sustain", "Amp Release",
                                           "Flt Attack", "Flt Decay", "Flt Sustain", "Flt Release"};
            addParam(mpc::intParam(ids[k], names[k], 0, 127, 0));
        }
        addParam(mpc::choiceParam("filter_model", "Filter Model", {"XP-80 12dB", "24dB", "LADDER"}, 0));
        addParam(mpc::intParam("filter_drive", "Filter Drive", 0, 127, 0));

        for (int s = 0; s < kSlots; ++s) want_[s].store(getInt(pSlotA + s) - 1);
        loader_ = std::thread([this] { loaderLoop(); });
    }

    ~XP08() override
    {
        quit_.store(true);
        if (loader_.joinable()) loader_.join();
    }

    void prepare(double sampleRate, int) override { engine_.prepare((float)sampleRate); }

    void reset() override { engine_.allSoundOff(); }

    void process(mpc::ProcessContext& ctx) override
    {
        inProcess_.store(true);
        const srjv::Card* cards[kSlots];
        for (int s = 0; s < kSlots; ++s)
        {
            int w = getInt(pSlotA + s) - 1;
            if (w != want_[s].load(std::memory_order_relaxed)) want_[s].store(w);
            cards[s] = live_[s].load();
        }
        engine_.setCards(cards);
        engine_.setInternal(internal_.load());
        engine_.setInternal80(internal80_.load());

        // settings from the panel (the tone switches are the patch's own: see parameterChanged)
        xp::Settings& st = engine_.settings;
        st.octave = getInt(pOctave);
        st.cutoff = getInt(pCutoff) / 63.0f;
        st.reso = getInt(pReso) / 63.0f;
        st.attack = getInt(pAttack) / 63.0f;
        st.release = getInt(pRelease) / 63.0f;
        st.efx = getBool(pEfx);
        st.chorus = getBool(pChorus);
        st.reverb = getBool(pReverb);
        st.polyphony = getInt(pPoly);
        if (ctx.transport.tempoValid && ctx.transport.bpm > 0) st.hostTempo = (float)ctx.transport.bpm;
        st.filterModel = getInt(pFilterModel);
        st.filterDrive = getInt(pFilterDrive) / 127.0f;
        applyPatch();
        if (fxDirty_.exchange(false)) engine_.refreshFx();

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : L;
        int done = 0;
        for (int i = 0; i <= ctx.numMidiEvents; ++i)
        {
            int until = i < ctx.numMidiEvents ? std::min(std::max(ctx.midi[i].sampleOffset, done), ctx.numFrames) : ctx.numFrames;
            if (until > done) { engine_.render(L + done, R + done, until - done); done = until; }
            if (i < ctx.numMidiEvents) midi(ctx.midi[i]);
        }
        float g = get(pVolume);
        for (int i = 0; i < ctx.numFrames; ++i) { L[i] *= g; if (R != L) R[i] *= g; }
        inProcess_.store(false);
    }

    // ---- edits: a parameter moved on the MPC (or a project is being restored)
    void parameterChanged(int index, float plain) override
    {
        if (syncing_.load()) return;                         // the plugin itself is setting it from the buffer
        srjv::Patch& p = buf_[cur_.load()];
        int v = (int)std::lround(plain);
        if (index == pEditTone) { displayReq_.store(true); syncReq_.store(true); return; }
        if (index >= pPatchDec && index <= pWaveInc)
        {
            if (v) stepReq_.fetch_or(1 << (index - pPatchDec));   // the loader thread steps, then lets the button up
            return;
        }
        if (index >= pTone1 && index <= pTone4) { storeByte(p.tone[index - pTone1][jv::T_SWITCH], v ? 1 : 0); return; }
        if (index >= pEfxN1 && index <= pEfxN12) return;   // captions only
        if (index >= pAmpA && index <= pFltR)
        {
            int t = std::max(0, std::min(getInt(pEditTone), 3));
            bool amp = index <= pAmpR;
            int stage = index - (amp ? pAmpA : pFltA);
            uint8_t* tp = p.tone[t];
            uint8_t* T = tp + (amp ? jv::T_AENV_T1 : jv::T_FENV_T1);
            uint8_t* L = tp + (amp ? jv::T_AENV_L1 : jv::T_FENV_L1);
            v = std::max(0, std::min(v, 127));
            switch (stage)
            {
                case 0: storeByte(T[0], v); break;
                case 1:
                {
                    int t2 = T[1], t3 = T[2], mx = std::max(t2, t3);
                    if (mx == 0) { storeByte(T[1], v); storeByte(T[2], v); }
                    else { storeByte(T[1], (int)std::lround((double)t2 * v / mx)); storeByte(T[2], (int)std::lround((double)t3 * v / mx)); }
                    break;
                }
                case 2: storeByte(L[2], v); break;
                default: storeByte(T[3], v); break;
            }
            syncReq_.store(true);                             // the PRO pages' T / L values
            return;
        }
        if (index >= pEfxP1 && index <= pEfxP12)
        {
            int k = index - pEfxP1, type = std::min<int>(p.common[jv::C_EFX_TYPE], 39);
            int mx = efx::fieldMax(efx::kTypes[type].f[k]);
            storeByte(p.common[jv::C_EFX_PARAM + k], std::max(0, std::min(v, mx)));
            fxDirty_.store(true);
            if (k == 0) syncReq_.store(true);                 // EFX Drive is parameter 1 too
            return;
        }
        if (index >= pToEfx1 && index <= pToEfx4)
        {
            storeByte(p.tone[index - pToEfx1][jv::T_OUTPUT], v ? 1 : 0);
            syncReq_.store(true);                             // the tone pages' Output shows it too
            return;
        }
        if (index < pFirstEdit || index >= pFirstEdit + edit::kCount) return;
        const edit::Def& d = edit::kDefs[index - pFirstEdit];
        int t = std::max(0, std::min(getInt(pEditTone), 3));
        uint8_t* base = d.tone ? p.tone[t] : p.common;
        switch (d.fmt)
        {
            case edit::WaveGroup:
            {
                static const uint8_t grp[4][2] = {{0, 1}, {0, 2}, {0, 0}, {1, 1}};   // INT-A, INT-B, JV-80 INT, PCM
                if (v < edit::G_EXP_A) { storeByte(base[jv::T_WAVE_GROUP_ID], grp[v][1]); storeByte(base[jv::T_WAVE_GROUP], grp[v][0]); }
                else
                {
                    const srjv::Card* c = live_[std::min(v - edit::G_EXP_A, kSlots - 1)].load();
                    storeByte(base[jv::T_WAVE_GROUP_ID], c && c->groupId >= 0 ? c->groupId : 127);
                    storeByte(base[jv::T_WAVE_GROUP], 2);
                }
                syncReq_.store(true);                         // the Wave shows the new group's wave
                break;
            }
            case edit::WaveNo:
            {
                int n = std::max(1, std::min(v, 255)) - 1;
                storeByte(base[jv::T_WAVE_HI], n >> 4);
                storeByte(base[jv::T_WAVE_LO], n & 15);
                break;
            }
            default:
                if (!d.tone && d.at == jv::C_EFX_TYPE && v != base[d.at] && v >= 0 && v < 40)
                    for (int k = 0; k < 12; ++k) storeByte(base[jv::C_EFX_PARAM + k], efx::kDefaults[v][k]);   // a new type: its settings
                storeByte(base[d.at], std::max(0, std::min(v, d.fmt == edit::Choice ? 255 : (int)d.max)));
                break;
        }
        if (!d.tone) fxDirty_.store(true);
        if (!d.tone && (d.at == jv::C_EFX_TYPE || d.at == jv::C_EFX_PARAM)) syncReq_.store(true);   // the EFX page's names and values
        if (!d.tone && d.at == jv::C_EFX_TYPE) displayReq_.store(true);
        if (d.tone && d.at == jv::T_OUTPUT) syncReq_.store(true);   // the EFX page shows each tone's output
        if (d.tone && ((d.at >= jv::T_AENV_T1 && d.at <= jv::T_AENV_L1 + 2) || (d.at >= jv::T_FENV_T1 && d.at <= jv::T_FENV_L1 + 3)))
            syncReq_.store(true);                             // the ADSR graphs
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        // version 2: the slots by file name (so a project finds its boards if roms/ changes),
        // then the patch the edit buffer came from and the buffer itself
        out.insert(out.end(), {'X', 'P', '0', '8', 2});
        for (int s = 0; s < kSlots; ++s)
        {
            int i = getInt(pSlotA + s) - 1;
            std::string name = (i >= 0 && i < (int)files_.size()) ? files_[(size_t)i].file : std::string();
            put16(out, name.size());
            out.insert(out.end(), name.begin(), name.end());
        }
        const srjv::Patch& p = buf_[cur_.load()];
        bool edited = appliedKey_ >= 0 && isEdited();
        put16(out, (size_t)std::max(0, appliedKey_.load()));
        out.push_back(edited ? 1 : 0);
        if (edited)
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&p);
            out.insert(out.end(), b, b + sizeof(srjv::Patch));
        }
    }

    void loadExtraState(const uint8_t* d, size_t size) override
    {
        if (size < 5 || std::memcmp(d, "XP08", 4) != 0 || d[4] < 1 || d[4] > 2) return;
        int version = d[4];
        size_t at = 5;
        for (int s = 0; s < kSlots && at + 2 <= size; ++s)
        {
            size_t n = (size_t)d[at] << 8 | d[at + 1];
            at += 2;
            if (at + n > size) return;
            std::string name((const char*)d + at, n);
            at += n;
            if (name.empty()) continue;           // an empty slot: the restored parameter says so already
            int idx = 0;
            for (size_t i = 0; i < files_.size(); ++i) if (files_[i].file == name) idx = (int)i + 1;
            if (idx == 0) mpc::log("XP-08: %s (slot %c) is not in roms/", name.c_str(), 'A' + s);
            if (idx != getInt(pSlotA + s)) set(pSlotA + s, (float)idx);
        }
        if (version >= 2 && at + 3 <= size)
        {
            int key = d[at] << 8 | d[at + 1];
            bool edited = d[at + 2] != 0;
            at += 3;
            if (edited && at + sizeof(srjv::Patch) <= size)
            {
                std::memcpy(&pending_, d + at, sizeof(srjv::Patch));
                pendingKey_.store(key);
            }
        }
        reload_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        int v = (int)std::lround(plain);
        if (index == pVolume) { out = std::to_string((int)(plain * 100.0f + 0.5f)) + "%"; return true; }
        if (index == pBank)
        {
            int s = std::max(0, std::min(v, kSlots - 1));
            int f = getInt(pSlotA + s) - 1;
            out = std::string(kBankNames[s]) + "  " + (f >= 0 && f < (int)files_.size() ? clip(files_[(size_t)f].id, 20) : "(empty)");
            return true;
        }
        if (index == pPatch)
        {
            char num[8];
            std::snprintf(num, sizeof num, "%03d", v);
            int s = std::max(0, std::min(getInt(pBank), kSlots - 1));
            const srjv::Card* c = live_[s].load();
            if (want_[s].load() != loadedIdx_[s].load()) { out = std::string(num) + " loading..."; return true; }
            if (!c) { out = std::string(num) + " ---"; return true; }
            if (v < 1 || v > (int)c->patches.size()) { out = std::string(num) + " ---"; return true; }
            out = std::string(num) + " " + c->patches[(size_t)v - 1].name();
            if (appliedKey_ == s * 1000 + v - 1 && isEdited()) out += " *";   // the edit mark
            return true;
        }
        if (index == pCutoff || index == pReso || index == pAttack || index == pRelease || index == pOctave)
        {
            out = signedText(v);
            return true;
        }
        if (index == pEditTone) { out = "TONE " + std::to_string(v + 1); return true; }
        if (index >= pAmpA && index <= pFltR)
        {
            int stage = (index - pAmpA) % 4;
            if (stage == 2) { out = std::to_string(v); return true; }   // a level
            float sec = jv::envSeconds(v);
            char b[24];
            if (sec < 1) std::snprintf(b, sizeof b, "%d ms", (int)std::lround(sec * 1000));
            else std::snprintf(b, sizeof b, "%.1f s", sec);
            out = b;
            return true;
        }
        if ((index >= pEfxP1 && index <= pEfxP12) || (index >= pEfxN1 && index <= pEfxN12))
        {
            const srjv::Patch& p = buf_[cur_.load()];
            int type = std::min<int>(p.common[jv::C_EFX_TYPE], 39);
            bool name = index >= pEfxN1;
            const efx::Field& f = efx::kTypes[type].f[index - (name ? pEfxN1 : pEfxP1)];
            float bpm = (float)std::max(20, std::min(250, p.common[jv::C_TEMPO_HI] * 16 + p.common[jv::C_TEMPO_LO]));
            out = name ? (f.name ? f.name : "---") : efx::fieldText(f, v, bpm);
            return true;
        }
        if (index < pFirstEdit || index >= pFirstEdit + edit::kCount) return false;
        const edit::Def& d = edit::kDefs[index - pFirstEdit];
        char buf[48];
        switch (d.fmt)
        {
            case edit::Raw:
                if (d.at == jv::C_BEND_DOWN && !d.tone) out = v ? "-" + std::to_string(v) : "0";
                else out = std::to_string(v);
                return true;
            case edit::Signed63: out = signedText(v - 63); return true;
            case edit::Pan: out = v < 64 ? "L" + std::to_string(64 - v) : v > 64 ? std::to_string(v - 64) + "R" : "0"; return true;
            case edit::Coarse: out = signedText(v - 48); return true;
            case edit::Fine: out = signedText(v - 50); return true;
            case edit::PDepth: out = signedText(v - 12); return true;
            case edit::Velo: out = signedText((v - 50) * 2) + "%"; return true;
            case edit::Keyf: out = signedText(jv::kKeyfollow[std::max(0, std::min(v, 15))]) + "%"; return true;
            case edit::TimeS: out = signedText(jv::kTimeSens[std::max(0, std::min(v, 14))]) + "%"; return true;
            case edit::RndPitch: out = std::to_string(jv::kRandomPitch[std::max(0, std::min(v, 30))]) + " cent"; return true;
            case edit::Damp:
                if (v >= 17) out = "BYPASS";
                else { std::snprintf(buf, sizeof buf, "%d Hz", jv::kHfDamp[std::max(0, v)]); out = buf; }
                return true;
            case edit::Note: out = noteName(std::max(0, std::min(v, 127))); return true;
            case edit::Octave: out = signedText(v - 3); return true;
            case edit::PreDelay:
                std::snprintf(buf, sizeof buf, "%.1f ms", v < 50 ? v * 0.1 : 5.0 + (v - 50) * 1.24);
                out = buf;
                return true;
            case edit::WaveNo:
            {
                const srjv::Patch& p = buf_[cur_.load()];
                int wave = 0;
                const srjv::Card* c = waveCard(p.tone[std::max(0, std::min(getInt(pEditTone), 3))], &wave);
                std::snprintf(buf, sizeof buf, "%03d ", v);
                int i = c ? wave : -1;
                out = std::string(buf) + (c && i >= 0 && i < (int)c->waves.size() ? c->waves[(size_t)i].name : "---");
                return true;
            }
            default: return false;   // choices: their own names
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "XP-08";
        info.vendor = "Stephane Schmitz";
        info.uniqueId = MPC_FOURCC('S', 'k', 'X', 'P');   // must equal "uid" in plugin.json
        info.version = 1000;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        return info;
    }

    // ---- the edit buffer
    bool isEdited() const
    {
        int key = appliedKey_;
        const srjv::Card* c = appliedCard_;
        if (key < 0 || !c) return false;
        int i = key % 1000;
        return i < (int)c->patches.size() && std::memcmp(&c->patches[(size_t)i], &buf_[cur_.load()], sizeof(srjv::Patch)) != 0;
    }

    // which card and which of its waves a tone plays (as the engine resolves it); null: none
    const srjv::Card* waveCard(const uint8_t* tp, int* wave) const
    {
        int n = tp[jv::T_WAVE_HI] << 4 | tp[jv::T_WAVE_LO], id = tp[jv::T_WAVE_GROUP_ID];
        *wave = n;
        if (tp[jv::T_WAVE_GROUP] == 0 && id == 0)
        {
            const srjv::Card* in = internal80_.load();
            return in && n < (int)in->waves.size() ? in : nullptr;
        }
        if (tp[jv::T_WAVE_GROUP] == 0)
        {
            const srjv::Card* in = internal_.load();
            if (!in || in->intB < 0 || (id != 1 && id != 2)) return nullptr;
            if (id == 2) *wave = n + in->intB;
            return (id == 1 && n >= in->intB) ? nullptr : in;
        }
        if (tp[jv::T_WAVE_GROUP] != 2) return nullptr;
        for (int s = 0; s < kSlots; ++s) { const srjv::Card* c = live_[s].load(); if (c && c->groupId == id) return c; }
        const srjv::Card* own = live_[std::max(0, std::min(getInt(pBank), kSlots - 1))].load();
        return own && own->groupId < 0 ? own : nullptr;
    }

    // the parameter's plain value from the buffer
    float readEdit(const edit::Def& d, const srjv::Patch& p, int t) const
    {
        const uint8_t* base = d.tone ? p.tone[t] : p.common;
        switch (d.fmt)
        {
            case edit::WaveGroup:
            {
                int g = base[jv::T_WAVE_GROUP], id = base[jv::T_WAVE_GROUP_ID];
                if (g == 0) return (float)(id == 1 ? edit::G_INT_A : id == 2 ? edit::G_INT_B : edit::G_JV80_INT);
                if (g == 1) return (float)edit::G_PCM;
                for (int s = 0; s < kSlots; ++s) { const srjv::Card* c = live_[s].load(); if (c && c->groupId == id) return (float)(edit::G_EXP_A + s); }
                return (float)(edit::G_EXP_A + std::max(0, std::min(getInt(pBank), kSlots - 1)));
            }
            case edit::WaveNo: return (float)((base[jv::T_WAVE_HI] << 4 | base[jv::T_WAVE_LO]) + 1);
            case edit::Choice: return (float)std::min<int>(base[d.at], (int)param(pFirstEdit + (int)(&d - edit::kDefs)).choices.size() - 1);
            default: return (float)std::min<int>(base[d.at], d.max);
        }
    }

    // the audio thread: the patch picked goes into the other buffer, which then plays
    void applyPatch()
    {
        int bank = std::max(0, std::min(getInt(pBank), kSlots - 1)), patch = getInt(pPatch) - 1;
        int key = bank * 1000 + patch;
        const srjv::Card* c = live_[bank].load();
        bool force = reload_.exchange(false);
        if (!force && key == appliedKey_ && c == appliedCard_) return;
        int next = 1 - cur_.load();
        engine_.stopVoicesOf(&buf_[next]);
        bool ok = c && patch >= 0 && patch < (int)c->patches.size();
        if (ok && pendingKey_.load() == key) { buf_[next] = pending_; pendingKey_.store(-1); }
        else if (ok) buf_[next] = c->patches[(size_t)patch];
        else buf_[next] = srjv::Patch{};
        cur_.store(next);
        engine_.setPatchData(bank, ok ? &buf_[next] : nullptr);
        appliedKey_ = key;
        appliedCard_ = c;
        displayReq_.store(true);
        syncReq_.store(true);
    }

    // the loader thread: every edit parameter (and the tone switches) to what the buffer holds
    void syncParams()
    {
        const srjv::Patch& p = buf_[cur_.load()];
        int t = std::max(0, std::min(getInt(pEditTone), 3));
        syncing_.store(true);
        std::vector<int> moved;
        auto put = [&](int i, float plain) {
            if (std::fabs(get(i) - plain) < 0.5f) return;
            set(i, plain);
            moved.push_back(i);
        };
        for (int k = 0; k < 4; ++k) put(pTone1 + k, p.tone[k][jv::T_SWITCH] ? 1.0f : 0.0f);
        for (int k = 0; k < 4; ++k) put(pToEfx1 + k, p.tone[k][jv::T_OUTPUT] == 1 ? 1.0f : 0.0f);
        for (int k = 0; k < 12; ++k) put(pEfxP1 + k, (float)p.common[jv::C_EFX_PARAM + k]);
        for (int amp = 1; amp >= 0; --amp)
        {
            const uint8_t* T = p.tone[t] + (amp ? jv::T_AENV_T1 : jv::T_FENV_T1);
            const uint8_t* L = p.tone[t] + (amp ? jv::T_AENV_L1 : jv::T_FENV_L1);
            int b = amp ? pAmpA : pFltA;
            put(b, T[0]); put(b + 1, std::max(T[1], T[2])); put(b + 2, L[2]); put(b + 3, T[3]);
        }
        for (int k = 0; k < edit::kCount; ++k) put(pFirstEdit + k, readEdit(edit::kDefs[k], p, t));
        syncing_.store(false);
        for (int i : moved) notifyHostParameterChanged(i);
        // the page is rebuilt only when names can have changed (another patch, tone, EFX type, the
        // waves loaded); values that move are told to the host one by one, as a knob turns
        if (displayReq_.exchange(false)) requestDisplayUpdate();
    }

    // the loader thread: the DEC / INC buttons (bit 0 Patch -, 1 Patch +, 2 Wave -, 3 Wave +)
    void doSteps(int steps)
    {
        auto release = [&](int i) { set(i, 0.0f); notifyHostParameterChanged(i); };
        int bank = std::max(0, std::min(getInt(pBank), kSlots - 1));
        const srjv::Card* c = live_[bank].load();
        int last = c && !c->patches.empty() ? (int)c->patches.size() : 256;
        for (int b = 0; b < 2; ++b)
            if (steps & (1 << b))
            {
                int p = std::max(1, std::min(getInt(pPatch) + (b ? 1 : -1), last));
                set(pPatch, (float)p);
                notifyHostParameterChanged(pPatch);
                release(pPatchDec + b);
            }
        static const int waveIdx = [] { for (int k = 0; k < edit::kCount; ++k) if (edit::kDefs[k].fmt == edit::WaveNo) return k; return 0; }();
        for (int b = 0; b < 2; ++b)
            if (steps & (4 << b))
            {
                const srjv::Patch& p = buf_[cur_.load()];
                int wave = 0;
                const srjv::Card* wc = waveCard(p.tone[std::max(0, std::min(getInt(pEditTone), 3))], &wave);
                int count = 255;
                if (wc) count = wc == internal_.load() && wc->intB >= 0 ? (wave >= wc->intB ? (int)wc->waves.size() - wc->intB : wc->intB) : (int)wc->waves.size();
                int i = pFirstEdit + waveIdx;
                set(i, (float)std::max(1, std::min(getInt(i) + (b ? 1 : -1), std::min(count, 255))));   // writes the buffer
                notifyHostParameterChanged(i);
                release(pWaveDec + b);
            }
        requestDisplayUpdate();
    }

    void midi(const mpc::MidiEvent& e)
    {
        switch (e.status())
        {
            case 0x90: if (e.velocity() > 0) { engine_.noteOn(e.note(), e.velocity()); break; } /* fall through */
            case 0x80: engine_.noteOff(e.note()); break;
            case 0xB0: engine_.controller(e.controller(), e.controllerValue()); break;
            case 0xE0: engine_.pitchBend(e.data[1] | e.data[2] << 7); break;
            case 0xD0: engine_.pressure(e.data[1]); break;
            case 0xC0:
                if (getBool(pPgmChg)) { set(pPatch, (float)(e.data[1] + 1)); applyPatch(); }
                break;
            default: break;
        }
    }

    // ---- the loader thread: decodes the boards the slots ask for, keeps the edit parameters in step
    void loaderLoop()
    {
        std::shared_ptr<srjv::Card> held[kSlots];
        std::vector<std::shared_ptr<srjv::Card>> retired;
        std::unique_ptr<srjv::Card> internal, internal80;   // loaded once, after the first boards; live until the plugin goes
        bool internalTried = false;
        while (!quit_.load())
        {
            bool changed = false;
            for (int s = 0; s < kSlots; ++s)
            {
                int w = want_[s].load();
                if (w == loadedIdx_[s].load()) continue;
                std::shared_ptr<srjv::Card> next;
                if (w >= 0 && w < (int)files_.size())
                {
                    for (int o = 0; o < kSlots; ++o)      // the same board in another slot: share it
                        if (o != s && held[o] && loadedIdx_[o].load() == w) next = held[o];
                    if (!next)
                    {
                        std::shared_ptr<srjv::Card> c(srjv::load(files_[(size_t)w].path, kJv80GroupBase + w).release());
                        if (c->ok()) next = c;
                        else mpc::log("XP-08: %s: %s", files_[(size_t)w].file.c_str(), c->error.c_str());
                    }
                }
                if (want_[s].load() != w) continue;       // changed again while loading
                live_[s].store(next.get());
                if (held[s]) retired.push_back(held[s]);
                held[s] = next;
                loadedIdx_[s].store(w);
                changed = true;
            }
            if (changed && !retired.empty())
            {
                // free only when no process() call can still hold the old pointers
                auto t0 = std::chrono::steady_clock::now();
                while (inProcess_.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                if (!inProcess_.load()) retired.clear();
            }
            if (!internalTried && !changed)
            {
                internalTried = true;
                const InternalFiles& f = internalFiles_;
                if (f.complete())
                {
                    internal = srjv::loadInternal(f.program, f.waveA, f.waveB);
                    if (internal->ok()) internal_.store(internal.get());
                    else mpc::log("XP-08: JV-1080 waves: %s", internal->error.c_str());
                }
                else if (!f.program.empty() || !f.waveB.empty() || !f.waveA.empty())
                    mpc::log("XP-08: JV-1080 waves need the program ROM, three INT-A and one INT-B wave ROM in roms/");
                if (f.complete80())
                {
                    internal80 = srjv::loadJv880(f.program80, f.wave80);
                    if (internal80->ok()) internal80_.store(internal80.get());
                    else mpc::log("XP-08: JV-880 waves: %s", internal80->error.c_str());
                }
                else if (!f.program80.empty() || !f.wave80.empty())
                    mpc::log("XP-08: JV-80 waves need a JV-880's program ROM (rom2) and its two wave ROMs in roms/");
                displayReq_.store(true);                  // the wave names can show now
                syncReq_.store(true);
            }
            if (changed) { displayReq_.store(true); syncReq_.store(true); }
            if (int steps = stepReq_.exchange(0)) doSteps(steps);
            if (syncReq_.exchange(false)) syncParams();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (int s = 0; s < kSlots; ++s) live_[s].store(nullptr);
        internal_.store(nullptr);
        internal80_.store(nullptr);
    }

    xp::Engine engine_;
    std::string romDir_;
    std::vector<CardFile> files_;
    InternalFiles internalFiles_;
    std::atomic<const srjv::Card*> live_[kSlots] = {};
    std::atomic<const srjv::Card*> internal_{nullptr}, internal80_{nullptr};
    std::atomic<int> want_[kSlots] = {};
    std::atomic<int> loadedIdx_[kSlots] = {{-1}, {-1}, {-1}, {-1}};
    std::atomic<bool> inProcess_{false}, quit_{false};
    std::thread loader_;

    // the edit buffers: cur_ plays; a newly picked patch goes into the other one
    srjv::Patch buf_[2] = {};
    std::atomic<int> cur_{0};
    std::atomic<int> appliedKey_{-1};      // bank * 1000 + patch index the buffer came from
    std::atomic<const srjv::Card*> appliedCard_{nullptr};
    srjv::Patch pending_ = {};             // an edited buffer from a project, for when its patch loads
    std::atomic<int> pendingKey_{-1};
    std::atomic<bool> reload_{false}, syncReq_{false}, syncing_{false}, fxDirty_{false};
    std::atomic<bool> displayReq_{true};      // the next sync also rebuilds the page (names changed)
    std::atomic<int> stepReq_{0};
};

MPC_PLUGIN_ENTRY(XP08)
