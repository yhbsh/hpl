#include <stdio.h>
#include <stdlib.h>

#include <raylib.h>

#include "video_decoder.h"

#define W 1280
#define H 720

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <url>\n", argv[0]);
        return 0;
    }

    SetTraceLogLevel(LOG_NONE);
    InitWindow(W, H, "WINDOW");
    SetTargetFPS(180);

    VideoDecoder decoder;
    if (vd_decoder_init(&decoder, argv[1], W, H) != 0) {
        CloseWindow();
        return -1;
    }

    uint8_t *data = NULL;
    Image img = {
        .data = NULL,
        .width = W,
        .height = H,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };

    Texture2D texture = LoadTextureFromImage(img);
    while (!WindowShouldClose()) {
        if (vd_decoder_next_frame(&decoder, &data) == 1 && data) { UpdateTexture(texture, data); }

        BeginDrawing();
        DrawTexture(texture, 0, 0, WHITE);
        EndDrawing();
    }

    UnloadTexture(texture);
    vd_decoder_close(&decoder);
    CloseWindow();
    return 0;
}
