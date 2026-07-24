#ifndef RECORDING_STORE_H
#define RECORDING_STORE_H

#include "cJSON.h"

int recording_store_init(void);
int recording_store_capture_profile(cJSON* profile);
int recording_store_clear(const char* profile_id);
int recording_store_reset(void);
cJSON* recording_store_list(void);
cJSON* recording_store_get(const char* profile_id);

#endif