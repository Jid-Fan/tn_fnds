# tn_fndScream

An experimental UTAU resampler primarily intended for harsh vocals, such as
metal screams and growls, and for recordings with strongly aperiodic or
unstable vocal structures.

It is not a general-purpose resampler for ordinary clean or powerful vocals.
Even a powerful voicebank with a clear harmonic structure may produce
synthetic artifacts, unstable pitch, or errors when used with the experimental
modes.
In particular, `hs` is not a reliable way to turn an extreme vocal into a
natural clean vocal. It is currently not recommended for that purpose, especially
at `hs100`.

## Quick start

Install `tn_fndScream.exe` as an OpenUtau resampler.

For OpenUtau expression controls, rename `tn_fndScream.en.yaml` to
`tn_fndScream.yaml` and place it in the same folder as the executable. Then,
in Project Expressions, choose “Add all expressions suggested by renderers”.

Start with the default settings, or try:

```text
W-1
W-1 with Loop Mode set to `pingpong`
W-1hq1
W-1mf100hq2
W-1hs100
W-1mf100
W-1 with Loop Mode set to `pingpong`, `ls500`, and `lf-300`
```

Suggested order:

1. Start with `W-1`.
2. Try Loop Mode `pingpong` if a forward loop sounds too obvious.
3. Adjust `ls` and `lf` if the loop boundaries are in the wrong places.
4. Adjust `cf` if the boundary produces a click, pop, or other short noise.
5. Compare `mf`, `hq1`, and `hq2` when you need pitch movement.
6. Try `hs` if you want to replace the stable vowel region with a clearer
   harmonic sound.
7. Try `sv` or `A` only after the basic loop and pitch settings sound right.

The best settings depend heavily on the voicebank's recording, `oto.ini`, and
`frq` files. Extreme voicebanks often require more experimentation than clean
voicebanks.

## Flags

### Voice and pitch

| Flag | Range | Description |
| --- | ---: | --- |
| `W` | `-1`, `0–50`, `51–1000` | Selects the pitch source used by the resampler. `W-1` avoids forcing an analyzed musical F0 and is usually the best starting point for chaotic extreme vocals. `W0–50` uses the analyzed pitch. `W51–1000` uses a fixed F0 value. |
| `mf` | `0–100` | Legacy granular pitch-following mode. It changes the spacing of residual grains. It can retain a useful high-pitched character, but may also sound granular or become less stable at larger pitch changes. |
| `hq1` | `0–1` | Enables a full-note continuous pitch-shifting path based on Rubber Band, with formant preservation. Its sound may be smoother or less suitable depending on the voicebank and pitch change, and it may use more CPU than the other path. |
| `hq2` | `0–2` | Experimental extreme-vocal hybrid path. The low-frequency pitch scaffold is shifted with Rubber Band while high-frequency rasp, noise, shimmer, and other unstable texture are kept from the original waveform. It is mainly intended for extreme vocals and may sound layered or out of tune on clean voicebanks. |
| `t` | approximately `-100–100` | Pitch offset, approximately 10 cents per unit. |
| `A` | `0–100` | Compensates volume changes caused by pitch movement. It does not replace normal volume automation. |
| `P` | `0–100` | Approximate loudness normalization. `P0` keeps the historical output level; higher values move the active RMS toward a common target. This is not LUFS metering. |

### Vocal character

| Flag | Range | Description |
| --- | ---: | --- |
| `hs` | `0–100` | Replaces the stable vowel region with a harmonic layer whose spectral envelope follows the rendered source over time. It is an extreme-vocal timbre experiment, not a natural clean-vocal converter; higher values may sound blurry, metallic, or synthesizer-like. `hs0` is the default and changes nothing. |
| `he` | `0–100` | Searches for actual spectral peaks near the requested harmonic positions and emphasizes them without adding harmonics or replacing the waveform. It tolerates some pitch drift in extreme vocals, making the effect easier to hear than an exact-frequency boost. |
| `hf` | `0–100` | Experimental forced melodic layer. When `he` finds little existing energy, `hf` adds a small random-phase periodic component at the requested positions. It can make an otherwise non-harmonic sound more melodic, but may sound clearly synthetic. |
| `ho` | `-2000–2000` ms | Manually moves the estimated `hs` onset. Positive values start harmonic cleaning later; negative values start it earlier. `ho0` leaves the automatic estimate unchanged. |

### Looping

| Flag | Range | Description |
| --- | ---: | --- |
| Loop Mode | `forward` / `pingpong` | Selects forward looping or continuous-waveform forward/reverse looping. `forward` is the default. |
| `ls` | milliseconds | Offsets the loop start. Positive values move the start later; negative values move it earlier. |
| `lf` | milliseconds | Offsets the loop end. Negative values move the end earlier. Positive values are not currently useful in the same way because the resampler cannot extend the available oto region indefinitely. |
| `cf` | `0–1000` ms | Crossfades the loop boundary. A longer value can smooth a discontinuity, but may also lower the apparent volume or smear important attacks. |
| `rs` | milliseconds | Gives each loop start a random offset within the selected range. Use small values first. |
| `rf` | milliseconds | Gives each loop end a random offset within the selected range. Use small values first. |
| `sv` | `0–100` | Applies slow volume stabilization. It can reduce large long-term level changes in short or unstable recordings while attempting to preserve faster shimmer. |

### Spectral and consonant controls

| Flag | Range | Description |
| --- | ---: | --- |
| `O` | `-100–100` | Changes spectral opening. Positive values generally make the spectrum more open or bright; negative values make it less open. |
| `b` | `0–100` | Increases the level of unvoiced consonants. |
| `B` | `0–100` | Breath/noise control inherited from the original resampler. It may be less predictable on extreme vocals. |
| `g` | `-100–100` | Gender/timbre control inherited from the original resampler. |
| `e` | option | Changes the original resampler's stretching behavior. It is not intended as a fix for extreme-vocal looping problems and may make extreme vocals less convincing. |

`g` and the original resampler's other standard controls are retained for
compatibility. The main experimental controls of `tn_fndScream` are `W`, `mf`,
`hq`, `hs`, `he`, `hf`, Loop Mode, `ls`, `lf`, `cf`, `rs`, `rf`, and `sv`.

`hs` is intentionally separate from `hq`: it is a vocal-character effect, not
a pitch-shifting algorithm selector.

## Choosing a looping method

### Forward looping

Forward looping repeats the selected region in the same direction. It is often
cleaner when the loop region is already stable and has a consistent volume.

### Ping-pong looping (`pingpong`)

Ping-pong looping plays the selected region forward and then backward. It can
work better for short extreme-vocal recordings when the beginning and end of a
loop do not match naturally.

However, reverse playback is not automatically better. If the selected region
contains a strong attack, a sudden volume change, or an unstable spectral
event, the reverse half may produce a synthetic tone or an audible seam. In
that
case, try changing `ls`, `lf`, or `cf`, or return to forward looping.

The old residual-grain reverse method has been removed. `pingpong` now uses
continuous-waveform reverse playback.

## Troubleshooting by ear

### “The loop suddenly sounds out of tune”

Try changing the loop start or end:

```text
ls-200
lf-300
```

The apparent pitch change may come from a loop boundary crossing a local
periodic structure, not from an intentional musical pitch change.

### “There is a click, pop, or ‘tap’ at the boundary”

Try a short crossfade:

```text
cf50
cf100
```

If a longer crossfade makes the area quieter or duller, reduce it. A crossfade
cannot repair a fundamentally unsuitable loop region.

### “The reverse section contains a continuous synthetic sound”

Try moving the loop boundaries with `ls` and `lf`. Avoid placing them over a
strong attack, a sudden fade, or a rapidly changing vocal event. If the reverse
section remains artificial, use forward looping instead.

### “The result sounds granular or gets stuck”

This is a known trade-off of the legacy `mf` path. Compare it with:

```text
hq1
```

`mf`, `hq1`, and `hq2` are different synthesis paths. Neither is guaranteed
to be better for extreme vocals; their results depend on the recording and
the requested pitch movement. `hq2` is intended to preserve more of the
original high-frequency extreme-vocal texture.

### “The volume changes across the loop”

Try:

```text
sv30
sv60
```

Use moderate values first. Extreme vocals often contain real shimmer and
nonlinear level changes; excessive stabilization can make them less lively.

### “I want a cleaner, more harmonic version of the extreme vocal”

Try:

```text
hs30
hs60
hs100
```

This is an extreme-vocal timbre experiment, not a clean-vocalization feature.
The current version may lose the original singer's articulation and timbre at
`hs100`. For a natural clean-vocal result, use a voicebank and resampler intended
for clean vocals.

The effect starts after an automatically estimated consonant/unstable-onset
boundary. Use `ho` if that estimate is too early or too late. The stable
vowel's broad spectral envelope is used as an approximate mouth/formant shape.
Higher values make the harmonic replacement more prominent. The feature
follows the source's changing spectral envelope instead of using one fixed
vowel shape, which preserves broad mouth movement without carrying over the
original shimmer and noise at `hs100`. It can still sound synthetic because
the added harmonic structure was not present in the recording.

## Known limitations

- The quality of `oto.ini` and `frq` has a major effect on the result.
- `W-1` does not create a truly pitch-free signal. It mainly avoids forcing the
  analyzed musical F0 into the loop process.
- `W0–50` depends more directly on the analyzed pitch data and may be unsuitable
  for chaotic or breath-heavy recordings.
- Fixed-F0 values above 50 can sound lower, brighter, or otherwise unexpected
  depending on the source material and the residual synthesis path.
- Short recordings provide very little room for a stable loop. Manual boundary
  adjustment may still be necessary.
- `hq1` and `hq2` may be slower and can produce a different vocal identity
  from the granular path.
- `hq2` is experimental and uses a conservative frequency split; it is not a
  guaranteed harmonic/noise separation.
- `hs` is an experimental harmonicizer, not a trained neural vocoder or a
  natural extreme-vocal-to-clean-vocal converter. It does not know the exact
  vowel identity and may become blurry, lose the original vocal identity, or
  sound buzzy, synthesizer-like, or metallic at high values.
- `pingpong` can still sound artificial when the chosen loop region contains strong
  changes in pitch, spectrum, or volume.
- The resampler is experimental. Results are not guaranteed to be better than
  established resamplers for clean voicebanks.

## Building from source

The project is built with MinGW and GNU Make on Windows:

```text
cd src
mingw32-make -f Makefile -j1
```

The alternative continuous pitch-shifting path uses Rubber Band R3, whose
source code and license are included under `vendor/rubberband-3.3.0`.

## License and origins

`tn_fndScream` is a modified version of the `tn_fnds` resampler. See
`doc/copying.txt` for the project license information.

Rubber Band is distributed under the GNU General Public License. Its license
text is included in the Rubber Band source directory.

This project is an unofficial experimental modification and is not the
official upstream `tn_fnds` release.

## Disclaimer

This README was prepared primarily with AI assistance. The descriptions of
parameters and audible behavior may contain inaccuracies, and no particular
sound quality is guaranteed. Always judge the result with your own voicebank
and test renders.
