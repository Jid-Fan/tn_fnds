#ifndef TN_FNDS_PITCH_SHIFT_RB_H
#define TN_FNDS_PITCH_SHIFT_RB_H

/*
 * Offline, mono pitch shifting for the W-1 melody-following path.
 *
 * The implementation uses Rubber Band R3 with formant preservation.
 * It returns zero on allocation/processing failure so the caller can
 * fall back to the existing granular renderer.
 */
int rubberBandPitchShift(const double *input, int inputLength, int fs,
                         double pitchScale, double *output);

/*
 * Experimental extreme-vocal path:
 * pitch-shift the low-frequency pitch scaffold while keeping the
 * high-frequency noisy/aperiodic texture from the original waveform.
 */
int hybridExtremePitchShift(const double *input, int inputLength, int fs,
                            double pitchScale, double *output);

#endif
