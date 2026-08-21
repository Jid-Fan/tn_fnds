#include "tn_fnds_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int readExact(FILE *file, void *data, size_t size)
{
    return fread(data, 1, size, file) == size;
}

void freeFrqData(FrqData *data)
{
    if(data == NULL) return;
    free(data->frequency);
    free(data->auxiliary);
    data->frequency = NULL;
    data->auxiliary = NULL;
    data->samplesPerFrame = 0;
    data->averageF0 = 0.0;
    data->frameCount = 0;
}

int loadFrqFile(const char *path, FrqData *data)
{
    if(path == NULL || data == NULL) return 0;
    memset(data, 0, sizeof(FrqData));

    FILE *file = fopen(path, "rb");
    if(file == NULL) return 0;

    char magic[8];
    int samplesPerFrame = 0;
    double averageF0 = 0.0;
    unsigned char reserved[16];
    int frameCount = 0;

    if(!readExact(file, magic, sizeof(magic)) ||
       memcmp(magic, "FREQ0003", 8) != 0 ||
       !readExact(file, &samplesPerFrame, sizeof(samplesPerFrame)) ||
       !readExact(file, &averageF0, sizeof(averageF0)) ||
       !readExact(file, reserved, sizeof(reserved)) ||
       !readExact(file, &frameCount, sizeof(frameCount)) ||
       samplesPerFrame <= 0 || frameCount <= 0)
    {
        fclose(file);
        return 0;
    }

    double *frequency = (double *)malloc(sizeof(double) * frameCount);
    double *auxiliary = (double *)malloc(sizeof(double) * frameCount);
    if(frequency == NULL || auxiliary == NULL)
    {
        free(frequency);
        free(auxiliary);
        fclose(file);
        return 0;
    }

    for(int i = 0; i < frameCount; i++)
    {
        if(!readExact(file, &frequency[i], sizeof(double)) ||
           !readExact(file, &auxiliary[i], sizeof(double)))
        {
            free(frequency);
            free(auxiliary);
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    data->samplesPerFrame = samplesPerFrame;
    data->averageF0 = averageF0;
    data->frameCount = frameCount;
    data->frequency = frequency;
    data->auxiliary = auxiliary;
    return 1;
}

static int tryPath(const char *path, char *result, int resultLength)
{
    FILE *file = fopen(path, "rb");
    if(file == NULL) return 0;
    fclose(file);

    if((int)strlen(path) + 1 > resultLength) return 0;
    strcpy(result, path);
    return 1;
}

int findFrqForWav(const char *wavPath, char *frqPath, int frqPathLength)
{
    if(wavPath == NULL || frqPath == NULL || frqPathLength <= 0) return 0;

    const char *extension = strrchr(wavPath, '.');
    int stemLength = extension == NULL ? (int)strlen(wavPath) :
        (int)(extension - wavPath);
    if(stemLength <= 0) return 0;

    char candidate[4096];
    if(stemLength + 10 < (int)sizeof(candidate))
    {
        memcpy(candidate, wavPath, stemLength);
        candidate[stemLength] = '\0';
        strcat(candidate, "_wav.frq");
        if(tryPath(candidate, frqPath, frqPathLength)) return 1;
    }

    if((int)strlen(wavPath) + 5 < (int)sizeof(candidate))
    {
        strcpy(candidate, wavPath);
        strcat(candidate, ".frq");
        if(tryPath(candidate, frqPath, frqPathLength)) return 1;
    }

    if(stemLength + 5 < (int)sizeof(candidate))
    {
        memcpy(candidate, wavPath, stemLength);
        candidate[stemLength] = '\0';
        strcat(candidate, ".frq");
        if(tryPath(candidate, frqPath, frqPathLength)) return 1;
    }

    return 0;
}
