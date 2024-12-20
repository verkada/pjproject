#ifndef AGC_H
#define AGC_H

#include <stdint.h>


typedef struct {
    float target_rms_linear;
    float max_gain;
    float attack;
    float release;
    float gain;
    int counter;
    int wav_created;
} AGC;

/**
 * Initialize AGC parameters.
 *
 * @param agc Pointer to AGC structure.
 * @param target_rms_dbfs Target RMS in dBFS.
 * @param max_gain Maximum allowable gain.
 * @param attack Attack rate for gain adjustment (0-1).
 * @param release Release rate for gain adjustment (0-1).
 */
void agc_init(AGC *agc, float target_rms_dbfs, float max_gain, float attack, float release);

/**
 * Process audio samples with AGC.
 *
 * @param agc Pointer to AGC structure.
 * @param audio_samples Pointer to audio samples (PCM 16-bit).
 * @param num_samples Number of samples to process.
 */
void agc_process(AGC *agc, int16_t *audio_samples, int num_samples);

#endif // AGC_H