# Hexwire — polyphonic guitar synth (AU / VST3 / Standalone)

Plug your guitar in and Hexwire turns what you play into synth voices. **Poly** mode picks out the
strongest notes of each chord (up to four), holds them steadily while the strings ring and changes
cleanly on the next strum; every note follows its own bends. **Mono** mode is the
fastest-responding option for single-note lines, slides and big bends.

| Section | Controls |
|---|---|
| **Engines** | Pick an Engine A and an Engine B: Cello, Marimba, Bells, Pluck, Flute, Brass, Choir, Organ, Pad, Chip, Kalimba, Sub, Analog, Soft |
| **Sound** | Oct A, Oct B, Detune B, Blend (A↔B), Tone, Vibrato, Drive, Glide |
| **Shape + tracking** | Attack, Release, Dynamics (0 = steady bowed-style sustain, higher = follows the guitar's fade), Thresh (ignore string noise below this) |
| **FX + output** | Chorus, Space (reverb), Mix (dry guitar ↔ synth), Output |
| **MIDI Out** | Off / MIDI (channel 1) / MPE (one channel per note, with per-note bends) |

131 factory presets, level-matched to the dry guitar, in: Clean Synths, Strings, Mallets, Bells, Plucks, Winds,
Brass, Choir, Organ, Pads, Leads, Bass, Chip, Hybrid, Ambient and Guitar +.

Tips: the neck pickup with the tone rolled back a bit tracks best. If notes trigger from string noise,
raise Thresh (the white line on the IN meter).

## Getting it

Every push builds the Mac installer on GitHub Actions. Download **Hexwire-Installer.pkg** from the
`latest` release and open it (macOS: Done → System Settings → Privacy & Security → Open Anyway).
It installs the AU and VST3 plug-ins plus the standalone Hexwire app in Applications.

## Building locally / tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHEXWIRE_TESTS=ON
cmake --build build --config Release
./build/HexwireTests_artefacts/Release/HexwireTests out        # tracking, engines, MIDI, UI screenshot, preset levels
```
