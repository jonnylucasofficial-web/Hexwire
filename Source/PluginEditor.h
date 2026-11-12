#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class HexwireLook : public juce::LookAndFeel_V4
{
public:
    HexwireLook();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

class HexwireEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HexwireEditor (HexwireProcessor&);
    ~HexwireEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    void refreshPresetBox();
    void setChoice (const char* id, int index);

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    HexwireProcessor& proc;
    HexwireLook look;
    juce::ComboBox presetBox;
    juce::TextButton prevButton { "<" }, nextButton { ">" };
    std::vector<std::unique_ptr<Knob>> knobs;
    std::array<juce::TextButton, 12> engA, engB;
    juce::TextButton monoButton { "MONO" }, polyButton { "POLY" };
    std::array<juce::TextButton, 3> midiButtons;
    juce::Rectangle<int> wiresArea, engAArea, engBArea, knobArea;

    std::array<float, HexwireProcessor::maxVoices> shownLevel {}, shownMidi {};
    float shownInput = -90.0f;
    float wobble = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexwireEditor)
};
