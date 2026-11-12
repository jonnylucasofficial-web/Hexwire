// Offline tests for Hexwire: tracking accuracy (single notes + chords), latency, engine safety,
// preset levels and a UI screenshot. Run: ./HexwireTests [outdir]
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "Presets.h"
#include "PluginEditor.h"
#include <set>

static constexpr double SR = 48000.0;
static constexpr int BS = 256;

// Karplus-Strong "guitar" with a bit of inharmonicity-free brightness
struct FakeString
{
    std::vector<float> buf; int pos = 0; float last = 0; float damp;
    FakeString (float midi, float amp, juce::Random& r, float bright = 0.6f)
    {
        const float f = 440.0f * std::exp2 ((midi - 69.0f) / 12.0f);
        buf.resize ((size_t) std::max (2, (int) std::round (SR / f - 0.5)));
        // plucked-string shape: triangle with the pluck point ~1/5 along the string, plus a little noise
        const int L = (int) buf.size(); const float pk = 0.2f * (float) L;
        float lp = 0;
        for (int i = 0; i < L; ++i)
        {
            const float tri = (float) i < pk ? (float) i / pk : 1.0f - ((float) i - pk) / ((float) L - pk);
            lp += (r.nextFloat() * 2 - 1 - lp) * bright;
            buf[(size_t) i] = amp * (2.0f * (tri - 0.5f) + 0.08f * lp);
        }
        damp = 0.996f;
    }
    float next()
    {
        const float y = buf[(size_t) pos];
        const float n = 0.5f * (y + last) * damp;
        last = y; buf[(size_t) pos] = n; pos = (pos + 1) % (int) buf.size();
        return y;
    }
};

static void setParam (HexwireProcessor& p, const char* id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

// renders input through the processor; returns output; collects tracked note set at time "probeAt"
static juce::AudioBuffer<float> run (HexwireProcessor& p, const std::vector<float>& in, double probeAt,
                                     std::set<int>* probed, double* firstOn = nullptr, juce::MidiBuffer* midiAll = nullptr)
{
    const int n = (int) in.size();
    juce::AudioBuffer<float> out (2, n);
    out.clear();
    juce::AudioBuffer<float> blk (2, BS);
    juce::MidiBuffer midi;
    bool gotFirst = false;
    for (int s = 0; s + BS <= n; s += BS)
    {
        for (int i = 0; i < BS; ++i) { blk.setSample (0, i, in[(size_t) (s + i)]); blk.setSample (1, i, in[(size_t) (s + i)]); }
        p.processBlock (blk, midi);
        if (midiAll) for (const auto m : midi) midiAll->addEvent (m.getMessage(), s + m.samplePosition);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, s, blk, c, 0, BS);
        if (firstOn && ! gotFirst)
            for (int v = 0; v < HexwireProcessor::maxVoices; ++v)
                if (p.displayMidi[(size_t) v].load() > 0) { *firstOn = (s + BS) / SR; gotFirst = true; break; }
        if (probed && s <= probeAt * SR && s + BS > probeAt * SR)
            for (int v = 0; v < HexwireProcessor::maxVoices; ++v)
            {
                const float m = p.displayMidi[(size_t) v].load();
                if (m > 0) probed->insert (juce::roundToInt (m));
            }
    }
    return out;
}

static std::vector<float> chordSignal (const std::vector<int>& notes, double seconds, juce::Random& r, float amp = 0.25f, double strum = 0.012)
{
    std::vector<float> x ((size_t) (seconds * SR), 0.0f);
    std::vector<FakeString> strs;
    std::vector<int> starts;
    for (size_t i = 0; i < notes.size(); ++i) { strs.emplace_back ((float) notes[i], amp, r); starts.push_back ((int) (0.1 * SR + i * strum * SR)); }
    for (size_t k = 0; k < x.size(); ++k)
        for (size_t i = 0; i < strs.size(); ++i)
            if ((int) k >= starts[i]) x[k] += strs[i].next();
    return x;
}

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    if (auto os = std::unique_ptr<juce::OutputStream> (f.createOutputStream()))
        if (auto w = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (os.get(), SR, 2, 24, {}, 0)))
        {
            os.release();
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
}

static std::string setStr (const std::set<int>& s) { std::string o; for (int v : s) o += std::to_string (v) + " "; return o; }

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile ("out"));
    outDir.createDirectory();
    const bool presetsOnly = argc > 2 && juce::String (argv[2]) == "levels";
    juce::Random rng (1234);
    int failures = 0;

    auto fresh = [] (int mode)
    {
        auto p = std::make_unique<HexwireProcessor>();
        p->setRateAndBufferSizeDetails (SR, BS);
        p->prepareToPlay (SR, BS);
        setParam (*p, "mode", (float) mode);
        return p;
    };

    if (argc > 3 && juce::String (argv[2]) == "one")
    {
        std::vector<int> notes; for (auto t : juce::StringArray::fromTokens (argv[3], ",", "")) notes.push_back (t.getIntValue());
        auto x = chordSignal (notes, 0.5, rng);
        auto p = fresh (1); std::set<int> got; run (*p, x, 0.45, &got);
        std::printf ("got { %s}\n", setStr (got).c_str()); return 0;
    }
    const bool enginesOnly = argc > 2 && juce::String (argv[2]) == "engines";
    if (! presetsOnly)
    {
      if (! enginesOnly)
      {
        // ---------- single notes, poly + mono
        int ok[2] = { 0, 0 }, total = 0;
        double latSum[2] = { 0, 0 };
        for (int note = 40; note <= 84; note += 3)
        {
            ++total;
            auto x = chordSignal ({ note }, 0.6, rng);
            for (int mode = 0; mode < 2; ++mode)
            {
                auto p = fresh (mode);
                std::set<int> got; double on = 9;
                run (*p, x, 0.35, &got, &on);
                if (got == std::set<int> { note }) ++ok[mode];
                else std::printf ("  %s note %d -> got { %s}\n", mode ? "POLY" : "MONO", note, setStr (got).c_str());
                latSum[mode] += on - 0.1;
            }
        }
        std::printf ("single notes: mono %d/%d  poly %d/%d   avg note-on latency: mono %.1f ms, poly %.1f ms\n",
                     ok[0], total, ok[1], total, 1000 * latSum[0] / total, 1000 * latSum[1] / total);
        if (ok[1] < total - 1 || ok[0] < total - 1) ++failures;

        // ---------- chords (poly)
        const std::vector<std::pair<const char*, std::vector<int>>> chords {
            { "E5 power", { 40, 47, 52 } }, { "A5 power", { 45, 52, 57 } },
            { "C major", { 48, 52, 55, 60, 64 } }, { "A minor", { 45, 52, 57, 60, 64 } },
            { "D major", { 50, 57, 62, 66 } }, { "E major", { 40, 47, 52, 56, 59, 64 } },
            { "G major", { 43, 47, 50, 55, 59, 67 } }, { "Fmaj7 triad", { 53, 57, 60, 64 } },
            { "Dyad 3rd", { 60, 64 } }, { "Dyad 5th hi", { 67, 74 } },
        };
        int correctNotes = 0, expectedNotes = 0, ghosts = 0;
        for (auto& [name, notes] : chords)
        {
            auto x = chordSignal (notes, 0.8, rng);
            auto p = fresh (1);
            std::set<int> got;
            auto out = run (*p, x, 0.45, &got);
            std::set<int> want (notes.begin(), notes.end());
            int hit = 0; for (int n : want) hit += got.count (n) ? 1 : 0;
            int ghost = 0; for (int n : got) ghost += want.count (n) ? 0 : 1;
            correctNotes += hit; expectedNotes += (int) want.size(); ghosts += ghost;
            std::printf ("  chord %-12s want { %s} got { %s} -> %d/%d, %d extra\n", name, setStr (want).c_str(), setStr (got).c_str(), hit, (int) want.size(), ghost);
        }
        std::printf ("chords: %d/%d notes found, %d extra notes\n", correctNotes, expectedNotes, ghosts);

        // ---------- bend follow (mono + poly): glide a note up 2 semitones
        for (int mode = 0; mode < 2; ++mode)
        {
            std::vector<float> x ((size_t) (1.0 * SR));
            double ph = 0;
            for (size_t k = 0; k < x.size(); ++k)
            {
                const double t = k / SR;
                const double m = 57 + (t > 0.4 ? std::min (2.0, (t - 0.4) * 8.0) : 0.0);
                const double f = 440 * std::exp2 ((m - 69) / 12);
                ph += f / SR;
                const double env = t < 0.1 ? 0 : 0.3;
                x[k] = (float) (env * (std::sin (6.2831853 * ph) + 0.5 * std::sin (2 * 6.2831853 * ph) + 0.25 * std::sin (3 * 6.2831853 * ph)));
            }
            auto p = fresh (mode);
            run (*p, x, 0, nullptr);
            float m = -1; for (int v = 0; v < HexwireProcessor::maxVoices; ++v) m = std::max (m, p->displayMidi[(size_t) v].load());
            std::printf ("bend %s: ended at %.2f (want 59.00)\n", mode ? "POLY" : "MONO", m);
            if (std::abs (m - 59.0f) > 0.3f) ++failures;
        }

      }
        // ---------- every engine on a chord, extreme settings: NaN / blow-up check
        auto chord = chordSignal ({ 40, 47, 52, 56, 59, 64 }, 1.5, rng, 0.45f);
        for (int e = 0; e < 12; ++e)
            for (int extreme = 0; extreme < 2; ++extreme)
            {
                auto p = fresh (1);
                setParam (*p, "engA", (float) e); setParam (*p, "engB", (float) ((e + 5) % 12));
                setParam (*p, "blend", 0.5f); setParam (*p, "mix", 1.0f);
                if (extreme)
                {
                    setParam (*p, "tone", 1); setParam (*p, "vibrato", 1); setParam (*p, "drive", 1);
                    setParam (*p, "octA", 2); setParam (*p, "octB", -2); setParam (*p, "chorus", 1); setParam (*p, "space", 1);
                    setParam (*p, "attack", 1); setParam (*p, "release", 6000); setParam (*p, "output", 12);
                }
                auto out = run (*p, chord, 0, nullptr);
                float peak = 0; bool bad = false;
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < out.getNumSamples(); ++i)
                    {
                        const float v = out.getSample (c, i);
                        if (! std::isfinite (v)) bad = true;
                        peak = std::max (peak, std::abs (v));
                    }
                if (bad || peak > 16.0f || (extreme == 0 && peak < 0.01f))
                {
                    std::printf ("  ENGINE %s extreme=%d: peak %.3f %s\n", HexwireProcessor::engineNames[e].toRawUTF8(), extreme, peak, bad ? "NaN!" : "");
                    ++failures;
                }
                if (extreme == 0) writeWav (outDir.getChildFile ("engine_" + HexwireProcessor::engineNames[e] + ".wav"), out);
            }
        std::printf ("engines checked\n");

        // ---------- MIDI out
        {
            auto p = fresh (1);
            setParam (*p, "midiout", 2);
            juce::MidiBuffer all;
            run (*p, chordSignal ({ 48, 52, 55 }, 0.8, rng), 0, nullptr, nullptr, &all);
            int ons = 0, offs = 0;
            for (const auto m : all) { ons += m.getMessage().isNoteOn() ? 1 : 0; offs += m.getMessage().isNoteOff() ? 1 : 0; }
            std::printf ("MPE out: %d note-ons, %d note-offs\n", ons, offs);
            if (ons < 3) ++failures;
        }

        // ---------- UI screenshot
        {
            auto p = fresh (1);
            run (*p, chordSignal ({ 40, 47, 52, 56, 59, 64 }, 0.5, rng), 0, nullptr);
            std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
            for (int i = 0; i < 10; ++i) static_cast<HexwireEditor*> (ed.get())->timerCallback();
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile ("ui.png"); f.deleteFile();
            if (auto os = f.createOutputStream()) png.writeImageToStream (img, *os);
            ed.reset();
            std::printf ("wrote ui.png\n");
        }
    }

    // ---------- preset levels: wet vs dry RMS on a test phrase
    {
        std::vector<float> phrase ((size_t) (4.0 * SR), 0.0f);
        auto add = [&] (std::vector<int> notes, double at, double len)
        {
            auto s = chordSignal (notes, len, rng, 0.2f, 0.01);
            for (size_t k = 0; k < s.size() && (size_t) (at * SR) + k < phrase.size(); ++k) phrase[(size_t) (at * SR) + k] += s[k];
        };
        add ({ 52 }, 0.0, 0.5); add ({ 55 }, 0.5, 0.5); add ({ 57 }, 1.0, 0.6); add ({ 45 }, 1.6, 0.5);
        add ({ 45, 52, 57, 60, 64 }, 2.1, 1.6);
        double inE = 0; for (float v : phrase) inE += v * v;
        const double inRms = std::sqrt (inE / phrase.size());
        for (int e = 0; e < 12; ++e)
        {
            auto p = fresh (1);
            setParam (*p, "engA", (float) e); setParam (*p, "blend", 0); setParam (*p, "mix", 1);
            setParam (*p, "space", 0); setParam (*p, "chorus", 0);
            auto out = run (*p, phrase, 0, nullptr);
            double e2 = 0;
            for (int s = 0; s < out.getNumSamples(); ++s) e2 += out.getSample (0, s) * out.getSample (0, s);
            std::printf ("ENGINE_LEVEL %s %.2f\n", HexwireProcessor::engineNames[e].toRawUTF8(), 20 * std::log10 (std::sqrt (e2 / out.getNumSamples()) / inRms + 1e-9));
        }
        std::printf ("PRESET LEVELS (dB, synth-only output vs dry input)\n");
        for (int i = 0; i < numHexwirePresets; ++i)
        {
            auto p = fresh (1);
            p->setCurrentProgram (i);
            setParam (*p, "mix", 1.0f);
            auto out = run (*p, phrase, 0, nullptr);
            double e = 0;
            for (int s = 0; s < out.getNumSamples(); ++s) e += 0.5 * (out.getSample (0, s) * out.getSample (0, s) + out.getSample (1, s) * out.getSample (1, s));
            const double db = 20 * std::log10 (std::sqrt (e / out.getNumSamples()) / inRms + 1e-9);
            std::printf ("LEVEL %d %.2f %s\n", i, db, hexwirePresets[i].name);
        }
    }

    std::printf (failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
