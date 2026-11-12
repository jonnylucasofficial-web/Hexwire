#include "PluginEditor.h"
#include "Presets.h"

namespace colours
{
    const juce::Colour bg     { 0xff0d0f14 };
    const juce::Colour panel  { 0xff161a22 };
    const juce::Colour track  { 0xff272d3a };
    const juce::Colour accent { 0xffff4fa3 };   // hot pink (sound)
    const juce::Colour accent2{ 0xff36d6ff };   // cyan (shape)
    const juce::Colour accent3{ 0xffffc94a };   // gold (fx)
    const juce::Colour text   { 0xffeef0f6 };
    const juce::Colour dim    { 0xff8a91a3 };
}

static const char* knobIds[] = { "octA", "octB", "detune", "blend", "tone", "vibrato", "drive", "glide",
                                 "attack", "release", "dynamics", "thresh", "chorus", "space", "mix", "output" };
static constexpr int numKnobs = 16;

static juce::String noteName (float midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = juce::roundToInt (midi);
    return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
}

HexwireLook::HexwireLook()
{
    setColour (juce::Slider::textBoxTextColourId, colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, colours::dim);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextButton::buttonColourId, colours::panel);
    setColour (juce::TextButton::buttonOnColourId, colours::accent);
    setColour (juce::TextButton::textColourOffId, colours::dim);
    setColour (juce::TextButton::textColourOnId, colours::bg);
    setColour (juce::ComboBox::backgroundColourId, colours::panel);
    setColour (juce::ComboBox::outlineColourId, colours::track);
    setColour (juce::ComboBox::textColourId, colours::text);
    setColour (juce::ComboBox::arrowColourId, colours::accent);
    setColour (juce::PopupMenu::backgroundColourId, colours::panel);
    setColour (juce::PopupMenu::textColourId, colours::text);
    setColour (juce::PopupMenu::headerTextColourId, colours::accent);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colours::accent.withAlpha (0.35f));
}

void HexwireLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                    float start, float end, juce::Slider& s)
{
    auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (6.0f);
    const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
    const auto c = b.getCentre();
    const float angle = start + pos * (end - start);
    const int group = (int) s.getProperties().getWithDefault ("group", 0);
    const auto col = group == 1 ? colours::accent2 : group == 2 ? colours::accent3 : colours::accent;

    juce::Path arcBg, arcVal;
    arcBg.addCentredArc (c.x, c.y, r - 3, r - 3, 0, start, end, true);
    g.setColour (colours::track);
    g.strokePath (arcBg, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? start + (end - start) * (float) s.valueToProportionOfLength (0.0) : start;
    arcVal.addCentredArc (c.x, c.y, r - 3, r - 3, 0, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (col.withAlpha (0.25f));
    g.strokePath (arcVal, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (col);
    g.strokePath (arcVal, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float kr = r - 11;
    g.setGradientFill (juce::ColourGradient (colours::panel.brighter (0.35f), c.x, c.y - kr,
                                             colours::panel.darker (0.3f), c.x, c.y + kr, false));
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2);
    g.setColour (colours::bg);
    g.drawEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2, 1.5f);

    juce::Path tick;
    tick.addRoundedRectangle (-1.5f, -kr + 4, 3.0f, kr * 0.45f, 1.5f);
    g.setColour (colours::text);
    g.fillPath (tick, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void HexwireLook::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                        bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.5f);
    const auto on = b.findColour (juce::TextButton::buttonOnColourId);
    auto fill = b.getToggleState() ? on : colours::panel;
    if (highlighted && ! b.getToggleState()) fill = fill.brighter (0.12f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (b.getToggleState() ? on.brighter (0.3f) : colours::track);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

juce::Font HexwireLook::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jmin (12.5f, (float) buttonHeight * 0.48f), juce::Font::bold));
}

HexwireEditor::HexwireEditor (HexwireProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&look);

    for (int i = 0; i < numKnobs; ++i)
    {
        auto k = std::make_unique<Knob>();
        auto* param = proc.apvts.getParameter (knobIds[i]);
        k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 96, 18);
        k->slider.getProperties().set ("group", i < 8 ? 0 : i < 12 ? 1 : 2);
        k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        k->label.setText (param->getName (20).toUpperCase(), juce::dontSendNotification);
        k->label.setJustificationType (juce::Justification::centred);
        k->label.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        addAndMakeVisible (k->slider);
        addAndMakeVisible (k->label);
        k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, knobIds[i], k->slider);
        knobs.push_back (std::move (k));
    }

    auto setupEngines = [this] (std::array<juce::TextButton, 12>& arr, const char* id, juce::Colour c)
    {
        for (int i = 0; i < 12; ++i)
        {
            auto& b = arr[(size_t) i];
            b.setButtonText (HexwireProcessor::engineNames[i].toUpperCase());
            b.setColour (juce::TextButton::buttonOnColourId, c);
            b.onClick = [this, id, i] { setChoice (id, i); };
            addAndMakeVisible (b);
        }
    };
    setupEngines (engA, "engA", colours::accent);
    setupEngines (engB, "engB", colours::accent2);

    monoButton.onClick = [this] { setChoice ("mode", 0); };
    polyButton.onClick = [this] { setChoice ("mode", 1); };
    for (auto* b : { &monoButton, &polyButton }) { b->setColour (juce::TextButton::buttonOnColourId, colours::accent3); addAndMakeVisible (*b); }

    const char* midiNames[] = { "OFF", "MIDI", "MPE" };
    for (int i = 0; i < 3; ++i)
    {
        midiButtons[(size_t) i].setButtonText (midiNames[i]);
        midiButtons[(size_t) i].setColour (juce::TextButton::buttonOnColourId, colours::accent2);
        midiButtons[(size_t) i].onClick = [this, i] { setChoice ("midiout", i); };
        addAndMakeVisible (midiButtons[(size_t) i]);
    }

    // presets
    const char* lastCat = "";
    for (int i = 0; i < numHexwirePresets; ++i)
    {
        if (juce::String (hexwirePresets[i].category) != lastCat)
        {
            if (i > 0) presetBox.addSeparator();
            presetBox.addSectionHeading (hexwirePresets[i].category);
            lastCat = hexwirePresets[i].category;
        }
        presetBox.addItem (hexwirePresets[i].name, i + 1);
    }
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this]
    {
        auto id = presetBox.getSelectedId();
        if (id > 0 && id - 1 != proc.getCurrentProgram())
            proc.setCurrentProgram (id - 1);
    };
    auto step = [this] (int d)
    {
        const int n = proc.getNumPrograms();
        proc.setCurrentProgram ((proc.getCurrentProgram() + d + n) % n);
        refreshPresetBox();
    };
    prevButton.onClick = [step] { step (-1); };
    nextButton.onClick = [step] { step (1); };
    addAndMakeVisible (presetBox);
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);
    refreshPresetBox();

    setSize (940, 680);
    timerCallback();
    startTimerHz (30);
}

HexwireEditor::~HexwireEditor() { setLookAndFeel (nullptr); }

void HexwireEditor::setChoice (const char* id, int index)
{
    if (auto* param = proc.apvts.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
        param->endChangeGesture();
    }
}

void HexwireEditor::refreshPresetBox()
{
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
}

void HexwireEditor::timerCallback()
{
    if (presetBox.getSelectedId() != proc.getCurrentProgram() + 1) refreshPresetBox();

    const int a = (int) proc.apvts.getRawParameterValue ("engA")->load();
    const int b = (int) proc.apvts.getRawParameterValue ("engB")->load();
    for (int i = 0; i < 12; ++i)
    {
        engA[(size_t) i].setToggleState (i == a, juce::dontSendNotification);
        engB[(size_t) i].setToggleState (i == b, juce::dontSendNotification);
    }
    const int mode = (int) proc.apvts.getRawParameterValue ("mode")->load();
    monoButton.setToggleState (mode == 0, juce::dontSendNotification);
    polyButton.setToggleState (mode == 1, juce::dontSendNotification);
    const int mo = (int) proc.apvts.getRawParameterValue ("midiout")->load();
    for (int i = 0; i < 3; ++i) midiButtons[(size_t) i].setToggleState (i == mo, juce::dontSendNotification);

    for (int i = 0; i < HexwireProcessor::maxVoices; ++i)
    {
        const float lvl = proc.displayLevel[(size_t) i].load();
        shownLevel[(size_t) i] = juce::jmax (lvl, shownLevel[(size_t) i] * 0.85f);
        const float m = proc.displayMidi[(size_t) i].load();
        if (m > 0) shownMidi[(size_t) i] = m;
    }
    shownInput = juce::jmax (proc.inputDb.load(), shownInput - 1.5f);
    wobble += 0.35f;
    repaint (wiresArea.expanded (4));
}

void HexwireEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::bg);

    // header
    auto header = getLocalBounds().removeFromTop (58).toFloat();
    g.setGradientFill (juce::ColourGradient (colours::panel.brighter (0.05f), 0, 0, colours::bg, 0, header.getBottom(), false));
    g.fillRect (header);
    g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    g.setColour (colours::accent.withAlpha (0.35f));
    g.drawText ("HEXWIRE", 21, 10, 200, 38, juce::Justification::centredLeft);
    g.setColour (colours::text);
    g.drawText ("HEXWIRE", 20, 9, 200, 38, juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (colours::dim);
    g.drawText ("POLYPHONIC GUITAR SYNTH", 172, 22, 200, 14, juce::Justification::centredLeft);

    // section panels
    auto drawPanel = [&] (juce::Rectangle<int> r, const juce::String& title, juce::Colour c)
    {
        g.setColour (colours::panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (colours::track);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.0f);
        g.setColour (c);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (title, r.getX() + 12, r.getY() + 6, 200, 14, juce::Justification::centredLeft);
    };
    drawPanel (engAArea, "ENGINE A", colours::accent);
    drawPanel (engBArea, "ENGINE B", colours::accent2);

    // wires: one per voice
    g.setColour (juce::Colour (0xff090b0f));
    g.fillRoundedRectangle (wiresArea.toFloat(), 8.0f);
    g.setColour (colours::track);
    g.drawRoundedRectangle (wiresArea.toFloat().reduced (0.5f), 8.0f, 1.0f);
    const auto wa = wiresArea.reduced (14, 10).toFloat();
    const float labelW = 70.0f;
    const float wireX0 = wa.getX() + labelW, wireX1 = wa.getRight() - 90.0f;
    for (int i = 0; i < HexwireProcessor::maxVoices; ++i)
    {
        const float y = wa.getY() + (wa.getHeight() / 6.0f) * ((float) i + 0.5f);
        const float lvl = juce::jlimit (0.0f, 1.0f, shownLevel[(size_t) i]);
        const bool on = proc.displayMidi[(size_t) i].load() > 0;
        const auto c = colours::accent.interpolatedWith (colours::accent2, (float) i / 5.0f);

        juce::Path wire;
        wire.startNewSubPath (wireX0, y);
        const int segs = 80;
        for (int s = 1; s <= segs; ++s)
        {
            const float t = (float) s / (float) segs;
            const float amp = lvl * 9.0f * std::sin (juce::MathConstants<float>::pi * t);
            const float freq = 4.0f + (on ? std::fmod (shownMidi[(size_t) i], 12.0f) : 0.0f);
            wire.lineTo (wireX0 + t * (wireX1 - wireX0), y + amp * std::sin (t * freq * 6.28f + wobble * (1.0f + 0.2f * (float) i)));
        }
        if (lvl > 0.02f)
        {
            g.setColour (c.withAlpha (0.18f * lvl + 0.05f));
            g.strokePath (wire, juce::PathStrokeType (7.0f));
        }
        g.setColour (lvl > 0.02f ? c.withAlpha (0.4f + 0.6f * lvl) : colours::track);
        g.strokePath (wire, juce::PathStrokeType (1.6f + 1.2f * lvl));

        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (colours::dim);
        g.drawText ("V" + juce::String (i + 1), (int) wa.getX(), (int) y - 8, 24, 16, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.setColour (on ? colours::text : colours::track);
        g.drawText (on ? noteName (shownMidi[(size_t) i]) : juce::String ("--"), (int) wa.getX() + 24, (int) y - 9, 44, 18, juce::Justification::centredLeft);
    }

    // input meter
    {
        auto m = juce::Rectangle<float> (wa.getRight() - 64, wa.getY() + 18, 10, wa.getHeight() - 26);
        g.setColour (colours::track);
        g.fillRoundedRectangle (m, 3.0f);
        const float thr = proc.apvts.getRawParameterValue ("thresh")->load();
        const float norm = juce::jlimit (0.0f, 1.0f, (shownInput + 72.0f) / 72.0f);
        auto fill = m.withTop (m.getBottom() - m.getHeight() * norm);
        g.setColour (shownInput > thr ? colours::accent3 : colours::dim);
        g.fillRoundedRectangle (fill, 3.0f);
        const float ty = m.getBottom() - m.getHeight() * juce::jlimit (0.0f, 1.0f, (thr + 72.0f) / 72.0f);
        g.setColour (colours::text);
        g.drawLine (m.getX() - 4, ty, m.getRight() + 4, ty, 1.5f);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (colours::dim);
        g.drawText ("IN", (int) m.getX() - 10, (int) wa.getY(), 30, 14, juce::Justification::centred);
        g.drawText ("THR", (int) m.getRight() + 6, (int) ty - 7, 30, 14, juce::Justification::centredLeft);
    }

    // knob row captions
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    const int colW = knobArea.getWidth() / 8;
    g.setColour (colours::accent);
    g.drawText ("SOUND", knobArea.getX() + 8, knobArea.getY() - 2, 200, 14, juce::Justification::centredLeft);
    g.setColour (colours::accent2);
    g.drawText ("SHAPE + TRACKING", knobArea.getX() + 8, knobArea.getCentreY() + 4, 200, 14, juce::Justification::centredLeft);
    g.setColour (colours::accent3);
    g.drawText ("FX + OUTPUT", knobArea.getX() + colW * 4 + 8, knobArea.getCentreY() + 4, 200, 14, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.setColour (colours::dim);
    g.drawText ("TRACK", monoButton.getX() - 50, monoButton.getY(), 46, monoButton.getHeight(), juce::Justification::centredRight);
    g.drawText ("MIDI OUT", midiButtons[0].getX() - 64, midiButtons[0].getY(), 60, midiButtons[0].getHeight(), juce::Justification::centredRight);
}

void HexwireEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (58).reduced (14, 13);
    // right side: midi out
    auto midiArea = header.removeFromRight (156);
    for (int i = 0; i < 3; ++i) midiButtons[(size_t) i].setBounds (midiArea.removeFromLeft (52).reduced (1, 0));
    header.removeFromRight (78);
    auto modeArea = header.removeFromRight (120);
    monoButton.setBounds (modeArea.removeFromLeft (60).reduced (1, 0));
    polyButton.setBounds (modeArea.reduced (1, 0));
    header.removeFromRight (60);
    header.removeFromLeft (330);
    nextButton.setBounds (header.removeFromRight (32));
    header.removeFromRight (4);
    prevButton.setBounds (header.removeFromLeft (32));
    header.removeFromLeft (4);
    presetBox.setBounds (header);

    r.reduce (14, 4);
    auto engines = r.removeFromTop (112);
    engAArea = engines.removeFromLeft ((engines.getWidth() - 10) / 2);
    engines.removeFromLeft (10);
    engBArea = engines;
    {
        auto area = engAArea.reduced (10, 0).withTrimmedTop (24).withTrimmedBottom (8);
        const int bw = area.getWidth() / 6, bh = area.getHeight() / 2;
        for (int i = 0; i < 12; ++i)
            engA[(size_t) i].setBounds (juce::Rectangle<int> (area.getX() + (i % 6) * bw, area.getY() + (i / 6) * bh, bw, bh).reduced (2));
        area = engBArea.reduced (10, 0).withTrimmedTop (24).withTrimmedBottom (8);
        for (int i = 0; i < 12; ++i)
            engB[(size_t) i].setBounds (juce::Rectangle<int> (area.getX() + (i % 6) * bw, area.getY() + (i / 6) * bh, bw, bh).reduced (2));
    }

    r.removeFromTop (10);
    wiresArea = r.removeFromTop (150);
    r.removeFromTop (14);
    knobArea = r.reduced (0, 0);

    const int rowH = knobArea.getHeight() / 2;
    const int colW = knobArea.getWidth() / 8;
    for (int i = 0; i < numKnobs; ++i)
    {
        const int row = i / 8, col = i % 8;
        auto cell = juce::Rectangle<int> (knobArea.getX() + col * colW, knobArea.getY() + row * rowH + (row == 0 ? 14 : 20), colW, rowH - 22);
        knobs[(size_t) i]->label.setBounds (cell.removeFromTop (16));
        knobs[(size_t) i]->slider.setBounds (cell.reduced (8, 0));
    }
}
