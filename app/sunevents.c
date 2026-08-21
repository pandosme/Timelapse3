#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <math.h>
#include <glib.h>
#include "ACAP.h"
#include "cJSON.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...) {}

static cJSON* SunEventsSettings = NULL;
static GSource* midnight_timer = NULL;
static GSource* sunnoon_timer = NULL;
static time_t last_scheduled_noon = 0;
static GRecMutex sun_events_lock;
static GThread* sun_events_main_thread = NULL;

static void Calculate_Sun_Events(double lat, double lon);
static void Setup_Midnight_Timer();

typedef struct {
    GMutex lock;
    GCond cond;
    double lat;
    double lon;
    int result;
    int done;
    int started;
    int abandoned;
} SunEventsUpdate;

static void SunEvents_Update_Free(SunEventsUpdate* update) {
    g_mutex_clear(&update->lock);
    g_cond_clear(&update->cond);
    g_free(update);
}

static double to_rad(double deg) {
    return deg * M_PI / 180.0;
}

static double to_deg(double rad) {
    return rad * 180.0 / M_PI;
}

// Timer callback for solar noon
static gboolean SunNoon_Timer_Callback(gpointer user_data) {
    LOG_TRACE("%s: Sun noon event triggered\n", __func__);
    ACAP_EVENTS_Fire("sunnoon");
    return G_SOURCE_REMOVE;  // Return NULL instead of continuing
}


static void Setup_SunNoon_Timer(time_t noon) {
    time_t now;
    time(&now);
    int seconds_to_noon = (int)(noon - now);
    
    if (seconds_to_noon < 0) {
        seconds_to_noon += 24 * 3600;
    }
    
    LOG_TRACE("%s: Input %d\n", __func__, seconds_to_noon);
    
    // Always clean up existing timer
    if (sunnoon_timer) {
        g_source_destroy(sunnoon_timer);
        g_source_unref(sunnoon_timer);
        sunnoon_timer = NULL;
    }
    
    LOG_TRACE("%s: Timer to sun noon %d\n", __func__, seconds_to_noon);
    sunnoon_timer = g_timeout_source_new_seconds(seconds_to_noon);
    g_source_set_callback(sunnoon_timer, SunNoon_Timer_Callback, NULL, NULL);
    g_source_attach(sunnoon_timer, NULL);
}



// Setup timer for next midnight
static gboolean Midnight_Timer_Callback(gpointer user_data) {
    LOG_TRACE("%s: Midnight timer triggered\n", __func__);
    g_rec_mutex_lock(&sun_events_lock);
    cJSON* lat_item = SunEventsSettings ? cJSON_GetObjectItem(SunEventsSettings, "lat") : NULL;
    cJSON* lon_item = SunEventsSettings ? cJSON_GetObjectItem(SunEventsSettings, "lon") : NULL;
    double lat = lat_item ? lat_item->valuedouble : 0.0;
    double lon = lon_item ? lon_item->valuedouble : 0.0;
    g_rec_mutex_unlock(&sun_events_lock);
    Calculate_Sun_Events(lat, lon);
	Setup_Midnight_Timer();
    return G_SOURCE_REMOVE;  // Return NULL instead of continuing
}

static void Setup_Midnight_Timer() {
    if (!SunEventsSettings) return;
    
    time_t now;
    time(&now);
    struct tm local_time;
    struct tm* local = localtime_r(&now, &local_time);
    int seconds_to_midnight = ((23 - local->tm_hour) * 3600) + 
                            ((59 - local->tm_min) * 60) + 
                            (60 - local->tm_sec);
    
    LOG_TRACE("%s: Setting up timer for %d seconds\n", __func__, seconds_to_midnight);
    
    if (midnight_timer) {
        g_source_destroy(midnight_timer);
        g_source_unref(midnight_timer);
        midnight_timer = NULL;
    }
    
    midnight_timer = g_timeout_source_new_seconds(seconds_to_midnight);
    if (!midnight_timer) {
        LOG_WARN("%s: Failed to create timer source\n", __func__);
        return;
    }
    
    g_source_set_callback(midnight_timer, Midnight_Timer_Callback, NULL, NULL);
    g_source_attach(midnight_timer, NULL);
}


static int Apply_SunEvents_Update(double lat, double lon) {
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        LOG_WARN("%s: Invalid coordinates lat=%f, lon=%f\n", __func__, lat, lon);
        return 0;
    }

	if( !ACAP_DEVICE_Set_Location( lat, lon) )
		LOG_WARN("%s: Error storing GeoLocation\n",__func__);
    LOG_TRACE("%s: Setting location to lat=%f, lon=%f\n", __func__, lat, lon);
    Calculate_Sun_Events(lat, lon);
    return 1;
}

static gboolean Run_SunEvents_Update(gpointer user_data) {
    SunEventsUpdate* update = (SunEventsUpdate*)user_data;
    g_mutex_lock(&update->lock);
    if (update->abandoned) {
        g_mutex_unlock(&update->lock);
        SunEvents_Update_Free(update);
        return G_SOURCE_REMOVE;
    }
    update->started = 1;
    g_mutex_unlock(&update->lock);

    int result = Apply_SunEvents_Update(update->lat, update->lon);
    g_mutex_lock(&update->lock);
    update->result = result;
    update->done = 1;
    g_cond_signal(&update->cond);
    g_mutex_unlock(&update->lock);
    return G_SOURCE_REMOVE;
}

int SunEvents_Set(cJSON* location) {
    if (!location || !SunEventsSettings) return -1;

    cJSON* lon_obj = cJSON_GetObjectItem(location, "lon");
    cJSON* lat_obj = cJSON_GetObjectItem(location, "lat");
    if (!lon_obj || !lat_obj) return -1;

    double lon = lon_obj->valuedouble;
    double lat = lat_obj->valuedouble;
    if (g_thread_self() == sun_events_main_thread) {
        return Apply_SunEvents_Update(lat, lon);
    }
    if (ACAP_Shutdown_Requested()) {
        return -1;
    }

    SunEventsUpdate* update = g_new0(SunEventsUpdate, 1);
    if (!update) {
        return -1;
    }
    update->lat = lat;
    update->lon = lon;
    g_mutex_init(&update->lock);
    g_cond_init(&update->cond);
    g_main_context_invoke(NULL, Run_SunEvents_Update, update);
    g_mutex_lock(&update->lock);
    while (!update->done) {
        if (!g_cond_wait_until(&update->cond, &update->lock, g_get_monotonic_time() + G_TIME_SPAN_SECOND) &&
            ACAP_Shutdown_Requested() && !update->started) {
            update->abandoned = 1;
            g_mutex_unlock(&update->lock);
            return -1;
        }
    }
    int result = update->result;
    g_mutex_unlock(&update->lock);
    SunEvents_Update_Free(update);
    return result;
}

int SunEvents_Between_Dawn_Dusk() {
    g_rec_mutex_lock(&sun_events_lock);
    if (!SunEventsSettings) {
        LOG_WARN("%s: SunEventsSettings is NULL\n", __func__);
        g_rec_mutex_unlock(&sun_events_lock);
        return 0;
    }

    cJSON* dawn_obj = cJSON_GetObjectItem(SunEventsSettings, "dawn");
    cJSON* dusk_obj = cJSON_GetObjectItem(SunEventsSettings, "dusk");
    if (!dawn_obj || !dusk_obj) {
        LOG_WARN("%s: Missing dawn or dusk in SunEventsSettings\n", __func__);
        g_rec_mutex_unlock(&sun_events_lock);
        return 0;
    }

    time_t dawn = (time_t)dawn_obj->valuedouble;
    time_t dusk = (time_t)dusk_obj->valuedouble;
    g_rec_mutex_unlock(&sun_events_lock);

    time_t now;
    time(&now);

    // Calculate seconds since midnight for now
    struct tm tm_now;
    localtime_r(&now, &tm_now); // Thread-safe version of localtime()
    time_t midnight_today = now - (tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec);
    int now_seconds = now - midnight_today;

    // Calculate seconds since midnight for dawn
    struct tm tm_dawn;
    localtime_r(&dawn, &tm_dawn); // Thread-safe version of localtime()
    time_t midnight_dawn = dawn - (tm_dawn.tm_hour * 3600 + tm_dawn.tm_min * 60 + tm_dawn.tm_sec);
    int dawn_seconds = dawn - midnight_dawn;

    // Calculate seconds since midnight for dusk
    struct tm tm_dusk;
    localtime_r(&dusk, &tm_dusk); // Thread-safe version of localtime()
    time_t midnight_dusk = dusk - (tm_dusk.tm_hour * 3600 + tm_dusk.tm_min * 60 + tm_dusk.tm_sec);
    int dusk_seconds = dusk - midnight_dusk;

    LOG_TRACE("%s: now_seconds=%d, dawn_seconds=%d, dusk_seconds=%d\n",
              __func__, now_seconds, dawn_seconds, dusk_seconds);

    return (now_seconds >= dawn_seconds && now_seconds <= dusk_seconds) ? 1 : 0;
}

int SunEvents_Between_Sunrise_Sunset() {
    g_rec_mutex_lock(&sun_events_lock);
    if (!SunEventsSettings) {
        LOG_WARN("%s: SunEventsSettings is NULL\n", __func__);
        g_rec_mutex_unlock(&sun_events_lock);
        return 0;
    }

    cJSON* sunrise_obj = cJSON_GetObjectItem(SunEventsSettings, "sunrise");
    cJSON* sunset_obj = cJSON_GetObjectItem(SunEventsSettings, "sunset");
    if (!sunrise_obj || !sunset_obj) {
        LOG_WARN("%s: Missing sunrise or sunset in SunEventsSettings\n", __func__);
        g_rec_mutex_unlock(&sun_events_lock);
        return 0;
    }

    time_t sunrise = (time_t)sunrise_obj->valuedouble;
    time_t sunset = (time_t)sunset_obj->valuedouble;
    g_rec_mutex_unlock(&sun_events_lock);

    time_t now;
    time(&now);

    // Calculate seconds since midnight for now
    struct tm tm_now;
    localtime_r(&now, &tm_now); // Thread-safe version of localtime()
    time_t midnight_today = now - (tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec);
    int now_seconds = now - midnight_today;

    // Calculate seconds since midnight for sunrise
    struct tm tm_sunrise;
    localtime_r(&sunrise, &tm_sunrise); // Thread-safe version of localtime()
    time_t midnight_sunrise = sunrise - (tm_sunrise.tm_hour * 3600 + tm_sunrise.tm_min * 60 + tm_sunrise.tm_sec);
    int sunrise_seconds = sunrise - midnight_sunrise;

    // Calculate seconds since midnight for sunset
    struct tm tm_sunset;
    localtime_r(&sunset, &tm_sunset); // Thread-safe version of localtime()
    time_t midnight_sunset = sunset - (tm_sunset.tm_hour * 3600 + tm_sunset.tm_min * 60 + tm_sunset.tm_sec);
    int sunset_seconds = sunset - midnight_sunset;

    LOG_TRACE("%s: now_seconds=%d, sunrise_seconds=%d, sunset_seconds=%d\n",
              __func__, now_seconds, sunrise_seconds, sunset_seconds);

    return (now_seconds >= sunrise_seconds && now_seconds <= sunset_seconds) ? 1 : 0;
}

static void HTTP_Endpoint_Sunevents(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        LOG_WARN("%s: Invalid Request Method\n", __func__);
        ACAP_HTTP_Respond_Error(response, 400, "Invalid Request Method");
        return;
    }
    
    LOG_TRACE("%s: Method=%s\n", __func__, method);
    
    if (strcmp(method, "POST") == 0) {
        if (!request->postData) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing POST data");
            return;
        }
        
        cJSON* location = cJSON_Parse(request->postData);
        if (!location) {
            ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON data");
            return;
        }
        
        int result = SunEvents_Set(location);
        cJSON_Delete(location);
        if (result <= 0) {
            ACAP_HTTP_Respond_Error(response, 400, "Invalid sun-event location");
            return;
        }
        g_rec_mutex_lock(&sun_events_lock);
        cJSON* snapshot = cJSON_Duplicate(SunEventsSettings, 1);
        g_rec_mutex_unlock(&sun_events_lock);
        ACAP_HTTP_Respond_JSON(response, snapshot);
        cJSON_Delete(snapshot);
        return;
    }
    
	if (strcmp(method, "GET") == 0) {
		if (!SunEventsSettings) {
			LOG_WARN("%s: SunEventsSettings is NULL\n", __func__);
			ACAP_HTTP_Respond_Error(response, 500, "Sun Events not initialized");
			return;
		}
		
        g_rec_mutex_lock(&sun_events_lock);
        cJSON* snapshot = cJSON_Duplicate(SunEventsSettings, 1);
        g_rec_mutex_unlock(&sun_events_lock);
        ACAP_HTTP_Respond_JSON(response, snapshot);
        cJSON_Delete(snapshot);
		return;
	}
    
    ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
}

int SunEvents_Init() {
    LOG_TRACE("%s: Initializing sun events\n", __func__);
    sun_events_main_thread = g_thread_self();
    
    double lat = ACAP_DEVICE_Latitude();
	double lon = ACAP_DEVICE_Longitude();

    SunEventsSettings = cJSON_CreateObject();
    if (!SunEventsSettings) {
        LOG_WARN("%s: Failed to create settings object\n", __func__);
        return -1;
    }

    // Initialize all required fields
    cJSON_AddNumberToObject(SunEventsSettings, "lat", lat);
    cJSON_AddNumberToObject(SunEventsSettings, "lon", lon);
    cJSON_AddNumberToObject(SunEventsSettings, "dawn", 0);
    cJSON_AddNumberToObject(SunEventsSettings, "sunrise", 0);
    cJSON_AddNumberToObject(SunEventsSettings, "sunnoon", 0);
    cJSON_AddNumberToObject(SunEventsSettings, "sunset", 0);
    cJSON_AddNumberToObject(SunEventsSettings, "dusk", 0);

    
    Calculate_Sun_Events(lat, lon);
    Setup_Midnight_Timer();
    ACAP_HTTP_Node("sunevents", HTTP_Endpoint_Sunevents);
    
    return 0;
}

static void Calculate_Sun_Events(double lat, double lon) {
	if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
		LOG_WARN("%s: Invalid coordinates lat=%f, lon=%f\n", __func__, lat, lon);
		return;
	}

	LOG_TRACE("%s: Calculating sun events for lat=%f, lon=%f\n", __func__, lat, lon);

	time_t now;
	time(&now);

    struct tm local_time;
    struct tm* local = localtime_r(&now, &local_time);
	if (!local) {
		LOG_WARN("%s: Failed to get local time\n", __func__);
		return;
	}

	// Day of year
	int n = local->tm_yday + 1;

	// Solar declination
	double declination = 23.45 * sin(to_rad((360 / 365.0) * (n - 81)));

	// Equation of time
	double B = to_rad((360 / 365.0) * (n - 81));
	double equation_of_time = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);

	// Solar noon in UTC
	double solar_noon_utc = (12.0 - (lon / 15.0)) - (equation_of_time / 60.0);

	// Hour angle for sunrise/sunset
	double lat_rad = to_rad(lat);
	double decl_rad = to_rad(declination);
	double ha_sunrise = acos(cos(to_rad(90.833)) / (cos(lat_rad) * cos(decl_rad)) -
							 tan(lat_rad) * tan(decl_rad));

	// Convert hour angle to hours
	double ha_hours_sunrise = to_deg(ha_sunrise) / 15.0;

	// Calculate sunrise and sunset in UTC
	double sunrise_utc = solar_noon_utc - ha_hours_sunrise;
	double sunset_utc = solar_noon_utc + ha_hours_sunrise;

	// Hour angle for civil twilight (dawn/dusk)
	double ha_twilight = acos(cos(to_rad(96)) / (cos(lat_rad) * cos(decl_rad)) -
							  tan(lat_rad) * tan(decl_rad));

	// Convert hour angle to hours
	double ha_hours_twilight = to_deg(ha_twilight) / 15.0;

	// Calculate dawn and dusk in UTC
	double dawn_utc = solar_noon_utc - ha_hours_twilight;
	double dusk_utc = solar_noon_utc + ha_hours_twilight;

	// Convert UTC times to local time
	int timezone_offset_seconds = local->tm_gmtoff; // Offset in seconds from UTC
	time_t midnight = now - (local->tm_hour * 3600 + local->tm_min * 60 + local->tm_sec);

	time_t dawn = midnight + (time_t)(dawn_utc * 3600) + timezone_offset_seconds;
	time_t sunrise = midnight + (time_t)(sunrise_utc * 3600) + timezone_offset_seconds;
	time_t solar_noon = midnight + (time_t)(solar_noon_utc * 3600) + timezone_offset_seconds;
	time_t sunset = midnight + (time_t)(sunset_utc * 3600) + timezone_offset_seconds;
	time_t dusk = midnight + (time_t)(dusk_utc * 3600) + timezone_offset_seconds;

	LOG_TRACE("%s: Calculated times - Dawn: %lld, Sunrise: %lld, Noon: %lld, Sunset: %lld, Dusk: %lld\n",
			  __func__, (long long)dawn, (long long)sunrise,
			  (long long)solar_noon, (long long)sunset,
			  (long long)dusk);

	// Validate calculated times
	if (dawn < midnight || dawn > midnight + 24 * 3600 ||
	   sunrise < midnight || sunrise > midnight + 24 * 3600 ||
	   solar_noon < midnight || solar_noon > midnight + 24 * 3600 ||
	   sunset < midnight || sunset > midnight + 24 * 3600 ||
	   dusk < midnight || dusk > midnight + 24 * 3600) {
	   LOG_WARN("%s: One or more calculated times are out of range\n", __func__);
	   return;
	}

    // Update JSON object with calculated values
    g_rec_mutex_lock(&sun_events_lock);
	cJSON_ReplaceItemInObject(SunEventsSettings, "lat", cJSON_CreateNumber(lat));
	cJSON_ReplaceItemInObject(SunEventsSettings, "lon", cJSON_CreateNumber(lon));
	cJSON_ReplaceItemInObject(SunEventsSettings, "dawn", cJSON_CreateNumber((double)dawn));
	cJSON_ReplaceItemInObject(SunEventsSettings, "sunrise", cJSON_CreateNumber((double)sunrise));
	cJSON_ReplaceItemInObject(SunEventsSettings, "sunnoon", cJSON_CreateNumber((double)solar_noon));
	cJSON_ReplaceItemInObject(SunEventsSettings, "sunset", cJSON_CreateNumber((double)sunset));
	cJSON_ReplaceItemInObject(SunEventsSettings, "dusk", cJSON_CreateNumber((double)dusk));
   
	char* json = cJSON_PrintUnformatted(SunEventsSettings);
	if(json) {
		LOG_TRACE("%s: %s\n",__func__,json);
		free(json);
	}
    g_rec_mutex_unlock(&sun_events_lock);
	
    // Setup timer for solar noon
    Setup_SunNoon_Timer(solar_noon);
}
