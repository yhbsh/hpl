#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define CHANNELS 1
#define BUFFER_SIZE 48000 // 1 second of audio

typedef struct {
    float buffer[BUFFER_SIZE];
    volatile size_t writeIndex;
    volatile size_t readIndex;
    volatile size_t availableSamples;
} RingBuffer;

void audio_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
    RingBuffer *ringBuffer = (RingBuffer *)pDevice->pUserData;
    float *pOutputF32      = (float *)pOutput;

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        if (ringBuffer->availableSamples > 0) {
            pOutputF32[i]         = ringBuffer->buffer[ringBuffer->readIndex];
            ringBuffer->readIndex = (ringBuffer->readIndex + 1) % BUFFER_SIZE;
            --ringBuffer->availableSamples;
        } else {
            pOutputF32[i] = 0.0f; // Silence if buffer is empty
        }
    }
}

int main() {
    ma_device_config config;
    ma_device device;
    RingBuffer ringBuffer = {.writeIndex = 0, .readIndex = 0, .availableSamples = 0};
    float phase           = 0.0f;

    config                   = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = CHANNELS;
    config.sampleRate        = SAMPLE_RATE;
    config.dataCallback      = audio_callback;
    config.pUserData         = &ringBuffer;

    if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
        fprintf(stderr, "ERROR: ma_device_init\n");
        return -1;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        fprintf(stderr, "ERROR: ma_device_start\n");
        ma_device_uninit(&device);
        return -2;
    }

    while (1) {
        printf("%zu\n", ringBuffer.writeIndex);
        if (ringBuffer.availableSamples < BUFFER_SIZE) {
            ringBuffer.buffer[ringBuffer.writeIndex] = sinf(phase);
            ringBuffer.writeIndex                    = (ringBuffer.writeIndex + 1) % BUFFER_SIZE;
            ++ringBuffer.availableSamples;

            phase += 2.0f * M_PI * 440.0f / SAMPLE_RATE; // Frequency: 440 Hz
            if (phase >= 2.0f * M_PI) {
                phase -= 2.0f * M_PI;
            }
        }
    }

    ma_device_uninit(&device);

    return 0;
}
