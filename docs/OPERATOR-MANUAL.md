# BUOY operator manual

## The idea

```
KEYS / ARP -> SEQ -> SCALE -> BELL 1, 2, 3  <- SEA        -> REVERB, ECHO
```

The **SEA** is an oscillator that never stops. It always sits on the scale's root, low (between C2 and B4).
You never play it.

The **BELLS** are three effects the sea runs through. A note retunes a bell: the bell picks that pitch out of the
sea, or moves the sea to it. The **ENV** opens and closes how much sea reaches the bell (that is the volume), and
**RING** is how long the bell sings after that.

Every page shows the path under its title: `SEA C3 ~> BELL 1 G4 -> SPACE`. The part the page changes is
underlined: teal for the sea, amber for the bell.

## Buttons and encoders

| Control | What it does |
| --- | --- |
| **HOME** | The harbour: the three bells over the sea, their rings as loud as they are. The mix on the knobs |
| **EDIT** | The sea |
| **FX** | The bells' effect: RING, COLOR, SPREAD, GRIT |
| **ENV** | The envelope of the bells |
| **LFO** | The LFO, on the three bells a third of a cycle apart |
| **ARP** | The arpeggiator. Press it again on its page: on or off |
| **SEQ** | Three tracks, one for each bell |
| **GLO** | The scale, the tempo, the glide, the volume. Here the keys transpose everything |
| **SEL** | **FREEZE**: every bell sounding now holds as it is. The keys and the ARP play the bells still free. Again: let them go |
| **SAVE** | Saves the sound in a USER preset (the one you are on, or the last USER one) |
| **PLAY/STOP** | Starts and stops the ARP and the SEQ. **Hold it**: every bell lets go, frozen ones too |
| **REC** | Records what the ARP plays into the selected track. Again: stop. **Hold it**: clear the track |
| **OCT− / OCT+** | The keys' and the ARP's octave. On GLO: the whole instrument, an octave |
| **SELECT** | Chooses the bell (1 to 3) the keys and the ARP play. On GLO: the scale |
| **ALGORITHM** | The bells' effect: RESONATOR, SHIFTER, PRISM (any page) |
| **PRESETS** | The sound: 8 presets, then USER 1 to 8 (any page) |

## The pages

### HOME
1 **SEA (DRY)**: the sea itself, without the bells (0 at the start: you only hear what the bells make of it) ·
2 **BELLS** · 3 **REVERB** · 4 **ECHO** (a dotted eighth, on the tempo).

### EDIT: the sea
1 **WAVE**: sine, triangle, saw, square, glass, reed, voice, organ, noise ·
2 **DETUNE**: a second oscillator, up to 50 cents away (it beats against the first) ·
3 **TONE**: dark to open · 4 **PUSH**: how hard the sea drives the bells.

Smooth waves make the RESONATOR and the PRISM sing in the overtones they share with the sea. Noise, or GRIT,
lets them sing exactly the note. The SHIFTER moves any wave to the note.

### FX: the bells
The picture is a strip of five octaves: teal lines, the sea's overtones on its root; amber, what the bells make,
at their notes. The arrow goes from the sea to the selected bell's note: the effect moved it there.

1 **RING**: how long a bell sings, 60 ms to 40 s. On the SHIFTER: feedback, which stacks the interval (shimmer) ·
2 **COLOR**: dark to bright · 3 **SPREAD**: a second note on every bell: an octave up, the scale's fifth, the
scale's third, an octave down · 4 **GRIT**: noise in the sea (the RESONATOR and the PRISM sing the exact note),
shorter, rougher grains (the SHIFTER).

### ENV
1 **ATTACK** · 2 **DECAY** · 3 **SUSTAIN** · 4 **RELEASE**, up to 20 s, then **FOREVER**: the bell never stops being
fed. With FOREVER, a key retunes a bell that keeps ringing. Hold PLAY/STOP to let every bell go.

### LFO
1 **TARGET**: pitch (± 2 semitones), volume, ring, color, grit, push · 2 **WAVE**: sine, triangle, random, drift
(random, smoothly) · 3 **RATE**: 0.01 to 20 Hz · 4 **DEPTH**. The dots under the wave are the three bells.

### ARP
1 **RATE**: 1 bar to 1/32 · 2 **PATTERN**: up, down, bounce, random, walk · 3 **RANGE**: 1 to 4 octaves ·
4 **CHANCE** that a step plays. The white keys light the scale steps it may use (none lit: the whole scale).
It plays the selected bell. It runs while PLAY is on.

### SEQ
REC records what the ARP plays into the selected track (up to 16 steps); REC again, or 16 steps, ends it and turns
the ARP off so you hear the tracks. Track 1 plays bell 1, track 2 bell 2, track 3 bell 3.
Knobs 1 to 3: each track's **LENGTH** (shorter, then longer again: the notes come back) · 4 **CHANCE** that a
recorded note plays. White keys 1 to 3 mute the tracks, 4 to 6 select them.

### GLO
1 **SCALE** (or SELECT) · 2 **TEMPO** · 3 **GLIDE**: the bells slide to a new note, up to 8 s · 4 **VOLUME**.
The keys transpose everything: the sea, the tracks, the held and frozen bells. C4 (the eighth key) is C.

## The scales

Chromatic, major, minor, major and minor pentatonic, blues, harmonic and melodic minor, dorian, phrygian, lydian,
mixolydian, locrian, lydian dominant, altered, whole tone, the two diminished, augmented, hirajoshi, in sen, iwato,
kumoi, yo, pelog, **slendro**, egyptian, chinese gong, hijaz, double harmonic, raga bhairav, bhairavi, todi, marwa,
hungarian minor and major, ukrainian dorian, persian, oriental, neapolitan minor and major, enigmatic, prometheus,
spanish 8‑tone, **just major**, **harmonic series**, fifths, octaves.

Slendro, just major and the harmonic series are between the piano keys: the bells tune there exactly; MIDI out
sends the nearest key. At power-on BUOY picks a scale at random, on C.

## Presets

BELL BUOY · FOGHORN · GLASS HARBOR · TIDE CHOIR · DEEP WATER · SHIMMER · REED STORM · STILL, then USER 1 to 8.
SAVE writes to flash: the sound stops for a moment.

## MIDI

In: notes (any channel) play the selected bell; clock, start and stop. Out: bells 1 to 3 on channels 1 to 3.

## Rescue

If BUOY fails to start, hold **OCT−** while switching on: the USB rescue mode.
