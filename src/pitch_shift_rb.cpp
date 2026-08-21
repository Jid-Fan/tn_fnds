#include "pitch_shift_rb.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <vector>

using RubberBand::RubberBandStretcher;

static void collectRubberBandOutput(RubberBandStretcher *stretcher,
                                    std::vector<float> *result)
{
    if(stretcher == NULL || result == NULL) return;

    float buffer[16384];
    float *channels[1] = { buffer };

    for(;;)
    {
        int available = stretcher->available();
        if(available <= 0) break;

        size_t request = (size_t)std::min(available,
            (int)(sizeof(buffer) / sizeof(buffer[0])));
        size_t got = stretcher->retrieve(channels, request);
        if(got == 0) break;
        result->insert(result->end(), buffer, buffer + got);
    }
}

int rubberBandPitchShift(const double *input, int inputLength, int fs,
                         double pitchScale, double *output)
{
    if(input == NULL || output == NULL || inputLength <= 0 || fs < 8000 ||
       !std::isfinite(pitchScale) || pitchScale <= 0.0)
        return 0;

    if(std::fabs(pitchScale - 1.0) < 0.00001)
    {
        for(int i = 0; i < inputLength; i++) output[i] = input[i];
        return 1;
    }

    /*
     * R3 is the finer engine. Formant preservation is important here:
     * hq1 should move the perceived note without reproducing the raw
     * resampling "gender changed" effect from the experimental mode.
     */
    RubberBandStretcher::Options options =
        RubberBandStretcher::OptionEngineFiner |
        RubberBandStretcher::OptionFormantPreserved |
        RubberBandStretcher::OptionWindowLong |
        RubberBandStretcher::OptionTransientsMixed |
        RubberBandStretcher::OptionThreadingNever |
        RubberBandStretcher::OptionPitchHighQuality;

    RubberBandStretcher stretcher((size_t)fs, 1, options, 1.0,
        pitchScale);
    stretcher.setMaxProcessSize(65536);
    stretcher.setExpectedInputDuration((size_t)inputLength);

    std::vector<float> inputFloat((size_t)inputLength);
    for(int i = 0; i < inputLength; i++)
        inputFloat[(size_t)i] = (float)input[i];

    const size_t blockSize = 65536;
    const float *inputChannels[1] = { inputFloat.data() };

    /*
     * Offline processing has a study pass and a process pass. Keeping
     * blocks below the library's maximum also handles long UTAU notes.
     */
    for(size_t position = 0; position < (size_t)inputLength;)
    {
        size_t count = std::min(blockSize,
            (size_t)inputLength - position);
        const float *blockChannels[1] = {
            inputChannels[0] + position
        };
        bool final = position + count >= (size_t)inputLength;
        stretcher.study(blockChannels, count, final);
        position += count;
    }

    std::vector<float> processed;
    processed.reserve((size_t)inputLength + 8192);

    for(size_t position = 0; position < (size_t)inputLength;)
    {
        size_t count = std::min(blockSize,
            (size_t)inputLength - position);
        const float *blockChannels[1] = {
            inputChannels[0] + position
        };
        bool final = position + count >= (size_t)inputLength;
        stretcher.process(blockChannels, count, final);
        collectRubberBandOutput(&stretcher, &processed);
        position += count;
    }

    for(int pass = 0; pass < 4 && stretcher.available() >= 0; pass++)
    {
        size_t before = processed.size();
        collectRubberBandOutput(&stretcher, &processed);
        if(processed.size() == before) break;
    }

    if(processed.empty()) return 0;

    /*
     * With timeRatio=1 the output should match the note duration. Rubber
     * Band may expose a small amount of padding, so trim or zero-pad at the
     * UTAU boundary instead of changing the rendered note length.
     */
    for(int i = 0; i < inputLength; i++)
    {
        output[i] = i < (int)processed.size() ?
            (double)processed[(size_t)i] : 0.0;
    }

    return 1;
}
