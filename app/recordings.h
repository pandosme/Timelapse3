#ifndef _recordings_h_
#define _recordings_h_

#include "cJSON.h"

int		Recordings_Init(void);
int		Recordings_Capture(cJSON* profile);
int		Recordings_Clear(const char* profileId);
int		Recordings_Delete_Profile_Media(const char* profileId);
/* Both return a deep copy the caller owns and must cJSON_Delete().
   Recordings_Get_Metadata() returns NULL for an unknown profile. */
cJSON*	Recordings_Get_List(void);
cJSON*	Recordings_Get_Metadata(const char* profileId);
void	Recordings_Reset();

#endif
