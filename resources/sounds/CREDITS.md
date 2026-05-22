# Bundled notification sounds — credits & license

The six bundled notification tones are from **Kenney's Interface Sounds**
pack, dedicated to the public domain under **CC0 1.0 Universal**.
Attribution is not required under CC0; credited here as a courtesy.

- Source pack: Kenney — Interface Sounds (https://kenney.nl/assets/interface-sounds)
- Redistribution used: https://github.com/Calinou/kenney-interface-sounds
  (CC0 1.0, WAV-packaged for reuse)

| TalQ tone | Kenney source file   |
|-----------|----------------------|
| chime     | glass_001.wav        |
| pop       | drop_003.wav         |
| ding      | confirmation_001.wav |
| notify    | maximize_001.wav     |
| soft      | glass_006.wav        |
| tone      | bong_001.wav         |

All files were transcoded to mono 16-bit 44.1 kHz PCM WAV (via
`gst-launch-1.0 ... ! audioconvert ! audioresample ! wavenc`) for the
`PlaySoundA(SND_MEMORY)` playback path.

To replace any tone with your own sound, drop a WAV at
`resources/sounds/<id>.wav` and rebuild — the loader is keyed by
filename. `scripts/gen-notification-sounds.py` remains in the tree as a
zero-dependency synthetic-tone fallback generator.
