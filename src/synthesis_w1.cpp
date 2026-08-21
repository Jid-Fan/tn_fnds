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

void setMelodyF0Control(double *melodyF0, int tLen, int strength)
{
    g_melodyF0 = melodyF0;
    g_melodyF0Length = max(0, tLen);
    g_melodyFollow = max(0, min(100, strength));
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
        return max(0, min(1, mode));
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
    int melodyActive = isAllUnvoiced(f0, tLen) &&
        g_melodyFollow > 0;

    /*
     * hq1 is deliberately opt-in. Render the same W-1 loop once at the
     * source/default texture pitch, then apply one continuous R3 pitch
     * shift to the complete note. This avoids independently moving each
     * residual grain, which was the source of the artificial grain-density
     * and metallic pitch behaviour in mf.
     */
    if(melodyActive &&
       getHighQualityPitchMode() == 1)
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
            if(rubberBandPitchShift(base, xLen, fs, pitchScale, shifted))
            {
                if(loudnessCompensation > 0)
                    applyLoudnessCompensation(shifted, base, xLen, fs,
                        loudnessCompensation);
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
            free(reference);
            return;
        }
    }

    synthesisPt101Impl(fixedDefault_f0, f0, tLen, aperiodicity,
        ResidualSpecgramLength, fixedResidualSpecgramIndex, volume,
        loopStartFrameHint, loopEndFrameHint, fftl, framePeriod, fs,
        synthesisOut, xLen);
}
