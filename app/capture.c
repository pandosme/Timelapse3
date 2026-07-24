#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "vdo-stream.h"
#include "vdo-frame.h"
#include "vdo-types.h"

#include "ACAP.h"
#include "capture.h"

#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

static int parse_resolution(const char* resolution, int* width, int* height) {
    if (!resolution || !width || !height) {
        return 0;
    }

    char* width_str = strdup(resolution);
    if (!width_str) {
        return 0;
    }

    char* height_str = strchr(width_str, 'x');
    if (!height_str) {
        free(width_str);
        return 0;
    }

    *height_str = '\0';
    height_str++;

    *width = atoi(width_str);
    *height = atoi(height_str);
    free(width_str);

    return *width > 0 && *height > 0;
}

int capture_snapshot(const cJSON* profile, JpegFrame* frame) {
    if (!profile || !frame) {
        return 0;
    }

    memset(frame, 0, sizeof(*frame));

    const char* resolution = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON*)profile, "resolution"));
    int width = 1920;
    int height = 1080;
    if (!parse_resolution(resolution, &width, &height)) {
        LOG_WARN("%s: Invalid resolution: %s\n", __func__, resolution ? resolution : "null");
        return 0;
    }

    VdoMap* vdo_settings = vdo_map_new();
    vdo_map_set_uint32(vdo_settings, "format", VDO_FORMAT_JPEG);
    vdo_map_set_uint32(vdo_settings, "width", width);
    vdo_map_set_uint32(vdo_settings, "height", height);

    cJSON* overlay = cJSON_GetObjectItem((cJSON*)profile, "overlay");
    if (overlay && overlay->type == cJSON_True) {
        vdo_map_set_string(vdo_settings, "overlays", "all,sync");
    }

    GError* error = NULL;
    VdoBuffer* buffer = vdo_stream_snapshot(vdo_settings, &error);
    g_clear_object(&vdo_settings);

    if (error != NULL) {
        LOG_WARN("%s: Snapshot capture failed: %s\n", __func__, error->message);
        g_error_free(error);
        return 0;
    }

    if (!buffer) {
        LOG_WARN("%s: Snapshot capture returned no buffer\n", __func__);
        return 0;
    }

    unsigned char* jpeg_data = vdo_buffer_get_data(buffer);
    unsigned int jpeg_size = vdo_frame_get_size(buffer);
    if (!jpeg_data || !jpeg_size) {
        LOG_WARN("%s: Invalid JPEG capture data\n", __func__);
        g_object_unref(buffer);
        return 0;
    }

    frame->data = malloc(jpeg_size);
    if (!frame->data) {
        LOG_WARN("%s: Failed to allocate JPEG frame buffer\n", __func__);
        g_object_unref(buffer);
        return 0;
    }

    memcpy(frame->data, jpeg_data, jpeg_size);
    frame->size = jpeg_size;
    frame->width = width;
    frame->height = height;
    frame->timestamp_ms = ACAP_DEVICE_Timestamp();

    g_object_unref(buffer);
    return 1;
}

void capture_frame_free(JpegFrame* frame) {
    if (!frame) {
        return;
    }

    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}