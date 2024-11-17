#include <portaudio.h>

#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static int audio_callback(const void *input, void *output, unsigned long frames, 
                    const PaStreamCallbackTimeInfo *time, 
                    PaStreamCallbackFlags flags, void *data) {
    float *out = (float *)output; // Use float for 32-bit floating-point samples
    float *state = (float *)data;

    for (int i = 0; i < frames; i++) {
        *out++ = 0.5f * sinf(*state); // Sine wave with amplitude 0.5
        *state += 2.0 * M_PI * 440.0 / 44100.0; // Wrap phase
    }

    return paContinue;
}

int main() {
    PaStream *stream;
    float state = 0.0;

    Pa_Initialize();
    Pa_OpenDefaultStream(&stream, 0, 1, paFloat32, 44100, 1920, audio_callback, &state);
    Pa_StartStream(stream);
    while (1);
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    return 0;
}
