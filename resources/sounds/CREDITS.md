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

## Ringtones (incoming call)

The bundled call ringtones (`ring_classic`, `ring_bright`, `ring_soft`)
are built from **CC0 1.0** bell tones, padded with a trailing silence gap
so they loop with a natural ring…pause…ring cadence under
`PlaySoundA(SND_LOOP)`.

- Source: lavenderdotpet/CC0-Public-Domain-Sounds (CC0 1.0)
  https://github.com/lavenderdotpet/CC0-Public-Domain-Sounds — `100-CC0-SFX/bell_0{1,2,3}.ogg`
- ring_classic ← bell_01, ring_bright ← bell_02, ring_soft ← bell_03
- Transcoded to mono S16LE 44.1 kHz WAV and padded with ~1.6–2.0 s
  silence for loop cadence.

The synthesized `generateIncomingRingtone()` in CallManager remains the
"Default (TalQ)" option and the fallback if a bundled ring is missing.
