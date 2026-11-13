#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"
#include <cmath>

namespace
{
   #ifdef HEXWIRE_DEBUG
    float envOr (const char* n, float d) { auto* v = std::getenv (n); return v ? (float) std::atof (v) : d; }
    float tunSmooth = envOr ("T_SM", 0.8f), tunFloor = envOr ("T_FL", 0.3f), tunRel = envOr ("T_REL", 0.15f);
    float tunFloorDb = envOr ("T_FLOORDB", -14.0f), tunMaxNotes = envOr ("T_MAXN", 4.0f), tunRingDb = envOr ("T_RINGDB", -15.0f), tunRelHold = envOr ("T_RELHOLD", 0.15f), tunFast = envOr ("T_FAST", 0.25f);
   #else
    constexpr float tunSmooth = 0.8f, tunFloor = 0.3f, tunRel = 0.15f;
    constexpr float tunFloorDb = -14.0f, tunMaxNotes = 4.0f, tunRingDb = -15.0f, tunRelHold = 0.15f, tunFast = 0.25f;
   #endif
    constexpr float twoPi = 6.28318530718f;

    enum Engine { Cello, Marimba, Bells, Pluck, Flute, Brass, Choir, Organ, Pad, Chip, Kalimba, Sub, Analog, Soft, NumEngines };
    // measured so every engine plays back at about the same loudness (tests/Test.cpp ENGINE_LEVEL)
    constexpr float engineGain[NumEngines] = { 0.738f, 1.569f, 1.405f, 3.371f, 0.592f, 0.663f, 0.817f, 0.888f, 0.944f, 0.700f, 1.271f, 0.419f, 0.626f, 0.722f };
    constexpr bool isPerc[NumEngines] = { false, true, true, true, false, false, false, false, false, false, true, false, false, false };

    inline float midiToHz (float m) { return 440.0f * std::exp2 ((m - 69.0f) / 12.0f); }
    inline float hzToMidi (float f) { return 69.0f + 12.0f * std::log2 (f / 440.0f); }

    inline float polyBlep (double t, double dt)
    {
        if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }
    inline float sawAt (double ph, double inc) { return (float) (2.0 * ph - 1.0) - polyBlep (ph, inc); }
    inline void advance (double& ph, double inc) { ph += inc; if (ph >= 1.0) ph -= std::floor (ph); }

    struct SvfOut { float lp, bp; };
    inline SvfOut svf (float x, float fc, float q, float sr, float& ic1, float& ic2)
    {
        fc = juce::jlimit (10.0f, sr * 0.45f, fc);
        const float g = std::tan (juce::MathConstants<float>::pi * fc / sr);
        const float k = 1.0f / q;
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1 };
    }
}

const juce::StringArray HexwireProcessor::engineNames { "Cello", "Marimba", "Bells", "Pluck", "Flute", "Brass",
                                                        "Choir", "Organ", "Pad", "Chip", "Kalimba", "Sub", "Analog", "Soft" };

HexwireProcessor::HexwireProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    for (auto& d : displayMidi) d = -1.0f;
    for (auto& d : displayLevel) d = 0.0f;
    setCurrentProgram (0);
}

juce::AudioProcessorValueTreeState::ParameterLayout HexwireProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto pct = [] (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto ms  = [] (float v, int) { return v < 1000.0f ? String (juce::roundToInt (v)) + " ms" : String (v / 1000.0f, 2) + " s"; };
    auto db  = [] (float v, int) { return String (v, 1) + " dB"; };
    auto oct = [] (float v, int) { const int o = juce::roundToInt (v); return o == 0 ? String ("0 oct") : (o > 0 ? "+" : "") + String (o) + " oct"; };

    auto knob = [&] (const char* id, const char* name, NormalisableRange<float> r, float def,
                     std::function<String (float, int)> fmt)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                       AudioParameterFloatAttributes().withStringFromValueFunction (fmt)));
    };

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "Mono", "Poly" }, 1));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "engA", 1 }, "Engine A", engineNames, Pad));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "engB", 1 }, "Engine B", engineNames, Pluck));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "midiout", 1 }, "MIDI Out", StringArray { "Off", "MIDI", "MPE" }, 0));

    knob ("octA",    "Oct A",    { -2.0f, 2.0f, 1.0f }, 0.0f, oct);
    knob ("octB",    "Oct B",    { -2.0f, 2.0f, 1.0f }, 0.0f, oct);
    knob ("detune",  "Detune B", { 0.0f, 25.0f, 0.1f }, 5.0f, [] (float v, int) { return String (v, 1) + " ct"; });
    knob ("blend",   "Blend",    { 0.0f, 1.0f }, 0.0f, [] (float v, int) {
              const int b = juce::roundToInt (v * 100.0f);
              return b == 0 ? String ("A") : b == 100 ? String ("B") : String (100 - b) + "/" + String (b); });
    knob ("tone",    "Tone",     { 0.0f, 1.0f }, 0.5f, pct);
    knob ("vibrato", "Vibrato",  { 0.0f, 1.0f }, 0.0f, pct);
    knob ("drive",   "Drive",    { 0.0f, 1.0f }, 0.0f, pct);
    knob ("glide",   "Glide",    { 0.0f, 500.0f, 0.0f, 0.4f }, 0.0f, ms);
    knob ("attack",  "Attack",   { 1.0f, 3000.0f, 0.0f, 0.3f }, 15.0f, ms);
    knob ("release", "Release",  { 10.0f, 6000.0f, 0.0f, 0.3f }, 250.0f, ms);
    knob ("dynamics","Dynamics", { 0.0f, 1.0f }, 0.3f, pct);
    knob ("thresh",  "Thresh",   { -72.0f, -24.0f, 0.1f }, -54.0f, db);
    knob ("chorus",  "Chorus",   { 0.0f, 1.0f }, 0.1f, pct);
    knob ("space",   "Space",    { 0.0f, 1.0f }, 0.25f, pct);
    knob ("mix",     "Mix",      { 0.0f, 1.0f }, 0.9f, [] (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "% synth"; });
    knob ("output",  "Output",   { -24.0f, 12.0f, 0.1f }, 0.0f, db);

    return { p.begin(), p.end() };
}

bool HexwireProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in  = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    return in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void HexwireProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    decim = juce::jmax (1, (int) std::round (sr / 11025.0));
    anaRate = sr / decim;
    decimCount = 0;

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) samplesPerBlock, 1 };
    const float aaFc = (float) (anaRate * 0.42);
    *aa1.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, aaFc, 0.5412f);
    *aa2.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, aaFc, 1.3066f);
    *hp.coefficients  = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 55.0f, 0.707f);
    aa1.prepare (spec); aa2.prepare (spec); hp.prepare (spec);
    aa1.reset(); aa2.reset(); hp.reset();

    ring.fill (0.0f);
    ringPos = 0; hopCount = 0;
    polyHop = juce::jmax (32, (int) std::round (anaRate * 0.0105));   // ~10.5 ms
    monoHop = juce::jmax (16, (int) std::round (anaRate * 0.004));    // ~4 ms

    // asymmetric window: long rise, short fall, so the newest audio counts (less latency)
    window.resize (frameLen);
    const int rise = (int) (frameLen * 0.78f), fall = frameLen - rise;
    double sum = 0;
    for (int i = 0; i < frameLen; ++i)
    {
        float w = i < rise ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) rise)
                           : 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - rise) / (float) fall);
        window[(size_t) i] = w;
        sum += w;
    }
    windowNorm = (float) (sum * 0.5);
    fftBuf.assign (fftSize * 2, 0.0f);
    mag.assign (fftSize / 2 + 1, 0.0f);
    resid = mag;
    updateCandidateTable();
    dets.reserve (16);
    numPending = 0;
    lastFrameRms = 0;

    yinBuf.assign (2048, 0.0f);
    yinD.assign (1024, 0.0f);
    monoMidi = monoPending = -1; monoPendingCount = monoMiss = monoHits = 0;

    const int ksLen = (int) (sr / 20.0) + 8;
    for (auto& v : voices)
    {
        v = Voice();
        v.a.ks.assign ((size_t) ksLen, 0.0f);
        v.b.ks.assign ((size_t) ksLen, 0.0f);
    }

    const int chLen = (int) (sr * 0.05) + 4;
    for (auto& c : chorusBuf) c.assign ((size_t) chLen, 0.0f);
    chorusW = 0; chorusPh = 0; driveLp = 0;

    reverb.setSampleRate (sr);
    reverb.reset();
    wet.setSize (2, samplesPerBlock);

    for (auto* s : { &mixS, &outS, &driveS, &spaceS, &chorusS, &blendS })
        s->reset (sr, 0.03);
    mixS.setCurrentAndTargetValue (apvts.getRawParameterValue ("mix")->load());
    outS.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load()));
    driveS.setCurrentAndTargetValue (apvts.getRawParameterValue ("drive")->load());
    spaceS.setCurrentAndTargetValue (apvts.getRawParameterValue ("space")->load());
    chorusS.setCurrentAndTargetValue (apvts.getRawParameterValue ("chorus")->load());
    blendS.setCurrentAndTargetValue (apvts.getRawParameterValue ("blend")->load());
    inEnv = 0;
    lastMode = -1;
}

void HexwireProcessor::updateCandidateTable()
{
    cands.clear(); harms.clear();
    const float binHz = (float) (anaRate / fftSize);
    const float maxF = (float) (anaRate * 0.45);
    for (float m = 38.0f; m <= 90.0f; m += 0.25f)
    {
        Candidate c;
        c.midi = m;
        c.f0 = midiToHz (m);
        c.firstHarm = (int) harms.size();
        c.numHarm = juce::jmin (12, (int) (maxF / c.f0));
        for (int h = 1; h <= c.numHarm; ++h)
        {
            const float fh = (float) h * c.f0 * (1.0f + 0.00008f * (float) (h * h));
            const float tol = fh * (0.012f + 0.0012f * (float) h);
            int lo = (int) std::floor ((fh - tol) / binHz), hi = (int) std::ceil ((fh + tol) / binHz);
            lo = juce::jlimit (1, fftSize / 2 - 1, lo);
            hi = juce::jlimit (lo + 1, fftSize / 2, hi);
            harms.push_back ({ lo, hi, (c.f0 + 27.0f) / ((float) h * c.f0 + 320.0f) });
        }
        cands.push_back (c);
    }
    salience.assign (cands.size(), 0.0f);
}

// ---------------------------------------------------------------- tracking

void HexwireProcessor::pushAnalysisSample (float x, int sampleIndex)
{
    x = hp.processSample (x);
    x = aa2.processSample (aa1.processSample (x));
    if (++decimCount < decim) return;
    decimCount = 0;
    ring[(size_t) ringPos] = x;
    ringPos = (ringPos + 1) & (ringSize - 1);

    const bool poly = apvts.getRawParameterValue ("mode")->load() > 0.5f;
    if (++hopCount >= (poly ? polyHop : monoHop))
    {
        hopCount = 0;
        if (poly) analysePoly (sampleIndex);
        else      analyseMono (sampleIndex);
    }
}

void HexwireProcessor::analysePoly (int sampleIndex)
{
    const float threshLin = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("thresh")->load());

    // windowed frame of the newest audio
    for (int i = 0; i < frameLen; ++i)
        fftBuf[(size_t) i] = ring[(size_t) ((ringPos - frameLen + i) & (ringSize - 1))] * window[(size_t) i];
    std::fill (fftBuf.begin() + frameLen, fftBuf.end(), 0.0f);

    // onset detector: energy of the newest hop vs the one before
    float e = 0;
    for (int i = 0; i < polyHop; ++i) { const float s = ring[(size_t) ((ringPos - 1 - i) & (ringSize - 1))]; e += s * s; }
    const float rms = std::sqrt (e / (float) polyHop);
    const bool onset = rms > lastFrameRms * 1.6f && rms > threshLin * 0.7f;
    lastFrameRms = rms;

    fft.performFrequencyOnlyForwardTransform (fftBuf.data(), true);
    const int nb = fftSize / 2 + 1;
    for (int k = 0; k < nb; ++k) resid[(size_t) k] = mag[(size_t) k] = fftBuf[(size_t) k] / windowNorm;

    const float binHz = (float) (anaRate / fftSize);
    auto peakIn = [&] (const HarmRange& r, int& at)
    {
        float m = 0; at = r.lo;
        for (int k = r.lo; k <= r.hi; ++k) if (resid[(size_t) k] > m) { m = resid[(size_t) k]; at = k; }
        return m;
    };

    dets.clear();
    float firstSal = 0;
   #ifdef HEXWIRE_DEBUG
    if (std::getenv ("HWDBG")) { std::printf ("frame rms %.4f onset %d anaRate %.0f\n", rms, (int) onset, anaRate);
        for (int k = 2; k < 600; ++k) if (mag[(size_t) k] > mag[(size_t) k-1] && mag[(size_t) k] >= mag[(size_t) k+1] && mag[(size_t) k] > 0.01f) std::printf ("    peak %.1f Hz %.4f\n", k * anaRate / fftSize, mag[(size_t) k]); }
   #endif
    for (int iter = 0; iter < (int) tunMaxNotes; ++iter)
    {
        for (size_t c = 0; c < cands.size(); ++c)
        {
            const auto& cd = cands[c];
            float s = 0; int at;
            for (int h = 0; h < cd.numHarm; ++h)
            {
                const auto& r = harms[(size_t) (cd.firstHarm + h)];
                s += r.w * peakIn (r, at);
            }
            salience[c] = s;
        }

        int best = -1;
        for (int tries = 0; tries < 12; ++tries)
        {
            best = -1; float bs = 0;
            for (size_t c = 0; c < cands.size(); ++c)
                if (salience[c] > bs) { bs = salience[c]; best = (int) c; }
            if (best < 0) break;

            const auto& cd = cands[(size_t) best];
            int at;
            const float p1 = peakIn (harms[(size_t) cd.firstHarm], at);
            float p23 = 0;
            for (int h = 1; h < juce::jmin (3, cd.numHarm); ++h) p23 = juce::jmax (p23, peakIn (harms[(size_t) (cd.firstHarm + h)], at));
            bool ok = p1 >= 0.1f * p23 && juce::jmax (p1, p23) > threshLin;
            for (auto& d : dets) if (std::abs (d.midi - cd.midi) < 0.75f) ok = false;
            if (ok) break;
            salience[(size_t) best] = 0;
            best = -1;
        }
        if (best < 0) break;

        const auto& cd = cands[(size_t) best];
        if (iter == 0) firstSal = salience[(size_t) best];
        else if (salience[(size_t) best] < tunRel * firstSal) break;

        // refine the pitch from the fundamental (or 2nd harmonic when the fundamental is weak)
        int k1, k2;
        const float p1 = peakIn (harms[(size_t) cd.firstHarm], k1);
        const float p2 = cd.numHarm > 1 ? peakIn (harms[(size_t) cd.firstHarm + 1], k2) : 0.0f;
        const bool useSecond = cd.numHarm > 1 && p1 < 0.3f * p2;
        const int kk = juce::jlimit (1, nb - 2, useSecond ? k2 : k1);
        const float a = std::log (resid[(size_t) kk - 1] + 1e-9f), b = std::log (resid[(size_t) kk] + 1e-9f), c = std::log (resid[(size_t) kk + 1] + 1e-9f);
        const float den = a - 2.0f * b + c;
        const float delta = std::abs (den) > 1e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den) : 0.0f;
        float f = ((float) kk + delta) * binHz / (useSecond ? 2.0f : 1.0f);
        float midi = hzToMidi (juce::jmax (20.0f, f));
        if (std::abs (midi - cd.midi) > 0.6f) midi = cd.midi;

        // amplitude estimate + cancel this note's partials (spectral smoothness keeps shared partials)
        std::array<float, 13> p {}; std::array<int, 13> pk {};
        for (int h = 0; h < cd.numHarm; ++h) p[(size_t) h] = peakIn (harms[(size_t) (cd.firstHarm + h)], pk[(size_t) h]);
        float amp2 = 0;
        for (int h = 0; h < juce::jmin (4, cd.numHarm); ++h) amp2 += p[(size_t) h] * p[(size_t) h];
        dets.push_back ({ midi, std::sqrt (amp2) });
       #ifdef HEXWIRE_DEBUG
        if (std::getenv ("HWDBG")) std::printf ("   it%d cand %.2f sal %.4f (first %.4f) p1 %.4f p2 %.4f p3 %.4f -> %.2f amp %.4f\n", iter, cd.midi, salience[(size_t) best], firstSal, p[0], p[1], p[2], midi, std::sqrt (amp2));
       #endif

        for (int h = 0; h < cd.numHarm; ++h)
        {
            const float ph = p[(size_t) h];
            if (ph <= 0) continue;
            float model = ph;
            if (h > 0)
            {
                const float nbAvg = 0.5f * (p[(size_t) h - 1] + (h + 1 < cd.numHarm ? p[(size_t) h + 1] : p[(size_t) h - 1]));
                model = juce::jmin (ph, juce::jmax (tunSmooth * nbAvg, tunFloor * ph));
            }
            const float keep = 1.0f - model / ph;
            for (int k = juce::jmax (0, pk[(size_t) h] - 10); k <= juce::jmin (nb - 1, pk[(size_t) h] + 10); ++k)
                resid[(size_t) k] *= keep;
        }
    }

    // --- note tracking: hold notes steadily while the string rings, change cleanly on a new strum
    float loudest = 0;
    for (auto& d : dets) loudest = juce::jmax (loudest, d.amp);

    // how strongly is this note still present in the raw spectrum? (fundamental or 2nd harmonic)
    auto presence = [&] (float midi)
    {
        const int ci = juce::jlimit (0, (int) cands.size() - 1, (int) std::round ((midi - 38.0f) * 4.0f));
        const auto& cd = cands[(size_t) ci];
        float m = 0;
        for (int h = 0; h < juce::jmin (2, cd.numHarm); ++h)
        {
            const auto& r = harms[(size_t) (cd.firstHarm + h)];
            for (int k = r.lo; k <= r.hi; ++k) m = juce::jmax (m, mag[(size_t) k]);
        }
        return m;
    };

    if (onset)
        for (auto& v : voices) if (v.gate) v.reconfirm = 4;   // a new strum: every held note must show up again

    for (auto& v : voices) v.matched = false;
    std::array<bool, 16> used {};
    for (int pass = 0; pass < 2; ++pass)
    {
        const float maxDist = pass == 0 ? 0.7f : 1.6f;
        for (size_t di = 0; di < dets.size(); ++di)
        {
            if (used[di]) continue;
            int bestV = -1; float bd = maxDist;
            for (int vi = 0; vi < maxVoices; ++vi)
            {
                auto& v = voices[(size_t) vi];
                if (! v.gate || v.matched || (pass == 1 && v.misses > 0)) continue;
                const float d = std::abs (v.targetMidi - dets[di].midi);
                if (d < bd) { bd = d; bestV = vi; }
            }
            if (bestV < 0) continue;
            auto& v = voices[(size_t) bestV];
            v.matched = true; used[di] = true;
            // smooth the pitch a little (median of the last three readings) so held notes don't warble
            v.hist[v.histN++ % 3] = dets[di].midi;
            float h0 = v.hist[0], h1 = v.hist[1], h2 = v.hist[2];
            const float med = v.histN >= 3 ? juce::jmax (juce::jmin (h0, h1), juce::jmin (juce::jmax (h0, h1), h2)) : dets[di].midi;
            v.targetMidi = std::abs (dets[di].midi - v.targetMidi) > 0.45f ? dets[di].midi : med;   // real bends still get through
            const bool repick = v.reconfirm > 0 && onset && dets[di].amp > v.ampTarget * 1.5f;
            v.ampTarget = dets[di].amp;
            v.peakAmp = juce::jmax (v.peakAmp * 0.999f, dets[di].amp);
            v.misses = 0;
            v.reconfirm = 0;
            if (repick) { v.strike = true; v.vel = juce::jlimit (0.15f, 1.0f, std::pow (dets[di].amp / 0.2f, 0.6f)); }
        }
    }

    for (int vi = 0; vi < maxVoices; ++vi)
    {
        auto& v = voices[(size_t) vi];
        if (! v.gate || v.matched) continue;
        const float pr = presence (v.targetMidi);
        bool maskedByNeighbour = false;   // leakage from a much louder note a semitone or two away
        for (auto& o : voices)
            if (&o != &v && o.gate && o.matched && std::abs (o.targetMidi - v.targetMidi) < 2.5f && o.ampTarget > 2.0f * pr)
                maskedByNeighbour = true;
        const bool ringing = ! maskedByNeighbour && pr > juce::jmax (threshLin * 0.7f, juce::jmax (v.peakAmp * juce::Decibels::decibelsToGain (tunRingDb), loudest * tunRelHold));
        if (v.reconfirm > 0)
        {
            // after a strum, a note that is still clearly there counts as re-played
            if (pr > 0.3f * loudest && ringing) { v.reconfirm = 0; v.misses = 0; v.ampTarget = pr; continue; }
            if (--v.reconfirm == 0) noteOff (vi, sampleIndex);
            continue;
        }
        if (ringing) { v.misses = 0; v.ampTarget = pr; continue; }   // string still sounding: keep holding
        v.ampTarget *= 0.7f;
        if (++v.misses >= 3) noteOff (vi, sampleIndex);
    }

    // --- new notes: two frames in a row (one on a clear pick), loud enough, and not a harmonic ghost
    float loudestHeld = 0;
    for (auto& v : voices) if (v.gate) loudestHeld = juce::jmax (loudestHeld, v.ampTarget);
    const float floorAmp = juce::jmax (loudest, loudestHeld) * juce::Decibels::decibelsToGain (tunFloorDb);
    auto isGhost = [&] (const Detection& d)
    {
        static const float ivals[] = { 12.0f, 19.0f, 24.0f, 28.0f, 31.0f };
        auto check = [&] (float m, float a)
        {
            for (float iv : ivals) if (std::abs (d.midi - m - iv) < 0.5f && d.amp < 0.6f * a) return true;
            // a weak note right next to a strong one is usually leakage from the strong note's attack
            if (std::abs (d.midi - m) > 0.5f && std::abs (d.midi - m) < 2.5f && d.amp < 0.5f * a) return true;
            return false;
        };
        for (auto& o : dets) if (&o != &d && check (o.midi, o.amp)) return true;
        for (auto& v : voices) if (v.gate && check (v.targetMidi, v.ampTarget)) return true;
        return false;
    };

    float newPending[8]; int newHits[8]; int nNew = 0;
    for (size_t di = 0; di < dets.size(); ++di)
    {
        if (used[di]) continue;
        const auto& d = dets[di];
        if (d.amp < floorAmp || isGhost (d)) continue;
        int hits = 1;
        for (int pi = 0; pi < numPending; ++pi)
            if (std::abs (pendingMidi[pi] - d.midi) < 0.6f) hits = pendingHits[pi] + 1;
        if (hits >= 2 || (prevOnset && ! onset && d.amp > threshLin * 3.0f && d.amp > tunFast * loudest))
        {
            int gatedCount = 0;
            for (auto& vv : voices) gatedCount += vv.gate ? 1 : 0;
            int vi = findFreeVoice();
            if (gatedCount >= (int) tunMaxNotes)
            {
                // full: replace the quietest held note only if this one is clearly louder
                float q = 1e9f; vi = -1;
                for (int k = 0; k < maxVoices; ++k) if (voices[(size_t) k].gate && voices[(size_t) k].ampTarget < q) { q = voices[(size_t) k].ampTarget; vi = k; }
                if (vi < 0 || d.amp < q * 1.5f) continue;
            }
            auto& v = voices[(size_t) vi];
            if (v.gate && d.amp < v.ampTarget * 1.5f) continue;   // don't steal a held note for a weaker one
            noteOn (vi, d.midi, d.amp, sampleIndex);
            v.peakAmp = d.amp; v.histN = 0; v.reconfirm = 0;
        }
        else if (nNew < 8) { newPending[nNew] = d.midi; newHits[nNew] = hits; ++nNew; }
    }
    numPending = nNew;
    for (int i = 0; i < nNew; ++i) { pendingMidi[i] = newPending[i]; pendingHits[i] = newHits[i]; }
    prevOnset = onset;
}

void HexwireProcessor::analyseMono (int sampleIndex)
{
    const float threshLin = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("thresh")->load());
    const int W = juce::jlimit (64, 512, (int) std::round (anaRate * 0.022));
    const int tauMax = juce::jmin (W, (int) (anaRate / 70.0));
    const int tauMin = juce::jmax (2, (int) (anaRate / 1500.0));
    const int len = W + tauMax;
    for (int i = 0; i < len; ++i) yinBuf[(size_t) i] = ring[(size_t) ((ringPos - len + i) & (ringSize - 1))];

    float e = 0;
    for (int i = len - W; i < len; ++i) e += yinBuf[(size_t) i] * yinBuf[(size_t) i];
    const float rms = std::sqrt (e / (float) W);
    float eHop = 0;
    for (int i = len - monoHop; i < len; ++i) eHop += yinBuf[(size_t) i] * yinBuf[(size_t) i];
    const float hopRms = std::sqrt (eHop / (float) monoHop);
    const bool onset = hopRms > lastFrameRms * 1.8f && hopRms > threshLin * 0.7f;
    lastFrameRms = 0.6f * lastFrameRms + 0.4f * hopRms;

    // YIN: compare the newest W samples with earlier copies of themselves
    const float* x = yinBuf.data() + tauMax;
    yinD[0] = 1.0f;
    float running = 0;
    int found = -1;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        float d = 0;
        for (int j = 0; j < W; ++j) { const float diff = x[j] - x[j - tau]; d += diff * diff; }
        running += d;
        yinD[(size_t) tau] = running > 0 ? d * (float) tau / running : 1.0f;
    }
    for (int tau = tauMin; tau < tauMax; ++tau)
        if (yinD[(size_t) tau] < 0.15f)
        {
            while (tau + 1 < tauMax && yinD[(size_t) tau + 1] < yinD[(size_t) tau]) ++tau;
            found = tau; break;
        }
    if (found < 0)
    {
        float m = 0.35f;
        for (int tau = tauMin; tau < tauMax; ++tau) if (yinD[(size_t) tau] < m) { m = yinD[(size_t) tau]; found = tau; }
    }

    auto& v = voices[0];
    if (found > 0 && rms > threshLin)
    {
        float t = (float) found;
        if (found > 1 && found < tauMax - 1)
        {
            const float a = yinD[(size_t) found - 1], b = yinD[(size_t) found], c = yinD[(size_t) found + 1];
            const float den = a - 2 * b + c;
            if (std::abs (den) > 1e-9f) t += juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den);
        }
        float midi = hzToMidi ((float) anaRate / t);

        // an octave jump has to show up twice in a row before we believe it
        if (v.gate && std::abs (std::abs (midi - v.targetMidi) - 12.0f) < 0.6f)
        {
            if (monoPending > 0 && std::abs (monoPending - midi) < 0.6f && ++monoPendingCount >= 2) { monoPending = -1; }
            else { if (monoPending < 0 || std::abs (monoPending - midi) >= 0.6f) { monoPending = midi; monoPendingCount = 1; } midi = v.targetMidi; }
        }
        else monoPending = -1;

        monoMiss = 0;
        if (! v.gate)
        {
            if (++monoHits >= 2 || onset) { noteOn (0, midi, rms * 1.41f, sampleIndex); monoHits = 0; }
        }
        else
        {
            if (onset && rms * 1.41f > v.ampTarget * 1.2f)
            {
                noteOff (0, sampleIndex);
                noteOn (0, midi, rms * 1.41f, sampleIndex);
            }
            v.targetMidi = midi;
            v.ampTarget = rms * 1.41f;
            v.peakAmp = juce::jmax (v.peakAmp, v.ampTarget);
        }
    }
    else
    {
        monoHits = 0;
        if (v.gate)
        {
            v.ampTarget = rms * 1.41f;
            if (rms < threshLin * 0.7f || ++monoMiss > 10) noteOff (0, sampleIndex);
        }
    }
}

// ---------------------------------------------------------------- voices

int HexwireProcessor::findFreeVoice() const
{
    for (int i = 0; i < maxVoices; ++i) if (! voices[(size_t) i].active) return i;
    int best = -1; float lowest = 1e9f;
    for (int i = 0; i < maxVoices; ++i)
        if (! voices[(size_t) i].gate && voices[(size_t) i].env < lowest) { lowest = voices[(size_t) i].env; best = i; }
    if (best >= 0) return best;
    for (int i = 0; i < maxVoices; ++i)
        if (voices[(size_t) i].ampTarget < lowest) { lowest = voices[(size_t) i].ampTarget; best = i; }
    return juce::jmax (0, best);
}

void HexwireProcessor::noteOn (int idx, float midi, float amp, int sampleOffset)
{
    auto& v = voices[(size_t) idx];
    if (v.gate) noteOff (idx, sampleOffset);
    const bool wasSounding = v.active && v.env > 0.05f;
    if (wasSounding) v.env *= 0.35f;    // duck before jumping to the new note (avoids a click)
    else v.gainSm = 0.0f;
    v.gate = v.active = true;
    v.targetMidi = v.curMidi = midi;
    v.ampTarget = v.amp = amp;
    v.peakAmp = amp;
    v.vel = juce::jlimit (0.15f, 1.0f, std::pow (amp / 0.2f, 0.6f));
    v.strike = true;
    v.misses = 0;
    v.age = 0;
    v.silentTime = 0;

    const int midiMode = (int) apvts.getRawParameterValue ("midiout")->load();
    if (midiMode > 0 && pendingMidiOut != nullptr)
    {
        const float octA = std::round (apvts.getRawParameterValue ("octA")->load());
        const float m = midi + 12.0f * octA;
        v.midiNote = juce::jlimit (0, 127, (int) std::round (m));
        v.midiChan = midiMode == 2 ? 2 + idx : 1;
        if (midiMode == 2)
        {
            const int bend = juce::jlimit (0, 16383, (int) std::round (8192.0f + (m - (float) v.midiNote) / 48.0f * 8192.0f));
            pendingMidiOut->addEvent (juce::MidiMessage::pitchWheel (v.midiChan, bend), sampleOffset);
        }
        pendingMidiOut->addEvent (juce::MidiMessage::noteOn (v.midiChan, v.midiNote, (juce::uint8) juce::jlimit (1, 127, (int) (v.vel * 127.0f))), sampleOffset);
    }
}

void HexwireProcessor::noteOff (int idx, int sampleOffset)
{
    auto& v = voices[(size_t) idx];
    v.gate = false;
    if (v.midiNote >= 0 && pendingMidiOut != nullptr)
        pendingMidiOut->addEvent (juce::MidiMessage::noteOff (v.midiChan, v.midiNote), sampleOffset);
    v.midiNote = -1;
}

float HexwireProcessor::renderEngine (EngineState& st, int engine, float f, Voice& v, bool strike, float tone, float, float dt)
{
    const float fsr = (float) sr;
    const double inc = juce::jlimit (0.0, 0.49, (double) f / sr);
    const float nyq = fsr * 0.45f;
    auto sine = [] (double ph) { return std::sin (twoPi * (float) ph); };
    auto noise = [this] { return rng.nextFloat() * 2.0f - 1.0f; };
    st.age += dt;
    if (strike) st.age = 0;

    switch (engine)
    {
        case Cello:
        {
            // two slightly detuned saws (a little ensemble movement), smooth key-tracked rolloff, soft body resonance
            const double inc2 = inc * 1.0025;
            float s = 0.6f * (sawAt (st.ph[0], inc) + sawAt (st.ph[1], inc2));
            advance (st.ph[0], inc); advance (st.ph[1], inc2);
            const auto lp = svf (s, f * (2.0f + 7.0f * tone), 0.6f, fsr, st.lp1, st.lp2);
            const auto lp2 = svf (lp.lp, f * (3.0f + 10.0f * tone), 0.6f, fsr, st.cp1, st.cp2);
            const auto fm = svf (lp.lp, 900.0f, 1.5f, fsr, st.bp1, st.bp2);
            return lp2.lp + 0.25f * fm.bp;
        }
        case Marimba:
        {
            if (strike) { st.modal[0] = v.vel; st.modal[1] = v.vel * (0.2f + 0.4f * tone); st.modal[2] = v.vel * 0.15f * tone; st.click = v.vel; }
            const float tau0 = juce::jlimit (0.25f, 1.4f, 140.0f / f);
            st.modal[0] *= std::exp (-dt / tau0);
            st.modal[1] *= std::exp (-dt / (tau0 * 0.3f));
            st.modal[2] *= std::exp (-dt / 0.05f);
            st.click *= std::exp (-dt / 0.003f);
            float out = st.modal[0] * sine (st.ph[0]);
            advance (st.ph[0], inc);
            if (f * 3.93f < nyq) out += st.modal[1] * sine (st.ph[1]);
            if (f * 9.4f < nyq)  out += st.modal[2] * sine (st.ph[2]);
            advance (st.ph[1], inc * 3.93); advance (st.ph[2], inc * 9.4);
            const auto c = svf (noise() * st.click, f * 4.0f, 1.0f, fsr, st.bp1, st.bp2);
            return out + 0.4f * tone * c.bp;
        }
        case Bells:
        {
            if (strike) { st.modal[0] = v.vel; st.modal[1] = 1.0f; st.modal[2] = v.vel; }
            st.modal[0] *= std::exp (-dt / juce::jlimit (0.6f, 3.0f, 900.0f / f));
            st.modal[1] *= std::exp (-dt / 0.5f);
            st.modal[2] *= std::exp (-dt / 1.0f);
            const float index = st.modal[1] * (0.6f + 4.0f * tone);
            float out = st.modal[0] * std::sin (twoPi * (float) st.ph[0] + index * sine (st.ph[1]));
            if (f * 2.76f < nyq) out += 0.3f * st.modal[2] * sine (st.ph[2]);
            advance (st.ph[0], inc); advance (st.ph[1], inc * 3.5); advance (st.ph[2], inc * 2.76);
            return out * 0.8f;
        }
        case Pluck:
        {
            const int len = (int) st.ks.size();
            if (strike)
            {
                float lp = 0; const float coef = 0.15f + 0.8f * tone;
                for (auto& s : st.ks) { lp += (noise() - lp) * coef; s = lp * v.vel * 1.6f; }
                st.ksLast = 0;
            }
            const float L = juce::jlimit (2.0f, (float) len - 3.0f, fsr / f - 0.5f);
            float rp = (float) st.ksW - L;
            if (rp < 0) rp += (float) len;
            int i0 = (int) rp; const float fr = rp - (float) i0;
            if (i0 >= len) i0 -= len;
            const float y = st.ks[(size_t) i0] + fr * (st.ks[(size_t) ((i0 + 1) % len)] - st.ks[(size_t) i0]);
            const float damp = v.gate ? 0.9975f + 0.0022f * tone : 0.97f;
            st.ks[(size_t) st.ksW] = 0.5f * (y + st.ksLast) * damp;
            st.ksLast = y;
            st.ksW = (st.ksW + 1) % len;
            return y;
        }
        case Flute:
        {
            if (strike) st.bright = 1.0f;
            st.bright *= std::exp (-dt / 0.06f);
            float out = sine (st.ph[0]);
            if (f * 2 < nyq) out += 0.15f * sine (st.ph[1]);
            if (f * 3 < nyq) out += 0.05f * sine (st.ph[2]);
            advance (st.ph[0], inc); advance (st.ph[1], inc * 2); advance (st.ph[2], inc * 3);
            const auto br = svf (noise(), juce::jmin (nyq, f * 2.0f), 1.5f, fsr, st.bp1, st.bp2);
            return 0.85f * out + br.bp * (0.04f + 0.1f * tone + 0.35f * st.bright);
        }
        case Brass:
        {
            if (strike) st.bright = 1.0f;
            st.bright += (0.55f - st.bright) * (1.0f - std::exp (-dt / 0.15f));
            float s = sawAt (st.ph[0], inc); advance (st.ph[0], inc);
            const float fc = f * (1.2f + (3.0f + 9.0f * tone) * st.bright * v.env);
            const auto lp = svf (s, fc, 0.9f, fsr, st.lp1, st.lp2);
            return std::tanh (1.6f * lp.lp) * 0.85f;
        }
        case Choir:
        {
            const double d = inc * 0.004;
            float s = sawAt (st.ph[0], inc) + sawAt (st.ph[1], inc + d) + sawAt (st.ph[2], inc - d);
            advance (st.ph[0], inc); advance (st.ph[1], inc + d); advance (st.ph[2], inc - d);
            s *= 0.33f;
            // vowel: oo -> ah -> eh
            const float t = tone * 2.0f;
            const float f1 = t < 1 ? 400 + 350 * t : 750 - 200 * (t - 1);
            const float f2 = t < 1 ? 800 + 350 * t : 1150 + 650 * (t - 1);
            const auto body = svf (s, juce::jmin (3200.0f, f * 10.0f), 0.6f, fsr, st.lp1, st.lp2);
            const auto a = svf (body.lp, f1, 3.0f, fsr, st.bp1, st.bp2);
            const auto b = svf (body.lp, f2, 3.5f, fsr, st.cp1, st.cp2);
            return 1.3f * a.bp + 0.8f * b.bp + 0.35f * body.lp;
        }
        case Organ:
        {
            if (strike) st.click = 1.0f;
            st.click *= std::exp (-dt / 0.002f);
            static const float ratios[6] = { 0.5f, 1, 2, 3, 4, 6 };
            const float amps[6] = { 0.5f, 1.0f, 0.8f, 0.25f + 0.4f * tone, 0.45f * tone, 0.35f * tone };
            float out = 0;
            for (int i = 0; i < 6; ++i)
            {
                if (f * ratios[i] < nyq) out += amps[i] * sine (st.ph[i]);
                advance (st.ph[i], inc * ratios[i]);
            }
            return out * 0.38f + st.click * noise() * 0.25f;
        }
        case Pad:
        {
            static const float cents[5] = { 0, 4, -4, 9, -9 };
            float s = 0;
            for (int i = 0; i < 5; ++i)
            {
                const double vi = inc * std::exp2 (cents[i] / 1200.0f);
                s += sawAt (st.ph[i], vi); advance (st.ph[i], vi);
            }
            const auto lp = svf (s * 0.3f, f * (1.5f + 9.0f * tone * tone) + 200.0f, 0.6f, fsr, st.lp1, st.lp2);
            const auto lp2 = svf (lp.lp, f * (3.0f + 12.0f * tone) + 300.0f, 0.6f, fsr, st.bp1, st.bp2);
            return lp2.lp;
        }
        case Chip:
        {
            const double duty = 0.5 - 0.25 * tone;
            double ph2 = st.ph[0] + duty; if (ph2 >= 1) ph2 -= 1;
            const float s = sawAt (st.ph[0], inc) - sawAt (ph2, inc);
            advance (st.ph[0], inc);
            const auto lp = svf (s, f * (6.0f + 20.0f * tone), 0.6f, fsr, st.lp1, st.lp2);
            return 0.55f * lp.lp;
        }
        case Kalimba:
        {
            if (strike) { st.modal[0] = v.vel; st.modal[1] = v.vel * (0.15f + 0.35f * tone); st.click = v.vel; }
            st.modal[0] *= std::exp (-dt / juce::jlimit (0.4f, 2.5f, 1.6f * std::sqrt (220.0f / f)));
            st.modal[1] *= std::exp (-dt / 0.12f);
            st.click *= std::exp (-dt / 0.004f);
            float out = st.modal[0] * sine (st.ph[0]);
            if (f * 5.9f < nyq) out += st.modal[1] * sine (st.ph[1]);
            advance (st.ph[0], inc); advance (st.ph[1], inc * 5.9);
            const auto th = svf (noise() * st.click, 300.0f, 1.0f, fsr, st.bp1, st.bp2);
            return out + 0.5f * th.bp;
        }
        case Analog:
        {
            // one clean saw per note + a quiet square an octave down, smooth 12 dB/oct key-tracked rolloff
            float s = sawAt (st.ph[0], inc); advance (st.ph[0], inc);
            const double incSub = inc * 0.5;
            const float sq = st.ph[1] < 0.5 ? 1.0f : -1.0f;
            s += 0.25f * (sq - polyBlep (st.ph[1], incSub) + polyBlep (std::fmod (st.ph[1] + 0.5, 1.0), incSub));
            advance (st.ph[1], incSub);
            const auto lp = svf (s, f * (1.8f + 9.0f * tone), 0.65f, fsr, st.lp1, st.lp2);
            const auto lp2 = svf (lp.lp, f * (2.5f + 14.0f * tone), 0.6f, fsr, st.bp1, st.bp2);
            return lp2.lp;
        }
        case Soft:
        {
            // warm rounded tone: triangle + sine octave, gently filtered (electric-piano / soft pad feel)
            const float tri = 4.0f * std::abs ((float) st.ph[0] - 0.5f) - 1.0f;
            float out = 0.8f * tri + 0.3f * sine (st.ph[1]) + 0.15f * tone * sawAt (st.ph[2], inc);
            advance (st.ph[0], inc); advance (st.ph[1], inc * 2.0); advance (st.ph[2], inc);
            const auto lp = svf (out, f * (3.0f + 10.0f * tone), 0.6f, fsr, st.lp1, st.lp2);
            return lp.lp;
        }
        case Sub:
        default:
        {
            const float s = sine (st.ph[0]); advance (st.ph[0], inc);
            const float g = 1.0f + 3.0f * tone;
            return std::tanh (s * g) / std::tanh (g);
        }
    }
}

// ---------------------------------------------------------------- process

void HexwireProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    midi.clear();
    juce::MidiBuffer midiOut;
    pendingMidiOut = &midiOut;

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const int mode = (int) get ("mode");
    const int midiMode = (int) get ("midiout");
    if (mode != lastMode || midiMode != lastMidiMode)
    {
        for (int i = 0; i < maxVoices; ++i) if (voices[(size_t) i].gate) noteOff (i, 0);
        if (lastMidiMode > 0 && midiMode != lastMidiMode)
            for (int ch = 1; ch <= 16; ++ch) midiOut.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        lastMode = mode; lastMidiMode = midiMode;
        numPending = 0; monoHits = 0;
    }

    const int engA = (int) get ("engA"), engB = (int) get ("engB");
    const float octA = std::round (get ("octA")), octB = std::round (get ("octB"));
    const float detune = get ("detune");
    const float tone = get ("tone"), vib = get ("vibrato");
    const float glideMs = juce::jmax (mode == 1 ? 12.0f : 4.0f, get ("glide"));
    const float attackS = get ("attack") * 0.001f, releaseS = get ("release") * 0.001f;
    const float dyn = get ("dynamics");
    mixS.setTargetValue (get ("mix"));
    outS.setTargetValue (juce::Decibels::decibelsToGain (get ("output")));
    driveS.setTargetValue (get ("drive"));
    spaceS.setTargetValue (get ("space"));
    chorusS.setTargetValue (get ("chorus"));
    blendS.setTargetValue (get ("blend"));

    const float dt = 1.0f / (float) sr;
    const float glideCoef = 1.0f - std::exp (-1.0f / (glideMs * 0.001f * (float) sr));
    const float ampCoef = 1.0f - std::exp (-1.0f / (0.02f * (float) sr));
    const float gainCoef = 1.0f - std::exp (-1.0f / (0.006f * (float) sr));
    const float attackInc = dt / juce::jmax (0.001f, attackS);
    const float releaseCoef = std::exp (-1.0f / (juce::jmax (0.01f, releaseS) * 0.25f * (float) sr));
    const float detRatio = std::exp2 (detune / 1200.0f);
    const float vibRate = 5.3f / (float) sr;
    const float envFollow = 1.0f - std::exp (-1.0f / (0.05f * (float) sr));

    if (wet.getNumSamples() < n) wet.setSize (2, n, false, false, true);
    auto* wl = wet.getWritePointer (0);
    auto* wr = wet.getWritePointer (1);
    const int chLen = (int) chorusBuf[0].size();
    float peakIn = 0;

    for (int i = 0; i < n; ++i)
    {
        float in = 0;
        for (int c = 0; c < numIn; ++c) in += buffer.getSample (c, i);
        in /= (float) juce::jmax (1, numIn);
        peakIn = juce::jmax (peakIn, std::abs (in));
        inEnv += (std::abs (in) - inEnv) * envFollow;

        pushAnalysisSample (in, i);

        vibPh += vibRate; if (vibPh >= 1) vibPh -= 1;
        const float blend = blendS.getNextValue();
        float synth = 0;

        for (auto& v : voices)
        {
            if (! v.active) continue;
            v.curMidi += (v.targetMidi - v.curMidi) * glideCoef;
            v.amp += (v.ampTarget - v.amp) * ampCoef;
            v.age += dt;
            if (v.gate) v.env = juce::jmin (1.0f, v.env + attackInc);
            else        v.env *= releaseCoef;

            const float vibAmt = vib * 0.3f * juce::jmin (1.0f, v.age / 0.6f);
            const float m = v.curMidi + vibAmt * std::sin (twoPi * (float) vibPh);
            const float fA = midiToHz (m + 12.0f * octA);
            const float fB = midiToHz (m + 12.0f * octB) * detRatio;
            const bool strike = v.strike;
            v.strike = false;

            // held notes sustain like a bow: loudness is set by how hard you picked; Dynamics adds back
            // some of the string's natural fade (0 = steady sustain, 1 = follows the guitar)
            const float fade = juce::jlimit (0.001f, 1.0f, v.amp / juce::jmax (1e-6f, v.peakAmp));
            const float dynTarget = std::sqrt (v.vel) * std::pow (fade, dyn * 0.6f) * 1.6f;
            v.gainSm += (dynTarget - v.gainSm) * gainCoef;   // smoothed: no clicks when a note is re-picked
            const float dynGain = v.gainSm;

            float s = 0;
            if (blend < 0.999f)
            {
                const float a = renderEngine (v.a, engA, fA, v, strike, tone, vib, dt);
                s += (1.0f - blend) * a * engineGain[engA] * (isPerc[engA] ? 1.0f : dynGain);
            }
            if (blend > 0.001f)
            {
                const float b = renderEngine (v.b, engB, fB, v, strike, tone, vib, dt);
                s += blend * b * engineGain[engB] * (isPerc[engB] ? 1.0f : dynGain);
            }
            s *= v.env;
            synth += s * 0.35f;

            if (! v.gate)
            {
                v.silentTime = std::abs (s) < 1e-4f ? v.silentTime + dt : 0.0f;
                if (v.env < 1e-4f || v.silentTime > 0.25f) { v.active = false; v.env = 0; }
            }
        }

        // drive
        const float drive = driveS.getNextValue();
        if (drive > 0.001f)
        {
            const float g = 1.0f + 6.0f * drive;
            synth = std::tanh (synth * g) / std::sqrt (g);
        }

        // stereo chorus
        const float ch = chorusS.getNextValue();
        chorusBuf[0][(size_t) chorusW] = synth;
        float l = synth, r = synth;
        if (ch > 0.001f)
        {
            chorusPh += 0.45 / sr; if (chorusPh >= 1) chorusPh -= 1;
            const float lfo = std::sin (twoPi * (float) chorusPh), lfo2 = std::cos (twoPi * (float) chorusPh);
            auto tap = [&] (float delaySec)
            {
                float rp = (float) chorusW - delaySec * (float) sr;
                while (rp < 0) rp += (float) chLen;
                int i0 = (int) rp; const float fr = rp - (float) i0;
                if (i0 >= chLen) i0 -= chLen;
                return chorusBuf[0][(size_t) i0] + fr * (chorusBuf[0][(size_t) ((i0 + 1) % chLen)] - chorusBuf[0][(size_t) i0]);
            };
            const float dl = tap (0.012f + 0.004f * lfo), dr = tap (0.014f + 0.004f * lfo2);
            l = synth * (1.0f - 0.4f * ch) + 0.8f * ch * dl;
            r = synth * (1.0f - 0.4f * ch) + 0.8f * ch * dr;
        }
        chorusW = (chorusW + 1) % chLen;

        wl[i] = l; wr[i] = r;
    }

    // reverb on the synth only
    {
        const float sp = spaceS.getTargetValue();
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.55f + 0.4f * sp;
        rp.damping = 0.45f;
        rp.width = 1.0f;
        rp.wetLevel = 0.4f * sp;
        rp.dryLevel = 1.0f - 0.25f * sp;
        reverb.setParameters (rp);
        if (sp > 0.001f) reverb.processStereo (wl, wr, n);
    }

    // mix with the dry guitar
    for (int i = 0; i < n; ++i)
    {
        const float mixv = mixS.getNextValue(), g = outS.getNextValue();
        float dryL = numIn > 0 ? buffer.getSample (0, i) : 0.0f;
        float dryR = numIn > 1 ? buffer.getSample (1, i) : dryL;
        // gentle safety limiter: transparent below about -3 dBFS, never hard-clips
        auto safe = [] (float x)
        {
            const float a = std::abs (x);
            return a < 0.7f ? x : std::copysign (0.7f + 0.29f * std::tanh ((a - 0.7f) / 0.29f), x);
        };
        const float L = safe ((dryL * (1.0f - mixv) + wl[i] * mixv) * g);
        const float R = safe ((dryR * (1.0f - mixv) + wr[i] * mixv) * g);
        if (numOut > 0) buffer.setSample (0, i, L);
        if (numOut > 1) buffer.setSample (1, i, R);
    }

    // MPE: keep each held note's pitch bend up to date
    if (midiMode == 2)
        for (int vi = 0; vi < maxVoices; ++vi)
        {
            auto& v = voices[(size_t) vi];
            if (! v.gate || v.midiNote < 0) continue;
            const float m = v.curMidi + 12.0f * octA;
            const int bend = juce::jlimit (0, 16383, (int) std::round (8192.0f + (m - (float) v.midiNote) / 48.0f * 8192.0f));
            midiOut.addEvent (juce::MidiMessage::pitchWheel (v.midiChan, bend), juce::jmax (0, n - 1));
        }

    midi.swapWith (midiOut);
    pendingMidiOut = nullptr;

    for (int vi = 0; vi < maxVoices; ++vi)
    {
        displayMidi[(size_t) vi] = voices[(size_t) vi].gate ? voices[(size_t) vi].curMidi : -1.0f;
        displayLevel[(size_t) vi] = voices[(size_t) vi].active ? voices[(size_t) vi].env * juce::jmin (1.0f, voices[(size_t) vi].amp / 0.2f) : 0.0f;
    }
    inputDb = juce::Decibels::gainToDecibels (peakIn, -90.0f);
}

// ---------------------------------------------------------------- presets & state

int HexwireProcessor::getNumPrograms() { return numHexwirePresets; }

const juce::String HexwireProcessor::getProgramName (int index)
{
    if (index < 0 || index >= numHexwirePresets) return {};
    return juce::String (hexwirePresets[index].category) + " - " + hexwirePresets[index].name;
}

void HexwireProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= numHexwirePresets) return;
    currentPreset = index;
    const auto& pr = hexwirePresets[index];
    for (int i = 0; i < numPresetParams; ++i)
        if (auto* p = apvts.getParameter (presetParamIds[i]))
            p->setValueNotifyingHost (p->convertTo0to1 (pr.v[i]));
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

void HexwireProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("preset", currentPreset, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void HexwireProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            currentPreset = juce::jlimit (0, numHexwirePresets - 1, (int) state.getProperty ("preset", 0));
            apvts.replaceState (state);
        }
}

juce::AudioProcessorEditor* HexwireProcessor::createEditor() { return new HexwireEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HexwireProcessor(); }
