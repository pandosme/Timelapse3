#ifndef RECORDING_STORE_H
#define RECORDING_STORE_H

#include "cJSON.h"

int recording_store_init(void);
int recording_store_capture_profile(cJSON* profile);
int recording_store_clear(const char* profile_id);
int recording_store_clear_if_unchanged(const char* profile_id, int expected_frames, double expected_last);
int recording_store_reset(void);

/* The recording state is written by capture on the GLib main thread and read by
 * the FastCGI thread and the media worker threads, so the tree itself is never
 * handed out. Both accessors return a deep copy the caller owns and must
 * cJSON_Delete(); recording_store_get_copy() returns NULL when unknown. */
cJSON* recording_store_snapshot(void);
cJSON* recording_store_get_copy(const char* profile_id);

#endif
