#include "world.h"
#include "tn_fnds_ext.h"
#include "pitch_shift_rb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern int __argc;
extern char **__argv;

#ifndef W1_CLOCK_JITTER
#define W1_CLOCK_JITTER 0.03
#endif

#ifndef W1_LOOP_CROSSFADE_FRAMES
#define W1_LOOP_CROSSFADE_FRAMES 8
#endif

#ifndef W1_REVERSE_MICRO_RANGE_FRAMES
#define W1_REVERSE_MICRO_RANGE_FRAMES 1
#endif

#ifndef MF_GRAIN_FRAMES
#define MF_GRAIN_FRAMES 8
#endif

#ifndef MF_MAX_GRAIN_FRAMES
#define MF_MAX_GRAIN_FRAMES 64
#endif

static double *g_melodyF0 = NULL;
static int g_melodyF0Length = 0;
static int g_melodyFollow = 0;
static const double *g_harmonicSource = NULL;
static int g_harmonicSourceLength = 0;
static int g_harmonicSourceFs = 0;
static int g_harmonicSourceBoundaryMs = 0;
static int g_harmonicOutputBoundaryMs = 0;

static int getIntegerFlag(char flagName);
static void fillPitchBend(const char *encoded, int *pitch, int count);

void setMelodyF0Control(double *melodyF0, int tLen, int strength)
{
    g_melodyF0 = melodyF0;
    g_melodyF0Length = max(0, tLen);
    g_melodyFollow = max(0, min(100, strength));
}

void setHarmonicSource(const double *source, int length, int fs,
                       int boundaryMs)
{
    g_harmonicSource = source;
    g_harmonicSourceLength = max(0, length);
    g_harmonicSourceFs = max(0, fs);
    g_harmonicSourceBoundaryMs = max(0, boundaryMs);
}

void setHarmonicOutputBoundary(int boundaryMs)
{
    g_harmonicOutputBoundaryMs = max(0, boundaryMs);
}

static double getSynthesisF0(double fixedDefault_f0, double *f0,
                             int sourceFrame)
{
    if(f0[sourceFrame] != 0.0) return f0[sourceFrame];

    if(g_melodyF0 != NULL && g_melodyFollow > 0 &&
       sourceFrame >= 0 && sourceFrame < g_melodyF0Length &&
       g_melodyF0[sourceFrame] > 1.0)
    {
        double targetF0 = g_melodyF0[sourceFrame];
        double strength = (double)g_melodyFollow / 100.0;
        return exp(log(max(1.0, fixedDefault_f0)) * (1.0 - strength) +
                   log(targetF0) * strength);
    }

    return fixedDefault_f0;
}

static int getReverseLoopMode()
{
    if(__argc > 5 && __argv != NULL && __argv[5] != NULL)
    {
        char *flag = __argv[5];
        for(int i = 0; flag[i] != '\0' && flag[i + 2] != '\0'; i++)
        {
            if(flag[i] == 'r' && flag[i + 1] == 'v')
            {
                if(flag[i + 2] == '0') return 0;
                if(flag[i + 2] == '1') return 1;
            }
            if(strncmp(flag + i, "forward", 7) == 0) return 0;
            if(strncmp(flag + i, "pingpong", 8) == 0) return 1;
        }
    }
    return -1;
}

static int hasWFlag()
{
    if(__argc > 5 && __argv != NULL && __argv[5] != NULL)
    {
        char *flag = __argv[5];
        for(int i = 0; flag[i] != '\0'; i++)
            if(flag[i] == 'W') return 1;
    }
    return 0;
}

static int getLoopOffset(char first, char second, int *offsetMs)
{
    if(__argc <= 5 || __argv == NULL || __argv[5] == NULL) return 0;

    char *flag = __argv[5];
    for(int i = 0; flag[i] != '\0' && flag[i + 1] != '\0'; i++)
    {
        if(flag[i] == first && flag[i + 1] == second)
        {
            char *cursor = flag + i + 2;
            char *end = NULL;
            long value = strtol(cursor, &end, 10);
            if(end == cursor) return 0;
            if(value > 3600000L) value = 3600000L;
            if(value < -3600000L) value = -3600000L;
            *offsetMs = (int)value;
            return 1;
        }
    }
    return 0;
}

static int getHighQualityPitchMode()
{
    int mode = 0;
    if(getLoopOffset('h', 'q', &mode))
        return max(0, min(2, mode));
    return 0;
}

static int getLoudnessCompensation()
{
    int strength = 0;
    if(getLoopOffset('l', 'c', &strength))
        return max(0, min(100, strength));
    return 0;
}

static int getMelodyFollowFlag()
{
    int strength = 0;
    if(getLoopOffset('m', 'f', &strength))
        return max(0, min(100, strength));
    return 0;
}

static int getMelodyTextureMode()
{
    int mode = 0;
    if(getLoopOffset('m', 't', &mode))
        return max(0, min(1, mode));
    return 0;
}

static int getHarmonicCleaningStrength()
{
    int strength = 0;
    if(getLoopOffset('h', 's', &strength))
        return max(0, min(100, strength));
    return 0;
}

static int getHarmonicEmphasisStrength()
{
    int strength = 0;
    if(getLoopOffset('h', 'e', &strength))
        return max(0, min(100, strength));
    return 0;
}

static int getHarmonicForceStrength()
{
    int strength = 0;
    if(getLoopOffset('h', 'f', &strength))
        return max(0, min(100, strength));
    return 0;
}

static int getHarmonicBoundaryOffsetMs()
{
    int offsetMs = 0;
    if(getLoopOffset('h', 'o', &offsetMs))
        return max(-3600000, min(3600000, offsetMs));
    return 0;
}

static double getHarmonicNoteFrequency(const char *scaleParam)
{
    if(scaleParam == NULL || scaleParam[0] == '\0') return 500.0;

    int semitone = -1;
    switch(scaleParam[0])
    {
    case 'C': semitone = 0; break;
    case 'D': semitone = 2; break;
    case 'E': semitone = 4; break;
    case 'F': semitone = 5; break;
    case 'G': semitone = 7; break;
    case 'A': semitone = 9; break;
    case 'B': semitone = 11; break;
    }
    if(semitone < 0) return 500.0;

    int octaveIndex = 1;
    if(scaleParam[1] == '#')
    {
        semitone++;
        octaveIndex++;
    }
    if(semitone >= 12)
    {
        semitone -= 12;
        octaveIndex++;
    }
    if(scaleParam[octaveIndex] < '0' ||
       scaleParam[octaveIndex] > '9')
        return 500.0;

    int octave = scaleParam[octaveIndex] - '0';
    return 440.0 * pow(2.0,
        (double)(octave - 4) + (double)(semitone - 9) / 12.0);
}

static void buildHarmonicTargetFrequency(double *target, int xLen, int fs,
                                         double fallbackFrequency)
{
    if(target == NULL || xLen <= 0 || fs <= 0)
        return;

    for(int i = 0; i < xLen; i++)
        target[i] = fallbackFrequency;

    int pitchStep = 0;
    int pitchLength = 0;
    int *pitch = NULL;
    if(__argc > 13 && __argv != NULL &&
       __argv[12] != NULL && __argv[13] != NULL)
    {
        double tempo = atof(__argv[12] + 1);
        if(tempo <= 1.0) tempo = 120.0;
        pitchStep = (int)(60.0 / 96.0 / tempo * fs + 0.5);
        pitchLength = xLen / max(1, pitchStep) + 1;
        pitch = (int *)calloc((size_t)pitchLength + 1, sizeof(int));
        if(pitch != NULL)
            fillPitchBend(__argv[13], pitch, pitchLength + 1);
    }

    double pitchOffset = (double)getIntegerFlag('t');
    double baseFrequency = fallbackFrequency *
        pow(2.0, pitchOffset / 120.0);
    for(int i = 0; i < xLen; i++)
    {
        double bend = 0.0;
        if(pitch != NULL && pitchStep > 0)
        {
            double position = (double)i / (double)pitchStep;
            int left = (int)floor(position);
            double fraction = position - (double)left;
            left = max(0, min(pitchLength, left));
            int right = min(pitchLength, left + 1);
            bend = pitch[left] * (1.0 - fraction) +
                pitch[right] * fraction;
        }
        target[i] = baseFrequency * pow(2.0, bend / 1200.0);
    }

    free(pitch);
}

static double getHarmonicFrequencyAt(const double *f0, int tLen,
                                     int sample, int xLen, int fs,
                                     double framePeriod,
                                     double fallbackFrequency)
{
    if(f0 == NULL || tLen <= 0 || xLen <= 0 || fs <= 0)
        return fallbackFrequency;

    double frame = (double)sample * 1000.0 /
        ((double)fs * framePeriod);
    frame = max(0.0, min((double)(tLen - 1), frame));
    int left = (int)floor(frame);
    int right = min(tLen - 1, left + 1);
    double fraction = frame - (double)left;
    double leftF0 = f0[left] > 1.0 ? f0[left] : 0.0;
    double rightF0 = f0[right] > 1.0 ? f0[right] : 0.0;

    double result = fallbackFrequency;
    if(leftF0 > 1.0 && rightF0 > 1.0)
        result = leftF0 + (rightF0 - leftF0) * fraction;
    else if(leftF0 > 1.0)
        result = leftF0;
    else if(rightF0 > 1.0)
        result = rightF0;

    return max(45.0, min(2200.0, result));
}

static int buildVowelSpectralEnvelope(const double *source, int xLen, int fs,
                                      int analysisStart, double **envelope,
                                      int *fftLength)
{
    if(source == NULL || envelope == NULL || fftLength == NULL ||
       xLen <= 0 || fs <= 0)
        return 0;

    int fftl = 1024;
    while(fftl > xLen && fftl > 128) fftl /= 2;
    if(fftl < 128) return 0;

    double *frame = (double *)malloc(sizeof(double) * (size_t)fftl);
    fft_complex *spectrum = (fft_complex *)malloc(
        sizeof(fft_complex) * (size_t)(fftl / 2 + 1));
    double *average = (double *)calloc((size_t)(fftl / 2 + 1),
        sizeof(double));
    if(frame == NULL || spectrum == NULL || average == NULL)
    {
        free(frame);
        free(spectrum);
        free(average);
        return 0;
    }

    analysisStart = max(0, min(xLen - fftl, analysisStart));
    int analysisEnd = max(analysisStart, xLen - fftl);
    int hop = max(1, fftl / 4);
    int frameCount = 0;
    fft_plan plan = fft_plan_dft_r2c_1d(fftl, frame, spectrum,
        FFT_ESTIMATE);

    for(int position = analysisStart;; position += hop)
    {
        for(int i = 0; i < fftl; i++)
        {
            double window = 0.5 - 0.5 * cos(
                2.0 * 3.14159265358979323846 * (double)(i + 1) /
                (double)(fftl + 1));
            frame[i] = source[position + i] * window;
        }

        fft_execute(plan);
        for(int k = 0; k <= fftl / 2; k++)
        {
            double real = spectrum[k][0];
            double imag = spectrum[k][1];
            average[k] += sqrt(real * real + imag * imag);
        }
        frameCount++;
        if(position >= analysisEnd) break;
        if(position + hop > analysisEnd) position = analysisEnd - hop;
    }

    fft_destroy_plan(plan);
    free(frame);
    free(spectrum);

    if(frameCount <= 0)
    {
        free(average);
        return 0;
    }

    for(int k = 0; k <= fftl / 2; k++)
        average[k] /= (double)frameCount;

    /*
     * Smooth over roughly 100 Hz. Narrow harmonic peaks are not the mouth
     * shape; broad spectral energy is. This also makes the method useful
     * when the source has no stable harmonic series of its own.
     */
    int radius = max(1, (int)(100.0 * (double)fftl /
        (double)fs + 0.5));
    double *smoothed = (double *)malloc(
        sizeof(double) * (size_t)(fftl / 2 + 1));
    if(smoothed == NULL)
    {
        free(average);
        return 0;
    }

    for(int k = 0; k <= fftl / 2; k++)
    {
        double sum = 0.0;
        int count = 0;
        for(int j = max(0, k - radius);
            j <= min(fftl / 2, k + radius); j++)
        {
            sum += average[j];
            count++;
        }
        smoothed[k] = sum / (double)max(1, count);
    }

    free(average);
    *envelope = smoothed;
    *fftLength = fftl;
    return 1;
}

typedef struct
{
    double *values;
    double *references;
    int frameCount;
    int fftLength;
    int hop;
    int firstPosition;
} HarmonicEnvelopeTrack;

static void freeHarmonicEnvelopeTrack(HarmonicEnvelopeTrack *track)
{
    if(track == NULL) return;
    free(track->values);
    free(track->references);
    track->values = NULL;
    track->references = NULL;
    track->frameCount = 0;
    track->fftLength = 0;
    track->hop = 0;
    track->firstPosition = 0;
}

typedef struct
{
    double *coefficients;
    int frameCount;
    int order;
    int frameLength;
    int hop;
    int firstPosition;
} VocalTractFilterTrack;

static void freeVocalTractFilterTrack(VocalTractFilterTrack *track)
{
    if(track == NULL) return;
    free(track->coefficients);
    track->coefficients = NULL;
    track->frameCount = 0;
    track->order = 0;
    track->frameLength = 0;
    track->hop = 0;
    track->firstPosition = 0;
}

typedef struct
{
    double *amplitudes;
    double *phaseOffsets;
    int harmonicCount;
    double periodicity;
} HarmonicExcitationTemplate;

static void freeHarmonicExcitationTemplate(
    HarmonicExcitationTemplate *source)
{
    if(source == NULL) return;
    free(source->amplitudes);
    free(source->phaseOffsets);
    source->amplitudes = NULL;
    source->phaseOffsets = NULL;
    source->harmonicCount = 0;
    source->periodicity = 0.0;
}

typedef struct
{
    double *amplitudes;
    double *phaseOffsets;
    double *frequencyRatios;
    double *waveforms;
    int frameCount;
    int harmonicCount;
    int hop;
    int firstPosition;
} HarmonicExcitationTrack;

static void freeHarmonicExcitationTrack(
    HarmonicExcitationTrack *track)
{
    if(track == NULL) return;
    free(track->amplitudes);
    free(track->phaseOffsets);
    free(track->frequencyRatios);
    free(track->waveforms);
    track->amplitudes = NULL;
    track->phaseOffsets = NULL;
    track->frequencyRatios = NULL;
    track->waveforms = NULL;
    track->frameCount = 0;
    track->harmonicCount = 0;
    track->hop = 0;
    track->firstPosition = 0;
}

/*
 * Estimate a stable all-pole vocal-tract filter for every short source
 * window.  The filter is represented by LPC coefficients and evaluated at
 * each generated harmonic frequency.  This lets the excitation acquire
 * resonances in the same way a glottal source acquires vowel colour through
 * a real vocal tract, without a recursive state at the output.
 */
static int buildVocalTractFilterTrack(
    const double *source, int xLen, int fs, int analysisStart,
    VocalTractFilterTrack *track)
{
    if(source == NULL || track == NULL || xLen <= 0 || fs <= 0)
        return 0;

    track->coefficients = NULL;
    track->frameCount = 0;
    track->order = 0;
    track->frameLength = 0;
    track->hop = 0;
    track->firstPosition = 0;

    int frameLength = 1024;
    while(frameLength > xLen && frameLength > 128) frameLength /= 2;
    if(frameLength < 128) return 0;

    /*
     * Sixteen LPC coefficients describe the broad vocal-tract shape with
     * enough detail for vowels such as /i/ and /e/ at a 44.1 kHz sample rate.
     * They are not sixteen formants.  The extra order restores vowel detail;
     * sharpness is still controlled separately by the lag window, reflection
     * limit, frequency averaging, and response mix below.
     */
    int order = min(16, max(12, frameLength / 64));
    int first = max(0, min(xLen - frameLength, analysisStart));
    int last = max(first, xLen - frameLength);
    int hop = max(1, frameLength / 4);
    int frameCount = 1 + (last - first + hop - 1) / hop;
    size_t coefficientCount = (size_t)frameCount *
        (size_t)(order + 1);
    double *coefficients = (double *)calloc(coefficientCount,
        sizeof(double));
    double *frame = (double *)malloc(sizeof(double) *
        (size_t)frameLength);
    double *autocorrelation = (double *)calloc((size_t)order + 1,
        sizeof(double));
    double *current = (double *)calloc((size_t)order + 1,
        sizeof(double));
    double *updated = (double *)calloc((size_t)order + 1,
        sizeof(double));
    if(coefficients == NULL || frame == NULL ||
       autocorrelation == NULL || current == NULL || updated == NULL)
    {
        free(coefficients);
        free(frame);
        free(autocorrelation);
        free(current);
        free(updated);
        return 0;
    }

    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        int position = min(last, first + frameIndex * hop);
        double mean = 0.0;
        for(int i = 0; i < frameLength; i++)
            mean += source[position + i];
        mean /= (double)frameLength;

        for(int i = 0; i < frameLength; i++)
        {
            double window = 0.5 - 0.5 * cos(
                2.0 * 3.14159265358979323846 * (double)(i + 1) /
                (double)(frameLength + 1));
            frame[i] = (source[position + i] - mean) * window;
        }

        for(int lag = 0; lag <= order; lag++)
        {
            double sum = 0.0;
            for(int i = lag; i < frameLength; i++)
                sum += frame[i] * frame[i - lag];
            autocorrelation[lag] = sum;
        }

        /*
         * A small lag window and diagonal loading keep noisy extreme-voice
         * frames from producing narrow, high-Q poles.  The response is used
         * as a colour correction, not as a resonator that should ring.
         */
        autocorrelation[0] *= 1.015;
        for(int lag = 1; lag <= order; lag++)
        {
            double normalizedLag = (double)lag / (double)order;
            autocorrelation[lag] *= exp(
                -0.18 * normalizedLag * normalizedLag);
        }

        double zeroLag = autocorrelation[0];
        double *destination = coefficients +
            (size_t)frameIndex * (size_t)(order + 1);
        destination[0] = 1.0;
        if(zeroLag <= 1.0e-12)
        {
            for(int k = 1; k <= order; k++) destination[k] = 0.0;
            continue;
        }

        for(int k = 0; k <= order; k++)
            current[k] = 0.0;
        current[0] = 1.0;
        double error = zeroLag;

        for(int k = 1; k <= order; k++)
        {
            double numerator = autocorrelation[k];
            for(int j = 1; j < k; j++)
                numerator += current[j] * autocorrelation[k - j];

            double reflection = -numerator / max(1.0e-12, error);
            /*
             * A very high reflection coefficient makes an all-pole filter
             * ring strongly when a formant is excited by a sparse harmonic
             * series.  Keep the tract resonant, but avoid whistle-like peaks.
             */
            /*
             * The old 0.94 pole radius made a newly started harmonic series
             * ring like a struck resonator.  Keep the higher order for vowel
             * detail, but reduce only the resonance strength.
             */
            reflection = max(-0.78, min(0.78, reflection));

            for(int j = 1; j < k; j++)
                updated[j] = current[j] + reflection *
                    current[k - j];
            updated[k] = reflection;
            for(int j = 1; j <= k; j++)
                current[j] = updated[j];

            error *= max(0.01, 1.0 - reflection * reflection);
        }

        for(int k = 1; k <= order; k++)
            destination[k] = current[k];
    }

    /*
     * LPC coefficients from adjacent windows can move too abruptly when the
     * source is noisy.  Smooth the coefficients in time before they are
     * interpolated at sample rate; this preserves mouth movement while
     * preventing short-lived resonant bursts.
     */
    double *smoothedCoefficients = (double *)malloc(
        sizeof(double) * coefficientCount);
    if(smoothedCoefficients != NULL)
    {
        int stride = order + 1;
        for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
        {
            int previous = max(0, frameIndex - 1);
            int next = min(frameCount - 1, frameIndex + 1);
            for(int k = 0; k <= order; k++)
            {
                double center = coefficients[
                    (size_t)frameIndex * stride + k];
                double before = coefficients[
                    (size_t)previous * stride + k];
                double after = coefficients[
                    (size_t)next * stride + k];
                smoothedCoefficients[
                    (size_t)frameIndex * stride + k] =
                    0.25 * before + 0.50 * center + 0.25 * after;
            }
        }
        free(coefficients);
        coefficients = smoothedCoefficients;
    }

    free(frame);
    free(autocorrelation);
    free(current);
    free(updated);
    track->coefficients = coefficients;
    track->frameCount = frameCount;
    track->order = order;
    track->frameLength = frameLength;
    track->hop = hop;
    track->firstPosition = first;
    return 1;
}

static double getVocalTractCoefficient(
    const VocalTractFilterTrack *track, int sample, int coefficient)
{
    if(track == NULL || track->coefficients == NULL ||
       track->frameCount <= 0 || track->order <= 0 ||
       track->hop <= 0 || coefficient < 0 ||
       coefficient > track->order)
        return coefficient == 0 ? 1.0 : 0.0;

    double position = (double)(sample - track->firstPosition) /
        (double)track->hop;
    position = max(0.0, min((double)(track->frameCount - 1), position));
    int left = (int)floor(position);
    int right = min(track->frameCount - 1, left + 1);
    double fraction = position - (double)left;
    int stride = track->order + 1;
    double leftValue = track->coefficients[left * stride + coefficient];
    double rightValue = track->coefficients[right * stride + coefficient];
    return leftValue + (rightValue - leftValue) * fraction;
}

/*
 * Evaluate the all-pole vocal-tract response at one harmonic frequency.
 * This is the same LPC filter as the recursive implementation, but in the
 * frequency domain.  It preserves the formant shape without carrying a
 * filter state from the attack into the vowel body.
 */
static double getVocalTractMagnitude(
    const VocalTractFilterTrack *track, int sample, int order,
    double frequency, int fs)
{
    if(track == NULL || order <= 0 || frequency <= 0.0 || fs <= 0)
        return 1.0;

    const double pi = 3.14159265358979323846;
    double real = 1.0;
    double imag = 0.0;
    for(int k = 1; k <= order; k++)
    {
        double coefficient = getVocalTractCoefficient(
            track, sample, k);
        double angle = 2.0 * pi * frequency * (double)k /
            (double)fs;
        real += coefficient * cos(angle);
        imag -= coefficient * sin(angle);
    }

    double denominator = sqrt(max(1.0e-12,
        real * real + imag * imag));
    return 1.0 / denominator;
}

static double getVocalTractMagnitudeSmoothed(
    const VocalTractFilterTrack *track, int sample, int order,
    double frequency, int fs)
{
    if(frequency <= 0.0)
        return 1.0;

    /*
     * A formant is a broad spectral region. Sampling one exact frequency
     * lets a generated harmonic land on a narrow LPC peak and sound like a
     * fixed whistle. Average a small neighbourhood before applying the
     * response to the excitation.
     */
    double width = 220.0;
    double left = max(1.0, frequency - width);
    double right = min((double)fs * 0.48, frequency + width);
    double centreResponse = getVocalTractMagnitude(
        track, sample, order, frequency, fs);
    double leftResponse = getVocalTractMagnitude(
        track, sample, order, left, fs);
    double rightResponse = getVocalTractMagnitude(
        track, sample, order, right, fs);
    return 0.25 * leftResponse + 0.50 * centreResponse +
        0.25 * rightResponse;
}

static double getVocalTractMagnitudeStable(
    const VocalTractFilterTrack *track, int sample, int order,
    double frequency, int fs)
{
    double centre = getVocalTractMagnitudeSmoothed(
        track, sample, order, frequency, fs);
    if(frequency <= 3000.0 || fs <= 0)
        return centre;

    /*
     * F1/F2 changes are useful articulation.  Above F2, a short-lived LPC
     * change is much more likely to be shimmer or a narrow metallic event,
     * so average a small temporal neighbourhood before applying it.
     */
    int radius = max(1, (int)((double)fs * 0.012 + 0.5));
    double before = getVocalTractMagnitudeSmoothed(
        track, sample - radius, order, frequency, fs);
    double after = getVocalTractMagnitudeSmoothed(
        track, sample + radius, order, frequency, fs);
    return 0.25 * before + 0.50 * centre + 0.25 * after;
}

/*
 * LPC magnitude contains its own absolute gain and spectral tilt. Using it
 * directly makes every generated note inherit the same artificial low/high
 * balance. Remove that gain with a broad geometric reference, then use only
 * the relative mouth shape as the colour correction.
 */
static double getVocalTractReference(
    const VocalTractFilterTrack *track, int sample, int order, int fs)
{
    if(track == NULL || order <= 0 || fs <= 0)
        return 1.0;

    const double frequencies[] = {
        250.0, 500.0, 750.0, 1000.0, 1500.0,
        2200.0, 3200.0, 5000.0, 7000.0
    };
    const int frequencyCount = (int)(
        sizeof(frequencies) / sizeof(frequencies[0]));
    double logSum = 0.0;
    int count = 0;
    for(int i = 0; i < frequencyCount; i++)
    {
        if(frequencies[i] >= (double)fs * 0.45)
            continue;
        double response = getVocalTractMagnitudeSmoothed(
            track, sample, order, frequencies[i], fs);
        logSum += log(max(0.05, min(20.0, response)));
        count++;
    }
    return count > 0 ? exp(logSum / (double)count) : 1.0;
}

/*
 * Build a slowly changing spectral-envelope track.  The old single envelope
 * was averaged over the whole vowel, so it erased mouth movement and made a
 * generated harmonic layer unnaturally stationary.
 */
static int buildVowelSpectralEnvelopeTrack(
    const double *source, int xLen, int fs, int analysisStart,
    HarmonicEnvelopeTrack *track)
{
    if(source == NULL || track == NULL || xLen <= 0 || fs <= 0)
        return 0;

    track->values = NULL;
    track->references = NULL;
    track->frameCount = 0;
    track->fftLength = 0;
    track->hop = 0;
    track->firstPosition = 0;

    int fftl = 1024;
    while(fftl > xLen && fftl > 128) fftl /= 2;
    if(fftl < 128) return 0;

    int first = max(0, min(xLen - fftl, analysisStart));
    int last = max(first, xLen - fftl);
    int hop = max(1, fftl / 4);
    int frameCount = 1 + (last - first + hop - 1) / hop;
    if(frameCount <= 0) return 0;

    int binCount = fftl / 2 + 1;
    size_t valueCount = (size_t)frameCount * (size_t)binCount;
    double *values = (double *)calloc(valueCount, sizeof(double));
    double *smoothed = (double *)calloc(valueCount, sizeof(double));
    double *references = (double *)calloc((size_t)frameCount,
        sizeof(double));
    double *frame = (double *)malloc(sizeof(double) * (size_t)fftl);
    fft_complex *spectrum = (fft_complex *)malloc(
        sizeof(fft_complex) * (size_t)binCount);
    if(values == NULL || smoothed == NULL || references == NULL ||
       frame == NULL || spectrum == NULL)
    {
        free(values);
        free(smoothed);
        free(references);
        free(frame);
        free(spectrum);
        return 0;
    }

    fft_plan plan = fft_plan_dft_r2c_1d(fftl, frame, spectrum,
        FFT_ESTIMATE);
    int frequencyRadius = max(1, (int)(100.0 * (double)fftl /
        (double)fs + 0.5));
    int lowBin = max(1, (int)(250.0 * (double)fftl / fs));
    int highBin = min(fftl / 2, (int)(8000.0 * (double)fftl / fs));

    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        int position = min(last, first + frameIndex * hop);
        for(int i = 0; i < fftl; i++)
        {
            double window = 0.5 - 0.5 * cos(
                2.0 * 3.14159265358979323846 * (double)(i + 1) /
                (double)(fftl + 1));
            frame[i] = source[position + i] * window;
        }
        fft_execute(plan);

        for(int k = 0; k < binCount; k++)
        {
            double sum = 0.0;
            int count = 0;
            for(int j = max(0, k - frequencyRadius);
                j <= min(fftl / 2, k + frequencyRadius); j++)
            {
                double real = spectrum[j][0];
                double imag = spectrum[j][1];
                sum += sqrt(real * real + imag * imag);
                count++;
            }
            values[(size_t)frameIndex * binCount + k] =
                sum / (double)max(1, count);
        }
    }
    fft_destroy_plan(plan);
    free(frame);
    free(spectrum);

    /*
     * A short temporal average removes frame-to-frame FFT flutter but keeps
     * real mouth movement.  It is deliberately much shorter than the old
     * whole-vowel average.
     */
    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        for(int k = 0; k < binCount; k++)
        {
            /*
             * Fast high-frequency events are usually shimmer/noise rather
             * than mouth movement. Average them over a longer interval,
             * while keeping the low and mid bands responsive enough for
             * vowel transitions.
             */
            double frameFrequency = (double)k * (double)fs /
                (double)fftl;
            int temporalRadius = frameFrequency >= 5000.0 ? 12 :
                (frameFrequency >= 3000.0 ? 8 : 1);
            int begin = max(0, frameIndex - temporalRadius);
            int end = min(frameCount - 1, frameIndex + temporalRadius);
            double sum = 0.0;
            int count = 0;
            if(frameFrequency >= 3000.0)
            {
                /*
                 * A short high-frequency burst is texture, not a stable
                 * mouth position.  Use a small trimmed mean so one or two
                 * extreme frames cannot become a new metallic formant.
                 */
                double sorted[25];
                for(int j = begin; j <= end && count < 25; j++)
                {
                    double value = values[(size_t)j * binCount + k];
                    int insert = count;
                    while(insert > 0 && sorted[insert - 1] > value)
                    {
                        sorted[insert] = sorted[insert - 1];
                        insert--;
                    }
                    sorted[insert] = value;
                    count++;
                }
                int trim = count / 4;
                for(int j = trim; j < count - trim; j++)
                    sum += sorted[j];
                count -= trim * 2;
            }
            else
            {
                for(int j = begin; j <= end; j++)
                    sum += values[(size_t)j * binCount + k];
                count = end - begin + 1;
            }
            smoothed[(size_t)frameIndex * binCount + k] =
                sum / (double)max(1, count);
        }

        double reference = 0.0;
        int count = 0;
        for(int k = lowBin; k <= highBin; k++)
        {
            reference += smoothed[(size_t)frameIndex * binCount + k];
            count++;
        }
        references[frameIndex] = reference / (double)max(1, count);
    }

    free(values);
    track->values = smoothed;
    track->references = references;
    track->frameCount = frameCount;
    track->fftLength = fftl;
    track->hop = hop;
    track->firstPosition = first;
    return 1;
}

static double sampleHarmonicEnvelopeTrack(
    const HarmonicEnvelopeTrack *track, int sample, int fs,
    double frequency, double *reference)
{
    if(track == NULL || track->values == NULL ||
       track->references == NULL || track->frameCount <= 0 ||
       track->fftLength <= 0 || track->hop <= 0 || fs <= 0 ||
       frequency <= 0.0)
    {
        if(reference != NULL) *reference = 1.0;
        return 1.0;
    }

    double framePosition = (double)(sample - track->firstPosition) /
        (double)track->hop;
    framePosition = max(0.0, min((double)(track->frameCount - 1),
        framePosition));
    int leftFrame = (int)floor(framePosition);
    int rightFrame = min(track->frameCount - 1, leftFrame + 1);
    double frameFraction = framePosition - (double)leftFrame;

    double ref = track->references[leftFrame] +
        (track->references[rightFrame] - track->references[leftFrame]) *
        frameFraction;
    if(reference != NULL) *reference = max(1.0e-12, ref);

    double bin = frequency * (double)track->fftLength / (double)fs;
    bin = max(0.0, min((double)(track->fftLength / 2), bin));
    int leftBin = (int)floor(bin);
    int rightBin = min(track->fftLength / 2, leftBin + 1);
    double binFraction = bin - (double)leftBin;
    int binCount = track->fftLength / 2 + 1;

    double leftValue = track->values[(size_t)leftFrame * binCount +
        leftBin] + (track->values[(size_t)leftFrame * binCount +
        rightBin] - track->values[(size_t)leftFrame * binCount +
        leftBin]) * binFraction;
    double rightValue = track->values[(size_t)rightFrame * binCount +
        leftBin] + (track->values[(size_t)rightFrame * binCount +
        rightBin] - track->values[(size_t)rightFrame * binCount +
        leftBin]) * binFraction;
    return leftValue + (rightValue - leftValue) * frameFraction;
}

static double sampleVowelSpectralEnvelope(const double *envelope,
                                          int fftLength, int fs,
                                          double frequency)
{
    if(envelope == NULL || fftLength <= 0 || fs <= 0 ||
       frequency <= 0.0)
        return 1.0;

    double bin = frequency * (double)fftLength / (double)fs;
    bin = max(0.0, min((double)(fftLength / 2), bin));
    int left = (int)floor(bin);
    int right = min(fftLength / 2, left + 1);
    double fraction = bin - (double)left;
    return envelope[left] +
        (envelope[right] - envelope[left]) * fraction;
}

static double sampleHarmonicWaveform(const double *source, int length,
                                     double position)
{
    if(source == NULL || length <= 0)
        return 0.0;

    position = max(0.0, min((double)(length - 1), position));
    int left = (int)floor(position);
    int right = min(length - 1, left + 1);
    double fraction = position - (double)left;
    return source[left] + (source[right] - source[left]) * fraction;
}

/*
 * Remove the locally estimated vocal-tract colour before extracting a
 * periodic excitation cycle.  The old code extracted cycles directly from
 * the recording, so the excitation already contained the source vowel
 * formants.  Reapplying the target formant response then made pitch changes
 * smear the vowel or favour the wrong low partials.
 */
static double getVocalTractResidualAtSample(
    const double *source, int length, int sample,
    const VocalTractFilterTrack *track)
{
    if(source == NULL || length <= 0)
        return 0.0;

    sample = max(0, min(length - 1, sample));
    if(track == NULL || track->coefficients == NULL ||
       track->order <= 0)
        return source[sample];

    double residual = source[sample];
    for(int k = 1; k <= track->order; k++)
    {
        int previous = sample - k;
        if(previous < 0) break;
        residual += getVocalTractCoefficient(track, sample, k) *
            source[previous];
    }
    return residual;
}

static double sampleVocalTractResidual(
    const double *source, int length, double position,
    const VocalTractFilterTrack *track)
{
    if(source == NULL || length <= 0)
        return 0.0;

    position = max(0.0, min((double)(length - 1), position));
    int left = (int)floor(position);
    int right = min(length - 1, left + 1);
    double fraction = position - (double)left;
    double leftValue = getVocalTractResidualAtSample(
        source, length, left, track);
    double rightValue = getVocalTractResidualAtSample(
        source, length, right, track);
    return leftValue + (rightValue - leftValue) * fraction;
}

static double mapHarmonicSourcePosition(int outputSample, int outputLength,
                                        int outputBoundary,
                                        int sourceBoundary,
                                        int sourceVowelStart,
                                        int sourceVowelEnd)
{
    if(outputLength <= 0)
        return (double)sourceVowelStart;

    double fraction;
    if(outputBoundary > 0 && outputSample < outputBoundary)
    {
        fraction = (double)outputSample /
            (double)max(1, outputBoundary);
        return (double)sourceBoundary +
            ((double)sourceVowelStart - (double)sourceBoundary) *
            max(0.0, min(1.0, fraction));
    }

    int remainingOutput = max(1, outputLength - outputBoundary);
    fraction = (double)(outputSample - outputBoundary) /
        (double)remainingOutput;
    fraction = max(0.0, min(1.0, fraction));
    return (double)sourceVowelStart +
        ((double)sourceVowelEnd - (double)sourceVowelStart) * fraction;
}

static double harmonicLocalRms(const double *source, int xLen, int center,
                               int radius)
{
    if(source == NULL || xLen <= 0)
        return 0.0;

    int left = max(0, center - radius);
    int right = min(xLen - 1, center + radius);
    double energy = 0.0;
    int count = 0;
    for(int i = left; i <= right; i++)
    {
        energy += source[i] * source[i];
        count++;
    }
    return count > 0 ? sqrt(energy / (double)count) : 0.0;
}

static int estimateHarmonicLag(const double *source, int xLen, int fs,
                               int center, int frameLength,
                               double *periodicity)
{
    if(periodicity != NULL) *periodicity = 0.0;
    if(source == NULL || xLen < 64 || fs <= 0 || frameLength < 32)
        return 0;

    frameLength = min(frameLength, xLen);
    int half = frameLength / 2;
    center = max(half, min(xLen - half - 1, center));
    int begin = center - half;

    double mean = 0.0;
    for(int i = 0; i < frameLength; i++)
        mean += source[begin + i];
    mean /= (double)frameLength;

    int minLag = max(2, (int)((double)fs / 1000.0 + 0.5));
    int maxLag = min(frameLength / 2,
        max(minLag + 1, (int)((double)fs / 45.0 + 0.5)));
    double bestCorrelation = -1.0;
    int bestLag = 0;
    for(int lag = minLag; lag <= maxLag; lag++)
    {
        double correlation = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        int count = frameLength - lag;
        for(int i = 0; i < count; i++)
        {
            double left = source[begin + i] - mean;
            double right = source[begin + i + lag] - mean;
            correlation += left * right;
            leftEnergy += left * left;
            rightEnergy += right * right;
        }
        if(leftEnergy <= 1.0e-12 || rightEnergy <= 1.0e-12)
            continue;
        double normalized = correlation /
            sqrt(leftEnergy * rightEnergy);
        if(normalized > bestCorrelation)
        {
            bestCorrelation = normalized;
            bestLag = lag;
        }
    }

    if(bestLag <= 0 || bestCorrelation <= 0.0)
        return 0;

    /*
     * Autocorrelation often prefers two periods for a smooth vowel. Pick
     * the shortest nearby lag that is almost as strong so the template does
     * not accidentally become a two-pulse subharmonic.
     */
    double acceptedCorrelation = bestCorrelation * 0.92;
    for(int lag = minLag; lag < bestLag; lag++)
    {
        double correlation = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        int count = frameLength - lag;
        for(int i = 0; i < count; i++)
        {
            double left = source[begin + i] - mean;
            double right = source[begin + i + lag] - mean;
            correlation += left * right;
            leftEnergy += left * left;
            rightEnergy += right * right;
        }
        if(leftEnergy <= 1.0e-12 || rightEnergy <= 1.0e-12)
            continue;
        double normalized = correlation /
            sqrt(leftEnergy * rightEnergy);
        if(normalized >= acceptedCorrelation)
        {
            bestLag = lag;
            break;
        }
    }

    if(periodicity != NULL)
        *periodicity = max(0.0, min(1.0, bestCorrelation));
    return bestLag;
}

static int fillHarmonicExcitationFromCycle(
    const double *cycle, int cycleLength,
    HarmonicExcitationTemplate *result)
{
    if(cycle == NULL || cycleLength < 32 || result == NULL)
        return 0;

    const double pi = 3.14159265358979323846;
    int maximumHarmonics = 48;
    result->amplitudes = (double *)calloc(
        (size_t)maximumHarmonics, sizeof(double));
    result->phaseOffsets = (double *)calloc(
        (size_t)maximumHarmonics, sizeof(double));
    if(result->amplitudes == NULL || result->phaseOffsets == NULL)
    {
        freeHarmonicExcitationTemplate(result);
        return 0;
    }

    double *normalisedCycle = (double *)malloc(
        sizeof(double) * (size_t)cycleLength);
    if(normalisedCycle == NULL)
    {
        freeHarmonicExcitationTemplate(result);
        return 0;
    }

    double mean = 0.0;
    for(int i = 0; i < cycleLength; i++)
        mean += cycle[i];
    mean /= (double)cycleLength;

    double energy = 0.0;
    for(int i = 0; i < cycleLength; i++)
    {
        normalisedCycle[i] = cycle[i] - mean;
        energy += normalisedCycle[i] * normalisedCycle[i];
    }
    double rms = sqrt(energy / (double)cycleLength);
    if(rms <= 1.0e-9)
    {
        free(normalisedCycle);
        freeHarmonicExcitationTemplate(result);
        return 0;
    }
    for(int i = 0; i < cycleLength; i++)
        normalisedCycle[i] /= rms;

    int harmonicCount = 0;
    for(int h = 1; h <= maximumHarmonics; h++)
    {
        double sineProjection = 0.0;
        double cosineProjection = 0.0;
        for(int i = 0; i < cycleLength; i++)
        {
            double angle = 2.0 * pi * (double)h *
                (double)i / (double)cycleLength;
            sineProjection += normalisedCycle[i] * sin(angle);
            cosineProjection += normalisedCycle[i] * cos(angle);
        }

        double amplitude = 2.0 / (double)cycleLength *
            sqrt(sineProjection * sineProjection +
                 cosineProjection * cosineProjection);
        result->amplitudes[h - 1] = amplitude;
        if(amplitude > 1.0e-4)
        {
            result->phaseOffsets[h - 1] = atan2(
                cosineProjection, sineProjection);
            harmonicCount = h;
        }
    }

    free(normalisedCycle);
    result->harmonicCount = max(1, harmonicCount);
    return 1;
}

static int buildHarmonicExcitationTemplate(
    const double *source, int xLen, int fs, int analysisStart,
    const VocalTractFilterTrack *vocalTractTrack,
    HarmonicExcitationTemplate *result)
{
    if(result == NULL)
        return 0;
    result->amplitudes = NULL;
    result->phaseOffsets = NULL;
    result->harmonicCount = 0;
    result->periodicity = 0.0;

    const int cycleLength = 512;
    double *cycle = (double *)malloc(
        sizeof(double) * (size_t)cycleLength);
    if(cycle == NULL)
        return 0;

    /*
     * The template selects one representative pulse shape for the note.  A
     * 2048-sample window is already long enough to distinguish its broad
     * shape; scanning a 4096-sample autocorrelation every 10 ms made hs
     * impractically slow without adding vowel detail.
     */
    int frameLength = min(2048, xLen);
    frameLength = max(128, frameLength);
    int half = frameLength / 2;
    int firstCenter = max(half,
        min(xLen - half - 1, analysisStart + half));
    int lastCenter = max(firstCenter,
        xLen - half - 1);
    int hop = max(1, (int)((double)fs * 0.040 + 0.5));
    double bestScore = 0.0;
    double bestPeriodicity = 0.0;
    int bestCenter = -1;
    int bestLag = 0;

    for(int center = firstCenter; center <= lastCenter; center += hop)
    {
        double periodicity = 0.0;
        int lag = estimateHarmonicLag(source, xLen, fs, center,
            frameLength, &periodicity);
        if(lag <= 0 || periodicity < 0.35)
            continue;
        double rms = harmonicLocalRms(source, xLen, center, half);
        double score = rms * periodicity * periodicity;
        if(score > bestScore)
        {
            bestScore = score;
            bestPeriodicity = periodicity;
            bestCenter = center;
            bestLag = lag;
        }
    }

    /*
     * Average several neighbouring periods. The stable glottal shape stays,
     * while cycle-to-cycle shimmer and noisy residual detail cancel out.
     * Extract the cycle after inverse vocal-tract filtering so its spectrum
     * is an excitation shape rather than a second copy of the vowel.
     */
    if(bestCenter >= 0 && bestLag > 0)
    {
        for(int i = 0; i < cycleLength; i++)
        {
            double phase = ((double)i + 0.5) /
                (double)cycleLength;
            double relative = (phase - 0.5) * (double)bestLag;
            double sum = 0.0;
            int count = 0;
            for(int cycleIndex = -2; cycleIndex <= 2; cycleIndex++)
            {
                double position = (double)bestCenter + relative +
                    (double)cycleIndex * (double)bestLag;
                if(position >= 0.0 && position <= (double)(xLen - 1))
                {
                    sum += sampleVocalTractResidual(
                        source, xLen, position, vocalTractTrack);
                    count++;
                }
            }
            cycle[i] = count > 0 ? sum / (double)count : 0.0;
        }
        if(fillHarmonicExcitationFromCycle(
            cycle, cycleLength, result))
        {
            result->periodicity = bestPeriodicity;
            free(cycle);
            return 1;
        }
    }

    /*
     * If the recording has no stable voiced period, use a quiet asymmetric
     * glottal pulse rather than a stack of zero-phase sine waves. This is
     * only a fallback for an unanalysable vowel; it never copies source noise.
     */
    for(int i = 0; i < cycleLength; i++)
    {
        double phase = ((double)i + 0.5) /
            (double)cycleLength;
        if(phase < 0.68)
            cycle[i] = 0.5 - 0.5 * cos(
                3.14159265358979323846 * phase / 0.68);
        else
            cycle[i] = 0.12 * (0.5 + 0.5 * cos(
                3.14159265358979323846 *
                (phase - 0.68) / 0.32));
    }
    int success = fillHarmonicExcitationFromCycle(
        cycle, cycleLength, result);
    free(cycle);
    return success;
}

/*
 * One excitation template for the whole note is still too much like an
 * oscillator preset: the vocal tract can move while the glottal shape stays
 * frozen. Build a short-time track from locally averaged periods instead.
 * Averaging three neighbouring periods removes fast shimmer and noisy residual
 * detail, while the 20 ms hop keeps smooth changes in attack and articulation.
 */
static int buildHarmonicExcitationTrack(
    const double *source, int xLen, int fs, int analysisStart,
    const VocalTractFilterTrack *vocalTractTrack,
    const HarmonicExcitationTemplate *fallback,
    HarmonicExcitationTrack *track)
{
    if(source == NULL || track == NULL || xLen < 64 || fs <= 0)
        return 0;

    track->amplitudes = NULL;
    track->phaseOffsets = NULL;
    track->frequencyRatios = NULL;
    track->waveforms = NULL;
    track->frameCount = 0;
    track->harmonicCount = 0;
    track->hop = 0;
    track->firstPosition = 0;

    const int maximumHarmonics = 48;
    /*
     * This track describes slow changes in glottal pulse shape.  It is not
     * the vocal-tract track, which remains at a much finer hop above.  A
     * 30 ms analysis hop removes most redundant autocorrelation work while
     * retaining audible onset and articulation changes.
     */
    int frameLength = min(1536, xLen);
    frameLength = max(128, frameLength);
    if(frameLength > xLen) frameLength = xLen;

    int half = frameLength / 2;
    int first = max(half,
        min(xLen - half - 1, analysisStart + half));
    int last = max(first, xLen - half - 1);
    int hop = max(1, (int)((double)fs * 0.030 + 0.5));
    int frameCount = 1 + (last - first + hop - 1) / hop;
    if(frameCount <= 0 || first >= xLen)
        return 0;

    size_t valueCount = (size_t)frameCount *
        (size_t)maximumHarmonics;
    double *amplitudes = (double *)calloc(valueCount, sizeof(double));
    double *phaseOffsets = (double *)calloc(valueCount, sizeof(double));
    double *frequencyRatios = (double *)calloc(
        (size_t)frameCount, sizeof(double));
    double *waveforms = (double *)calloc(
        (size_t)frameCount * 512, sizeof(double));
    double *smoothedAmplitudes = (double *)calloc(
        valueCount, sizeof(double));
    double *smoothedPhases = (double *)calloc(valueCount, sizeof(double));
    double *smoothedFrequencyRatios = (double *)calloc(
        (size_t)frameCount, sizeof(double));
    double *smoothedWaveforms = (double *)calloc(
        (size_t)frameCount * 512, sizeof(double));
    double *cycle = (double *)malloc(sizeof(double) * 512);
    if(amplitudes == NULL || phaseOffsets == NULL ||
       frequencyRatios == NULL || waveforms == NULL ||
       smoothedAmplitudes == NULL || smoothedPhases == NULL ||
       smoothedFrequencyRatios == NULL || smoothedWaveforms == NULL ||
       cycle == NULL)
    {
        free(amplitudes);
        free(phaseOffsets);
        free(frequencyRatios);
        free(waveforms);
        free(smoothedAmplitudes);
        free(smoothedPhases);
        free(smoothedFrequencyRatios);
        free(smoothedWaveforms);
        free(cycle);
        return 0;
    }

    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        int center = min(last, first + frameIndex * hop);
        double periodicity = 0.0;
        int lag = estimateHarmonicLag(source, xLen, fs, center,
            frameLength, &periodicity);
        HarmonicExcitationTemplate local = {};
        int localSuccess = lag > 0 && periodicity >= 0.45;
        if(localSuccess)
        {
            for(int i = 0; i < 512; i++)
            {
                double phase = ((double)i + 0.5) / 512.0;
                double relative = (phase - 0.5) * (double)lag;
                double sum = 0.0;
                int count = 0;
                for(int cycleIndex = -1; cycleIndex <= 1;
                    cycleIndex++)
                {
                    double position = (double)center + relative +
                        (double)cycleIndex * (double)lag;
                    if(position >= 0.0 &&
                       position <= (double)(xLen - 1))
                    {
                        sum += sampleVocalTractResidual(
                            source, xLen, position, vocalTractTrack);
                        count++;
                    }
                }
                cycle[i] = count > 0 ? sum / (double)count : 0.0;
            }
            localSuccess = fillHarmonicExcitationFromCycle(
                cycle, 512, &local);
        }

        double *destinationAmplitude = amplitudes +
            (size_t)frameIndex * (size_t)maximumHarmonics;
        double *destinationPhase = phaseOffsets +
            (size_t)frameIndex * (size_t)maximumHarmonics;
        const double *sourceAmplitude = fallback != NULL
            ? fallback->amplitudes : NULL;
        const double *sourcePhase = fallback != NULL
            ? fallback->phaseOffsets : NULL;
        int sourceCount = fallback != NULL
            ? min(maximumHarmonics, fallback->harmonicCount) : 0;

        for(int h = 0; h < maximumHarmonics; h++)
        {
            if(localSuccess && h < local.harmonicCount)
            {
                destinationAmplitude[h] = local.amplitudes[h];
                destinationPhase[h] = local.phaseOffsets[h];
            }
            else if(sourceAmplitude != NULL && h < sourceCount)
            {
                destinationAmplitude[h] = sourceAmplitude[h];
                destinationPhase[h] = sourcePhase[h];
            }
        }
        const double *waveAmplitude = localSuccess
            ? local.amplitudes : sourceAmplitude;
        const double *wavePhase = localSuccess
            ? local.phaseOffsets : sourcePhase;
        int waveCount = localSuccess
            ? min(32, min(maximumHarmonics, local.harmonicCount))
            : min(32, sourceCount);
        double mean = 0.0;
        double energy = 0.0;
        if(waveAmplitude != NULL && wavePhase != NULL)
        {
            for(int i = 0; i < 512; i++)
            {
                double phase = 2.0 * 3.14159265358979323846 *
                    ((double)i + 0.5) / 512.0;
                double value = 0.0;
                for(int h = 0; h < waveCount; h++)
                    value += waveAmplitude[h] /
                        pow((double)(h + 1), 0.10) * sin(
                        (double)(h + 1) * phase + wavePhase[h]);
                cycle[i] = value;
                mean += value;
            }
            mean /= 512.0;
            for(int i = 0; i < 512; i++)
            {
                cycle[i] -= mean;
                energy += cycle[i] * cycle[i];
            }
        }
        if(energy > 1.0e-12)
        {
            double rms = sqrt(energy / 512.0);
            for(int i = 0; i < 512; i++)
                waveforms[(size_t)frameIndex * 512 + i] =
                    cycle[i] / rms;
        }
        if(localSuccess)
            frequencyRatios[frameIndex] = (double)fs / (double)lag;
        freeHarmonicExcitationTemplate(&local);
    }
    free(cycle);

    double logFrequencySum = 0.0;
    int frequencyCount = 0;
    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        if(frequencyRatios[frameIndex] > 1.0)
        {
            logFrequencySum += log(frequencyRatios[frameIndex]);
            frequencyCount++;
        }
    }
    double referenceFrequency = frequencyCount > 0
        ? exp(logFrequencySum / (double)frequencyCount) : 1.0;
    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        double ratio = frequencyRatios[frameIndex] > 1.0
            ? frequencyRatios[frameIndex] / referenceFrequency : 1.0;
        frequencyRatios[frameIndex] = max(0.92, min(1.08, ratio));
    }

    /*
     * Smooth amplitude and phase separately. High partials use a wider
     * temporal window because a single noisy cycle can otherwise become a
     * 20 ms metallic whistle. Low partials keep the shorter window so broad
     * mouth movement remains responsive. Phase is averaged on the unit
     * circle, otherwise a harmless wrap from +pi to -pi creates a click.
     */
    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        for(int h = 0; h < maximumHarmonics; h++)
        {
            int radius = h >= 3 ? 3 : 1;
            int begin = max(0, frameIndex - radius);
            int end = min(frameCount - 1, frameIndex + radius);
            double amplitudeSum = 0.0;
            double sine = 0.0;
            double cosine = 0.0;
            double weightSum = 0.0;
            for(int j = begin; j <= end; j++)
            {
                int distance = j - frameIndex;
                if(distance < 0) distance = -distance;
                double weight = (double)(radius + 1 - distance);
                amplitudeSum += weight * amplitudes[
                    (size_t)j * maximumHarmonics + h];
                double phase = phaseOffsets[
                    (size_t)j * maximumHarmonics + h];
                sine += weight * sin(phase);
                cosine += weight * cos(phase);
                weightSum += weight;
            }
            smoothedAmplitudes[
                (size_t)frameIndex * maximumHarmonics + h] =
                amplitudeSum / max(1.0e-12, weightSum);
            smoothedPhases[
                (size_t)frameIndex * maximumHarmonics + h] =
                atan2(sine, cosine);
        }
        int frequencyRadius = 1;
        int frequencyBegin = max(0, frameIndex - frequencyRadius);
        int frequencyEnd = min(frameCount - 1,
            frameIndex + frequencyRadius);
        double frequencySum = 0.0;
        double frequencyWeightSum = 0.0;
        for(int j = frequencyBegin; j <= frequencyEnd; j++)
        {
            int distance = j - frameIndex;
            if(distance < 0) distance = -distance;
            double weight = (double)(frequencyRadius + 1 - distance);
            frequencySum += weight * frequencyRatios[j];
            frequencyWeightSum += weight;
        }
        smoothedFrequencyRatios[frameIndex] =
            frequencySum / max(1.0e-12, frequencyWeightSum);

        int waveformRadius = 2;
        int waveformBegin = max(0, frameIndex - waveformRadius);
        int waveformEnd = min(frameCount - 1,
            frameIndex + waveformRadius);
        for(int i = 0; i < 512; i++)
        {
            double waveformSum = 0.0;
            double waveformWeightSum = 0.0;
            for(int j = waveformBegin; j <= waveformEnd; j++)
            {
                int distance = j - frameIndex;
                if(distance < 0) distance = -distance;
                double weight = (double)(waveformRadius + 1 - distance);
                waveformSum += weight * waveforms[
                    (size_t)j * 512 + i];
                waveformWeightSum += weight;
            }
            smoothedWaveforms[(size_t)frameIndex * 512 + i] =
                waveformSum / max(1.0e-12, waveformWeightSum);
        }
    }

    /*
     * Preserve the average glottal shape, but make slow local changes in
     * harmonic phase audible. This is a bounded enhancement of the
     * 20 ms-period track, not a copy of cycle-to-cycle shimmer.
     */
    for(int h = 0; h < maximumHarmonics; h++)
    {
        double sine = 0.0;
        double cosine = 0.0;
        for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
        {
            double phase = smoothedPhases[
                (size_t)frameIndex * maximumHarmonics + h];
            sine += sin(phase);
            cosine += cos(phase);
        }
        double basePhase = atan2(sine, cosine);
        for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
        {
            size_t index = (size_t)frameIndex * maximumHarmonics + h;
            double delta = atan2(
                sin(smoothedPhases[index] - basePhase),
                cos(smoothedPhases[index] - basePhase));
            delta = max(-0.45, min(0.45, delta));
            smoothedPhases[index] = basePhase + delta * 1.35;
        }
    }

    free(amplitudes);
    free(phaseOffsets);
    free(frequencyRatios);
    free(waveforms);
    track->amplitudes = smoothedAmplitudes;
    track->phaseOffsets = smoothedPhases;
    track->frequencyRatios = smoothedFrequencyRatios;
    track->waveforms = smoothedWaveforms;
    track->frameCount = frameCount;
    track->harmonicCount = maximumHarmonics;
    track->hop = hop;
    track->firstPosition = first;
    return 1;
}

static void sampleHarmonicExcitationTrack(
    const HarmonicExcitationTrack *track, int sample, int harmonic,
    double *amplitude, double *phaseOffset, double *frequencyRatio)
{
    if(amplitude != NULL) *amplitude = 0.0;
    if(phaseOffset != NULL) *phaseOffset = 0.0;
    if(frequencyRatio != NULL) *frequencyRatio = 1.0;
    if(track == NULL || track->amplitudes == NULL ||
       track->phaseOffsets == NULL || track->frequencyRatios == NULL ||
       track->frameCount <= 0 ||
       track->harmonicCount <= 0 || track->hop <= 0 ||
       harmonic < 0 || harmonic >= track->harmonicCount)
        return;

    double position = (double)(sample - track->firstPosition) /
        (double)track->hop;
    position = max(0.0, min((double)(track->frameCount - 1), position));
    int left = (int)floor(position);
    int right = min(track->frameCount - 1, left + 1);
    double fraction = position - (double)left;
    int stride = track->harmonicCount;
    double leftAmplitude = track->amplitudes[
        (size_t)left * stride + harmonic];
    double rightAmplitude = track->amplitudes[
        (size_t)right * stride + harmonic];
    double leftPhase = track->phaseOffsets[
        (size_t)left * stride + harmonic];
    double rightPhase = track->phaseOffsets[
        (size_t)right * stride + harmonic];
    double sine = (1.0 - fraction) * sin(leftPhase) +
        fraction * sin(rightPhase);
    double cosine = (1.0 - fraction) * cos(leftPhase) +
        fraction * cos(rightPhase);
    double leftFrequencyRatio = track->frequencyRatios[left];
    double rightFrequencyRatio = track->frequencyRatios[right];
    if(amplitude != NULL)
        *amplitude = leftAmplitude +
            (rightAmplitude - leftAmplitude) * fraction;
    if(phaseOffset != NULL)
        *phaseOffset = atan2(sine, cosine);
    if(frequencyRatio != NULL)
        *frequencyRatio = leftFrequencyRatio +
            (rightFrequencyRatio - leftFrequencyRatio) * fraction;
}

static double sampleHarmonicExcitationWaveform(
    const HarmonicExcitationTrack *track, int sample, double phase)
{
    if(track == NULL || track->waveforms == NULL ||
       track->frameCount <= 0 || track->hop <= 0)
        return 0.0;

    double position = (double)(sample - track->firstPosition) /
        (double)track->hop;
    position = max(0.0, min((double)(track->frameCount - 1), position));
    int leftFrame = (int)floor(position);
    int rightFrame = min(track->frameCount - 1, leftFrame + 1);
    double frameFraction = position - (double)leftFrame;
    phase /= 2.0 * 3.14159265358979323846;
    phase -= floor(phase);
    double cyclePosition = phase * 512.0;
    int leftCycle = (int)floor(cyclePosition);
    int rightCycle = (leftCycle + 1) % 512;
    double cycleFraction = cyclePosition - (double)leftCycle;
    double leftWave = track->waveforms[
        (size_t)leftFrame * 512 + leftCycle] +
        (track->waveforms[(size_t)leftFrame * 512 + rightCycle] -
         track->waveforms[(size_t)leftFrame * 512 + leftCycle]) *
        cycleFraction;
    double rightWave = track->waveforms[
        (size_t)rightFrame * 512 + leftCycle] +
        (track->waveforms[(size_t)rightFrame * 512 + rightCycle] -
         track->waveforms[(size_t)rightFrame * 512 + leftCycle]) *
        cycleFraction;
    return leftWave + (rightWave - leftWave) * frameFraction;
}

static double sampleHarmonicExcitationTemplateWaveform(
    const HarmonicExcitationTemplate *source, double phase)
{
    if(source == NULL || source->amplitudes == NULL ||
       source->phaseOffsets == NULL || source->harmonicCount <= 0)
        return 0.0;

    const double pi = 3.14159265358979323846;
    phase -= floor(phase / (2.0 * pi)) * 2.0 * pi;
    int harmonicCount = min(32, source->harmonicCount);
    double value = 0.0;
    double energy = 0.0;
    for(int h = 1; h <= harmonicCount; h++)
    {
        double amplitude = source->amplitudes[h - 1] /
            pow((double)h, 0.10);
        value += amplitude * sin((double)h * phase +
            source->phaseOffsets[h - 1]);
        energy += amplitude * amplitude;
    }
    double rms = sqrt(max(1.0e-12, energy * 0.5));
    return value / rms;
}

/*
 * A fully periodic oscillator is clean, but its upper partials line up too
 * perfectly and expose the synthesis immediately. Add only a small,
 * deterministic, slowly moving variation to upper harmonics. It is not
 * derived from the recording, so hs100 still removes source shimmer and
 * noise; it only prevents every generated cycle from being identical.
 */
static double getProceduralHarmonicPhaseJitter(int harmonic, double time)
{
    if(harmonic <= 2)
        return 0.0;

    const double pi = 3.14159265358979323846;
    double band = max(0.0, min(1.0,
        ((double)harmonic - 3.0) / 18.0));
    double h = (double)harmonic;
    double slow = sin(2.0 * pi * (1.15 + 0.071 * h) *
        time + 0.83 * h);
    double medium = sin(2.0 * pi * (3.40 + 0.113 * h) *
        time + 1.91 * h);
    double fast = sin(2.0 * pi * (7.20 + 0.157 * h) *
        time + 2.37 * h);
    double noisePosition = time * (18.0 + 0.57 * h) + 0.37 * h;
    int noiseLeft = (int)floor(noisePosition);
    double noiseFraction = noisePosition - (double)noiseLeft;
    noiseFraction = noiseFraction * noiseFraction *
        (3.0 - 2.0 * noiseFraction);
    unsigned int leftState = (unsigned int)(noiseLeft * 2654435761u) ^
        (unsigned int)(harmonic * 2246822519u);
    unsigned int rightState =
        (unsigned int)((noiseLeft + 1) * 2654435761u) ^
        (unsigned int)(harmonic * 2246822519u);
    leftState = leftState * 1664525u + 1013904223u;
    rightState = rightState * 1664525u + 1013904223u;
    double randomLeft = ((double)(leftState & 0xffffu) /
        32767.5) - 1.0;
    double randomRight = ((double)(rightState & 0xffffu) /
        32767.5) - 1.0;
    double smoothNoise = randomLeft +
        (randomRight - randomLeft) * noiseFraction;
    double depth = 0.018 + 0.20 * band;
    return depth * (0.46 * slow + 0.25 * medium + 0.11 * fast +
        0.12 * smoothNoise);
}

static double getProceduralHarmonicAmplitudeModulation(
    int harmonic, double time)
{
    if(harmonic <= 2)
        return 1.0;

    const double pi = 3.14159265358979323846;
    double band = max(0.0, min(1.0,
        ((double)harmonic - 3.0) / 18.0));
    double h = (double)harmonic;
    double slow = sin(2.0 * pi * (0.82 + 0.053 * h) *
        time + 1.27 * h);
    double medium = sin(2.0 * pi * (2.65 + 0.097 * h) *
        time + 0.41 * h);
    double noisePosition = time * (16.0 + 0.43 * h) + 0.61 * h;
    int noiseLeft = (int)floor(noisePosition);
    double noiseFraction = noisePosition - (double)noiseLeft;
    noiseFraction = noiseFraction * noiseFraction *
        (3.0 - 2.0 * noiseFraction);
    unsigned int leftState = (unsigned int)(noiseLeft * 2246822519u) ^
        (unsigned int)(harmonic * 3266489917u);
    unsigned int rightState =
        (unsigned int)((noiseLeft + 1) * 2246822519u) ^
        (unsigned int)(harmonic * 3266489917u);
    leftState = leftState * 1664525u + 1013904223u;
    rightState = rightState * 1664525u + 1013904223u;
    double randomLeft = ((double)(leftState & 0xffffu) /
        32767.5) - 1.0;
    double randomRight = ((double)(rightState & 0xffffu) /
        32767.5) - 1.0;
    double smoothNoise = randomLeft +
        (randomRight - randomLeft) * noiseFraction;
    double depth = 0.010 + 0.035 * band;
    double modulation = 1.0 + depth * (0.58 * slow +
        0.22 * medium + 0.20 * smoothNoise);
    return max(0.90, min(1.10, modulation));
}

/*
 * The tract envelope, excitation spectrum, and phase offsets are analysis
 * controls, not sample-rate signals.  Keeping them at a short control hop
 * avoids evaluating the same LPC response tens of millions of times per note.
 * The oscillator phase itself remains sample-accurate below.
 */
typedef struct
{
    double *weights;
    double *phaseSines;
    double *phaseCosines;
    double *normalizers;
    int frameCount;
    int harmonicCount;
    int hop;
    int firstSample;
} HarmonicRenderControlTrack;

static void freeHarmonicRenderControlTrack(
    HarmonicRenderControlTrack *track)
{
    if(track == NULL) return;
    free(track->weights);
    free(track->phaseSines);
    free(track->phaseCosines);
    free(track->normalizers);
    track->weights = NULL;
    track->phaseSines = NULL;
    track->phaseCosines = NULL;
    track->normalizers = NULL;
    track->frameCount = 0;
    track->harmonicCount = 0;
    track->hop = 0;
    track->firstSample = 0;
}

static double sampleHarmonicCycleTable(const double *table, int length,
                                       double phase)
{
    if(table == NULL || length <= 0) return 0.0;

    const double pi = 3.14159265358979323846;
    phase /= 2.0 * pi;
    phase -= floor(phase);
    double position = phase * (double)length;
    int left = (int)floor(position);
    int right = (left + 1) % length;
    double fraction = position - (double)left;
    return table[left] + (table[right] - table[left]) * fraction;
}

static int buildHarmonicRenderControlTrack(
    HarmonicRenderControlTrack *control, int firstSample, int xLen, int fs,
    const double *frequencies, const double *tractSource,
    int tractLength, int sourceMapped, int protectedSamples,
    int sourceBoundarySample, int sourceProtectedSample,
    const VocalTractFilterTrack *vocalTractTrack,
    const HarmonicEnvelopeTrack *vowelTrack,
    const HarmonicExcitationTrack *excitationTrack,
    const HarmonicExcitationTemplate *excitationTemplate)
{
    if(control == NULL || frequencies == NULL || xLen <= 0 || fs <= 0)
        return 0;

    control->weights = NULL;
    control->phaseSines = NULL;
    control->phaseCosines = NULL;
    control->normalizers = NULL;
    control->frameCount = 0;
    control->harmonicCount = 0;
    control->hop = 0;
    control->firstSample = 0;

    const int harmonicCount = 48;
    int hop = max(64, (int)((double)fs * 0.005 + 0.5));
    firstSample = max(0, min(xLen - 1, firstSample));
    int frameCount = 1 + (xLen - 1 - firstSample + hop - 1) / hop;
    size_t valueCount = (size_t)frameCount * (size_t)harmonicCount;
    double *weights = (double *)calloc(valueCount, sizeof(double));
    double *phaseSines = (double *)calloc(valueCount, sizeof(double));
    double *phaseCosines = (double *)calloc(valueCount, sizeof(double));
    double *normalizers = (double *)calloc((size_t)frameCount,
        sizeof(double));
    if(weights == NULL || phaseSines == NULL || phaseCosines == NULL ||
       normalizers == NULL)
    {
        free(weights);
        free(phaseSines);
        free(phaseCosines);
        free(normalizers);
        return 0;
    }

    int hasVocalTractTrack = vocalTractTrack != NULL &&
        vocalTractTrack->coefficients != NULL;
    int hasVowelTrack = vowelTrack != NULL && vowelTrack->values != NULL;
    int hasExcitationTrack = excitationTrack != NULL &&
        excitationTrack->amplitudes != NULL;
    int hasExcitationTemplate = excitationTemplate != NULL &&
        excitationTemplate->amplitudes != NULL;
    int filterOrder = hasVocalTractTrack
        ? min(16, vocalTractTrack->order) : 0;
    const double pi = 3.14159265358979323846;

    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        int sample = min(xLen - 1, firstSample + frameIndex * hop);
        int tractSample = sample;
        if(sourceMapped && tractLength > 0)
        {
            double mapped = mapHarmonicSourcePosition(sample, xLen,
                protectedSamples, sourceBoundarySample,
                sourceProtectedSample, tractLength - 1);
            tractSample = max(0, min(tractLength - 1,
                (int)(mapped + 0.5)));
        }

        double frequency = max(45.0, min(2200.0, frequencies[sample]));
        double tractReference = hasVocalTractTrack
            ? getVocalTractReference(vocalTractTrack, tractSample,
                filterOrder, fs)
            : 1.0;
        double baseExcitationAmplitude = 0.0;
        if(hasExcitationTrack)
        {
            sampleHarmonicExcitationTrack(excitationTrack, tractSample,
                0, &baseExcitationAmplitude, NULL, NULL);
        }
        else if(hasExcitationTemplate &&
                excitationTemplate->harmonicCount > 0)
        {
            baseExcitationAmplitude = excitationTemplate->amplitudes[0];
        }

        double sumSquares = 0.0;
        double time = (double)sample / (double)fs;
        for(int h = 1; h <= harmonicCount; h++)
        {
            int harmonic = h - 1;
            double harmonicFrequency = frequency * (double)h;
            double highBandGain = 1.0;
            const double highBandStart = 9000.0;
            const double highBandEnd = 15000.0;
            if(harmonicFrequency > highBandStart)
            {
                double fraction = (harmonicFrequency - highBandStart) /
                    (highBandEnd - highBandStart);
                fraction = max(0.0, min(1.0, fraction));
                highBandGain = 0.5 + 0.5 * cos(pi * fraction);
            }
            if(harmonicFrequency >= highBandEnd)
                highBandGain = 0.0;

            const double antiAliasStart = 0.36 * (double)fs;
            const double antiAliasEnd = 0.48 * (double)fs;
            if(harmonicFrequency > antiAliasStart)
            {
                double fraction = (harmonicFrequency - antiAliasStart) /
                    (antiAliasEnd - antiAliasStart);
                fraction = max(0.0, min(1.0, fraction));
                highBandGain *= 0.5 + 0.5 * cos(pi * fraction);
            }
            if(harmonicFrequency >= antiAliasEnd)
                highBandGain = 0.0;

            double excitationAmplitude = 0.0;
            double phaseOffset = 0.0;
            bool useLocalExcitation = hasExcitationTrack &&
                (h <= 6 || harmonicFrequency < 1800.0);
            if(useLocalExcitation)
            {
                sampleHarmonicExcitationTrack(excitationTrack, tractSample,
                    harmonic, &excitationAmplitude, &phaseOffset, NULL);
            }
            else if(hasExcitationTemplate &&
                    h <= excitationTemplate->harmonicCount)
            {
                excitationAmplitude =
                    excitationTemplate->amplitudes[harmonic];
                phaseOffset = excitationTemplate->phaseOffsets[harmonic];
            }
            /*
             * This small floor is only for vowel intelligibility. Extending
             * it into the presence band made some syllables (notably /me/)
             * acquire a sharp synthetic edge even though their source did not
             * have a stable high-frequency vowel component.
             */
            if(h >= 2 && harmonicFrequency <= 3800.0 &&
               baseExcitationAmplitude > 1.0e-5)
            {
                double clarityFloor = 0.060 * baseExcitationAmplitude /
                    pow((double)h, 0.85);
                excitationAmplitude = max(excitationAmplitude, clarityFloor);
            }
            if(excitationAmplitude <= 1.0e-5 ||
               highBandGain <= 1.0e-5)
                continue;

            double formant = 1.0;
            double spectralPresence = 1.0;
            if(hasVocalTractTrack)
            {
                double response = getVocalTractMagnitudeStable(
                    vocalTractTrack, tractSample, filterOrder,
                    harmonicFrequency, fs);
                response /= max(0.05, tractReference);
                double tractFormant = 0.68 + 0.32 * pow(
                    max(0.45, min(2.20, response)), 0.60);
                if(hasVowelTrack)
                {
                    double localReference = 1.0;
                    double spectral = sampleHarmonicEnvelopeTrack(
                        vowelTrack, tractSample, fs, harmonicFrequency,
                        &localReference);
                    /*
                     * The broad FFT envelope is the direct record of mouth
                     * shape. Give it a little more influence in the vowel
                     * bands than LPC, whose intentionally damped poles are
                     * safer but can make different vowels converge.
                     */
                    double spectralFloor = harmonicFrequency > 3000.0
                        ? 0.12 : 0.35;
                    double spectralFormant = pow(max(spectralFloor, min(2.2,
                        spectral / localReference)), 0.62);
                    spectralPresence = spectralFormant;
                    double spectralWeight = 0.70;
                    if(harmonicFrequency > 3000.0)
                    {
                        double highFraction = (harmonicFrequency - 3000.0) /
                            3000.0;
                        highFraction = max(0.0, min(1.0, highFraction));
                        /*
                         * Above the vowel-formant band, the damped LPC
                         * response can still carry a broad but artificial
                         * brightness. Prefer the heavily time-smoothed
                         * recording envelope there: it is not a copy of
                         * shimmer, but it does say whether this vowel has
                         * sustained presence energy at all.
                         */
                        spectralWeight += 0.25 * highFraction;
                    }
                    formant = (1.0 - spectralWeight) * tractFormant +
                        spectralWeight * spectralFormant;
                }
                else
                {
                    formant = tractFormant;
                }
            }
            else if(hasVowelTrack)
            {
                double localReference = 1.0;
                double spectral = sampleHarmonicEnvelopeTrack(
                    vowelTrack, tractSample, fs, harmonicFrequency,
                    &localReference);
                formant = pow(max(0.12, min(4.0,
                    spectral / localReference)), 0.38);
                spectralPresence = formant;
            }

            double proceduralAmplitude = hasExcitationTrack ? 1.0 :
                getProceduralHarmonicAmplitudeModulation(h, time);
            double proceduralPhase = hasExcitationTrack ? 0.0 :
                getProceduralHarmonicPhaseJitter(h, time);
            double presenceShelf = 1.0;
            if(harmonicFrequency > 2200.0)
            {
                double fraction = (harmonicFrequency - 2200.0) / 1300.0;
                fraction = max(0.0, min(1.0, fraction));
                /*
                 * Do not add presence just because a harmonic happens to
                 * land in this band. A vowel with sustained high-band energy
                 * keeps it; a noisy or dark vowel lets the clean layer taper
                 * it away instead of becoming piercing.
                 */
                double minimumShelf = 1.0 - 0.65 * fraction;
                double sustainedPresence = max(0.0, min(1.0,
                    spectralPresence));
                /*
                 * Extreme vocals can retain broadband distortion even after
                 * the temporal smoothing above. Past the presence band, do
                 * not let that distortion fully cancel the clean-layer
                 * shelf. The cap still leaves a controlled bright component,
                 * but prevents a syllable such as /me/ becoming piercing.
                 */
                if(harmonicFrequency > 3500.0)
                    sustainedPresence = min(0.45, sustainedPresence);
                presenceShelf = minimumShelf +
                    (1.0 - minimumShelf) * sustainedPresence;
            }
            double weight = excitationAmplitude /
                pow((double)h, 0.16) * formant * presenceShelf *
                highBandGain * proceduralAmplitude;
            if(h >= 4 && baseExcitationAmplitude > 1.0e-5)
            {
                double concentrationLimit = baseExcitationAmplitude *
                    0.72 / pow((double)h, 0.18);
                if(weight > concentrationLimit)
                    weight = concentrationLimit +
                        0.25 * (weight - concentrationLimit);
            }

            size_t index = (size_t)frameIndex * harmonicCount + harmonic;
            weights[index] = weight;
            phaseSines[index] = sin(phaseOffset + proceduralPhase);
            phaseCosines[index] = cos(phaseOffset + proceduralPhase);
            sumSquares += weight * weight;
        }
        normalizers[frameIndex] = sqrt(max(1.0e-12,
            sumSquares * 0.5));
    }

    /*
     * The high bands do not encode instantaneous mouth position. Smooth them
     * in the control domain, so a single noisy analysis frame cannot create a
     * brief bright-metallic flash in the rendered vowel.
     */
    for(int h = 0; h < harmonicCount; h++)
    {
        int radius = h >= 15 ? 6 : (h >= 7 ? 3 : 1);
        double *smoothed = (double *)malloc(sizeof(double) *
            (size_t)frameCount);
        if(smoothed == NULL) continue;
        for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
        {
            int begin = max(0, frameIndex - radius);
            int end = min(frameCount - 1, frameIndex + radius);
            double sum = 0.0;
            double weightSum = 0.0;
            for(int j = begin; j <= end; j++)
            {
                double weight = (double)(radius + 1 - abs(j - frameIndex));
                sum += weight * weights[
                    (size_t)j * harmonicCount + h];
                weightSum += weight;
            }
            smoothed[frameIndex] = sum / max(1.0e-12, weightSum);
        }
        for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
            weights[(size_t)frameIndex * harmonicCount + h] =
                smoothed[frameIndex];
        free(smoothed);
    }
    for(int frameIndex = 0; frameIndex < frameCount; frameIndex++)
    {
        double sumSquares = 0.0;
        for(int h = 0; h < harmonicCount; h++)
        {
            double weight = weights[(size_t)frameIndex * harmonicCount + h];
            sumSquares += weight * weight;
        }
        normalizers[frameIndex] = sqrt(max(1.0e-12,
            sumSquares * 0.5));
    }

    control->weights = weights;
    control->phaseSines = phaseSines;
    control->phaseCosines = phaseCosines;
    control->normalizers = normalizers;
    control->frameCount = frameCount;
    control->harmonicCount = harmonicCount;
    control->hop = hop;
    control->firstSample = firstSample;
    return 1;
}

static int compareHarmonicRms(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double harmonicStableRms(const double *source, int xLen, int fs,
                                int startSample)
{
    if(source == NULL || xLen <= 0 || fs <= 0)
        return 0.0;

    int frameRadius = max(1, (int)((double)fs * 0.010 + 0.5));
    int hop = max(1, (int)((double)fs * 0.005 + 0.5));
    int start = max(0, min(xLen - 1, startSample));
    int maximumCount = max(1, (xLen - start + hop - 1) / hop);
    double *values = (double *)malloc(sizeof(double) *
        (size_t)maximumCount);
    if(values == NULL) return 0.0;

    double maximum = 0.0;
    for(int center = start; center < xLen; center += hop)
    {
        maximum = max(maximum,
            harmonicLocalRms(source, xLen, center, frameRadius));
    }
    if(maximum <= 1.0e-9)
    {
        free(values);
        return maximum;
    }

    int count = 0;
    for(int center = start; center < xLen; center += hop)
    {
        double rms = harmonicLocalRms(source, xLen, center, frameRadius);
        if(rms >= maximum * 0.30)
        {
            if(count < maximumCount) values[count++] = rms;
        }
    }

    if(count <= 0)
    {
        free(values);
        return maximum * 0.65;
    }

    qsort(values, (size_t)count, sizeof(double),
        compareHarmonicRms);
    int begin = count / 5;
    int end = count - begin;
    double sum = 0.0;
    int trimmedCount = 0;
    for(int i = begin; i < end; i++)
    {
        sum += values[i];
        trimmedCount++;
    }
    double result = trimmedCount > 0
        ? sum / (double)trimmedCount
        : values[count / 2];
    free(values);
    return result;
}

static int buildHarmonicPhaseOffsets(const double *source, int xLen, int fs,
                                     int analysisStart, int originSample,
                                     double referenceFrequency,
                                     double *phaseOffsets, int maxHarmonics)
{
    if(source == NULL || xLen < 64 || fs <= 0 ||
       phaseOffsets == NULL || maxHarmonics <= 0 ||
       referenceFrequency <= 1.0)
        return 0;

    int frameLength = min(2048, xLen);
    frameLength = max(64, frameLength);
    if(frameLength > xLen) frameLength = xLen;
    int center = max(frameLength / 2,
        min(xLen - frameLength / 2 - 1,
            analysisStart + frameLength / 2));
    int begin = center - frameLength / 2;
    double mean = 0.0;
    for(int i = 0; i < frameLength; i++)
        mean += source[begin + i];
    mean /= (double)frameLength;

    int count = 0;
    for(int h = 1; h <= maxHarmonics; h++)
    {
        double frequency = referenceFrequency * (double)h;
        if(frequency >= (double)fs * 0.45)
            break;

        double sineProjection = 0.0;
        double cosineProjection = 0.0;
        double windowEnergy = 0.0;
        for(int i = 0; i < frameLength; i++)
        {
            double relative = (double)(begin + i - center);
            double window = 0.5 - 0.5 * cos(
                2.0 * 3.14159265358979323846 *
                (double)(i + 1) / (double)(frameLength + 1));
            double value = (source[begin + i] - mean) * window;
            double angle = 2.0 * 3.14159265358979323846 *
                frequency * relative / (double)fs;
            sineProjection += value * sin(angle);
            cosineProjection += value * cos(angle);
            windowEnergy += value * value;
        }

        double projection = sqrt(sineProjection * sineProjection +
            cosineProjection * cosineProjection);
        if(projection > sqrt(max(1.0e-12, windowEnergy)) * 0.002)
        {
            /*
             * The projections are measured at the analysis centre. Convert
             * that phase to the oscillator's origin so every harmonic starts
             * with a source-informed waveform shape rather than a
             * phase-locked synthetic sawtooth.
             */
            double phaseAtCentre = atan2(cosineProjection,
                sineProjection);
            phaseOffsets[h - 1] = phaseAtCentre +
                2.0 * 3.14159265358979323846 * frequency *
                (double)(center - originSample) / (double)fs;
        }
        else
        {
            phaseOffsets[h - 1] = 0.0;
        }
        count++;
    }
    return count;
}

static double harmonicPeriodicity(const double *source, int xLen,
                                  int center, int frameLength, int fs)
{
    if(source == NULL || xLen <= 0 || frameLength < 8 || fs <= 0)
        return 0.0;

    int left = center - frameLength / 2;
    if(left < 0 || left + frameLength >= xLen)
        return 0.0;

    int minLag = max(1, (int)((double)fs / 900.0 + 0.5));
    int maxLag = min(frameLength / 2,
        max(minLag + 1, (int)((double)fs / 55.0 + 0.5)));
    double best = 0.0;
    for(int lag = minLag; lag <= maxLag; lag++)
    {
        double correlation = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        int count = frameLength - lag;
        for(int i = 0; i < count; i++)
        {
            double a = source[left + i];
            double b = source[left + i + lag];
            correlation += a * b;
            leftEnergy += a * a;
            rightEnergy += b * b;
        }
        if(leftEnergy > 1.0e-12 && rightEnergy > 1.0e-12)
        {
            double normalized = correlation /
                sqrt(leftEnergy * rightEnergy);
            best = max(best, normalized);
        }
    }
    return max(0.0, min(1.0, best));
}

static int estimateHarmonicBoundaryMs(int fs, int fallbackOutputMs)
{
    if(g_harmonicSource == NULL || g_harmonicSourceLength <= 0 ||
       g_harmonicSourceFs != fs || fs <= 0)
        return max(0, fallbackOutputMs);

    int sourceStart = (int)((double)g_harmonicSourceBoundaryMs *
        (double)fs / 1000.0 + 0.5);
    sourceStart = max(0, min(g_harmonicSourceLength - 1, sourceStart));

    int consonantMs = 0;
    if(__argc > 8 && __argv != NULL && __argv[8] != NULL)
        consonantMs = max(0, atoi(__argv[8]));

    double stretch = 1.0;
    if(consonantMs > 0 && g_harmonicOutputBoundaryMs > 0)
        stretch = (double)g_harmonicOutputBoundaryMs /
            (double)consonantMs;
    stretch = max(0.25, min(8.0, stretch));

    int sourceSearchLength;
    if(fallbackOutputMs > 0)
    {
        sourceSearchLength = max(1, (int)(
            (double)fallbackOutputMs / stretch * (double)fs / 1000.0 +
            0.5));
    }
    else
    {
        /*
         * A vowel-only oto can have a zero consonant length.  The old
         * early-return made the detector return zero in that case, so hs
         * started synthesizing before the actual vowel onset.
         */
        sourceSearchLength = max(1, g_harmonicSourceLength - sourceStart);
    }
    int frameLength = max(32, (int)((double)fs * 0.020 + 0.5));
    int hop = max(16, (int)((double)fs * 0.010 + 0.5));
    int firstCenter = sourceStart + frameLength / 2;
    int lastCenter = min(g_harmonicSourceLength - frameLength / 2 - 1,
        sourceStart + sourceSearchLength);
    if(lastCenter <= firstCenter)
        return max(0, fallbackOutputMs);

    double maximumRms = 0.0;
    for(int center = firstCenter; center <= lastCenter; center += hop)
        maximumRms = max(maximumRms,
            harmonicLocalRms(g_harmonicSource, g_harmonicSourceLength,
                center, frameLength / 2));
    if(maximumRms < 1.0e-6)
        return max(0, fallbackOutputMs);

    /*
     * First recognise a clearly voiced onset. This is useful for vowel-only
     * recordings such as あ, whose OpenUtau red line can be very close to the
     * beginning. A strong periodicity run is required so a noisy consonant is
     * not mistaken for a vowel.
     */
    int voicedRun = 0;
    int voicedCenter = -1;
    int voicedLastCenter = min(lastCenter,
        firstCenter + max(hop, sourceSearchLength * 45 / 100));
    for(int center = firstCenter; center <= voicedLastCenter; center += hop)
    {
        double rms = harmonicLocalRms(g_harmonicSource,
            g_harmonicSourceLength, center, frameLength / 2);
        double periodicity = harmonicPeriodicity(g_harmonicSource,
            g_harmonicSourceLength, center, frameLength, fs);
        bool voiced = periodicity >= 0.65 &&
            rms >= maximumRms * 0.12;
        voicedRun = voiced ? voicedRun + 1 : 0;
        if(voicedRun >= 3)
        {
            voicedCenter = center;
            break;
        }
    }

    if(voicedCenter >= 0)
    {
        int sourceOffset = max(0, voicedCenter - sourceStart);
        int outputMs = (int)((double)sourceOffset * 1000.0 /
            (double)fs * stretch + 0.5);
        int outputLimit = fallbackOutputMs > 0
            ? fallbackOutputMs : 0x3fffffff;
        return max(12, min(outputLimit, outputMs));
    }

    /*
     * For consonant-led recordings, the exact red line (preutterance) is not
     * included in the command-line arguments sent by OpenUtau. The consonant
     * boundary is available, so use a conservative fraction of it as a
     * fallback and never start harmonic replacement earlier than that. This
     * is much closer to the red line than treating the whole consonant
     * boundary as the red line.
     */
    int energyRun = 0;
    int detectedCenter = -1;
    for(int center = firstCenter; center <= lastCenter; center += hop)
    {
        double rms = harmonicLocalRms(g_harmonicSource,
            g_harmonicSourceLength, center, frameLength / 2);
        bool energetic = rms >= maximumRms * 0.20;
        energyRun = energetic ? energyRun + 1 : 0;
        if(energyRun >= 3)
        {
            detectedCenter = center;
            break;
        }
    }

    if(detectedCenter < 0)
        return max(0, fallbackOutputMs);

    int sourceOffset = max(0, detectedCenter - sourceStart);
    int outputMs = (int)((double)sourceOffset * 1000.0 /
        (double)fs * stretch + 0.5);
    int conservativeBoundary = (int)((double)consonantMs * 0.52 *
        stretch + 0.5);
    outputMs = max(outputMs, conservativeBoundary);
    outputMs = max(12, min(fallbackOutputMs, outputMs));
    return outputMs;
}

static void applyHarmonicCleaning(double *output, int xLen, int fs,
                                  const double *f0, int tLen,
                                  double framePeriod,
                                  double fallbackFrequency,
                                  int strength)
{
    if(output == NULL || xLen <= 0 || fs <= 0 || strength <= 0)
        return;

    double *source = (double *)malloc(sizeof(double) * (size_t)xLen);
    if(source == NULL) return;
    for(int i = 0; i < xLen; i++) source[i] = output[i];

    /*
     * argv[8] is the oto consonant length, not the red line shown by
     * OpenUtau. Estimate the end of the unstable onset from the original
     * recording instead. This keeps consonants and the changing vowel head
     * intact without requiring OpenUtau to pass preutterance separately.
     */
    int boundaryMs = estimateHarmonicBoundaryMs(fs,
        max(0, g_harmonicOutputBoundaryMs));
    boundaryMs += getHarmonicBoundaryOffsetMs();
    boundaryMs = max(0, min(
        (int)((double)xLen * 1000.0 / (double)fs + 0.5),
        boundaryMs));
    int protectedSamples = (int)((double)boundaryMs *
        (double)fs / 1000.0 + 0.5);
    protectedSamples = max(0, min(xLen, protectedSamples));
    int transitionSamples = max(1, (int)((double)fs * 0.020 + 0.5));
    int transitionStart = max(0, protectedSamples - transitionSamples);
    int sourceBoundarySample = 0;
    int sourceProtectedSample = 0;
    double sourceStretch = 1.0;

    double *vowelEnvelope = NULL;
    int vowelFftLength = 0;
    int analysisStart = min(xLen,
        protectedSamples + max(transitionSamples, (int)((double)fs * 0.030)));
    const double *envelopeSource = source;
    int envelopeLength = xLen;
    int envelopeStart = analysisStart;
    if(g_harmonicSource != NULL && g_harmonicSourceLength > 0 &&
       g_harmonicSourceFs == fs)
    {
        envelopeSource = g_harmonicSource;
        envelopeLength = g_harmonicSourceLength;
        sourceBoundarySample = (int)((double)g_harmonicSourceBoundaryMs *
            (double)fs / 1000.0 + 0.5);
        int consonantMs = 0;
        if(__argc > 8 && __argv != NULL && __argv[8] != NULL)
            consonantMs = max(0, atoi(__argv[8]));
        sourceStretch = consonantMs > 0 &&
            g_harmonicOutputBoundaryMs > 0
            ? (double)g_harmonicOutputBoundaryMs /
              (double)consonantMs
            : 1.0;
        sourceStretch = max(0.25, min(8.0, sourceStretch));
        sourceProtectedSample = (int)((double)boundaryMs /
            sourceStretch *
            (double)fs / 1000.0 + 0.5);
        sourceBoundarySample = max(0, min(
            g_harmonicSourceLength - 1, sourceBoundarySample));
        sourceProtectedSample = max(sourceBoundarySample,
            min(g_harmonicSourceLength - 1, sourceBoundarySample +
                sourceProtectedSample));
        envelopeStart = sourceBoundarySample +
            (sourceProtectedSample - sourceBoundarySample) +
            max(transitionSamples, (int)((double)fs * 0.030));
    }
    int hasVowelEnvelope = buildVowelSpectralEnvelope(envelopeSource,
        envelopeLength, fs, envelopeStart, &vowelEnvelope,
        &vowelFftLength);
    /*
     * The dynamic track is a magnitude-only description of the recording.
     * It is used as a broad mouth/formant envelope, so it contains no
     * original waveform phase or residual noise.
     */
    HarmonicEnvelopeTrack vowelTrack;
    int hasVowelTrack = buildVowelSpectralEnvelopeTrack(
        envelopeSource, envelopeLength, fs, envelopeStart, &vowelTrack);
    const double *tractSource = source;
    int tractLength = xLen;
    int tractAnalysisStart = analysisStart;
    if(g_harmonicSource != NULL && g_harmonicSourceLength > 0 &&
       g_harmonicSourceFs == fs)
    {
        /*
         * W-1 is a texture reconstruction, not a reliable vocal-tract
         * analysis signal.  LPC must see the recording, otherwise its poles
         * follow residual holes and spikes and the generated vowel inherits
         * those level dips.
         */
        tractSource = g_harmonicSource;
        tractLength = g_harmonicSourceLength;
        tractAnalysisStart = sourceProtectedSample +
            max(transitionSamples, (int)((double)fs * 0.030));
    }
    VocalTractFilterTrack vocalTractTrack = {};
    int hasVocalTractTrack = buildVocalTractFilterTrack(
        tractSource, tractLength, fs, tractAnalysisStart,
        &vocalTractTrack);
    HarmonicExcitationTemplate excitationTemplate = {};
    int hasExcitationTemplate = buildHarmonicExcitationTemplate(
        tractSource, tractLength, fs, tractAnalysisStart,
        hasVocalTractTrack ? &vocalTractTrack : NULL,
        &excitationTemplate);
    HarmonicExcitationTrack excitationTrack = {};
    int hasExcitationTrack = buildHarmonicExcitationTrack(
        tractSource, tractLength, fs, tractAnalysisStart,
        hasVocalTractTrack ? &vocalTractTrack : NULL,
        hasExcitationTemplate ? &excitationTemplate : NULL,
        &excitationTrack);
    double envelopeReference = 1.0;
    if(hasVowelEnvelope)
    {
        double sum = 0.0;
        int count = 0;
        int lowBin = max(1, (int)(250.0 * vowelFftLength / fs));
        int highBin = min(vowelFftLength / 2,
            (int)(8000.0 * vowelFftLength / fs));
        for(int k = lowBin; k <= highBin; k++)
        {
            sum += vowelEnvelope[k];
            count++;
        }
        envelopeReference = count > 0 ? sum / (double)count : 1.0;
    if(envelopeReference < 1.0e-12) envelopeReference = 1.0;
    }

    /*
     * W-1 deliberately leaves the analysed source F0 at zero.  The
     * harmonic layer still needs a musical target in that mode, otherwise
     * it can follow the same 500 Hz fallback for every piano-roll note and
     * it cannot react to pitch bends.  When a valid source F0 exists below,
     * that source contour remains authoritative; this target is only the
     * fallback for unvoiced/unknown frames.
     */
    double *targetFrequency = (double *)malloc(
        sizeof(double) * (size_t)xLen);
    if(targetFrequency == NULL)
    {
        free(vowelEnvelope);
        freeHarmonicEnvelopeTrack(&vowelTrack);
        freeVocalTractFilterTrack(&vocalTractTrack);
        freeHarmonicExcitationTrack(&excitationTrack);
        freeHarmonicExcitationTemplate(&excitationTemplate);
        free(source);
        return;
    }
    buildHarmonicTargetFrequency(targetFrequency, xLen, fs,
        fallbackFrequency);
    double *harmonicOutput = (double *)calloc((size_t)xLen,
        sizeof(double));
    double *waveformOutput = (double *)calloc((size_t)xLen,
        sizeof(double));
    double *harmonicTargetRms = (double *)calloc((size_t)xLen,
        sizeof(double));
    double *sourceEnergyPrefix = NULL;
    if(g_harmonicSource != NULL && g_harmonicSourceLength > 0 &&
       g_harmonicSourceFs == fs)
    {
        sourceEnergyPrefix = (double *)calloc(
            (size_t)g_harmonicSourceLength + 1, sizeof(double));
        if(sourceEnergyPrefix != NULL)
        {
            for(int i = 0; i < g_harmonicSourceLength; i++)
                sourceEnergyPrefix[i + 1] = sourceEnergyPrefix[i] +
                    g_harmonicSource[i] * g_harmonicSource[i];
        }
    }
    if(harmonicOutput == NULL || waveformOutput == NULL ||
       harmonicTargetRms == NULL)
    {
        free(harmonicTargetRms);
        free(targetFrequency);
        free(waveformOutput);
        free(sourceEnergyPrefix);
        free(vowelEnvelope);
        freeHarmonicEnvelopeTrack(&vowelTrack);
        freeVocalTractFilterTrack(&vocalTractTrack);
        freeHarmonicExcitationTrack(&excitationTrack);
        freeHarmonicExcitationTemplate(&excitationTemplate);
        free(source);
        return;
    }

    /*
     * The harmonic layer follows the source's changing mouth/formant shape,
     * but the original waveform is still treated as removable material.
     * At hs100 the result must not re-introduce the shimmer and noise that
     * this feature is intended to clean up.
     */
    const double pi = 3.14159265358979323846;
    /*
     * The extracted cycle tables use phase 0 at the beginning of the
     * analysis cycle and phase pi at its centre. Starting every replacement
     * at phase 0 creates the same artificial edge at the hs hand-over. Use
     * the cycle centre as the initial origin so the attack follows the
     * analysed waveform shape instead of a fixed oscillator reset.
     */
    double phase = hasExcitationTrack ? pi : 0.0;
    int rmsRadius = max(1, (int)((double)fs * 0.012 + 0.5));
    double stableRms = harmonicStableRms(source, xLen, fs, analysisStart);
    /*
     * W-1 can contain isolated empty or explosive residual frames.  They
     * must not become the level contour of the replacement layer.  Use the
     * original recording only as a fallback for the absolute reference;
     * the rendered signal keeps the old overall gain staging.
     */
    if(g_harmonicSource != NULL && g_harmonicSourceLength > 0 &&
       g_harmonicSourceFs == fs)
    {
        int sourceBoundary = (int)((double)g_harmonicSourceBoundaryMs *
            (double)fs / 1000.0 + 0.5);
        int consonantMs = 0;
        if(__argc > 8 && __argv != NULL && __argv[8] != NULL)
            consonantMs = max(0, atoi(__argv[8]));
        double stretch = consonantMs > 0 &&
            g_harmonicOutputBoundaryMs > 0
            ? (double)g_harmonicOutputBoundaryMs /
              (double)consonantMs
            : 1.0;
        stretch = max(0.25, min(8.0, stretch));
        int sourceProtected = (int)((double)boundaryMs / stretch *
            (double)fs / 1000.0 + 0.5);
        int sourceLevelStart = sourceBoundary + sourceProtected +
            max(transitionSamples, (int)((double)fs * 0.030 + 0.5));
        double originalRms = harmonicStableRms(g_harmonicSource,
            g_harmonicSourceLength, fs, sourceLevelStart);
        if(originalRms > 1.0e-9)
        {
            /*
             * The W-1 signal is already an overlap-add reconstruction and
             * its absolute level can be dominated by one residual spike.
             * The recording is the only stable reference for hs loudness.
             */
            stableRms = originalRms;
        }
    }
    if(stableRms <= 1.0e-9)
        stableRms = harmonicLocalRms(source, xLen,
            max(0, min(xLen - 1, analysisStart)), rmsRadius);

    /*
     * Use one stable vowel level for the harmonic excitation.  The previous
     * version derived this value at every output sample from W-1's local RMS.
     * That made a residual gap become a hole in the clean layer, and made an
     * isolated W-1 attack become an unnaturally loud vowel head.  The onset is
     * already handled by the protected/transition interval above; after that
     * interval the replacement layer should have a steady level.
     */
    double targetRms = stableRms * 0.82;
    for(int i = 0; i < xLen; i++)
    {
        double transition = 0.0;
        if(i >= transitionStart)
        {
            transition = i < protectedSamples
                ? (double)(i - transitionStart) /
                  (double)max(1, transitionSamples)
                : 1.0;
            transition = max(0.0, min(1.0, transition));
            transition = transition * transition *
                (3.0 - 2.0 * transition);
        }
        harmonicTargetRms[i] = targetRms * transition;
    }

    /*
     * Keep the rendered prefix untouched. Retiming the raw recording into
     * this protected area can move a consonant burst to the wrong output
     * time, making hs100 produce a short metallic attack before the harmonic
     * layer has even taken over. The raw recording is still used below only
     * for the intentional consonant-to-vowel transition.
     */

    double referenceFrequency = targetFrequency[
        max(0, min(xLen - 1, transitionStart))];
    double smoothedFrequency = referenceFrequency;
    double frequencyFollower = 1.0 - exp(-1.0 /
        max(1.0, (double)fs * 0.012));
    double *renderFrequency = (double *)calloc((size_t)xLen,
        sizeof(double));
    if(renderFrequency != NULL)
    {
        for(int i = transitionStart; i < xLen; i++)
        {
            double frequency = getHarmonicFrequencyAt(f0, tLen, i, xLen,
                fs, framePeriod, targetFrequency[i]);
            if(smoothedFrequency <= 1.0)
                smoothedFrequency = frequency;
            else
                smoothedFrequency += (frequency - smoothedFrequency) *
                    frequencyFollower;
            frequency = smoothedFrequency;
            if(hasExcitationTrack)
            {
                int tractSample = i;
                if(tractSource == g_harmonicSource &&
                   g_harmonicSource != NULL &&
                   g_harmonicSourceLength > 0)
                {
                    double mapped = mapHarmonicSourcePosition(i, xLen,
                        protectedSamples, sourceBoundarySample,
                        sourceProtectedSample,
                        g_harmonicSourceLength - 1);
                    tractSample = (int)(mapped + 0.5);
                }
                double sourceFrequencyRatio = 1.0;
                sampleHarmonicExcitationTrack(&excitationTrack, tractSample,
                    0, NULL, NULL, &sourceFrequencyRatio);
                frequency *= pow(max(0.92, min(1.08,
                    sourceFrequencyRatio)), 0.22);
            }
            renderFrequency[i] = frequency;
        }
    }
    smoothedFrequency = referenceFrequency;
    HarmonicRenderControlTrack renderControl = {};
    int hasRenderControl = renderFrequency != NULL &&
        buildHarmonicRenderControlTrack(&renderControl, transitionStart,
            xLen, fs, renderFrequency, tractSource, tractLength,
            tractSource == g_harmonicSource &&
                g_harmonicSource != NULL &&
                g_harmonicSourceLength > 0,
            protectedSamples, sourceBoundarySample, sourceProtectedSample,
            &vocalTractTrack, &vowelTrack, &excitationTrack,
            &excitationTemplate);
    double templateWaveform[512] = {};
    if(hasExcitationTemplate)
    {
        for(int i = 0; i < 512; i++)
        {
            double tablePhase = 2.0 * pi * ((double)i + 0.5) / 512.0;
            templateWaveform[i] = sampleHarmonicExcitationTemplateWaveform(
                &excitationTemplate, tablePhase);
        }
    }
    double upperHarmonicAttackSamples = max(1.0,
        (double)fs * 0.040);
    double textureAlpha = 1.0 - exp(-2.0 * pi * 1800.0 /
        (double)fs);
    for(int i = transitionStart; i < xLen; i++)
    {
        if(hasRenderControl)
        {
            double frequency = renderFrequency[i];
            int tractSample = i;
            if(tractSource == g_harmonicSource &&
               g_harmonicSource != NULL &&
               g_harmonicSourceLength > 0)
            {
                double mapped = mapHarmonicSourcePosition(i, xLen,
                    protectedSamples, sourceBoundarySample,
                    sourceProtectedSample, g_harmonicSourceLength - 1);
                tractSample = (int)(mapped + 0.5);
            }
            phase += 2.0 * pi * frequency / (double)fs;
            if(phase > 2.0 * pi) phase -= 2.0 * pi;

            double time = (double)i / (double)fs;
            double naturalPhase = phase +
                0.014 * sin(2.0 * pi * 1.31 * time + 0.37) +
                0.006 * sin(2.0 * pi * 2.47 * time + 1.19);
            double localExcitation = hasExcitationTrack
                ? sampleHarmonicExcitationWaveform(
                    &excitationTrack, tractSample, naturalPhase)
                : 0.0;
            double stableExcitation = hasExcitationTemplate
                ? sampleHarmonicCycleTable(templateWaveform, 512,
                    naturalPhase)
                : localExcitation;
            double naturalExcitation = hasExcitationTrack &&
                hasExcitationTemplate
                ? 0.15 * localExcitation + 0.85 * stableExcitation
                : localExcitation;

            double position = (double)(i - renderControl.firstSample) /
                (double)renderControl.hop;
            position = max(0.0, min((double)(renderControl.frameCount - 1),
                position));
            int leftFrame = (int)floor(position);
            int rightFrame = min(renderControl.frameCount - 1,
                leftFrame + 1);
            double fraction = position - (double)leftFrame;
            double normalizer = renderControl.normalizers[leftFrame] +
                (renderControl.normalizers[rightFrame] -
                 renderControl.normalizers[leftFrame]) * fraction;
            double baseSine = sin(phase);
            double baseCosine = cos(phase);
            double harmonicSine = baseSine;
            double harmonicCosine = baseCosine;
            double harmonicValue = 0.0;
            for(int h = 1; h <= renderControl.harmonicCount; h++)
            {
                int harmonic = h - 1;
                size_t leftIndex = (size_t)leftFrame *
                    renderControl.harmonicCount + harmonic;
                size_t rightIndex = (size_t)rightFrame *
                    renderControl.harmonicCount + harmonic;
                double weight = renderControl.weights[leftIndex] +
                    (renderControl.weights[rightIndex] -
                     renderControl.weights[leftIndex]) * fraction;
                double phaseSine = renderControl.phaseSines[leftIndex] +
                    (renderControl.phaseSines[rightIndex] -
                     renderControl.phaseSines[leftIndex]) * fraction;
                double phaseCosine = renderControl.phaseCosines[leftIndex] +
                    (renderControl.phaseCosines[rightIndex] -
                     renderControl.phaseCosines[leftIndex]) * fraction;
                double upperHarmonicAttack = 1.0;
                if(frequency * (double)h > 1800.0)
                {
                    double attackFraction = (double)(i - transitionStart) /
                        upperHarmonicAttackSamples;
                    attackFraction = max(0.0, min(1.0, attackFraction));
                    upperHarmonicAttack = attackFraction * attackFraction *
                        (3.0 - 2.0 * attackFraction);
                }
                harmonicValue += weight * upperHarmonicAttack *
                    (harmonicSine * phaseCosine +
                     harmonicCosine * phaseSine);
                double nextSine = harmonicSine * baseCosine +
                    harmonicCosine * baseSine;
                harmonicCosine = harmonicCosine * baseCosine -
                    harmonicSine * baseSine;
                harmonicSine = nextSine;
            }

            double harmonicLevel = harmonicTargetRms[i];
            if(sourceEnergyPrefix != NULL && harmonicLevel > 0.0)
            {
                double mapped = mapHarmonicSourcePosition(i, xLen,
                    protectedSamples, sourceBoundarySample,
                    sourceProtectedSample, g_harmonicSourceLength - 1);
                int centre = max(0, min(g_harmonicSourceLength - 1,
                    (int)(mapped + 0.5)));
                int radius = max(1, (int)((double)fs * 0.012 + 0.5));
                int begin = max(0, centre - radius);
                int end = min(g_harmonicSourceLength, centre + radius + 1);
                double localRms = sqrt(max(0.0,
                    (sourceEnergyPrefix[end] - sourceEnergyPrefix[begin]) /
                    (double)max(1, end - begin)));
                double envelopeRatio = stableRms > 1.0e-9
                    ? localRms / stableRms : 1.0;
                envelopeRatio = pow(max(0.35, min(2.2,
                    envelopeRatio)), 0.65);
                harmonicLevel *= max(0.55, min(1.45, envelopeRatio));
            }
            harmonicOutput[i] = harmonicValue /
                max(1.0e-12, normalizer) * harmonicLevel;
            waveformOutput[i] = naturalExcitation * harmonicLevel;
            continue;
        }

        double frequency = getHarmonicFrequencyAt(f0, tLen, i, xLen, fs,
            framePeriod, targetFrequency[i]);
        if(smoothedFrequency <= 1.0)
            smoothedFrequency = frequency;
        else
            smoothedFrequency += (frequency - smoothedFrequency) *
                frequencyFollower;
        frequency = smoothedFrequency;
        int tractSample = i;
        if((hasVocalTractTrack || hasExcitationTrack) &&
           tractSource == g_harmonicSource &&
           g_harmonicSource != NULL && g_harmonicSourceLength > 0)
        {
            double mapped = mapHarmonicSourcePosition(i, xLen,
                protectedSamples, sourceBoundarySample,
                sourceProtectedSample,
                g_harmonicSourceLength - 1);
            tractSample = (int)(mapped + 0.5);
        }
        if(hasExcitationTrack)
        {
            double sourceFrequencyRatio = 1.0;
            sampleHarmonicExcitationTrack(&excitationTrack, tractSample,
                0, NULL, NULL, &sourceFrequencyRatio);
            frequency *= pow(max(0.92, min(1.08,
                sourceFrequencyRatio)), 0.22);
        }
        phase += 2.0 * pi * frequency / (double)fs;
        if(phase > 2.0 * pi) phase -= 2.0 * pi;

        /*
         * Do not let the extreme voice's frame-by-frame shimmer modulate
         * the clean layer. The harmonic layer uses one stable-vowel level;
         * a small amount of high-frequency source texture is mixed below
         * without changing this level.
         */
        double harmonicLevel = harmonicTargetRms[i];
        if(sourceEnergyPrefix != NULL && harmonicLevel > 0.0)
        {
            /*
             * Preserve slow syllable and vowel-shape movement without
             * copying shimmer. A 24 ms RMS window is too slow to follow
             * individual residual events, but fast enough to retain
             * articulation and vowel opening/closing.
             */
            double mapped = mapHarmonicSourcePosition(i, xLen,
                protectedSamples, sourceBoundarySample,
                sourceProtectedSample,
                g_harmonicSourceLength - 1);
            int centre = max(0, min(g_harmonicSourceLength - 1,
                (int)(mapped + 0.5)));
            int radius = max(1, (int)((double)fs * 0.012 + 0.5));
            int begin = max(0, centre - radius);
            int end = min(g_harmonicSourceLength, centre + radius + 1);
            int count = max(1, end - begin);
            double localRms = sqrt(max(0.0,
                (sourceEnergyPrefix[end] - sourceEnergyPrefix[begin]) /
                (double)count));
            double envelopeRatio = stableRms > 1.0e-9
                ? localRms / stableRms : 1.0;
            envelopeRatio = pow(max(0.35, min(2.2,
                envelopeRatio)), 0.65);
            envelopeRatio = max(0.55, min(1.45, envelopeRatio));
            harmonicLevel *= envelopeRatio;
        }

        int filterOrder = min(16, vocalTractTrack.order);
        double tractReference = hasVocalTractTrack
            ? getVocalTractReference(&vocalTractTrack, tractSample,
                filterOrder, fs)
            : 1.0;
        /*
         * Do not turn a partial on or off when the tracked frequency moves
         * by a fraction of a semitone. That discrete boundary is audible as
         * an occasional metallic tick. Keep a fixed harmonic range and fade
         * the upper band continuously instead.
         */
        /*
         * Keep the number of candidate partials fixed.  The previous
         * frequency-dependent count switched the last partial on or off when
         * F0 moved slightly, which could produce a brief high-frequency tick.
         * Harmonics are now faded continuously near the upper band instead.
         */
        int harmonicCount = 48;
        double sumSquares = 0.0;
        double harmonicValue = 0.0;
        double time = (double)i / (double)fs;
        double baseExcitationAmplitude = 0.0;
        if(hasExcitationTrack)
        {
            sampleHarmonicExcitationTrack(&excitationTrack, tractSample,
                0, &baseExcitationAmplitude, NULL, NULL);
        }
        else if(hasExcitationTemplate &&
                excitationTemplate.harmonicCount > 0)
        {
            baseExcitationAmplitude =
                excitationTemplate.amplitudes[0];
        }
        double naturalPhase = phase +
            0.014 * sin(2.0 * pi * 1.31 * time + 0.37) +
            0.006 * sin(2.0 * pi * 2.47 * time + 1.19);
        double localExcitation = hasExcitationTrack
            ? sampleHarmonicExcitationWaveform(
                &excitationTrack, tractSample, naturalPhase)
            : 0.0;
        double stableExcitation = hasExcitationTemplate
            ? sampleHarmonicExcitationTemplateWaveform(
                &excitationTemplate, naturalPhase)
            : localExcitation;
        /*
         * The stable template carries the average glottal shape. Keep some
         * local movement for expression, but do not let one short-time cycle
         * rewrite the entire upper spectrum of the clean layer.
         */
        double naturalExcitation = hasExcitationTrack &&
            hasExcitationTemplate
            ? 0.15 * localExcitation + 0.85 * stableExcitation
            : localExcitation;
        for(int h = 1; h <= harmonicCount; h++)
        {
            double harmonicFrequency = frequency * (double)h;
            double highBandGain = 1.0;
            const double highBandStart = 9000.0;
            const double highBandEnd = 15000.0;
            if(harmonicFrequency > highBandStart)
            {
                double fraction = (harmonicFrequency - highBandStart) /
                    (highBandEnd - highBandStart);
                fraction = max(0.0, min(1.0, fraction));
                highBandGain = 0.5 + 0.5 * cos(pi * fraction);
            }
            if(harmonicFrequency >= highBandEnd)
                highBandGain = 0.0;
            /*
             * Avoid aliasing without a pitch-dependent hard cutoff.  The
             * raised-cosine fade reaches zero before Nyquist, so a partial
             * does not suddenly enter the output as the note moves.
             */
            double antiAliasGain = 1.0;
            const double antiAliasStart = 0.36 * (double)fs;
            const double antiAliasEnd = 0.48 * (double)fs;
            if(harmonicFrequency > antiAliasStart)
            {
                double fraction = (harmonicFrequency - antiAliasStart) /
                    (antiAliasEnd - antiAliasStart);
                fraction = max(0.0, min(1.0, fraction));
                antiAliasGain = 0.5 + 0.5 * cos(pi * fraction);
            }
            if(harmonicFrequency >= antiAliasEnd)
                antiAliasGain = 0.0;
            highBandGain *= antiAliasGain;
            if(highBandGain <= 1.0e-5)
                continue;
            /*
             * Let the pitch-forming lower partials establish the vowel
             * before the upper partials enter. A full-band attack can expose
             * one high harmonic as a short metallic burst at the hand-over.
             */
            double upperHarmonicAttack = 1.0;
            if(harmonicFrequency > 1800.0)
            {
                double attackFraction = (double)(i - transitionStart) /
                    upperHarmonicAttackSamples;
                attackFraction = max(0.0, min(1.0, attackFraction));
                upperHarmonicAttack = attackFraction *
                    attackFraction * (3.0 - 2.0 * attackFraction);
            }
            /*
             * A gentler source tilt keeps upper harmonics intelligible
             * instead of making the LPC-shaped result dull and muddy.
             */
            /*
             * The excitation template already contains the natural source
             * tilt. Keep only a very small extra tilt so the upper partials
             * do not disappear after the tract colour is applied.
             */
            double tilt = 1.0 / pow((double)h, 0.06);
            double excitationAmplitude = 0.0;
            double phaseOffset = 0.0;
            /*
             * Let the local track carry only the low pitch scaffold.  Its
             * upper partial amplitudes are the place where short residual
             * events can otherwise leak back into hs100.  The stable template
             * still supplies a natural non-sinusoidal source shape.
             */
            bool useLocalExcitation = hasExcitationTrack &&
                (h <= 6 || harmonicFrequency < 1800.0);
            if(useLocalExcitation)
            {
                sampleHarmonicExcitationTrack(&excitationTrack, tractSample,
                    h - 1, &excitationAmplitude, &phaseOffset, NULL);
            }
            else if(hasExcitationTemplate &&
                    h <= excitationTemplate.harmonicCount)
            {
                excitationAmplitude =
                    excitationTemplate.amplitudes[h - 1];
                phaseOffset = excitationTemplate.phaseOffsets[h - 1];
            }
            /*
             * A very dark source cycle can have almost no upper partials
             * after averaging. Add a quiet, source-independent floor only in
             * the intelligibility band. It is shaped by the tract envelope
             * below and is phase-diffused, so it does not become a fixed
             * sawtooth or whistle.
             */
            if(h >= 2 && h <= 24 && baseExcitationAmplitude > 1.0e-5)
            {
                /*
                 * This is only a small intelligibility scaffold.  A floor
                 * across all 48 harmonics was effectively a bright oscillator
                 * added to every voice, including voices that already had a
                 * usable upper spectrum. Keep the support below the
                 * sibilance band and retain the source-derived phase pattern.
                 */
                double clarityFloor = 0.085 *
                    baseExcitationAmplitude / pow((double)h, 0.85);
                excitationAmplitude = max(
                    excitationAmplitude, clarityFloor);
            }
            if(excitationAmplitude <= 1.0e-5)
                continue;

            double formant;
            if(hasVocalTractTrack)
            {
                /*
                 * Use the LPC vocal-tract response directly.  Unlike the
                 * recursive form, this cannot ring up when the harmonic layer
                 * starts or when a residual gap occurs.
                 */
                double response = getVocalTractMagnitudeStable(
                    &vocalTractTrack, tractSample, filterOrder,
                    harmonicFrequency, fs);
                response /= max(0.05, tractReference);
                double tractFormant = 0.68 + 0.32 * pow(
                    max(0.45, min(2.20, response)), 0.60);
                if(hasVowelTrack)
                {
                    double localReference = 1.0;
                    double spectral = sampleHarmonicEnvelopeTrack(
                        &vowelTrack, tractSample, fs,
                        harmonicFrequency, &localReference);
                    double spectralFormant = pow(max(0.30, min(2.4,
                        spectral / localReference)), 0.48);
                    double spectralWeight = 0.58;
                    if(harmonicFrequency > 3000.0)
                    {
                        double highFraction = (harmonicFrequency - 3000.0) /
                            5000.0;
                        highFraction = max(0.0, min(1.0, highFraction));
                        spectralWeight *= 1.0 - 0.72 * highFraction;
                    }
                    formant = (1.0 - spectralWeight) * tractFormant +
                        spectralWeight * spectralFormant;
                }
                else
                {
                    formant = tractFormant;
                }
            }
            else if(hasVowelTrack)
            {
                double localReference = 1.0;
                double spectral = sampleHarmonicEnvelopeTrack(
                    &vowelTrack, i, fs, harmonicFrequency,
                    &localReference);
                formant = pow(max(0.12, min(4.0,
                    spectral / localReference)), 0.38);
            }
            else if(hasVowelEnvelope)
            {
                double spectral = sampleVowelSpectralEnvelope(
                    vowelEnvelope, vowelFftLength, fs, harmonicFrequency);
                formant = pow(max(0.12, min(4.0,
                    spectral / envelopeReference)), 0.45);
            }
            else
            {
                formant = 1.0 +
                    0.45 * exp(-0.5 * pow(
                        (harmonicFrequency - 750.0) / 260.0, 2.0)) +
                    0.30 * exp(-0.5 * pow(
                        (harmonicFrequency - 1500.0) / 420.0, 2.0)) +
                    0.20 * exp(-0.5 * pow(
                        (harmonicFrequency - 2800.0) / 700.0, 2.0));
            }
            double clarity = 1.0;
            if(harmonicFrequency > 1200.0)
            {
                double clarityFraction = (harmonicFrequency - 1200.0) /
                    6000.0;
                clarityFraction = max(0.0, min(1.0, clarityFraction));
                clarity = 1.0 + 0.18 * clarityFraction;
            }
            /*
             * When a locally averaged excitation track exists, its smooth
             * source-derived movement is enough. Adding another independent
             * phase/amplitude modulation layer makes upper partials shimmer
             * against one another and can sound metallic. Keep only a very
             * small procedural fallback when no track could be built.
             */
            double proceduralAmplitude = hasExcitationTrack ? 1.0 :
                getProceduralHarmonicAmplitudeModulation(h, time);
            double proceduralPhase = hasExcitationTrack ? 0.0 :
                getProceduralHarmonicPhaseJitter(h, time);
            double weight = excitationAmplitude * tilt * formant *
                clarity * highBandGain * upperHarmonicAttack *
                proceduralAmplitude;
            /*
             * A broad formant may raise a group of neighbouring partials,
             * but one isolated partial should not dominate the whole layer.
             * Compress only that excess relative to H1; this keeps vowel
             * colour while preventing a narrow metallic whistle.
             */
            if(h >= 4 && baseExcitationAmplitude > 1.0e-5)
            {
                double concentrationLimit = baseExcitationAmplitude *
                    0.72 / pow((double)h, 0.18);
                if(weight > concentrationLimit)
                    weight = concentrationLimit +
                        0.25 * (weight - concentrationLimit);
            }
            sumSquares += weight * weight;
            harmonicValue += weight * sin((double)h * phase +
                phaseOffset + proceduralPhase);
        }

        double harmonicRms = sqrt(max(1.0e-12, sumSquares * 0.5));
        double harmonicExcitation = harmonicValue / harmonicRms;
        double harmonic = harmonicExcitation * harmonicLevel;

        if(i >= transitionStart)
        {
            harmonicOutput[i] = harmonic;
            waveformOutput[i] = naturalExcitation * harmonicLevel;
        }
    }

    /*
     * Normalize against the stable target level.  This removes exceptional
     * formant-dependent level dips while retaining the detected onset.
     */
    double *harmonicEnergyPrefix = (double *)calloc(
        (size_t)xLen + 1, sizeof(double));
    if(harmonicEnergyPrefix == NULL)
    {
        freeHarmonicRenderControlTrack(&renderControl);
        free(renderFrequency);
        free(harmonicTargetRms);
        free(harmonicOutput);
        free(waveformOutput);
        free(sourceEnergyPrefix);
        freeHarmonicEnvelopeTrack(&vowelTrack);
        freeVocalTractFilterTrack(&vocalTractTrack);
        freeHarmonicExcitationTrack(&excitationTrack);
        freeHarmonicExcitationTemplate(&excitationTemplate);
        free(targetFrequency);
        free(vowelEnvelope);
        free(source);
        return;
    }
    for(int i = 0; i < xLen; i++)
        harmonicEnergyPrefix[i + 1] = harmonicEnergyPrefix[i] +
            harmonicOutput[i] * harmonicOutput[i];

    int harmonicRadius = max(1, (int)((double)fs * 0.045 + 0.5));
    double levelCorrection = 1.0;
    double correctionFollower = 1.0 - exp(-1.0 /
        max(1.0, (double)fs * 0.020));
    double lowPassOne = 0.0;
    double lowPassTwo = 0.0;
    for(int i = transitionStart; i < xLen; i++)
    {
        double transition;
        if(i < protectedSamples)
        {
            transition = (double)(i - transitionStart) /
                (double)max(1, transitionSamples);
            transition = transition * transition *
                (3.0 - 2.0 * transition);
        }
        else
        {
            transition = 1.0;
        }
        double mix = (double)strength / 100.0 * transition;

        int begin = max(transitionStart, i - harmonicRadius);
        int end = min(xLen, i + harmonicRadius + 1);
        int count = max(1, end - begin);
        double localEnergy = (harmonicEnergyPrefix[end] -
            harmonicEnergyPrefix[begin]) / (double)count;
        double localRms = sqrt(max(0.0, localEnergy));
        double desiredRms = harmonicTargetRms[i];
        double desiredCorrection = 1.0;
        if(desiredRms <= 1.0e-8)
            desiredCorrection = 0.0;
        else if(localRms > 1.0e-8)
            desiredCorrection = desiredRms / localRms;
        desiredCorrection = max(0.12, min(2.00, desiredCorrection));
        /*
         * Pull down a resonant over-shoot quickly, but raise a quiet middle
         * more gently so the correction itself does not become a new pumping
         * sound.
         */
        double correctionTime = desiredCorrection < levelCorrection
            ? 0.004 : 0.012;
        double correctionAlpha = 1.0 - exp(-1.0 /
            max(1.0, (double)fs * correctionTime));
        levelCorrection += (desiredCorrection - levelCorrection) *
            max(correctionFollower, correctionAlpha);

        /*
         * Give the synthesized layer a short independent attack ramp.  This
         * prevents filter state and the first strong formant from making the
         * vowel head jump out before the level follower has settled.
         */
        double attack = (double)(i - transitionStart) /
            (double)max(1, (int)((double)fs * 0.020 + 0.5));
        attack = max(0.0, min(1.0, attack));
        attack = attack * attack * (3.0 - 2.0 * attack);
        double harmonicComponent = harmonicOutput[i] *
            levelCorrection * attack;
        double waveformComponent = waveformOutput[i] *
            levelCorrection * attack;
        /*
         * The periodic pulse shape keeps the result from becoming a stack of
         * bare sine waves, while the filtered harmonic component must carry
         * most of the weight so the vowel envelope remains intelligible.
         */
        double harmonicForMix = 0.30 * waveformComponent +
            0.70 * harmonicComponent;
        double harmonicPeakLimit = max(0.18, targetRms * 3.2);
        harmonicForMix = harmonicPeakLimit * tanh(
            harmonicForMix / harmonicPeakLimit);

        /*
         * Fade the original waveform and its high-frequency texture out as
         * the cleaning strength rises.  hs100 is a full harmonic
         * replacement, while lower values remain useful for blending.
         */
         double sourceForMix = source[i];
        lowPassOne += textureAlpha * (sourceForMix - lowPassOne);
        lowPassTwo += textureAlpha * (lowPassOne - lowPassTwo);
        double sourceTexture = sourceForMix - lowPassTwo;
        double sourceGain = cos(0.5 * pi * mix);
        double textureGain = 0.12 * (1.0 - mix);
        double harmonicGain = sin(0.5 * pi * mix);
        output[i] = sourceForMix * sourceGain +
            sourceTexture * textureGain + harmonicForMix * harmonicGain;
    }

    free(harmonicEnergyPrefix);
    freeHarmonicRenderControlTrack(&renderControl);
    free(renderFrequency);
    free(harmonicTargetRms);
    free(harmonicOutput);
    free(waveformOutput);
    free(sourceEnergyPrefix);
    freeHarmonicEnvelopeTrack(&vowelTrack);
    freeVocalTractFilterTrack(&vocalTractTrack);
    freeHarmonicExcitationTrack(&excitationTrack);
    freeHarmonicExcitationTemplate(&excitationTemplate);
    free(targetFrequency);
    free(vowelEnvelope);
    free(source);
}

static double getNoteFrequency(const char *scaleParam)
{
    if(scaleParam == NULL || scaleParam[0] == '\0') return 0.0;

    int semitone;
    switch(scaleParam[0])
    {
    case 'C': semitone = 0; break;
    case 'D': semitone = 2; break;
    case 'E': semitone = 4; break;
    case 'F': semitone = 5; break;
    case 'G': semitone = 7; break;
    case 'A': semitone = 9; break;
    case 'B': semitone = 11; break;
    default: return 0.0;
    }

    int index = 1;
    if(scaleParam[index] == '#')
    {
        semitone++;
        index++;
    }
    if(scaleParam[index] < '0' || scaleParam[index] > '9') return 0.0;

    int octave = scaleParam[index] - '0';
    int midi = (octave + 1) * 12 + semitone;
    return 440.0 * pow(2.0, ((double)midi - 69.0) / 12.0);
}

static int getIntegerFlag(char flagName)
{
    if(__argc <= 5 || __argv == NULL || __argv[5] == NULL) return 0;
    char *flags = __argv[5];
    for(int i = 0; flags[i] != '\0'; i++)
    {
        if(flags[i] == flagName)
        {
            char *end = NULL;
            long value = strtol(flags + i + 1, &end, 10);
            if(end != flags + i + 1) return (int)value;
        }
    }
    return 0;
}

static int decodePitchValue(char c)
{
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return 0;
}

static void fillPitchBend(const char *encoded, int *pitch, int count)
{
    if(encoded == NULL || pitch == NULL || count <= 0) return;

    int output = 0;
    int previous = 0;
    int length = (int)strlen(encoded);
    for(int i = 0; i < length && output < count; i += 2)
    {
        if(encoded[i] == '#')
        {
            i++;
            int repeat = atoi(encoded + i);
            for(int j = 0; j < repeat && output < count; j++)
                pitch[output++] = previous;
            while(i < length && encoded[i] != '#') i++;
            i--;
            continue;
        }

        if(i + 1 >= length) break;
        int value = decodePitchValue(encoded[i]) * 64 +
            decodePitchValue(encoded[i + 1]);
        if(value > 2047) value -= 4096;
        previous = value;
        pitch[output++] = value;
    }
}

static void prepareMelodyF0(double *f0, int tLen, double framePeriod,
                            int fs, int xLen)
{
    int strength = getMelodyFollowFlag();
    if(strength <= 0 || __argc <= 3 || __argv == NULL)
    {
        free(g_melodyF0);
        g_melodyF0 = NULL;
        g_melodyF0Length = 0;
        g_melodyFollow = 0;
        return;
    }

    double targetF0 = getNoteFrequency(__argv[3]);
    if(targetF0 <= 1.0)
    {
        strength = 0;
    }

    free(g_melodyF0);
    g_melodyF0 = NULL;
    g_melodyF0Length = 0;
    g_melodyFollow = 0;
    if(strength <= 0) return;

    g_melodyF0 = (double *)malloc(sizeof(double) * tLen);
    if(g_melodyF0 == NULL) return;
    g_melodyF0Length = tLen;
    g_melodyFollow = strength;

    int pitchOffset = getIntegerFlag('t');
    targetF0 *= pow(2.0, (double)pitchOffset / 120.0);

    int *pitch = NULL;
    int pitchLength = 0;
    int pitchStep = 0;
    if(__argc > 13 && __argv[12] != NULL && __argv[13] != NULL)
    {
        double tempo = atof(__argv[12] + 1);
        if(tempo <= 1.0) tempo = 120.0;
        pitchStep = (int)(60.0 / 96.0 / tempo * fs + 0.5);
        pitchLength = xLen / max(1, pitchStep) + 1;
        pitch = (int *)calloc(pitchLength + 1, sizeof(int));
        if(pitch != NULL)
            fillPitchBend(__argv[13], pitch, pitchLength + 1);
    }

    for(int i = 0; i < tLen; i++)
    {
        double bend = 0.0;
        if(pitch != NULL && pitchStep > 0)
        {
            double samplePosition = (double)i * framePeriod / 1000.0 *
                (double)fs / (double)pitchStep;
            int left = (int)floor(samplePosition);
            double fraction = samplePosition - left;
            left = max(0, min(pitchLength, left));
            int right = min(pitchLength, left + 1);
            bend = pitch[left] * (1.0 - fraction) +
                pitch[right] * fraction;
        }

        // Do not put the source .frq contour into the residual playback
        // clock.  A frame-by-frame contour would turn pitch-estimation
        // errors into audible high-frequency jitter.  Melody following is
        // intentionally tied to the note and pitch bend here; .frq can be
        // used later as a confidence/variation source without controlling
        // the grain timing directly.
        g_melodyF0[i] = targetF0 *
            pow(2.0, bend / 1200.0);
    }

    free(pitch);
}

static int millisecondsToFrames(int milliseconds, double framePeriod)
{
    double frames = (double)milliseconds / framePeriod;
    return (int)floor(frames + (frames < 0.0 ? -0.5 : 0.5));
}

static int getLoopPeriod(int loopLength, int reverse)
{
    if(loopLength <= 1) return 1;
    return reverse ? loopLength * 2 - 2 : loopLength;
}

static int mapLoopFrame(int loopStart, int loopLength, int loopOffset,
                        int reverse)
{
    int period = getLoopPeriod(loopLength, reverse);
    int offset = loopOffset % period;

    if(!reverse || loopLength <= 1) return loopStart + offset;
    if(offset < loopLength) return loopStart + offset;
    return loopStart + period - offset;
}

static int nextRandomSignedFrames(unsigned int *state, int rangeMs,
                                  double framePeriod)
{
    int rangeFrames = millisecondsToFrames(abs(rangeMs), framePeriod);
    if(rangeFrames <= 0) return 0;

    *state = *state * 1664525u + 1013904223u;
    unsigned int span = (unsigned int)rangeFrames * 2u + 1u;
    return (int)(*state % span) - rangeFrames;
}

static int chooseLoopBounds(int baseStartFrame, int baseEndFrame,
                            int forwardEndFrame, int startOffsetFrames,
                            int endOffsetFrames, int *loopStartFrame,
                            int *loopLength)
{
    if(forwardEndFrame < 2) return 0;

    int start = baseStartFrame + startOffsetFrames;
    int end = baseEndFrame + endOffsetFrames;
    int startLimit = forwardEndFrame - 2;
    start = max(0, min(startLimit, start));
    end = min(forwardEndFrame, end);
    end = max(start + 2, end);
    end = min(forwardEndFrame, end);

    if(end <= start + 1)
    {
        start = max(0, forwardEndFrame - 2);
        end = forwardEndFrame;
    }
    if(end <= start + 1) return 0;

    *loopStartFrame = start;
    *loopLength = end - start;
    return 1;
}

static double getResidualFrameEnergy(int sourceFrame,
                                     int *residualSpecgramLength,
                                     int *fixedResidualSpecgramIndex,
                                     double **aperiodicity, double *volume)
{
    int residualIndex = fixedResidualSpecgramIndex[sourceFrame];
    int length = residualSpecgramLength[residualIndex];
    if(length <= 0) return 0.0;

    double sum = 0.0;
    for(int i = 0; i < length; i++)
    {
        double value = aperiodicity[residualIndex][i];
        sum += value * value;
    }
    double frameVolume = volume[sourceFrame];
    return sum / (double)length * frameVolume * frameVolume;
}

static void buildLoopStabilityGain(int loopStartFrame, int loopLength,
                                   int reverse, int strength,
                                   int *residualSpecgramLength,
                                   int *fixedResidualSpecgramIndex,
                                   double **aperiodicity, double *volume,
                                   double *gain)
{
    int period = getLoopPeriod(loopLength, reverse);
    if(period <= 0) return;

    double *energy = (double *)malloc(sizeof(double) * period);
    if(energy == NULL) return;

    for(int i = 0; i < period; i++)
    {
        int sourceFrame = mapLoopFrame(loopStartFrame, loopLength, i,
            reverse);
        energy[i] = getResidualFrameEnergy(sourceFrame,
            residualSpecgramLength, fixedResidualSpecgramIndex,
            aperiodicity, volume);
    }

    // The correction follows a slow envelope, so short shimmer remains in
    // the residual instead of being flattened frame by frame.
    int halfWindow = min(10, max(1, period / 8));
    double targetEnergy = 0.0;
    double *smoothEnergy = (double *)malloc(sizeof(double) * period);
    if(smoothEnergy == NULL)
    {
        free(energy);
        return;
    }

    for(int i = 0; i < period; i++)
    {
        double sum = 0.0;
        int count = 0;
        for(int j = -halfWindow; j <= halfWindow; j++)
        {
            int index = (i + j) % period;
            if(index < 0) index += period;
            sum += energy[index];
            count++;
        }
        smoothEnergy[i] = sum / (double)count;
        targetEnergy += smoothEnergy[i];
    }
    targetEnergy /= (double)period;

    double strengthRatio = (double)strength / 100.0;
    for(int i = 0; i < period; i++)
    {
        if(targetEnergy <= 1.0e-20 || smoothEnergy[i] <= 1.0e-20)
        {
            gain[i] = 1.0;
            continue;
        }

        double correction = sqrt(targetEnergy / smoothEnergy[i]);
        correction = max(0.25, min(4.0, correction));
        gain[i] = pow(correction, strengthRatio);
    }

    free(smoothEnergy);
    free(energy);
}

/*
 * Residual frames at opposite ends of an oto loop do not necessarily share
 * waveform phase.  Crossfading their energy helps, but a large sample step
 * can still survive and be heard as a short "t" or "p" click.  Correct only
 * exceptional steps at known loop-wrap positions, then fade the correction
 * away over a very short interval.  Normal waveform slopes are left alone.
 */
static void addResidualFrameToBuffer(int sourceFrame, int position,
                                     double frameGain, int *residualSpecgramLength,
                                     int *fixedResidualSpecgramIndex,
                                     double **aperiodicity, double *volume,
                                     double *buffer, int bufferLength,
                                     int resampledLength)
{
    if(position >= bufferLength) return;

    int residualIndex = fixedResidualSpecgramIndex[sourceFrame];
    int residualLength = residualSpecgramLength[residualIndex];
    if(residualLength <= 0) return;

    int outputLength = resampledLength > 0 ? resampledLength :
        residualLength;
    int begin = max(0, -position);
    int end = min(outputLength, bufferLength - position);
    for(int j = begin; j < end; j++)
    {
        double sourcePosition = outputLength <= 1 ? 0.0 :
            (double)j * (double)(residualLength - 1) /
            (double)(outputLength - 1);
        int sourceIndex = (int)floor(sourcePosition);
        int nextIndex = min(residualLength - 1, sourceIndex + 1);
        double fraction = sourcePosition - sourceIndex;
        double value = aperiodicity[residualIndex][sourceIndex] +
            (aperiodicity[residualIndex][nextIndex] -
             aperiodicity[residualIndex][sourceIndex]) * fraction;
        buffer[position + j] += value * volume[sourceFrame] * frameGain;
    }
}

static int renderForwardWaveBlock(int startFrame, int frameCount,
                                  double fixedDefault_f0, double *f0,
                                  int *residualSpecgramLength,
                                  int *fixedResidualSpecgramIndex,
                                  double **aperiodicity, double *volume,
                                  double *frameGain, double framePeriod,
                                  int fs, double **blockOut)
{
    if(frameCount < 2) return 0;

    double currentTime = 0.0;
    for(int i = 0; i < frameCount; i++)
    {
        int sourceFrame = startFrame + i;
        double currentF0 = getSynthesisF0(fixedDefault_f0, f0,
            sourceFrame);
        if(currentF0 <= 1.0) currentF0 = DEFAULT_F0;
        currentTime += 1.0 / currentF0;
    }

    int blockLength = max(1, (int)(currentTime * (double)fs + 0.5));
    double *block = (double *)calloc(blockLength, sizeof(double));
    if(block == NULL) return 0;

    currentTime = 0.0;
    for(int i = 0; i < frameCount; i++)
    {
        int sourceFrame = startFrame + i;
        int position = (int)(currentTime * (double)fs + 0.5);
        double gain = frameGain == NULL ? 1.0 : frameGain[i];
        double currentF0 = getSynthesisF0(fixedDefault_f0, f0,
            sourceFrame);
        if(currentF0 <= 1.0) currentF0 = DEFAULT_F0;
        // Melody following changes the grain clock, but deliberately keeps
        // the residual waveform at its analyzed length. Resampling every
        // grain to the target period makes aperiodic material too regular
        // and produces a sharp, synthetic tone.
        int resampledLength = 0;
        addResidualFrameToBuffer(sourceFrame, position, gain,
            residualSpecgramLength, fixedResidualSpecgramIndex,
            aperiodicity, volume, block, blockLength, resampledLength);

        currentTime += 1.0 / currentF0;
    }

    *blockOut = block;
    return blockLength;
}

/*
 * Melody following needs a wider time-scale unit than one residual frame.
 * Resampling one residual frame at a time makes aperiodic material regular
 * and metallic.  This routine renders several source frames at their
 * original texture speed, then time-scales the complete short block and
 * overlaps it with neighbouring blocks.
 */
static int renderMelodyTextureChunk(
    const int *sourceFrames, int contentCount, int advanceCount,
    double fixedDefault_f0, double *f0,
    int *residualSpecgramLength, int *fixedResidualSpecgramIndex,
    double **aperiodicity, double *volume, const double *frameGain,
    int fs, int outputPosition, double *synthesisOut,
    double *weightOut, int xLen)
{
    if(sourceFrames == NULL || contentCount < 2 ||
       advanceCount < 1 || outputPosition >= xLen)
        return 0;

    if(fixedDefault_f0 <= 1.0) fixedDefault_f0 = DEFAULT_F0;
    double sourcePeriod = (double)fs / fixedDefault_f0;
    double sourceStep = sourcePeriod * (double)advanceCount;
    double targetStep = 0.0;

    for(int i = 0; i < advanceCount; i++)
    {
        int sourceFrame = sourceFrames[i];
        double targetF0 = getSynthesisF0(fixedDefault_f0, f0,
            sourceFrame);
        if(targetF0 <= 1.0) targetF0 = DEFAULT_F0;
        targetStep += (double)fs / targetF0;
    }

    if(sourceStep <= 1.0 || targetStep <= 1.0) return 0;

    int sourceLength = max(2, (int)(sourcePeriod *
        (double)contentCount + 0.5));
    for(int i = 0; i < contentCount; i++)
    {
        int sourceFrame = sourceFrames[i];
        int residualIndex = fixedResidualSpecgramIndex[sourceFrame];
        int residualLength = residualSpecgramLength[residualIndex];
        int position = (int)(sourcePeriod * (double)i + 0.5);
        sourceLength = max(sourceLength, position + residualLength);
    }

    double *sourceBlock = (double *)calloc(sourceLength, sizeof(double));
    if(sourceBlock == NULL) return 0;

    for(int i = 0; i < contentCount; i++)
    {
        int sourceFrame = sourceFrames[i];
        double gain = frameGain == NULL ? 1.0 : frameGain[i];
        int position = (int)(sourcePeriod * (double)i + 0.5);
        addResidualFrameToBuffer(sourceFrame, position, gain,
            residualSpecgramLength, fixedResidualSpecgramIndex,
            aperiodicity, volume, sourceBlock, sourceLength, 0);
    }

    /*
     * Do not resample the texture block.  Resampling shifts every spectral
     * component together and is heard as a gender/formant change.  The
     * pitch change is produced by moving intact texture grains closer
     * together or farther apart, with overlap-add smoothing the result.
     */
    int outputLength = sourceLength;
    int outputStep = max(1, (int)(targetStep + 0.5));

    for(int j = 0; j < outputLength; j++)
    {
        int outputIndex = outputPosition + j;
        if(outputIndex < 0 || outputIndex >= xLen) break;

        double sourcePosition = outputLength <= 1 ? 0.0 :
            (double)j * (double)(sourceLength - 1) /
            (double)(outputLength - 1);
        int sourceIndex = (int)floor(sourcePosition);
        int nextIndex = min(sourceLength - 1, sourceIndex + 1);
        double fraction = sourcePosition - sourceIndex;
        double value = sourceBlock[sourceIndex] +
            (sourceBlock[nextIndex] - sourceBlock[sourceIndex]) * fraction;

        // Square-root Hann: equal-power overlap without a dip at seams.
        double phase = (double)(j + 0.5) / (double)outputLength;
        double window = sqrt(max(0.0,
            0.5 - 0.5 * cos(2.0 * PI * phase)));
        synthesisOut[outputIndex] += value * window;
        weightOut[outputIndex] += window;
    }

    free(sourceBlock);
    return outputStep;
}

static int synthesisMelodyGranular(
    double fixedDefault_f0, double *f0, int tLen,
    double **aperiodicity, int *residualSpecgramLength,
    int *fixedResidualSpecgramIndex, double *volume,
    int loopStartFrameHint, int loopEndFrameHint,
    int volumeStability, int loopStartOffsetMs, int loopEndOffsetMs,
    int randomStartRangeMs, int randomEndRangeMs, int pingPong,
    double framePeriod, int fs, double *synthesisOut, int xLen)
{
    int baseStartFrame = loopStartFrameHint +
        millisecondsToFrames(loopStartOffsetMs, framePeriod);
    int baseEndFrame = loopEndFrameHint +
        millisecondsToFrames(loopEndOffsetMs, framePeriod);

    baseStartFrame = max(0, min(tLen - 2, baseStartFrame));
    baseEndFrame = max(baseStartFrame + 2, min(tLen, baseEndFrame));
    if(baseEndFrame <= baseStartFrame + 1) return 0;

    double *weightOut = (double *)calloc(xLen, sizeof(double));
    if(weightOut == NULL) return 0;

    int sourceFrames[MF_MAX_GRAIN_FRAMES + 1];
    double frameGain[MF_MAX_GRAIN_FRAMES + 1];
    int outputPosition = 0;

    // Render the attack in chunks, with the first loop frame as lookahead.
    int attackFrame = 0;
    while(attackFrame < baseStartFrame && outputPosition < xLen)
    {
        int advanceCount = min(MF_GRAIN_FRAMES,
            baseStartFrame - attackFrame);
        double targetStep = 0.0;
        for(int i = 0; i < advanceCount; i++)
        {
            double targetF0 = getSynthesisF0(fixedDefault_f0, f0,
                attackFrame + i);
            if(targetF0 <= 1.0) targetF0 = DEFAULT_F0;
            targetStep += (double)fs / targetF0;
        }
        int requiredContent = (int)ceil(targetStep * 1.35 /
            ((double)fs / max(1.0, fixedDefault_f0)));
        int contentCount = max(advanceCount + 1,
            min(MF_MAX_GRAIN_FRAMES, requiredContent + 1));
        for(int i = 0; i < contentCount; i++)
        {
            int frame = attackFrame + i;
            sourceFrames[i] = frame < baseStartFrame ?
                frame : baseStartFrame;
            frameGain[i] = 1.0;
        }

        int step = renderMelodyTextureChunk(sourceFrames, contentCount,
            advanceCount, fixedDefault_f0, f0, residualSpecgramLength,
            fixedResidualSpecgramIndex, aperiodicity, volume, frameGain,
            fs, outputPosition, synthesisOut, weightOut, xLen);
        if(step <= 0) break;
        outputPosition += step;
        attackFrame += advanceCount;
    }

    unsigned int randomState = 0x7f4a7c15u;
    while(outputPosition < xLen)
    {
        int randomStartFrames = nextRandomSignedFrames(&randomState,
            randomStartRangeMs, framePeriod);
        int randomEndFrames = nextRandomSignedFrames(&randomState,
            randomEndRangeMs, framePeriod);
        int cycleStartFrame = -1;
        int cycleLength = 0;
        if(!chooseLoopBounds(baseStartFrame, baseEndFrame, tLen,
            randomStartFrames, randomEndFrames, &cycleStartFrame,
            &cycleLength))
            break;

        int cyclePeriod = getLoopPeriod(cycleLength, pingPong);
        double *cycleGain = NULL;
        if(volumeStability > 0)
        {
            cycleGain = (double *)malloc(sizeof(double) * cyclePeriod);
            if(cycleGain != NULL)
            {
                for(int i = 0; i < cyclePeriod; i++)
                    cycleGain[i] = 1.0;
                buildLoopStabilityGain(cycleStartFrame, cycleLength,
                    pingPong, volumeStability, residualSpecgramLength,
                    fixedResidualSpecgramIndex, aperiodicity, volume,
                    cycleGain);
            }
        }

        int loopPosition = 0;
        while(loopPosition < cyclePeriod && outputPosition < xLen)
        {
            int advanceCount = min(MF_GRAIN_FRAMES,
                cyclePeriod - loopPosition);
            double targetStep = 0.0;
            for(int i = 0; i < advanceCount; i++)
            {
                int position = loopPosition + i;
                int frame = mapLoopFrame(cycleStartFrame, cycleLength,
                    position, pingPong);
                double targetF0 = getSynthesisF0(fixedDefault_f0, f0,
                    frame);
                if(targetF0 <= 1.0) targetF0 = DEFAULT_F0;
                targetStep += (double)fs / targetF0;
            }
            int requiredContent = (int)ceil(targetStep * 1.35 /
                ((double)fs / max(1.0, fixedDefault_f0)));
            int contentCount = max(advanceCount + 1,
                min(MF_MAX_GRAIN_FRAMES, requiredContent + 1));
            for(int i = 0; i < contentCount; i++)
            {
                int position = loopPosition + i;
                sourceFrames[i] = mapLoopFrame(cycleStartFrame,
                    cycleLength, position, pingPong);
                frameGain[i] = cycleGain == NULL ? 1.0 :
                    cycleGain[position % cyclePeriod];
            }

            int step = renderMelodyTextureChunk(sourceFrames,
                contentCount, advanceCount, fixedDefault_f0, f0,
                residualSpecgramLength, fixedResidualSpecgramIndex,
                aperiodicity, volume, frameGain, fs, outputPosition,
                synthesisOut, weightOut, xLen);
            if(step <= 0) break;
            outputPosition += step;
            loopPosition += advanceCount;
        }

        free(cycleGain);
        if(loopPosition <= 0) break;
    }

    for(int i = 0; i < xLen; i++)
    {
        if(weightOut[i] > 1.0e-12)
            synthesisOut[i] /= weightOut[i];
    }

    free(weightOut);
    return outputPosition > 0;
}

static int synthesisWaveformPingPong(
    double fixedDefault_f0, double *f0, int tLen,
    double **aperiodicity, int *residualSpecgramLength,
    int *fixedResidualSpecgramIndex, double *volume,
    int loopStartFrameHint, int loopEndFrameHint,
    int volumeStability, int loopStartOffsetMs, int loopEndOffsetMs,
    int randomStartRangeMs, int randomEndRangeMs,
    int pingPong,
    double framePeriod, int fs, double *synthesisOut, int xLen)
{
    int baseStartFrame = loopStartFrameHint +
        millisecondsToFrames(loopStartOffsetMs, framePeriod);
    int baseEndFrame = loopEndFrameHint +
        millisecondsToFrames(loopEndOffsetMs, framePeriod);

    baseStartFrame = max(0, min(tLen - 2, baseStartFrame));
    baseEndFrame = max(baseStartFrame + 2, min(tLen, baseEndFrame));
    if(baseEndFrame <= baseStartFrame + 1) return 0;

    // Render the non-looping attack once with the ordinary forward order.
    double currentTime = 0.0;
    for(int i = 0; i < baseStartFrame; i++)
    {
        int position = (int)(currentTime * (double)fs + 0.5);
        double currentF0 = getSynthesisF0(fixedDefault_f0, f0, i);
        if(currentF0 <= 1.0) currentF0 = DEFAULT_F0;
        int resampledLength = 0;
        addResidualFrameToBuffer(i, position, 1.0,
            residualSpecgramLength, fixedResidualSpecgramIndex,
            aperiodicity, volume, synthesisOut, xLen, resampledLength);

        currentTime += 1.0 / currentF0;
    }

    int outputPosition = (int)(currentTime * (double)fs + 0.5);
    unsigned int randomState = 0x3c6ef372u;
    int cycleIndex = 0;

    while(outputPosition < xLen)
    {
        int randomStartFrames = nextRandomSignedFrames(&randomState,
            randomStartRangeMs, framePeriod);
        int randomEndFrames = nextRandomSignedFrames(&randomState,
            randomEndRangeMs, framePeriod);
        int cycleStartFrame = -1;
        int cycleLength = 0;
        if(!chooseLoopBounds(baseStartFrame, baseEndFrame, tLen,
            randomStartFrames, randomEndFrames,
            &cycleStartFrame, &cycleLength))
            break;

        double *frameGain = NULL;
        if(volumeStability > 0)
        {
            frameGain = (double *)malloc(sizeof(double) * cycleLength);
            if(frameGain != NULL)
            {
                for(int i = 0; i < cycleLength; i++) frameGain[i] = 1.0;
                buildLoopStabilityGain(cycleStartFrame, cycleLength, 0,
                    volumeStability, residualSpecgramLength,
                    fixedResidualSpecgramIndex, aperiodicity, volume,
                    frameGain);
            }
        }

        double *block = NULL;
        int blockLength = renderForwardWaveBlock(cycleStartFrame,
            cycleLength, fixedDefault_f0, f0, residualSpecgramLength,
            fixedResidualSpecgramIndex, aperiodicity, volume, frameGain,
            framePeriod, fs, &block);
        free(frameGain);
        if(blockLength <= 0 || block == NULL) break;

        int writeLength = min(blockLength, xLen - outputPosition);
        if(!pingPong || (cycleIndex & 1) == 0)
        {
            for(int i = 0; i < writeLength; i++)
                synthesisOut[outputPosition + i] += block[i];
        }
        else
        {
            for(int i = 0; i < writeLength; i++)
                synthesisOut[outputPosition + i] +=
                    block[blockLength - 1 - i];
        }

        free(block);
        outputPosition += blockLength;
        cycleIndex++;
    }

    return 1;
}

void synthesisPt100(double *f0, int tLen, double **aperiodicity, int fftl,
                    double framePeriod, int fs, double *synthesisOut, int xLen)
{
    int i, j;
    double currentTime = 0.0;
    int currentPosition = 0;
    int currentFrame = 0;

    for(i = 0;; i++)
    {
        currentPosition = (int)(currentTime * (double)fs);
        for(j = 0; j < fftl / 2; j++)
        {
            if(j + currentPosition >= xLen) break;
            synthesisOut[j + currentPosition] += aperiodicity[currentFrame][j];
        }

        currentTime += 1.0 / (f0[currentFrame] == 0.0 ? DEFAULT_F0 : f0[currentFrame]);
        currentFrame = (int)(currentTime / (framePeriod / 1000.0) + 0.5);
        currentPosition = (int)(currentTime * (double)fs);

        if(j + currentPosition >= xLen || currentFrame >= tLen) break;
    }
}

static int isAllUnvoiced(double *f0, int tLen)
{
    for(int i = 0; i < tLen; i++)
        if(f0[i] != 0.0) return 0;
    return 1;
}

static double getMelodyPitchScale(double fixedDefault_f0)
{
    if(fixedDefault_f0 <= 1.0 || g_melodyF0 == NULL ||
       g_melodyF0Length <= 0 || g_melodyFollow <= 0)
        return 1.0;

    /*
     * Pitch ratios are multiplicative, so a geometric mean is less
     * sensitive to a short pitch-bend tail than an arithmetic mean.
     */
    double logarithmicSum = 0.0;
    int count = 0;
    for(int i = 0; i < g_melodyF0Length; i++)
    {
        if(g_melodyF0[i] > 1.0)
        {
            logarithmicSum += log(g_melodyF0[i] / fixedDefault_f0);
            count++;
        }
    }
    if(count <= 0) return 1.0;

    double targetRatio = exp(logarithmicSum / (double)count);
    double strength = (double)g_melodyFollow / 100.0;
    double scale = exp(log(max(0.25, min(4.0, targetRatio))) *
        strength);
    return max(0.25, min(4.0, scale));
}

static void applyLoudnessCompensation(double *target,
                                      const double *reference,
                                      int length, int fs, int strength)
{
    if(target == NULL || reference == NULL || length <= 0 ||
       fs <= 0 || strength <= 0)
        return;

    /*
     * Compare broad RMS envelopes rather than individual grains.  A
     * 120-ms analysis window and a slower gain follower leave shimmer and
     * short residual events intact while correcting the level drift caused
     * by changed grain density or pitch-shifter spectral redistribution.
     */
    int radius = max(1, (int)((double)fs * 0.060 + 0.5));
    double *targetPrefix = (double *)calloc((size_t)length + 1,
        sizeof(double));
    double *referencePrefix = (double *)calloc((size_t)length + 1,
        sizeof(double));
    if(targetPrefix == NULL || referencePrefix == NULL)
    {
        free(targetPrefix);
        free(referencePrefix);
        return;
    }

    for(int i = 0; i < length; i++)
    {
        targetPrefix[i + 1] = targetPrefix[i] + target[i] * target[i];
        referencePrefix[i + 1] = referencePrefix[i] +
            reference[i] * reference[i];
    }

    double strengthRatio = (double)max(0, min(100, strength)) / 100.0;
    double gain = 1.0;
    double follower = 1.0 - exp(-1.0 /
        max(1.0, (double)fs * 0.030));
    const double silenceRms = 1.0e-7;

    for(int i = 0; i < length; i++)
    {
        int begin = max(0, i - radius);
        int end = min(length, i + radius + 1);
        int count = max(1, end - begin);
        double targetEnergy = (targetPrefix[end] -
            targetPrefix[begin]) / (double)count;
        double referenceEnergy = (referencePrefix[end] -
            referencePrefix[begin]) / (double)count;
        double targetRms = sqrt(max(0.0, targetEnergy));
        double referenceRms = sqrt(max(0.0, referenceEnergy));

        double desiredGain = 1.0;
        if(targetRms > silenceRms && referenceRms > silenceRms)
        {
            desiredGain = referenceRms / targetRms;
            desiredGain = max(0.5, min(2.0, desiredGain));
            desiredGain = exp(log(desiredGain) * strengthRatio);
        }

        gain += (desiredGain - gain) * follower;
        target[i] *= gain;
    }

    free(targetPrefix);
    free(referencePrefix);
}

/*
 * Emphasise energy that is already present near the requested harmonic
 * positions. This is deliberately an STFT gain mask rather than oscillator
 * synthesis: the complex spectrum keeps its original phase, and empty
 * harmonic locations remain empty.
 */
static void applyHarmonicEmphasis(double *output, int xLen, int fs,
                                  const double *f0, int tLen,
                                  double framePeriod,
                                  double fallbackFrequency, int strength,
                                  int forceStrength)
{
    if(output == NULL || xLen <= 0 || fs <= 0 ||
       (strength <= 0 && forceStrength <= 0))
        return;

    int fftl = 2048;
    while(fftl > xLen && fftl > 256) fftl /= 2;
    if(fftl < 256) return;
    int hop = fftl / 2;

    double *frame = (double *)calloc((size_t)fftl, sizeof(double));
    double *processed = (double *)calloc((size_t)xLen, sizeof(double));
    double *weight = (double *)calloc((size_t)xLen, sizeof(double));
    fft_complex *spectrum = (fft_complex *)calloc(
        (size_t)fftl, sizeof(fft_complex));
    if(frame == NULL || processed == NULL || weight == NULL ||
       spectrum == NULL)
    {
        free(frame);
        free(processed);
        free(weight);
        free(spectrum);
        return;
    }

    fft_plan forward = fft_plan_dft_r2c_1d(fftl, frame, spectrum,
        FFT_ESTIMATE);
    fft_plan inverse = fft_plan_dft_c2r_1d(fftl, spectrum, frame,
        FFT_ESTIMATE);

    const double pi = 3.14159265358979323846;
    double strengthRatio = (double)max(0, min(100, strength)) / 100.0;
    double forceRatio = (double)max(0, min(100, forceStrength)) / 100.0;
    /*
     * he now searches for a real local peak around each target partial.  The
     * search tolerates the unstable pitch of an extreme vocal, while the
     * confidence test prevents a random noise bin from being treated as a
     * useful harmonic.
     */
    double amount = 0.78 * strengthRatio;
    int harmonicLimit = 24;
    double binFrequency = (double)fs / (double)fftl;
    double bandwidth = max(42.0, 0.055 * (double)fs /
        (double)fftl);
    double forcePhase = 0.0;

    for(int start = 0; start < xLen; start += hop)
    {
        for(int j = 0; j < fftl; j++)
        {
            int sample = start + j;
            double value = sample < xLen ? output[sample] : 0.0;
            double window = 0.5 - 0.5 * cos(
                2.0 * pi * ((double)j + 0.5) / (double)fftl);
            frame[j] = value * window;
        }

        fft_execute(forward);
        int centre = min(xLen - 1, start + fftl / 2);
        double frequency = getHarmonicFrequencyAt(f0, tLen, centre,
            xLen, fs, framePeriod, fallbackFrequency);
        frequency = max(45.0, min(2200.0, frequency));

        double *magnitude = (double *)calloc(
            (size_t)fftl / 2 + 1, sizeof(double));
        if(magnitude == NULL)
            break;
        for(int k = 0; k <= fftl / 2; k++)
            magnitude[k] = sqrt(spectrum[k][0] * spectrum[k][0] +
                spectrum[k][1] * spectrum[k][1]);

        for(int h = 1; h <= harmonicLimit; h++)
        {
            double targetFrequency = frequency * (double)h;
            if(targetFrequency >= (double)fs * 0.48)
                break;

            int targetBin = (int)(targetFrequency / binFrequency + 0.5);
            int searchRadius = max(2, (int)(
                max(42.0, min(0.075 * targetFrequency,
                    0.30 * frequency)) / binFrequency + 0.5));
            int searchBegin = max(1, targetBin - searchRadius);
            int searchEnd = min(fftl / 2 - 1, targetBin + searchRadius);
            int peakBin = targetBin;
            double peakMagnitude = targetBin <= fftl / 2
                ? magnitude[targetBin] : 0.0;
            double bandSum = 0.0;
            int bandCount = 0;
            for(int k = searchBegin; k <= searchEnd; k++)
            {
                if(magnitude[k] > peakMagnitude)
                {
                    peakMagnitude = magnitude[k];
                    peakBin = k;
                }
                bandSum += magnitude[k];
                bandCount++;
            }

            double bandMean = bandCount > 0 ? bandSum /
                (double)bandCount : peakMagnitude;
            double peakRatio = peakMagnitude /
                max(1.0e-12, bandMean);
            double confidence = max(0.0, min(1.0,
                (peakRatio - 1.12) / 0.90));
            double peakFrequency = (double)peakBin * binFrequency;

            /*
             * Boost the actual nearby peak, retaining its original complex
             * phase. This is the part that remains strictly "no added
             * harmonics".
             */
            if(strengthRatio > 0.0 && confidence > 0.0)
            {
                for(int k = max(0, peakBin - (int)(
                    bandwidth / binFrequency + 0.5));
                    k <= min(fftl / 2, peakBin + (int)(
                        bandwidth / binFrequency + 0.5)); k++)
                {
                    double distance = (double)k * binFrequency -
                        peakFrequency;
                    double mask = exp(-0.5 * distance * distance /
                        (bandwidth * bandwidth));
                    double gain = 1.0 + amount * confidence * mask;
                    spectrum[k][0] *= gain;
                    spectrum[k][1] *= gain;
                }
            }

            /*
             * hf is deliberately separate from he. If the search band has
             * no convincing peak, add a small random-phase component at the
             * requested position. The local spectrum supplies its level, so
             * this is a shaped experiment rather than a full-scale sine comb.
             */
            if(forceRatio > 0.0 && confidence < 0.55)
            {
                int envelopeRadius = max(3, (int)(
                    120.0 / binFrequency + 0.5));
                int envelopeBegin = max(1, targetBin - envelopeRadius);
                int envelopeEnd = min(fftl / 2 - 1,
                    targetBin + envelopeRadius);
                double envelopeSum = 0.0;
                int envelopeCount = 0;
                for(int k = envelopeBegin; k <= envelopeEnd; k++)
                {
                    envelopeSum += magnitude[k];
                    envelopeCount++;
                }
                double envelope = envelopeCount > 0 ? envelopeSum /
                    (double)envelopeCount : bandMean;
                /*
                 * Keep one deterministic phase offset per harmonic.  The
                 * common forcePhase advances from frame to frame, so the
                 * added layer remains a continuous pitched waveform instead
                 * of turning into independent noisy bursts.
                 */
                double phaseSeed = 2.0 * pi *
                    fmod((double)h * 0.61803398875, 1.0);
                double missingness = 1.0 - confidence / 0.55;
                double forcedMagnitude = envelope * 0.60 *
                    forceRatio * max(0.0, min(1.0, missingness));
                double forceBandwidth = max(30.0, bandwidth * 0.75);
                int forceRadius = max(1, (int)(
                    forceBandwidth / binFrequency + 0.5));
                for(int k = max(1, targetBin - forceRadius);
                    k <= min(fftl / 2 - 1, targetBin + forceRadius); k++)
                {
                    double distance = (double)k * binFrequency -
                        targetFrequency;
                    double mask = exp(-0.5 * distance * distance /
                        (forceBandwidth * forceBandwidth));
                    double phase = phaseSeed + (double)h * forcePhase;
                    spectrum[k][0] += forcedMagnitude * mask *
                        cos(phase);
                    spectrum[k][1] += forcedMagnitude * mask *
                        sin(phase);
                }
            }
        }
        free(magnitude);

        fft_execute(inverse);
        for(int j = 0; j < fftl; j++)
        {
            int sample = start + j;
            if(sample < 0 || sample >= xLen) continue;
            double window = 0.5 - 0.5 * cos(
                2.0 * pi * ((double)j + 0.5) / (double)fftl);
            processed[sample] += frame[j] / (double)fftl * window;
            weight[sample] += window * window;
        }
        forcePhase += 2.0 * pi * frequency * (double)hop /
            (double)fs;
        forcePhase -= floor(forcePhase / (2.0 * pi)) * 2.0 * pi;
    }

    double sourceEnergy = 0.0;
    double processedEnergy = 0.0;
    double sourcePeak = 0.0;
    double processedPeak = 0.0;
    for(int i = 0; i < xLen; i++)
    {
        if(weight[i] <= 1.0e-8)
            continue;
        double sourceValue = output[i];
        double processedValue = processed[i] / weight[i];
        sourceEnergy += sourceValue * sourceValue;
        processedEnergy += processedValue * processedValue;
        sourcePeak = max(sourcePeak, fabs(sourceValue));
        processedPeak = max(processedPeak, fabs(processedValue));
        processed[i] = processedValue;
    }

    /*
     * The resampler performs a final peak normalization after synthesis.
     * Match the input RMS first so a few emphasized peaks cannot make the
     * whole note quieter there.
     */
    if(sourceEnergy > 1.0e-12 && processedEnergy > 1.0e-12)
    {
        double rmsCorrection = sqrt(sourceEnergy / processedEnergy);
        rmsCorrection = max(0.85, min(1.15, rmsCorrection));
        for(int i = 0; i < xLen; i++)
            if(weight[i] > 1.0e-8)
                processed[i] *= rmsCorrection;
        processedPeak *= rmsCorrection;
    }

    /*
     * Keep the new emphasis from creating a large crest-factor jump. This is
     * a local soft limiter used only by he; it does not alter the old path.
     */
    if(sourcePeak > 1.0e-8 && processedPeak > sourcePeak)
    {
        double peakLimit = sourcePeak * (1.0 + 0.10 * strengthRatio);
        double knee = peakLimit * 0.85;
        double range = max(1.0e-9, peakLimit - knee);
        double normalizer = tanh(1.0);
        for(int i = 0; i < xLen; i++)
        {
            if(weight[i] <= 1.0e-8)
                continue;
            double value = processed[i];
            double magnitude = fabs(value);
            if(magnitude <= knee)
                continue;
            double excess = magnitude - knee;
            double compressed = knee + range *
                tanh(excess / range) / normalizer;
            processed[i] = value < 0.0 ? -compressed : compressed;
        }
    }

    for(int i = 0; i < xLen; i++)
    {
        if(weight[i] > 1.0e-8)
            output[i] = processed[i];
    }

    fft_destroy_plan(forward);
    fft_destroy_plan(inverse);
    free(frame);
    free(processed);
    free(weight);
    free(spectrum);
}

static void synthesisPt101Impl(double fixedDefault_f0, double *f0, int tLen,
                    double **aperiodicity, int *ResidualSpecgramLength,
                    int *fixedResidualSpecgramIndex, double *volume,
                    int loopStartFrameHint, int loopEndFrameHint,
                    int fftl, double framePeriod, int fs,
                    double *synthesisOut, int xLen)
{
    int i, j;
    double currentTime = 0.0;
    double sourceClockTime = 0.0;
    int currentPosition = 0;
    int currentFrame = 0;
    int allUnvoiced = 1;
    int wFlagPresent = hasWFlag();
    unsigned int w1RandomState = 0x6d2b79f5u;
    int w1LoopStartFrame = -1;
    int w1LoopLength = 0;
    int reverseLoopMode = getReverseLoopMode();
    int loopStartOffsetMs = 0;
    int loopEndOffsetMs = 0;
    int hasLoopStartOffset = getLoopOffset('l', 's', &loopStartOffsetMs);
    int hasLoopEndOffset = getLoopOffset('l', 'f', &loopEndOffsetMs);
    int randomStartRangeMs = 0;
    int randomEndRangeMs = 0;
    int hasRandomStartRange = getLoopOffset('r', 's',
        &randomStartRangeMs);
    int hasRandomEndRange = getLoopOffset('r', 'f', &randomEndRangeMs);
    int crossfadeMs = 0;
    int hasCrossfadeFlag = getLoopOffset('c', 'f', &crossfadeMs);
    int volumeStability = 0;
    int hasVolumeStability = getLoopOffset('s', 'v', &volumeStability);

    if(randomStartRangeMs < 0) randomStartRangeMs = -randomStartRangeMs;
    if(randomEndRangeMs < 0) randomEndRangeMs = -randomEndRangeMs;
    if(volumeStability < 0) volumeStability = 0;
    volumeStability = min(100, volumeStability);

    // Forward looping keeps the historical short fade.  Ping-pong looping
    // already returns through adjacent source frames; applying the same
    // default fade there smears every turn and can create a sustained,
    // synthetic-sounding layer.  An explicit cf flag still enables it.
    int crossfadeFrames = reverseLoopMode == 1 ?
        0 : W1_LOOP_CROSSFADE_FRAMES;
    if(hasCrossfadeFlag)
    {
        crossfadeMs = max(0, crossfadeMs);
        crossfadeFrames = millisecondsToFrames(crossfadeMs, framePeriod);
    }

    // Zero-valued offsets are a no-op. In particular, pingpong with ls0lf0
    // must retain
    // the original stretchTime ping-pong path instead of selecting a second
    // loop mapper with a different phase.
    int hasLoopOffset = (hasLoopStartOffset && loopStartOffsetMs != 0) ||
        (hasLoopEndOffset && loopEndOffsetMs != 0);
    int hasRandomLoopOffset =
        (hasRandomStartRange && randomStartRangeMs != 0) ||
        (hasRandomEndRange && randomEndRangeMs != 0);
    int mappedLoopReverse = reverseLoopMode == 1;
    int useMappedLoop = 0;
    int w1LoopBaseStartFrame = -1;
    int w1LoopBaseEndFrame = -1;
    int w1LoopForwardEndFrame = -1;

    for(i = 0; i < tLen; i++)
    {
        if(f0[i] != 0.0)
        {
            allUnvoiced = 0;
            break;
        }
    }

    /*
     * For melody following, use multi-frame texture grains.  A single
     * residual frame is too short and too phase-sensitive to stretch over a
     * large pitch interval without gaps or a metallic, locked tone.
     */
    if(allUnvoiced && g_melodyFollow > 0 &&
       getMelodyTextureMode() == 1 &&
       loopStartFrameHint >= 0 &&
       loopEndFrameHint > loopStartFrameHint + 1)
    {
        if(synthesisMelodyGranular(
            fixedDefault_f0, f0, tLen, aperiodicity,
            ResidualSpecgramLength, fixedResidualSpecgramIndex, volume,
            loopStartFrameHint, loopEndFrameHint, volumeStability,
            loopStartOffsetMs, loopEndOffsetMs, randomStartRangeMs,
            randomEndRangeMs, reverseLoopMode == 1, framePeriod, fs,
            synthesisOut, xLen))
        {
            return;
        }
    }

    /*
     * The old ping-pong path reversed residual-frame indices.  That keeps
     * every residual internally forward, so the turn can expose a phase
     * discontinuity as a sustained synthetic tone.  For W-1, render a
     * continuous forward block first and reverse the rendered samples on
     * alternating cycles, matching the safer post-production technique.
     */
    if(allUnvoiced &&
       reverseLoopMode == 1 &&
       loopStartFrameHint >= 0 &&
       loopEndFrameHint > loopStartFrameHint + 1)
    {
        if(synthesisWaveformPingPong(
            fixedDefault_f0, f0, tLen, aperiodicity,
            ResidualSpecgramLength, fixedResidualSpecgramIndex, volume,
            loopStartFrameHint, loopEndFrameHint, volumeStability,
            loopStartOffsetMs, loopEndOffsetMs, randomStartRangeMs,
            randomEndRangeMs, reverseLoopMode == 1,
            framePeriod, fs, synthesisOut, xLen))
        {
            return;
        }
    }

    int forceForwardLoop = reverseLoopMode == 0 ||
        (reverseLoopMode < 0 && (allUnvoiced || wFlagPresent));
    int w1ForwardLoop = allUnvoiced && forceForwardLoop;
    int jitterStartFrame = w1LoopStartFrame;

    int needsMappedLoop = forceForwardLoop ||
        (wFlagPresent && reverseLoopMode == 1) || hasLoopOffset ||
        hasRandomLoopOffset || (hasCrossfadeFlag && crossfadeFrames >= 0) ||
        (hasVolumeStability && volumeStability > 0);
    if(!useMappedLoop && needsMappedLoop)
    {
        int forwardTurn = -1;
        int reverseTurn = -1;
        int baseStartFrame = loopStartFrameHint;
        int baseEndFrame = loopEndFrameHint;
        int forwardEndFrame = loopEndFrameHint;
        int haveLoopHint = baseStartFrame >= 0 &&
            baseEndFrame > baseStartFrame + 1;

        if(!haveLoopHint)
        {
            for(i = 2; i + 1 < tLen; i++)
            {
                int previousDelta = fixedResidualSpecgramIndex[i - 1] -
                    fixedResidualSpecgramIndex[i - 2];
                int delta = fixedResidualSpecgramIndex[i] -
                    fixedResidualSpecgramIndex[i - 1];
                if(forwardTurn < 0 && previousDelta > 0 && delta < 0)
                {
                    forwardTurn = i;
                }
                else if(forwardTurn >= 0 && previousDelta < 0 && delta > 0)
                {
                    reverseTurn = i;
                    break;
                }
            }

            if(forwardTurn >= 0)
            {
                if(reverseTurn > forwardTurn)
                {
                    int loopLength = reverseTurn - forwardTurn;
                    baseStartFrame = forwardTurn - loopLength;
                    baseEndFrame = forwardTurn;
                    forwardEndFrame = min(tLen, forwardTurn);
                    haveLoopHint = baseEndFrame > baseStartFrame + 1;
                }
                else if(!mappedLoopReverse)
                {
                    // A short note may enter the original reverse leg
                    // without reaching the next forward turn. Recover the
                    // forward range from the last rising run instead of
                    // accepting that partial reverse leg.
                    int risingStart = forwardTurn - 1;
                    while(risingStart > 0 &&
                          fixedResidualSpecgramIndex[risingStart] -
                          fixedResidualSpecgramIndex[risingStart - 1] > 0)
                        risingStart--;
                    baseStartFrame = risingStart;
                    baseEndFrame = forwardTurn;
                    forwardEndFrame = min(tLen, forwardTurn);
                    haveLoopHint = baseEndFrame > baseStartFrame + 1;
                }
            }
        }

        if(haveLoopHint)
        {
            baseStartFrame += millisecondsToFrames(loopStartOffsetMs,
                framePeriod);
            baseEndFrame += millisecondsToFrames(loopEndOffsetMs,
                framePeriod);

            // Never let a forward loop read the original reverse leg.
            forwardEndFrame = min(forwardEndFrame, tLen);
            int loopStartFrame = -1;
            int mappedLoopLength = 0;
            if(chooseLoopBounds(baseStartFrame, baseEndFrame, forwardEndFrame,
                0, 0, &loopStartFrame, &mappedLoopLength))
            {
                w1LoopStartFrame = loopStartFrame;
                w1LoopLength = mappedLoopLength;
                w1LoopBaseStartFrame = loopStartFrame;
                w1LoopBaseEndFrame = loopStartFrame + mappedLoopLength;
                w1LoopForwardEndFrame = forwardEndFrame;
                useMappedLoop = 1;
                jitterStartFrame = w1LoopStartFrame;
            }
        }
    }

    unsigned int w1LoopRandomState = 0xa511e9b3u;
    int cycleStartFrame = w1LoopStartFrame;
    int cycleLoopStartFrame = w1LoopStartFrame;
    int cycleLoopLength = w1LoopLength;
    int cyclePeriod = useMappedLoop ?
        getLoopPeriod(cycleLoopLength, mappedLoopReverse) : 0;
    int previousLoopStartFrame = -1;
    int previousLoopLength = 0;
    int previousLoopPeriod = 0;
    int previousLoopValid = 0;
    int reverseMicroRangeFrames = 0;
    if(allUnvoiced && mappedLoopReverse &&
       W1_REVERSE_MICRO_RANGE_FRAMES > 0)
    {
        reverseMicroRangeFrames = W1_REVERSE_MICRO_RANGE_FRAMES;
    }
    double *currentLoopGain = NULL;
    double *previousLoopGain = NULL;

    if(useMappedLoop)
    {
        int randomStartFrames = nextRandomSignedFrames(&w1LoopRandomState,
            randomStartRangeMs, framePeriod);
        int randomEndFrames = nextRandomSignedFrames(&w1LoopRandomState,
            randomEndRangeMs, framePeriod);
        if(!chooseLoopBounds(w1LoopBaseStartFrame, w1LoopBaseEndFrame,
            w1LoopForwardEndFrame, randomStartFrames, randomEndFrames,
            &cycleLoopStartFrame, &cycleLoopLength))
        {
            useMappedLoop = 0;
            cycleStartFrame = -1;
            cyclePeriod = 0;
        }
        else
        {
            w1LoopStartFrame = cycleLoopStartFrame;
            w1LoopLength = cycleLoopLength;
            cyclePeriod = getLoopPeriod(cycleLoopLength,
                mappedLoopReverse);
        }
    }

    if(useMappedLoop && volumeStability > 0)
    {
        int maxLoopPeriod = max(1, tLen * 2);
        currentLoopGain = (double *)malloc(sizeof(double) * maxLoopPeriod);
        previousLoopGain = (double *)malloc(sizeof(double) * maxLoopPeriod);
        if(currentLoopGain == NULL || previousLoopGain == NULL)
        {
            free(currentLoopGain);
            free(previousLoopGain);
            currentLoopGain = NULL;
            previousLoopGain = NULL;
            volumeStability = 0;
        }
        else
        {
            for(i = 0; i < maxLoopPeriod; i++)
            {
                currentLoopGain[i] = 1.0;
                previousLoopGain[i] = 1.0;
            }
            buildLoopStabilityGain(cycleLoopStartFrame, cycleLoopLength,
                mappedLoopReverse, volumeStability,
                ResidualSpecgramLength, fixedResidualSpecgramIndex,
                aperiodicity, volume, currentLoopGain);
        }
    }

    // pingpong without ls/lf keeps stretchTime's original ping-pong sequence, so
    // there is no mapped loop start. Still vary W-1's clock to avoid an exact
    // repetition of the residual sequence.
    if(allUnvoiced && reverseLoopMode == 1 && jitterStartFrame < 0)
        jitterStartFrame = 0;

    for(i = 0;; i++)
    {
        int melodyLoopContinuation =
            allUnvoiced && g_melodyFollow > 0 &&
            useMappedLoop && cyclePeriod > 0;
        if(currentFrame < 0 || currentPosition >= xLen ||
           (currentFrame >= tLen && !melodyLoopContinuation))
            break;

        /*
         * In the old mf granular mode, the output clock can reach the end
         * of the source timeline before it reaches the requested UTAU note
         * length, especially when following a high note.  Once a mapped
         * loop exists, keep the synthetic timeline running and let the
         * loop mapper select valid source frames beyond tLen.  The direct
         * array access here is only a placeholder and is clamped because
         * the mapped-loop section below replaces it before rendering.
         */
        int safeCurrentFrame = max(0, min(tLen - 1, currentFrame));
        int residualIndex = fixedResidualSpecgramIndex[safeCurrentFrame];
        int transitionResidualIndex = -1;
        double transitionWeight = 0.0;
        int sourceFrame = currentFrame;
        int transitionSourceFrame = sourceFrame;
        double sourceLoopGain = 1.0;
        double transitionLoopGain = 1.0;

        if(useMappedLoop && cycleStartFrame >= 0 && cycleLoopLength > 0 &&
           currentFrame >= cycleStartFrame)
        {
            while(useMappedLoop && currentFrame >= cycleStartFrame +
                  cyclePeriod)
            {
                previousLoopStartFrame = cycleLoopStartFrame;
                previousLoopLength = cycleLoopLength;
                previousLoopPeriod = cyclePeriod;
                previousLoopValid = 1;
                if(currentLoopGain != NULL && previousLoopGain != NULL)
                {
                    for(int k = 0; k < previousLoopPeriod; k++)
                        previousLoopGain[k] = currentLoopGain[k];
                }

                cycleStartFrame += cyclePeriod;
                int randomStartFrames = nextRandomSignedFrames(
                    &w1LoopRandomState, randomStartRangeMs, framePeriod);
                int randomEndFrames = nextRandomSignedFrames(
                    &w1LoopRandomState, randomEndRangeMs, framePeriod);
                if(reverseMicroRangeFrames > 0)
                {
                    // A one-frame deterministic boundary variation is small
                    // enough to retain the vowel texture, but prevents W-1
                    // from replaying exactly the same residual period forever.
                    randomStartFrames += nextRandomSignedFrames(
                        &w1LoopRandomState,
                        (int)(reverseMicroRangeFrames * framePeriod + 0.5),
                        framePeriod);
                    randomEndFrames += nextRandomSignedFrames(
                        &w1LoopRandomState,
                        (int)(reverseMicroRangeFrames * framePeriod + 0.5),
                        framePeriod);
                }
                if(!chooseLoopBounds(w1LoopBaseStartFrame,
                    w1LoopBaseEndFrame, w1LoopForwardEndFrame,
                    randomStartFrames, randomEndFrames,
                    &cycleLoopStartFrame, &cycleLoopLength))
                {
                    useMappedLoop = 0;
                    break;
                }
                cyclePeriod = getLoopPeriod(cycleLoopLength,
                    mappedLoopReverse);
                if(currentLoopGain != NULL)
                {
                    for(int k = 0; k < cyclePeriod; k++)
                        currentLoopGain[k] = 1.0;
                    buildLoopStabilityGain(cycleLoopStartFrame,
                        cycleLoopLength, mappedLoopReverse, volumeStability,
                        ResidualSpecgramLength, fixedResidualSpecgramIndex,
                        aperiodicity, volume, currentLoopGain);
                }
            }

            if(!useMappedLoop)
            {
                cycleStartFrame = -1;
                cyclePeriod = 0;
            }

            if(useMappedLoop)
            {
                int loopPosition = currentFrame - cycleStartFrame;
                sourceFrame = mapLoopFrame(cycleLoopStartFrame,
                    cycleLoopLength, loopPosition, mappedLoopReverse);
                residualIndex = fixedResidualSpecgramIndex[sourceFrame];
                if(currentLoopGain != NULL)
                    sourceLoopGain = currentLoopGain[loopPosition];

                int fadeFrames = min(crossfadeFrames,
                    cycleLoopLength / 2);
                if(previousLoopValid)
                    fadeFrames = min(fadeFrames, previousLoopLength / 2);
                if(fadeFrames > 0 && loopPosition < fadeFrames)
                {
                    int transitionPosition;
                    if(previousLoopValid)
                    {
                        transitionPosition = previousLoopPeriod -
                            fadeFrames + loopPosition;
                        transitionSourceFrame = mapLoopFrame(
                            previousLoopStartFrame, previousLoopLength,
                            transitionPosition, mappedLoopReverse);
                        if(previousLoopGain != NULL)
                            transitionLoopGain =
                                previousLoopGain[transitionPosition];
                    }
                    else
                    {
                        transitionPosition = cyclePeriod - fadeFrames +
                            loopPosition;
                        transitionSourceFrame = mapLoopFrame(
                            cycleLoopStartFrame, cycleLoopLength,
                            transitionPosition, mappedLoopReverse);
                        if(currentLoopGain != NULL)
                            transitionLoopGain =
                                currentLoopGain[transitionPosition];
                    }
                    int transitionFrame = transitionSourceFrame;
                    transitionResidualIndex =
                        fixedResidualSpecgramIndex[transitionFrame];
                    transitionWeight =
                        (double)(fadeFrames - loopPosition) /
                        (double)(fadeFrames + 1);
                }
            }
        }

        int residualLength = ResidualSpecgramLength[residualIndex];
        int transitionLength = transitionResidualIndex >= 0 ?
            ResidualSpecgramLength[transitionResidualIndex] : 0;
        int outputLength = max(residualLength, transitionLength);

        double newGain = 1.0;
        double oldGain = 0.0;
        double seamScale = 1.0;
        if(transitionResidualIndex >= 0)
        {
            // Linear amplitude fades lose about 3 dB for unrelated residuals
            // at the midpoint.  Equal-power gains preserve their energy much
            // better, while the later RMS correction also handles partial
            // phase cancellation.
            double theta = transitionWeight * PI / 2.0;
            newGain = cos(theta);
            oldGain = sin(theta);

            double mixedEnergy = 0.0;
            double expectedEnergy = 0.0;
            for(j = 0; j < outputLength; j++)
            {
                double newValue = j < residualLength ?
                    aperiodicity[residualIndex][j] * volume[sourceFrame] *
                        sourceLoopGain : 0.0;
                double oldValue = j < transitionLength ?
                    aperiodicity[transitionResidualIndex][j] *
                        volume[transitionSourceFrame] * transitionLoopGain :
                        0.0;
                double mixedValue = newGain * newValue + oldGain * oldValue;
                mixedEnergy += mixedValue * mixedValue;
                expectedEnergy += newGain * newGain * newValue * newValue +
                    oldGain * oldGain * oldValue * oldValue;
            }

            if(mixedEnergy > 1.0e-20 && expectedEnergy > 1.0e-20)
            {
                seamScale = sqrt(expectedEnergy / mixedEnergy);
                seamScale = max(0.5, min(2.0, seamScale));
            }
        }

        for(j = 0; j < outputLength; j++)
        {
            if(j + currentPosition >= xLen) break;
            double newValue = j < residualLength ?
                aperiodicity[residualIndex][j] * volume[sourceFrame] *
                    sourceLoopGain : 0.0;
            if(transitionResidualIndex >= 0)
            {
                double oldValue = j < transitionLength ?
                    aperiodicity[transitionResidualIndex][j] : 0.0;
                oldValue *= volume[transitionSourceFrame] *
                    transitionLoopGain;
                newValue = (newGain * newValue + oldGain * oldValue) *
                    seamScale;
            }
            synthesisOut[max(0, j + currentPosition)] +=
                newValue;
        }

        double currentF0 = getSynthesisF0(fixedDefault_f0, f0,
            sourceFrame);
        if(currentF0 <= 1.0) currentF0 = DEFAULT_F0;
        if(allUnvoiced && (w1ForwardLoop || reverseLoopMode == 1) &&
           currentFrame >= jitterStartFrame && jitterStartFrame >= 0)
        {
            // W-1 has no musical F0 to preserve. A small deterministic clock
            // variation prevents a short residual loop from becoming a
            // perfectly repeating tone while keeping the original texture.
            w1RandomState = w1RandomState * 1664525u + 1013904223u;
            double variation = ((double)(w1RandomState & 0xffffu) / 65535.0) * 2.0 - 1.0;
            currentF0 *= 1.0 + W1_CLOCK_JITTER * variation;
        }
        currentTime += 1.0 / currentF0;

        /*
         * Melody following changes where the residual is written in the
         * output, but it must not also change which source texture frame is
         * selected.  Doing both makes a small mf value sweep through the
         * source loop at a different rate and produces a sharp, synthetic
         * re-ordering of the residual grains.
         */
        double sourceF0 = currentF0;
        if(allUnvoiced && g_melodyFollow > 0)
            sourceF0 = fixedDefault_f0;
        if(sourceF0 <= 1.0) sourceF0 = DEFAULT_F0;
        sourceClockTime += 1.0 / sourceF0;
        currentFrame = (int)(sourceClockTime /
            (framePeriod / 1000.0) + 0.5);
        currentPosition = (int)(currentTime * (double)fs);

        if(j + currentPosition >= xLen) break;
        if(currentFrame >= tLen &&
           !(allUnvoiced && g_melodyFollow > 0 &&
             useMappedLoop && cyclePeriod > 0))
            break;
    }

    free(currentLoopGain);
    free(previousLoopGain);
}

void synthesisPt101(double fixedDefault_f0, double *f0, int tLen,
                    double **aperiodicity, int *ResidualSpecgramLength,
                    int *fixedResidualSpecgramIndex, double *volume,
                    int loopStartFrameHint, int loopEndFrameHint,
                    int fftl, double framePeriod, int fs,
                    double *synthesisOut, int xLen)
{
    prepareMelodyF0(f0, tLen, framePeriod, fs, xLen);
    int loudnessCompensation = getLoudnessCompensation();
    int harmonicCleaning = getHarmonicCleaningStrength();
    int harmonicEmphasis = getHarmonicEmphasisStrength();
    int harmonicForce = getHarmonicForceStrength();
    int melodyActive = isAllUnvoiced(f0, tLen) &&
        g_melodyFollow > 0;
    int consonantMs = 0;
    if(__argc > 8 && __argv != NULL && __argv[8] != NULL)
        consonantMs = atoi(__argv[8]);
    double harmonicFallback = getHarmonicNoteFrequency(
        __argc > 3 && __argv != NULL ? __argv[3] : NULL);

    /*
     * hq1 is deliberately opt-in. Render the same W-1 loop once at the
     * source/default texture pitch, then apply one continuous R3 pitch
     * shift to the complete note. This avoids independently moving each
     * residual grain, which was the source of the artificial grain-density
     * and metallic pitch behaviour in mf.
     */
    if(melodyActive &&
       (getHighQualityPitchMode() == 1 ||
        getHighQualityPitchMode() == 2))
    {
        double *base = (double *)calloc((size_t)xLen, sizeof(double));
        double *shifted = (double *)calloc((size_t)xLen, sizeof(double));
        if(base != NULL && shifted != NULL)
        {
            int savedMelodyFollow = g_melodyFollow;
            g_melodyFollow = 0;
            synthesisPt101Impl(fixedDefault_f0, f0, tLen,
                aperiodicity, ResidualSpecgramLength,
                fixedResidualSpecgramIndex, volume, loopStartFrameHint,
                loopEndFrameHint, fftl, framePeriod, fs, base, xLen);
            g_melodyFollow = savedMelodyFollow;

            double pitchScale = getMelodyPitchScale(fixedDefault_f0);
            int pitchSucceeded = getHighQualityPitchMode() == 2 ?
                hybridExtremePitchShift(base, xLen, fs, pitchScale,
                    shifted) :
                rubberBandPitchShift(base, xLen, fs, pitchScale, shifted);
            if(pitchSucceeded)
            {
                if(loudnessCompensation > 0)
                    applyLoudnessCompensation(shifted, base, xLen, fs,
                        loudnessCompensation);
                applyHarmonicEmphasis(shifted, xLen, fs, f0, tLen,
                    framePeriod, harmonicFallback, harmonicEmphasis,
                    harmonicForce);
                applyHarmonicCleaning(shifted, xLen, fs, f0, tLen,
                    framePeriod, harmonicFallback, harmonicCleaning);
                for(int i = 0; i < xLen; i++)
                    synthesisOut[i] = shifted[i];
                free(base);
                free(shifted);
                return;
            }
        }
        free(base);
        free(shifted);
    }

    /*
     * For hq0, render a no-mf reference as well.  The old method changes
     * the density and overlap of residual grains, so its level drift cannot
     * be inferred reliably from the target pitch ratio alone.  The
     * compensation is intentionally broad-band and slow, leaving shimmer
     * and individual residual events untouched.
     */
    if(melodyActive && loudnessCompensation > 0)
    {
        double *reference = (double *)calloc((size_t)xLen,
            sizeof(double));
        if(reference != NULL)
        {
            int savedMelodyFollow = g_melodyFollow;
            g_melodyFollow = 0;
            synthesisPt101Impl(fixedDefault_f0, f0, tLen,
                aperiodicity, ResidualSpecgramLength,
                fixedResidualSpecgramIndex, volume, loopStartFrameHint,
                loopEndFrameHint, fftl, framePeriod, fs, reference, xLen);
            g_melodyFollow = savedMelodyFollow;

            synthesisPt101Impl(fixedDefault_f0, f0, tLen,
                aperiodicity, ResidualSpecgramLength,
                fixedResidualSpecgramIndex, volume, loopStartFrameHint,
                loopEndFrameHint, fftl, framePeriod, fs, synthesisOut,
                xLen);
            applyLoudnessCompensation(synthesisOut, reference, xLen, fs,
                loudnessCompensation);
            applyHarmonicEmphasis(synthesisOut, xLen, fs, f0, tLen,
                framePeriod, harmonicFallback, harmonicEmphasis,
                harmonicForce);
            applyHarmonicCleaning(synthesisOut, xLen, fs, f0, tLen,
                framePeriod, harmonicFallback, harmonicCleaning);
            free(reference);
            return;
        }
    }

    synthesisPt101Impl(fixedDefault_f0, f0, tLen, aperiodicity,
        ResidualSpecgramLength, fixedResidualSpecgramIndex, volume,
        loopStartFrameHint, loopEndFrameHint, fftl, framePeriod, fs,
        synthesisOut, xLen);
    applyHarmonicEmphasis(synthesisOut, xLen, fs, f0, tLen,
        framePeriod, harmonicFallback, harmonicEmphasis, harmonicForce);
    applyHarmonicCleaning(synthesisOut, xLen, fs, f0, tLen,
        framePeriod, harmonicFallback, harmonicCleaning);
}
