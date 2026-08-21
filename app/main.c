#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>

#include "vdo-stream.h"
#include "vdo-frame.h"
#include "vdo-types.h"
#include "ACAP.h"
#include "cJSON.h"
#include "timelapse.h"
#include "recordings.h"
#include "migration.h"
#include "sunevents.h"
#include "storage.h"
#include "media.h"
#include <sys/statvfs.h>

#define APP_PACKAGE "timelapse2"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

void MAIN_Timelapse_Trigger(cJSON* profile) {
	char* json = cJSON_PrintUnformatted(profile);
	if (json) {
		LOG_TRACE("%s: %s\n", __func__, json);
		free(json);
	}

	// Check if profile has conditions
	const char* conditions = cJSON_GetStringValue(cJSON_GetObjectItem(profile, "conditions"));
	LOG_TRACE("%s: D2D= %d S2S= %d Conditions= %s\n",
              __func__, SunEvents_Between_Dawn_Dusk(), SunEvents_Between_Sunrise_Sunset(), conditions ? conditions : "None");

	if (conditions) {
		if (strcmp(conditions, "dawn-dusk") == 0 && SunEvents_Between_Dawn_Dusk() == 0 ) {
			LOG_TRACE("%s: Condition 'dawn-dusk' not met\n", __func__);
			return;
		}
		if (strcmp(conditions, "sunrise-sunset") == 0 && SunEvents_Between_Sunrise_Sunset() == 0 ) {
			LOG_TRACE("%s: Condition 'sunrise-sunset' not met\n", __func__);
			return;
		}
	}

	// All conditions met or no conditions, capture the recording
	LOG_TRACE("%s: All conditions met, capturing recording\n", __func__);
	Recordings_Capture(profile);
}


void Settings_Updated_Callback(const char* service, cJSON* data) {
    char* json = cJSON_PrintUnformatted(data);
    LOG_TRACE("%s: Service=%s Data=%s\n", __func__, service, json);
    free(json);
}

static void
HTTP_Endpoint_Reset(const ACAP_HTTP_Response response,
                              const ACAP_HTTP_Request request) {
	LOG("Resetting everything\n");

	if (!media_job_try_admit()) {
		ACAP_HTTP_Respond_Error(response, 409, "A media operation is already running");
		return;
	}

	media_storage_transaction_lock();
	media_exclusive_lock();
	Timelapse_Pause();

    char error_message[256];
    if (!storage_reset(error_message, sizeof(error_message))) {
        LOG_WARN("%s: %s\n", __func__, error_message);
		media_exclusive_unlock();
		media_storage_transaction_unlock();
		media_job_release();
        ACAP_HTTP_Respond_Error(response, 500, error_message);
        return;
    }

	Recordings_Reset();
	Timelapse_Reset();
	media_exclusive_unlock();
	media_storage_transaction_unlock();
	media_job_release();
	LOG("Everythin reset\n");
    ACAP_HTTP_Respond_Text(response, "OK");
}

static GMainLoop *main_loop = NULL;
static int services_started = 0;

static gboolean update_storage_status(gpointer user_data);

static void start_services(void) {
	if (services_started) {
		return;
	}

	LOG("Timelapse settings OK\n");
	Timelapse_Init(MAIN_Timelapse_Trigger);
	LOG("Timelapse recording OK\n");
	Recordings_Init();
	LOG("Sun events OK\n");
	SunEvents_Init();
	services_started = 1;
	ACAP_STATUS_SetString("app", "status", "Timelapse is running");
}

static void migration_complete(void) {
	LOG("Migration complete; starting services\n");
	start_services();
}

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
	ACAP_Request_Shutdown();
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

static gboolean update_storage_status(gpointer user_data) {
	struct statvfs storage;
	if (statvfs(storage_root(), &storage) == 0 && storage.f_frsize > 0) {
		unsigned long long total = (unsigned long long)storage.f_blocks * storage.f_frsize;
		unsigned long long free_bytes = (unsigned long long)storage.f_bavail * storage.f_frsize;
		unsigned long long used = total > free_bytes ? total - free_bytes : 0;
		double used_percent = total > 0 ? ((double)used * 100.0) / (double)total : 0.0;

		ACAP_STATUS_SetNumber("sdcard", "totalBytes", (double)total);
		ACAP_STATUS_SetNumber("sdcard", "usedBytes", (double)used);
		ACAP_STATUS_SetNumber("sdcard", "freeBytes", (double)free_bytes);
		ACAP_STATUS_SetNumber("sdcard", "usedPercent", used_percent);
	}
	return G_SOURCE_CONTINUE;
}

static gboolean delayed_Init(gpointer user_data) {
	int dir_ok = 0;
	char error_message[256];
	LOG("SD Card will be initializing\n");

	LOG("Check SD Card\n");
	dir_ok = storage_ensure_root(error_message, sizeof(error_message));
	if (dir_ok) {
		LOG("Directory verified: %s", storage_root());
		ACAP_STATUS_SetBool("sdcard","present",1);
		ACAP_STATUS_SetString("sdcard","message","");
	} else {
		LOG_WARN("%s", error_message);
		ACAP_STATUS_SetBool("sdcard","present",0);
		ACAP_STATUS_SetString("sdcard","message",error_message);
	}

    // 3. Update status and handle services
    if (dir_ok) {
		update_storage_status(NULL);
		Migration_Init(migration_complete);
		if (Migration_Is_Pending()) {
			LOG("Legacy migration pending; recording services paused\n");
			ACAP_STATUS_SetString("app", "status", "Waiting for AVI migration decision");
		} else {
			start_services();
		}
    } else {
        LOG_WARN("Cannot initialize services - directory unavailable");
    }

    LOG("SD Card OK\n");
    return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
    LOG("------ Starting %s ------\n",APP_PACKAGE);
    ACAP_STATUS_SetString("app", "status", "The application is starting");


    ACAP(APP_PACKAGE, Settings_Updated_Callback);

	//Last resort for a corrupt file system on SD Card
    ACAP_HTTP_Node("reset", HTTP_Endpoint_Reset);

	LOG("SD Card will be initialized in 10 seconds\n");
		g_timeout_add_seconds(30, update_storage_status, NULL);
	ACAP_STATUS_SetBool("sdcard","present",0);
	ACAP_STATUS_SetString("sdcard","message","Intializaing SD Card");
	g_timeout_add_seconds(6, delayed_Init, NULL);

    // Create and run the main loop
	main_loop = g_main_loop_new(NULL, FALSE);

    LOG("Entering main loop\n");
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed");
	}

    g_main_loop_run(main_loop);
	LOG("------ Exit %s ------\n",APP_PACKAGE);
    ACAP_Cleanup();
    closelog();
    return 0;
}
