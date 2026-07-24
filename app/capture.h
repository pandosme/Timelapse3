#ifndef CAPTURE_H
#define CAPTURE_H

#include "cJSON.h"

typedef struct {
    unsigned char* data;
    unsigned int size;
    int width;
    int height;
    double timestamp_ms;
} JpegFrame;

int capture_snapshot(const cJSON* profile, JpegFrame* frame);
void capture_frame_free(JpegFrame* frame);

#endif