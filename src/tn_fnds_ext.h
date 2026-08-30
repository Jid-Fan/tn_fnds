#ifndef TN_FNDS_EXT_H
#define TN_FNDS_EXT_H

typedef struct
{
    int samplesPerFrame;
    double averageF0;
    int frameCount;
    double *frequency;
    double *auxiliary;
} FrqData;

int loadFrqFile(const char *path, FrqData *data);
void freeFrqData(FrqData *data);
int findFrqForWav(const char *wavPath, char *frqPath, int frqPathLength);

void setMelodyF0Control(double *melodyF0, int tLen, int strength);
void setHarmonicSource(const double *source, int length, int fs,
                       int boundaryMs);
void setHarmonicOutputBoundary(int boundaryMs);

#endif
