#ifndef _timelapse_
#define _timelapse_

#include "cJSON.h"

#ifdef  __cplusplus
extern "C" {
#endif

typedef void (*Timelapse_Callback)(cJSON* profile);

int		Timelapse_Init( Timelapse_Callback callback );
int 	Timelapse_Save_Profiles(void);
void	Timelapse_Reset(void);
int		Timelapse_Remove_Profile_By_Id( const char* id );

/* The profile store is shared between the GLib main thread, the FastCGI thread
 * and the media worker threads, so nothing outside timelapse.c may hold a
 * pointer into it. These accessors take the store lock and hand back either a
 * value or a deep copy the caller owns and must cJSON_Delete().             */
cJSON*	Timelapse_Get_Profiles(void);                 /* copy of the array   */
cJSON*	Timelapse_Get_Profile( const char* id );      /* copy, or NULL       */
int		Timelapse_Get_Profile_Int( const char* id, const char* key, int fallback );
int		Timelapse_Set_Profile_Number( const char* id, const char* key, double value );

#ifdef  __cplusplus
}
#endif

#endif
