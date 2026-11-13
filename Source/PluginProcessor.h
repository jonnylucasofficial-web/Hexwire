#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>

// Hexwire - polyphonic guitar synth.
//
// POLY mode: a spectral tracker (asymmetric-window FFT, harmonic salience with iterative
// cancellation) finds up to six notes at once, so chords work. Each note gets its own voice and
// follows its own bends.
// MONO mode: a YIN tracker for the lowest latency and smooth single-note bends and slides.
//
// Every voice plays two engines (A and B) blended together, then drive, chorus and reverb.
// Tracked notes can also be sent out as MIDI (plain or MPE).
class HexwireProcessor : public juce::AudioProcessor
{
public:
    HexwireProcessor();
    ~HexwireProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Hexwire"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const juce::StringArray engineNames;

    static constexpr int maxVoices = 6;

    // for the editor (and tests): what each voice is playing. midi < 0 = idle
    std::array<std::atomic<float>, maxVoices> displayMidi;
    std::array<std::atomic<float>, maxVoices> displayLevel;
    std::atomic<float> inputDb { -90.0f };

private:
    // ------------------------------------------------------------------ voices
    struct EngineState
    {
        double ph[6] {};                 // oscillator phases (0..1)
        float lp1 = 0, lp2 = 0;          // SVF state (main filter)
        float bp1 = 0, bp2 = 0;          // SVF state (formant 1)
        float cp1 = 0, cp2 = 0;          // SVF state (formant 2)
        float modal[4] {};               // percussive partial amplitudes
        float click = 0;                 // strike transient
        float bright = 0;                // brass/organ brightness envelope
        float ksLast = 0, ksTone = 0;    // Karplus-Strong loop filter state
        std::vector<float> ks;           // Karplus-Strong delay line
        int ksW = 0;
        float age = 0;                   // seconds since strike
        void reset() { for (auto& p : ph) p = 0; lp1 = lp2 = bp1 = bp2 = cp1 = cp2 = 0; for (auto& m : modal) m = 0;
                       click = bright = ksLast = ksTone = 0; std::fill (ks.begin(), ks.end(), 0.0f); age = 0; }
    };

    struct Voice
    {
        bool gate = false;          // note held
        bool active = false;        // still sounding (gate or release tail)
        float targetMidi = 60, curMidi = 60;
        float ampTarget = 0, amp = 0;
        float vel = 0.8f;
        float env = 0;              // attack/release envelope 0..1
        float gainSm = 0;           // smoothed output gain
        float silentTime = 0;
        bool strike = false;        // trigger percussive engines on next sample
        int misses = 0;             // tracker frames without a detection
        bool matched = false;
        float age = 0;              // seconds since note on (for delayed vibrato)
        float peakAmp = 0;          // loudest this note has been (for the 'still ringing' test)
        int reconfirm = 0;          // frames left for a held note to show up again after a new strum
        float hist[3] {}; int histN = 0;   // recent pitch readings
        int midiNote = -1;          // MIDI out note number
        int midiChan = 1;
        EngineState a, b;
    };
    std::array<Voice, maxVoices> voices;

    float renderEngine (EngineState& st, int engine, float freq, Voice& v, bool strike, float tone, float vibr, float dt);
    void noteOn (int idx, float midi, float amp, int sampleOffset);
    void noteOff (int idx, int sampleOffset);
    int findFreeVoice() const;

    // ------------------------------------------------------------------ tracking
    void pushAnalysisSample (float x, int sampleIndex);
    void analysePoly (int sampleIndex);
    void analyseMono (int sampleIndex);
    void updateCandidateTable();

    double sr = 44100.0;
    int decim = 4, decimCount = 0;
    double anaRate = 11025.0;
    juce::dsp::IIR::Filter<float> aa1, aa2, hp;   // anti-alias + rumble filter before decimation
    static constexpr int ringSize = 4096;
    std::array<float, ringSize> ring {};
    int ringPos = 0;
    int hopCount = 0;
    int polyHop = 128, monoHop = 48;

    // poly: FFT
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder, frameLen = 1024;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, fftBuf, mag, resid;
    float windowNorm = 1.0f;

    struct Candidate { float midi, f0; int firstHarm, numHarm; };
    struct HarmRange { int lo, hi; float w; };
    std::vector<Candidate> cands;
    std::vector<HarmRange> harms;
    std::vector<float> salience;

    struct Detection { float midi; float amp; };
    std::vector<Detection> dets;
    float pendingMidi[8] {}; int pendingHits[8] {}; int numPending = 0;
    float lastFrameRms = 0;
    bool prevOnset = false;

    // mono: YIN
    std::vector<float> yinBuf, yinD;
    float monoMidi = -1, monoPending = -1; int monoPendingCount = 0;
    int monoMiss = 0, monoHits = 0;

    // level follower on the raw input (for onsets and display)
    float inEnv = 0;

    // ------------------------------------------------------------------ fx
    std::vector<float> chorusBuf[2];
    int chorusW = 0;
    double chorusPh = 0;
    float driveLp = 0;
    juce::Reverb reverb;
    juce::AudioBuffer<float> wet;

    juce::SmoothedValue<float> mixS, outS, driveS, spaceS, chorusS, blendS;
    juce::Random rng;
    int lastMode = -1, lastMidiMode = -1;
    int currentPreset = 0;
    juce::MidiBuffer* pendingMidiOut = nullptr;   // MIDI collected during the block
    double vibPh = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexwireProcessor)
};
