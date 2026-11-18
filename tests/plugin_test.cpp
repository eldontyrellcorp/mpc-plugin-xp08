// plugin_test.cpp - the native plugin, through the VST2 interface the MPC uses, in a plugin
// folder of its own with the test card in roms/: the board loads in the background, the screen
// texts follow, notes sound, patches change from the parameter and from MIDI program changes,
// and a project's state brings its slot back.
//
//   plugin_test <plugin folder>/xp-08.so      (tests/run.sh sets the folder up)
#include "mpc_vst2_abi.h"

#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static AEffect* e;
static int fails = 0;
static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    static VstTimeInfo ti;
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterGetTime) { std::memset(&ti, 0, sizeof ti); ti.sampleRate = 44100; ti.tempo = 120; return (VstIntPtr)&ti; }
    return 0;
}
static std::string pname(int i) { char b[128] = {0}; e->dispatcher(e, effGetParamName, i, 0, b, 0); return b; }
static std::string pdisp(int i) { char b[128] = {0}; e->dispatcher(e, effGetParamDisplay, i, 0, b, 0); return b; }
static int find(const char* n) { for (int i = 0; i < e->numParams; i++) if (pname(i) == n) return i; std::printf("FAIL  no parameter %s\n", n); fails++; return 0; }
static void check(bool ok, const std::string& what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) fails++; }

struct Ev { VstEvents head; VstEvent* more[16]; VstMidiEvent m[16]; };
static void midi(unsigned char a, unsigned char b, unsigned char c)
{
    static Ev ev;
    std::memset(&ev, 0, sizeof ev);
    ev.head.numEvents = 1;
    ev.m[0].type = kVstMidiType;
    ev.m[0].byteSize = sizeof(VstMidiEvent);
    ev.m[0].midiData[0] = (char)a; ev.m[0].midiData[1] = (char)b; ev.m[0].midiData[2] = (char)c;
    ev.head.events[0] = (VstEvent*)&ev.m[0];
    e->dispatcher(e, effProcessEvents, 0, 0, &ev, 0);
}
// render `blocks` blocks of 128 at the audio clock's pace; returns the RMS in dB
static double run(int blocks, bool realtime = true)
{
    static float L[128], R[128];
    float* o[2] = {L, R};
    double s = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        static Ev none; none.head.numEvents = 0;
        e->dispatcher(e, effProcessEvents, 0, 0, &none, 0);
        e->processReplacing(e, nullptr, o, 128);
        for (int i = 0; i < 128; ++i) s += (double)L[i] * L[i] + (double)R[i] * R[i];
        if (realtime) std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)((b + 1) * 128 * 1e6 / 44100)));
    }
    return 10 * std::log10(s / (blocks * 256.0) + 1e-20);
}

int main(int argc, char** argv)
{
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::printf("FAIL  %s\n", dlerror()); return 1; }
    auto entry = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    e = entry(host);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 44100.f);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    int slotA = find("Slot A"), bank = find("Bank"), patch = find("Patch"), tone1 = find("Tone 1");

    check(pdisp(slotA) == "TEST", "slot A holds the card found in roms/, named from its file: \"" + pdisp(slotA) + "\"");
    auto t0 = std::chrono::steady_clock::now();
    std::string p;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
    {
        run(10);
        p = pdisp(patch);
        if (p.find("loading") == std::string::npos) break;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(p == "001 Sine Test", "the board loads in the background (" + std::to_string((int)ms) + " ms) and the screen shows \"" + p + "\"");
    check(pdisp(bank) == "EXP-A  TEST", "bank: \"" + pdisp(bank) + "\"");

    check(run(40) < -100, "silent before a note");
    midi(0x90, 69, 100);
    double on = run(100);
    midi(0x80, 69, 0);
    run(80);
    double off = run(40);
    check(on > -30 && off < -90, "a note sounds (" + std::to_string(on) + " dB) and ends (" + std::to_string(off) + " dB)");

    e->setParameter(e, tone1, 0.0f);
    midi(0x90, 69, 100);
    double muted = run(40);
    midi(0x80, 69, 0);
    run(80);
    check(muted < -100, "TONE 1 off mutes the patch's only tone");
    e->setParameter(e, tone1, 1.0f);

    e->setParameter(e, patch, 4.0f / 255.0f);   // patch 5
    run(2);
    check(pdisp(patch) == "005 Filter Sweep", "the Patch parameter selects \"" + pdisp(patch) + "\"");
    midi(0xC0, 1, 0);                           // program change 2 -> patch 2
    run(2);
    check(pdisp(patch) == "002 Saw Split", "a MIDI program change selects \"" + pdisp(patch) + "\"");
    midi(0x90, 48, 100);
    double saw = run(60);
    midi(0x80, 48, 0);
    run(60);
    check(saw > -30, "patch 2 sounds (" + std::to_string(saw) + " dB)");

    // ---- editing: the edit parameters show the patch, TONE SELECT picks the tone, edits play
    e->setParameter(e, patch, 4.0f / 255.0f);   // patch 5 "Filter Sweep": tone 1 LPF, cutoff 40, resonance 90
    run(30);
    int filter = find("Filter Type"), cutoff = find("TVF Cutoff"), reso = find("TVF Reso"), editTone = find("Edit Tone");
    int wave = find("Wave"), group = find("Wave Group"), level = find("Tone Level"), efxType = find("EFX Type");
    check(pdisp(filter) == "LPF" && pdisp(cutoff) == "40" && pdisp(reso) == "90",
          "the TVF page shows patch 5's tone 1: " + pdisp(filter) + ", cutoff " + pdisp(cutoff) + ", resonance " + pdisp(reso));
    check(pdisp(efxType) == "SPECTRUM", "the EFX page shows patch 5's EFX type: " + pdisp(efxType));
    e->setParameter(e, patch, 1.0f / 255.0f);   // patch 2 "Saw Split": tone 1 SAW TEST, tone 2 HIT TEST
    run(30);
    check(pdisp(wave) == "002 SAW TEST" && pdisp(group) == "EXP-A", "tone 1 plays wave '" + pdisp(wave) + "' of " + pdisp(group));
    e->setParameter(e, editTone, 1.0f / 3.0f);  // TONE SELECT 2
    run(30);
    check(pdisp(editTone) == "TONE 2" && pdisp(wave) == "003 HIT TEST", "TONE SELECT 2: the page shows tone 2's wave '" + pdisp(wave) + "'");
    e->setParameter(e, editTone, 0.0f);
    run(30);
    check(pdisp(patch) == "002 Saw Split", "not edited yet: \"" + pdisp(patch) + "\"");
    e->setParameter(e, level, 0.0f);            // tone 1's level to 0
    run(30);
    check(pdisp(patch) == "002 Saw Split *", "an edit marks the patch: \"" + pdisp(patch) + "\"");
    midi(0x90, 48, 100);
    double edited = run(40);
    midi(0x80, 48, 0);
    run(60);
    check(edited < -100, "the edit plays at once: tone 1 at level 0 is silent (" + std::to_string(edited) + " dB)");
    void* ec = nullptr;
    int en = (int)e->dispatcher(e, effGetChunk, 0, 0, &ec, 0);
    std::vector<char> editedState((char*)ec, (char*)ec + en);
    e->setParameter(e, patch, 4.0f / 255.0f);   // another patch: the edit is gone, as on the XP-80
    run(30);
    e->setParameter(e, patch, 1.0f / 255.0f);
    run(30);
    check(pdisp(patch) == "002 Saw Split" && pdisp(level) == "127", "picking the patch again gives it unedited (level " + pdisp(level) + ")");
    e->dispatcher(e, effSetChunk, 0, en, editedState.data(), 0);
    run(40);
    check(pdisp(patch) == "002 Saw Split *" && pdisp(level) == "0", "a project brings its edit back: \"" + pdisp(patch) + "\", level " + pdisp(level));
    e->setParameter(e, level, 1.0f);
    run(30);

    // DEC / INC: one patch at a time (the knob moves 256 values in steps of about 2)
    {
        int inc = find("Patch +"), dec = find("Patch -");
        e->setParameter(e, patch, 1.0f / 255.0f);   // patch 2
        run(30);
        e->setParameter(e, inc, 1.0f);
        run(30);
        std::string up = pdisp(patch);
        bool released = e->getParameter(e, inc) < 0.5f;
        e->setParameter(e, dec, 1.0f);
        run(30);
        e->setParameter(e, dec, 1.0f);
        run(30);
        check(up.compare(0, 3, "003") == 0 && pdisp(patch).compare(0, 3, "001") == 0 && released,
              "Patch + then Patch - twice: \"" + up + "\", then \"" + pdisp(patch) + "\"; the button lets go");
        e->setParameter(e, patch, 1.0f / 255.0f);
        run(30);
    }

    // the EFX type: patch 3 "Drive Hit" sends its tone to the EFX (OVERDRIVE); a new type is heard
    {
        int efxType = find("EFX Type"), drive = find("EFX Drive");
        e->setParameter(e, patch, 2.0f / 255.0f);
        run(30);
        auto hit = [&]() {
            static float L[128], R[128];
            float* o[2] = {L, R};
            std::vector<float> x;
            midi(0x90, 60, 127);
            for (int b = 0; b < 60; ++b)
            {
                static Ev none; none.head.numEvents = 0;
                e->dispatcher(e, effProcessEvents, 0, 0, &none, 0);
                e->processReplacing(e, nullptr, o, 128);
                x.insert(x.end(), L, L + 128);
            }
            midi(0x80, 60, 0);
            run(60);
            return x;
        };
        std::string before = pdisp(efxType);
        std::vector<float> od = hit();
        e->setParameter(e, drive, 1.0f);              // drive all the way up
        run(5);
        e->setParameter(e, efxType, 2.0f / 39.0f);    // DISTORTION
        run(5);
        std::vector<float> ds = hit();
        e->setParameter(e, efxType, 0.0f);            // STEREO-EQ: no processing here
        run(5);
        std::vector<float> eq = hit();
        auto diff = [](const std::vector<float>& a, const std::vector<float>& b) {
            double d = 0, s = 0;
            for (size_t i = 0; i < a.size(); ++i) { d += (a[i] - b[i]) * (a[i] - b[i]); s += a[i] * a[i]; }
            return 10 * std::log10(d / (s + 1e-20) + 1e-20);
        };
        double dOdDs = diff(od, ds), dDsEq = diff(ds, eq);
        check(before == "OVERDRIVE" && dOdDs > -20 && dDsEq > -20, "EFX type " + before + " -> DISTORTION -> STEREO-EQ: each change is heard (" +
              std::to_string((int)dOdDs) + " dB, " + std::to_string((int)dDsEq) + " dB of difference)");
        // patch 1 "Sine Test": its tone goes to MIX, so the EFX is not heard until the tone is sent to it
        int toEfx = find("Tone 1 > EFX");
        e->setParameter(e, patch, 0.0f);
        run(30);
        std::string routed = pdisp(toEfx);
        std::vector<float> mix = hit();
        e->setParameter(e, efxType, 2.0f / 39.0f);    // DISTORTION, drive all the way up
        e->setParameter(e, drive, 1.0f);
        run(5);
        std::vector<float> mixDs = hit();
        e->setParameter(e, toEfx, 1.0f);              // Tone 1 > EFX
        run(30);
        std::string output = pdisp(find("Output"));
        std::vector<float> efxDs = hit();
        double same = diff(mix, mixDs), changed = diff(mix, efxDs);
        check(routed == "MIX" && same < -100 && changed > -20 && output == "EFX",
              "a tone on MIX skips the EFX (" + std::to_string((int)same) + " dB); 'Tone 1 > EFX' sends it through (" +
              std::to_string((int)changed) + " dB), and the PAN/OUT page's Output shows " + output);
        e->setParameter(e, patch, 1.0f / 255.0f);
        run(30);
    }

    // EFX EDIT: the knobs take the type's own parameters, named and shown in their units
    {
        int efxType = find("EFX Type"), p2 = find("EFX 2"), n2 = find("EFX 2 Name"), n1 = find("EFX 1 Name");
        e->setParameter(e, patch, 0.0f);              // on patch 1 (picking patch 2 again later drops the edit)
        run(30);
        e->setParameter(e, efxType, 16.0f / 39.0f);   // STEREO-DELAY
        run(30);
        std::string name = pdisp(n2);
        check(pdisp(find("EFX 10")) == "D70:30E" && pdisp(find("EFX 11")) == "127",
              "picking STEREO-DELAY loads its initial settings: Balance " + pdisp(find("EFX 10")) + ", Level " + pdisp(find("EFX 11")));
        e->setParameter(e, p2, 111.0f / 127.0f);
        run(30);
        std::string value = pdisp(p2);
        e->setParameter(e, efxType, 23.0f / 39.0f);   // REVERB
        run(30);
        check(name == "Delay L" && value == "250 ms" && pdisp(n2) == "Pre Dly" && pdisp(n1) == "Type",
              "EFX EDIT: STEREO-DELAY's knob 2 is \"" + name + "\" = " + value + "; on REVERB it becomes \"" + pdisp(n2) + "\"");
        e->setParameter(e, patch, 1.0f / 255.0f);   // back to patch 2 for the state test
        run(30);
    }

    // the ADSR view (TONE key, AMP / FILTER pages): reads the patch's T / L values, writes them back
    {
        e->setParameter(e, patch, 0.0f);              // "Sine Test": A-Env T1-T4 0 0 0 20, L1-L3 127
        run(30);
        int aA = find("Amp Attack"), aD = find("Amp Decay"), aS = find("Amp Sustain"), aR = find("Amp Release");
        std::string view = pdisp(aA) + " / " + pdisp(aD) + " / " + pdisp(aS) + " / " + pdisp(aR);
        e->setParameter(e, aD, 40.0f / 127.0f);       // Decay: T2 and T3 (both 0) become 40
        e->setParameter(e, aS, 90.0f / 127.0f);       // Sustain: L3
        run(30);
        std::string t2 = pdisp(find("A-Env T2")), t3 = pdisp(find("A-Env T3")), l3 = pdisp(find("A-Env L3"));
        e->setParameter(e, find("A-Env T4"), 50.0f / 127.0f);   // and back: the PRO page's T4 moves Release
        run(30);
        check(view == "0 ms / 0 ms / 127 / 65 ms" && t2 == "40" && t3 == "40" && l3 == "90" && pdisp(aR) == "407 ms",
              "ADSR view: \"" + view + "\"; Decay 40 / Sustain 90 write T2 " + t2 + ", T3 " + t3 + ", L3 " + l3 +
              "; T4 50 shows as Release " + pdisp(aR));
        e->setParameter(e, patch, 1.0f / 255.0f);     // back to patch 2 (unedited) for the state test
        run(30);
    }

    // state: the slot comes back by file name
    void* chunk = nullptr;
    int n = (int)e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
    std::vector<char> saved((char*)chunk, (char*)chunk + n);
    e->setParameter(e, slotA, 0.0f);            // empty the slot
    run(30);
    check(pdisp(patch).find("---") != std::string::npos, "an empty slot: \"" + pdisp(patch) + "\"");
    e->dispatcher(e, effSetChunk, 0, n, saved.data(), 0);
    t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
    {
        run(10);
        p = pdisp(patch);
        if (p.find("loading") == std::string::npos && p.find("---") == std::string::npos) break;
    }
    check(pdisp(slotA) == "TEST" && p == "002 Saw Split", "the project's state brings the card back: \"" + p + "\"");

    e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    std::printf("%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
