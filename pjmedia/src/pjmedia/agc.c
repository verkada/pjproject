#include "agc.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define MAX_AMPLITUDE 32768.0f
#define EPSILON 1e-6f

drwav input_wav;
drwav output_wav;

static float calculate_rms(const int16_t *samples, int num_samples)
{
    float sum_squares = 0.0f;
    for (int i = 0; i < num_samples; ++i)
    {
        sum_squares += samples[i] * samples[i];
    }
    return sqrtf(sum_squares / (num_samples + EPSILON));
}

static float dbfs_to_linear(float dbfs)
{
    return powf(10.0f, dbfs / 20.0f);
}

void agc_init(AGC *agc, float target_rms_dbfs, float max_gain, float attack, float release)
{
    agc->target_rms_linear = MAX_AMPLITUDE * dbfs_to_linear(target_rms_dbfs);
    agc->max_gain = max_gain;
    agc->attack = attack;
    agc->release = release;
    agc->gain = 1.0f;
    agc->counter = 0;

    printf("JSS AGC initialized: Target RMS=%.2f linear RMS=%.2f, Max Gain=%.2f, Attack=%.2f, Release=%.2f\n", target_rms_dbfs, agc->target_rms_linear, agc->max_gain, agc->attack, agc->release);

    // if (agc->wav_created)
    // {
    //     return;
    // }

    // if (!drwav_init_file_write(&input_wav, "input.wav", &(drwav_data_format){.container = drwav_container_riff, .format = DR_WAVE_FORMAT_PCM, .channels = 1, .sampleRate = 8000, .bitsPerSample = 16}, NULL))
    // {
    //     fprintf(stderr, "Error opening WAV file for writing\n");
    //     return;
    // }

    // if (!drwav_init_file_write(&output_wav, "output.wav", &(drwav_data_format){.container = drwav_container_riff, .format = DR_WAVE_FORMAT_PCM, .channels = 1, .sampleRate = 8000, .bitsPerSample = 16}, NULL))
    // {
    //     fprintf(stderr, "Error opening WAV file for writing\n");
    //     return;
    // }

    // agc->wav_created = 1;
}

void agc_process(AGC *agc, int16_t *audio_samples, int num_samples)
{
    // Calculate current frame RMS

    usleep(10);

    if (agc->wav_created)
    {
        drwav_write_pcm_frames(&input_wav, num_samples, audio_samples);
    }

    float rms_linear = calculate_rms(audio_samples, num_samples);
    float desired_gain = agc->target_rms_linear / (rms_linear + EPSILON);

    // Clamp the desired gain
    if (desired_gain > agc->max_gain)
    {
        desired_gain = agc->max_gain;
    }

    // Smoothly adjust gain
    if (desired_gain > agc->gain)
    {
        agc->gain += (desired_gain - agc->gain) * agc->attack;
    }
    else
    {
        agc->gain += (desired_gain - agc->gain) * agc->release;
    }

    // Apply gain to the current frame
    for (int i = 0; i < num_samples; ++i)
    {
        float sample = audio_samples[i] * agc->gain;
        if (sample > 32767.0f)
            sample = 32767.0f;
        if (sample < -32768.0f)
            sample = -32768.0f;
        audio_samples[i] = (int16_t)sample;
    }

    // calculate linear RMS in dBFS
    float rms_dbfs = 20.0f * log10f(rms_linear / MAX_AMPLITUDE);

    // Debugging log
    agc->counter++;
    if (agc->counter % 100 == 0)
    {
        printf("JSS RMS (linear)=%.2f, RMS (dBFS)=%.2f, Gain=%.4f, num_samples=%d\n", rms_linear, rms_dbfs, agc->gain, num_samples);
    }

    if (agc->wav_created)
        drwav_write_pcm_frames(&output_wav, num_samples, audio_samples);

    if (agc->counter > 1000)
    {
        drwav_uninit(&input_wav);
        drwav_uninit(&output_wav);
        agc->wav_created = 0;
    }
}
