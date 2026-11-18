// inspect.cpp - what XP-08 makes of real SR-JV80 dumps: header, tables, and every patch played
// (C4, velocity 100, half a second): how many sound, how many tones point at waves the board
// does not have. JV-1080 program and wave ROM dumps among the files give the internal waves.
//   inspect [-v] <dump>...
#include "srjv.h"
#include "jvparams.h"
#include "engine.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
// a sample's fundamental (autocorrelation, parabolic refinement); 0 if it is not clearly pitched
static double f0(const int16_t* x, size_t n, double sr)
{
    std::vector<double> r(1601, 0);
    for (int d = 16; d < 1600 && (size_t)d * 2 < n; ++d)
    {
        double a = 0, e0 = 0, e1 = 0;
        size_t m = std::min<size_t>(n - (size_t)d, 6000);
        for (size_t i = 0; i < m; ++i) { a += (double)x[i] * x[i + (size_t)d]; e0 += (double)x[i] * x[i]; e1 += (double)x[i + (size_t)d] * x[i + (size_t)d]; }
        r[(size_t)d] = a / std::sqrt(e0 * e1 + 1e-9);
    }
    double best = -1;
    int lag = 0;
    for (int d = 17; d < 1599; ++d) if (r[d] > r[d - 1] && r[d] >= r[d + 1] && r[d] > best) { best = r[d]; lag = d; }
    for (int d = 17; d < lag; ++d) if (r[d] > r[d - 1] && r[d] >= r[d + 1] && r[d] > best * 0.95) { lag = d; break; }
    if (!lag || best < 0.9) return 0;
    double y0 = r[lag - 1], y1 = r[lag], y2 = r[lag + 1];
    return sr / (lag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
}

// how far each pitched sample is from its root key less its fine tune (the correction the engine applies), in cents
static void tuning(const srjv::Card& c)
{
    std::vector<double> dev;
    for (const srjv::Sample& s : c.samples)
    {
        size_t n = s.end - s.start, skip = std::min<size_t>(n / 4, 4000);
        if (n < 4000) continue;
        double f = f0(&c.pcm[s.start + skip], n - skip, c.sampleRate);
        if (f <= 0) continue;
        double cents = 1200 * std::log2(f / (440.0 * std::pow(2.0, (s.rootKey - 69 - s.fineSemis) / 12.0)));
        while (cents > 600) cents -= 1200;   // octave errors of the detector
        while (cents < -600) cents += 1200;
        dev.push_back(cents);
    }
    std::sort(dev.begin(), dev.end());
    if (!dev.empty())
        std::printf("  tuning: %zu of %zu samples pitched, median %+.1f cents from their root keys (quartiles %+.1f / %+.1f)\n",
                    dev.size(), c.samples.size(), dev[dev.size() / 2], dev[dev.size() / 4], dev[3 * dev.size() / 4]);
}

int main(int argc, char** argv)
{
    bool verbose = false;
    std::vector<bool> part((size_t)argc, false);
    std::string program, waveB, program80;
    std::vector<std::string> waveA, wave80;
    for (int i = 1; i < argc; ++i)
        switch (srjv::internalPart(argv[i]))
        {
            case srjv::InternalPart::Program: program = argv[i]; part[(size_t)i] = true; break;
            case srjv::InternalPart::WaveA: waveA.push_back(argv[i]); part[(size_t)i] = true; break;
            case srjv::InternalPart::WaveB: waveB = argv[i]; part[(size_t)i] = true; break;
            case srjv::InternalPart::Jv880Program: program80 = argv[i]; part[(size_t)i] = true; break;
            case srjv::InternalPart::Jv880Wave: wave80.push_back(argv[i]); part[(size_t)i] = true; break;
            case srjv::InternalPart::None: break;
        }
    std::unique_ptr<srjv::Card> in;
    if (!program.empty())
    {
        in = srjv::loadInternal(program, waveA, waveB);
        std::printf("JV-1080 internal waves: %s | %s | %zu samples, %zu waves (INT-B from %d)\n",
                    in->ok() ? "ok" : in->error.c_str(), in->date.c_str(), in->samples.size(), in->waves.size(), in->intB);
        if (in->ok()) tuning(*in); else in.reset();
    }
    std::unique_ptr<srjv::Card> in80;
    if (!program80.empty())
    {
        in80 = srjv::loadJv880(program80, wave80);
        std::printf("JV-880 (JV-80) internal waves: %s | %s | %zu samples, %zu waves\n",
                    in80->ok() ? "ok" : in80->error.c_str(), in80->date.c_str(), in80->samples.size(), in80->waves.size());
        if (in80->ok()) tuning(*in80); else in80.reset();
    }
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "-v")) { verbose = true; continue; }
        if (part[(size_t)i]) continue;
        auto c = srjv::load(argv[i]);
        std::printf("%s\n  %s | id \"%s\" %s %u Hz | %zu samples, %zu waves, %zu patches (%s), group id %d\n", argv[i],
                    c->ok() ? "ok" : c->error.c_str(), c->id.c_str(), c->date.c_str(), c->sampleRate, c->samples.size(),
                    c->waves.size(), c->patches.size(), c->jv80 ? "JV-80 table" : "JV-1080 table", c->groupId);
        if (!c->ok() || c->patches.empty()) continue;
        static xp::Engine e;
        e.prepare(44100);
        const srjv::Card* cards[4] = {c.get(), nullptr, nullptr, nullptr};
        e.setCards(cards);
        e.setInternal(in.get());
        e.setInternal80(in80.get());
        int silent = 0, internal = 0, loud = 0;
        std::vector<float> L(22050), R(22050);
        for (size_t p = 0; p < c->patches.size(); ++p)
        {
            e.allSoundOff();
            e.setPatch(0, (int)p);
            for (int t = 0; t < 4; ++t)
            {
                const uint8_t* tp = c->patches[p].tone[t];
                bool intOk = (in && tp[jv::T_WAVE_GROUP] == 0 && (tp[jv::T_WAVE_GROUP_ID] == 1 || tp[jv::T_WAVE_GROUP_ID] == 2)) ||
                             (in80 && tp[jv::T_WAVE_GROUP] == 0 && tp[jv::T_WAVE_GROUP_ID] == 0);
                if (tp[jv::T_SWITCH] && tp[jv::T_WAVE_GROUP] != 2 && !intOk) ++internal;
            }
            e.noteOn(60, 100);
            double s = 0, pk = 0;
            for (int k = 0; k < 22050; k += 128)
            {
                int n = std::min(128, 22050 - k);
                e.render(&L[(size_t)k], &R[(size_t)k], n);
                for (int j = k; j < k + n; ++j) { s += L[(size_t)j] * L[(size_t)j]; pk = std::fmax(pk, std::fabs(L[(size_t)j])); }
            }
            e.noteOff(60);
            double db = 10 * std::log10(s / 22050 + 1e-20);
            if (db < -60) ++silent;
            if (pk > 1.0) ++loud;
            if (verbose || (db < -60 && silent <= 5))
                std::printf("    %3zu %-12s %6.1f dB peak %.2f%s\n", p + 1, c->patches[p].name().c_str(), db, pk, db < -60 ? "  (silent)" : "");
        }
        std::printf("  played all: %zu sound, %d silent, %d clip; %d tones on waves not available\n",
                    c->patches.size() - (size_t)silent, silent, loud, internal);
    }
}
