#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib.h>
#include "ACAP.h"
#include "cJSON.h"
#include "timelapse.h"
#include "recordings.h"
#include "media.h"
#include "storage.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define PROFILE_ID_MAX 128

/* Concurrency model
 * -----------------
 * Two threads reach this module: the GLib main thread (capture timers, event
 * dispatch, midnight jobs) and the FastCGI thread (every HTTP endpoint). Media
 * worker threads reach it too, via the public read accessors.
 *
 * Rules, and the reason each one exists:
 *
 * 1. TimelapseProfiles and the timer/subscription tables are only ever read or
 *    written while holding profiles_lock. Two threads running cJSON alloc/free
 *    over the same tree corrupts the heap; that is what used to abort the
 *    process with "malloc(): unaligned tcache chunk detected" when a profile
 *    was saved while its capture timer was due.
 *
 * 2. Profile lifecycle changes (add / remove / reload) additionally run on the
 *    GLib main thread, marshalled there by Run_On_Main_Thread(). GSource and
 *    ax_event subscriptions are torn down here, and neither g_source_destroy()
 *    nor ax_event_handler_unsubscribe() waits for a callback that is already
 *    executing. Doing the teardown on the dispatch thread makes that wait
 *    unnecessary: a callback and a teardown can no longer overlap.
 *
 * 3. Nothing outside this file ever holds a pointer into TimelapseProfiles.
 *    The public accessors hand out deep copies, and callbacks are handed a
 *    copy too. A background archive job can run ffmpeg for minutes on its
 *    snapshot without caring that the profile was edited meanwhile.
 *
 * 4. Content-only updates (Timelapse_Set_Profile_Number, Timelapse_Save_Profiles)
 *    take the lock but are NOT marshalled to the main thread. Media workers call
 *    them, and the main thread waits for those workers at shutdown - marshalling
 *    would close that loop into a deadlock.
 */
static cJSON *TimelapseProfiles = NULL;
static Timelapse_Callback Timelapse_ServiceCallBack = 0;

static GRecMutex profiles_lock;
static GThread* main_thread = NULL;
static int timelapse_initialized = 0;

/* id -> GSource* (owned by the table) */
static GHashTable* timelapse_timers = NULL;
/* id -> EventSubscription* (owned by the table) */
static GHashTable* timelapse_subscriptions = NULL;

typedef struct {
    char profile_id[PROFILE_ID_MAX];
    int  interval;
} TimelapseTimer;

typedef struct {
    int    subscription_id;
    cJSON* id_node;   /* owned; handed to ACAP_EVENTS_Subscribe as user_data */
} EventSubscription;

static void Dispatch_Profile_Trigger(const char* profile_id);

/*-----------------------------------------------------
 * Main-thread marshalling
 *-----------------------------------------------------*/

typedef struct {
    GMutex   lock;
    GCond    cond;
    gboolean done;
    gboolean started;
    gboolean abandoned;
    int      result;
    int    (*fn)(void* arg);
    void*    arg;
} MainThreadTask;

static void Main_Thread_Task_Free(MainThreadTask* task) {
    g_mutex_clear(&task->lock);
    g_cond_clear(&task->cond);
    g_free(task);
}

static gboolean Run_Main_Thread_Task(gpointer user_data) {
    MainThreadTask* task = (MainThreadTask*)user_data;
    g_mutex_lock(&task->lock);
    if (task->abandoned) {
        g_mutex_unlock(&task->lock);
        Main_Thread_Task_Free(task);
        return G_SOURCE_REMOVE;
    }
    task->started = TRUE;
    g_mutex_unlock(&task->lock);

    int result = task->fn(task->arg);

    g_mutex_lock(&task->lock);
    task->result = result;
    task->done = TRUE;
    g_cond_signal(&task->cond);
    g_mutex_unlock(&task->lock);
    return G_SOURCE_REMOVE;
}

/* Runs fn on the GLib main thread and blocks until it has finished. Runs it
 * inline when the caller already is the main thread, and also before the main
 * loop exists (startup), where there is nothing to marshal to yet. */
static int Run_On_Main_Thread(int (*fn)(void*), void* arg) {
    if (!main_thread || g_thread_self() == main_thread) {
        return fn(arg);
    }
    if (ACAP_Shutdown_Requested()) {
        return 0;
    }

    MainThreadTask* task = g_new0(MainThreadTask, 1);
    if (!task) {
        return 0;
    }
    g_mutex_init(&task->lock);
    g_cond_init(&task->cond);
    task->fn = fn;
    task->arg = arg;

    g_main_context_invoke(NULL, Run_Main_Thread_Task, task);

    g_mutex_lock(&task->lock);
    while (!task->done) {
        if (!g_cond_wait_until(&task->cond, &task->lock, g_get_monotonic_time() + G_TIME_SPAN_SECOND) &&
            ACAP_Shutdown_Requested() && !task->started) {
            task->abandoned = TRUE;
            g_mutex_unlock(&task->lock);
            return 0;
        }
    }
    int result = task->result;
    g_mutex_unlock(&task->lock);

    Main_Thread_Task_Free(task);
    return result;
}

/*-----------------------------------------------------
 * Timers
 *-----------------------------------------------------*/

static gboolean
Timer_Callback(gpointer user_data) {
    const TimelapseTimer* timer = (const TimelapseTimer*)user_data;
    if (timer) {
        Dispatch_Profile_Trigger(timer->profile_id);
    }
    return G_SOURCE_CONTINUE;
}

/* GHashTable value destructor for the timer table. The TimelapseTimer payload is
 * owned by the GSource (freed through the callback's GDestroyNotify), not by us:
 * GLib keeps the callback data alive for the duration of a dispatch, so removing
 * a timer while its callback is mid-flight is safe. */
static void Timer_Source_Dispose(gpointer data) {
    GSource* source = (GSource*)data;
    if (!source) {
        return;
    }
    g_source_destroy(source);
    g_source_unref(source);
}

/* Main thread, profiles_lock held. */
static void Cleanup_Timer(const char* id) {
    if (!timelapse_timers || !id) {
        return;
    }
    if (g_hash_table_remove(timelapse_timers, id)) {
        LOG_TRACE("%s: Removed timer for profile %s\n", __func__, id);
    }
}

/* Main thread, profiles_lock held. Replacing an existing entry disposes the old
 * source through Timer_Source_Dispose. */
static void Setup_Timer(const char* id, int interval) {
    if (!timelapse_timers || !id || interval <= 0) {
        LOG_WARN("%s: Skipping, invalid timer for profile %s\n", __func__, id ? id : "(null)");
        return;
    }

    TimelapseTimer* payload = g_new0(TimelapseTimer, 1);
    g_strlcpy(payload->profile_id, id, sizeof(payload->profile_id));
    payload->interval = interval;

    GSource* source = g_timeout_source_new_seconds(interval);
    g_source_set_callback(source, Timer_Callback, payload, g_free);
    g_source_attach(source, NULL);

    g_hash_table_insert(timelapse_timers, g_strdup(id), source);
    LOG("%s: Started repeating timer for %s every %d seconds\n", __func__, id, interval);
}

/*-----------------------------------------------------
 * Event subscriptions
 *-----------------------------------------------------*/

/* GHashTable value destructor. Main thread only - see rule 2 at the top. */
static void Event_Subscription_Dispose(gpointer data) {
    EventSubscription* sub = (EventSubscription*)data;
    if (!sub) {
        return;
    }
    if (sub->subscription_id) {
        ACAP_EVENTS_Unsubscribe(sub->subscription_id);
    }
    /* Freed only after unsubscribing: the event handler holds this node as its
     * user_data, so it must outlive the subscription. */
    if (sub->id_node) {
        cJSON_Delete(sub->id_node);
    }
    g_free(sub);
}

/* Main thread, profiles_lock held. */
static void Cleanup_Subscription(const char* id) {
    if (!timelapse_subscriptions || !id) {
        return;
    }
    g_hash_table_remove(timelapse_subscriptions, id);
}

/* Main thread, profiles_lock held. Returns the subscription id, or 0 on failure. */
static int Setup_Subscription(const char* id, cJSON* triggerEvent) {
    if (!timelapse_subscriptions || !id || !triggerEvent) {
        return 0;
    }

    Cleanup_Subscription(id);

    EventSubscription* sub = g_new0(EventSubscription, 1);
    /* The subscription carries the profile id, never the profile itself: the
     * profile object is replaced on every edit, the id is stable. */
    sub->id_node = cJSON_CreateString(id);
    if (!sub->id_node) {
        g_free(sub);
        return 0;
    }

    sub->subscription_id = ACAP_EVENTS_Subscribe(triggerEvent, (void*)sub->id_node);
    if (!sub->subscription_id) {
        cJSON_Delete(sub->id_node);
        g_free(sub);
        return 0;
    }

    g_hash_table_insert(timelapse_subscriptions, g_strdup(id), sub);
    return sub->subscription_id;
}

/*-----------------------------------------------------
 * Trigger dispatch
 *-----------------------------------------------------*/

/* Hands the service callback a private copy, so a concurrent profile edit can
 * never pull the object out from under a capture in progress. */
static void Dispatch_Profile_Trigger(const char* profile_id) {
    if (!profile_id || !Timelapse_ServiceCallBack) {
        return;
    }

    cJSON* snapshot = Timelapse_Get_Profile(profile_id);
    if (!snapshot) {
        LOG_TRACE("%s: Profile %s no longer exists\n", __func__, profile_id);
        return;
    }

    Timelapse_ServiceCallBack(snapshot);
    cJSON_Delete(snapshot);
}

void
Timelapse_Event_Callback(cJSON *event, void* user_data) {
    //Only capture on triggers and stateful true
    cJSON* props = event ? event->child : NULL;
    while (props) {
        if (props->type == cJSON_False) {
            return;
        }
        props = props->next;
    }

    const char* profile_id = user_data ? ((cJSON*)user_data)->valuestring : NULL;
    if (!profile_id) {
        LOG_WARN("%s: Event without a profile id\n", __func__);
        return;
    }

    Dispatch_Profile_Trigger(profile_id);
}

/*-----------------------------------------------------
 * Profile helpers
 *-----------------------------------------------------*/

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int estimate_images_per_day(cJSON* profile) {
    cJSON* timer = cJSON_GetObjectItem(profile, "timer");
    if (timer && timer->type == cJSON_Number && timer->valueint > 0) {
        return clamp_int((86400 + timer->valueint - 1) / timer->valueint, 1, 86400);
    }
    return 1;
}

static const char* archive_schedule_from_days(int days) {
    if (days <= 1) return "daily";
    if (days <= 7) return "weekly";
    return "monthly";
}

static int is_valid_archive_schedule(const char* schedule) {
    return schedule && (
    strcmp(schedule, "none") == 0 ||
        strcmp(schedule, "daily") == 0 ||
        strcmp(schedule, "weekly") == 0 ||
        strcmp(schedule, "monthly") == 0);
}

static void ensure_archive_defaults(cJSON* profile) {
    const char* schedule = cJSON_GetStringValue(cJSON_GetObjectItem(profile, "archiveSchedule"));
    if (!is_valid_archive_schedule(schedule)) {
        cJSON* archive_interval = cJSON_GetObjectItem(profile, "archiveIntervalDays");
        const char* default_schedule = archive_interval && archive_interval->type == cJSON_Number ?
            archive_schedule_from_days(archive_interval->valueint) : "monthly";
        cJSON_DeleteItemFromObject(profile, "archiveSchedule");
        cJSON_AddStringToObject(profile, "archiveSchedule", default_schedule);
    }

    cJSON_DeleteItemFromObject(profile, "archiveIntervalDays");

    cJSON* expected_images = cJSON_GetObjectItem(profile, "expectedImagesPerDay");
    if (!expected_images || expected_images->type != cJSON_Number || expected_images->valueint < 1) {
        if (expected_images) {
            cJSON_DeleteItemFromObject(profile, "expectedImagesPerDay");
        }
        cJSON_AddNumberToObject(profile, "expectedImagesPerDay", estimate_images_per_day(profile));
    } else if (expected_images->valueint > 86400) {
        cJSON_SetNumberValue(expected_images, 86400);
    }
}

/*-----------------------------------------------------
 * Re-encode background job
 *-----------------------------------------------------*/

typedef struct {
    char profile_id[PROFILE_ID_MAX];
    int old_fps;
    int new_fps;
} ReencodeTask;

static gpointer Reencode_Export_Thread(gpointer user_data) {
    ReencodeTask* task = (ReencodeTask*)user_data;
    if (!task) {
        ACAP_Background_Job_End();
        return NULL;
    }

    while (!media_job_try_admit()) {
        if (ACAP_Shutdown_Requested()) {
            g_free(task);
            ACAP_Background_Job_End();
            return NULL;
        }
        g_usleep(100 * 1000);
    }

    LOG("%s: start profile=%s old_fps=%d new_fps=%d\n", __func__, task->profile_id, task->old_fps, task->new_fps);
    if (!media_reencode_export(task->profile_id, task->old_fps, task->new_fps)) {
        LOG_WARN("%s: failed profile=%s (%d->%d): %s\n", __func__, task->profile_id, task->old_fps, task->new_fps, media_last_error());
    } else {
        LOG("%s: done profile=%s (%d->%d)\n", __func__, task->profile_id, task->old_fps, task->new_fps);
    }

    g_free(task);
    media_job_release();
    ACAP_Background_Job_End();
    return NULL;
}

static void Queue_Reencode_Export(const char* profile_id, int old_fps, int new_fps) {
    if (!profile_id || old_fps == new_fps) {
        return;
    }

    ReencodeTask* task = g_new0(ReencodeTask, 1);
    if (!task) {
        LOG_WARN("%s: failed to allocate task for %s\n", __func__, profile_id);
        return;
    }

    snprintf(task->profile_id, sizeof(task->profile_id), "%s", profile_id);
    task->old_fps = old_fps;
    task->new_fps = new_fps;

    ACAP_Background_Job_Begin();
    GThread* thread = g_thread_new("reencode", Reencode_Export_Thread, task);
    if (!thread) {
        ACAP_Background_Job_End();
        LOG_WARN("%s: failed to create re-encode thread for %s\n", __func__, profile_id);
        g_free(task);
        return;
    }
    g_thread_unref(thread);
}

static int Ensure_Directory_Exists(const char* id) {
    char profiles_path[1024];
    char path[1024];
    char error[256];

    if (!storage_profiles_dir(profiles_path, sizeof(profiles_path)) ||
        !storage_profile_dir(path, sizeof(path), id)) {
        LOG_WARN("%s: Failed to build profile directory for %s\n", __func__, id);
        return -1;
    }

    if (!storage_ensure_directory(profiles_path, error, sizeof(error))) {
        LOG_WARN("%s: %s\n", __func__, error);
        return -1;
    }

    LOG_TRACE("%s: Checking directory %s\n", __func__, path);
    if (!storage_ensure_directory(path, error, sizeof(error))) {
        LOG_WARN("%s: %s\n", __func__, error);
        return -1;
    }

    return 0;
}

/*-----------------------------------------------------
 * Profile store - internal, profiles_lock held by caller
 *-----------------------------------------------------*/

static cJSON* Find_Profile_Locked(const char* id) {
    if (!TimelapseProfiles || !id) {
        return NULL;
    }
    cJSON* profile = TimelapseProfiles->child;
    while (profile) {
        cJSON* item = cJSON_GetObjectItem(profile, "id");
        if (item && item->valuestring && strcmp(item->valuestring, id) == 0) {
            return profile;
        }
        profile = profile->next;
    }
    return NULL;
}

static int Save_Profiles_Locked(void) {
    char profiles_path[1024];
    if (!storage_profiles_path(profiles_path, sizeof(profiles_path))) {
        LOG_WARN("%s: Failed to build profiles path\n", __func__);
        return -1;
    }

    LOG_TRACE("%s: Saving profiles to %s\n", __func__, profiles_path);
    if (!TimelapseProfiles) {
        TimelapseProfiles = cJSON_CreateArray();
    }

    FILE *file = fopen(profiles_path, "w");
    if (!file) {
        LOG_WARN("Error opening %s for writing\n", profiles_path);
        return -1;
    }

    char *json_str = cJSON_Print(TimelapseProfiles);
    if (!json_str) {
        LOG_WARN("Error converting profiles to JSON string\n");
        fclose(file);
        return -1;
    }

    size_t written = fwrite(json_str, strlen(json_str), 1, file);
    free(json_str);
    fclose(file);

    if (written != 1) {
        LOG_WARN("Error writing to %s\n", profiles_path);
        return -1;
    }

    return 0;
}

/* Main thread, profiles_lock held. Drops the timer and event subscription first,
 * so no callback can be scheduled against a profile that is about to be freed. */
static void Remove_Profile_Locked(const char* id) {
    if (!TimelapseProfiles || !id) {
        return;
    }

    int index = 0;
    int removeIndex = -1;
    cJSON* profile = TimelapseProfiles->child;
    while (profile) {
        cJSON* item = cJSON_GetObjectItem(profile, "id");
        if (item && item->valuestring && strcmp(item->valuestring, id) == 0) {
            removeIndex = index;
        }
        index++;
        profile = profile->next;
    }

    Cleanup_Subscription(id);
    Cleanup_Timer(id);

    if (removeIndex >= 0) {
        cJSON_DeleteItemFromArray(TimelapseProfiles, removeIndex);
        LOG_TRACE("%s: Profile %s removed\n", __func__, id);
    } else {
        LOG_TRACE("%s: Profile %s not found\n", __func__, id);
    }
}

/*-----------------------------------------------------
 * Profile store - main-thread entry points
 *-----------------------------------------------------*/

/* Takes ownership of profile: it is either inserted into TimelapseProfiles or
 * deleted here. Returns 1 on success. */
static int Activate_Profile_Main(cJSON* profile) {
    if (!profile) {
        LOG_WARN("%s: profile is NULL\n", __func__);
        return 0;
    }

    cJSON* idItem = cJSON_GetObjectItem(profile, "id");
    const char* profileId = idItem ? idItem->valuestring : NULL;
    if (!profileId || !strlen(profileId)) {
        LOG_WARN("%s: Missing id in profile\n", __func__);
        cJSON_Delete(profile);
        return 0;
    }

    cJSON* nameItem = cJSON_GetObjectItem(profile, "name");
    if (!nameItem || !nameItem->valuestring || !strlen(nameItem->valuestring)) {
        LOG_WARN("%s: Profile is missing name\n", __func__);
        cJSON_Delete(profile);
        return 0;
    }

    cJSON* resolutionItem = cJSON_GetObjectItem(profile, "resolution");
    if (!resolutionItem || !resolutionItem->valuestring || !strlen(resolutionItem->valuestring)) {
        LOG_WARN("%s: Profile is missing resolution\n", __func__);
        cJSON_Delete(profile);
        return 0;
    }

    cJSON* conditionsItem = cJSON_GetObjectItem(profile, "conditions");
    if (!conditionsItem || !conditionsItem->valuestring || !strlen(conditionsItem->valuestring)) {
        LOG_WARN("%s: Profile is missing conditions\n", __func__);
        cJSON_Delete(profile);
        return 0;
    }

    ensure_archive_defaults(profile);
    cJSON_DeleteItemFromObject(profile, "subscriptionId");

    /* Copy the id: profileId points into the profile, which is about to be
     * handed to the array, and Remove_Profile_Locked frees the old profile
     * that may own an identical string. */
    char id[PROFILE_ID_MAX];
    g_strlcpy(id, profileId, sizeof(id));

    g_rec_mutex_lock(&profiles_lock);

    if (!TimelapseProfiles) {
        TimelapseProfiles = cJSON_CreateArray();
    }

    Remove_Profile_Locked(id);
    cJSON_AddItemToArray(TimelapseProfiles, profile);

    cJSON* timer = cJSON_GetObjectItem(profile, "timer");
    if (timer && timer->type == cJSON_Number) {
        Setup_Timer(id, timer->valueint);
    }

    cJSON* triggerEvent = cJSON_GetObjectItem(profile, "triggerEvent");
    if (triggerEvent && triggerEvent->type == cJSON_Object) {
        int subscriptionId = Setup_Subscription(id, triggerEvent);
        if (!subscriptionId) {
            LOG_WARN("%s: Unable to subscribe to event for %s\n", __func__, id);
            Remove_Profile_Locked(id);
            g_rec_mutex_unlock(&profiles_lock);
            return 0;
        }
        cJSON_AddNumberToObject(profile, "subscriptionId", subscriptionId);
    }

    g_rec_mutex_unlock(&profiles_lock);
    return 1;
}

static int Activate_Profile_Task(void* arg) {
    return Activate_Profile_Main((cJSON*)arg);
}

/* Takes ownership of profile. Safe to call from any thread. */
static int Timelapse_Activate_Profile(cJSON* profile) {
    return Run_On_Main_Thread(Activate_Profile_Task, profile);
}

static int Remove_Profile_Task(void* arg) {
    g_rec_mutex_lock(&profiles_lock);
    Remove_Profile_Locked((const char*)arg);
    g_rec_mutex_unlock(&profiles_lock);
    return 1;
}

int Timelapse_Remove_Profile_By_Id(const char* id) {
    if (!id) {
        return 0;
    }
    return Run_On_Main_Thread(Remove_Profile_Task, (void*)id);
}

/*-----------------------------------------------------
 * Public accessors - every one hands out a copy
 *-----------------------------------------------------*/

cJSON* Timelapse_Get_Profile(const char* id) {
    if (!id) {
        return NULL;
    }
    g_rec_mutex_lock(&profiles_lock);
    cJSON* profile = Find_Profile_Locked(id);
    cJSON* copy = profile ? cJSON_Duplicate(profile, 1) : NULL;
    g_rec_mutex_unlock(&profiles_lock);
    return copy;
}

cJSON* Timelapse_Get_Profiles(void) {
    g_rec_mutex_lock(&profiles_lock);
    cJSON* copy = TimelapseProfiles ? cJSON_Duplicate(TimelapseProfiles, 1) : NULL;
    g_rec_mutex_unlock(&profiles_lock);
    return copy ? copy : cJSON_CreateArray();
}

int Timelapse_Get_Profile_Int(const char* id, const char* key, int fallback) {
    if (!id || !key) {
        return fallback;
    }
    g_rec_mutex_lock(&profiles_lock);
    cJSON* profile = Find_Profile_Locked(id);
    cJSON* item = profile ? cJSON_GetObjectItem(profile, key) : NULL;
    int value = (item && item->type == cJSON_Number) ? item->valueint : fallback;
    g_rec_mutex_unlock(&profiles_lock);
    return value;
}

/* In-place value update. Deliberately not marshalled to the main thread: media
 * worker threads call this, and the main thread waits for those workers during
 * shutdown. */
int Timelapse_Set_Profile_Number(const char* id, const char* key, double value) {
    if (!id || !key) {
        return 0;
    }

    g_rec_mutex_lock(&profiles_lock);
    cJSON* profile = Find_Profile_Locked(id);
    if (!profile) {
        g_rec_mutex_unlock(&profiles_lock);
        return 0;
    }

    cJSON* item = cJSON_GetObjectItem(profile, key);
    if (item && item->type == cJSON_Number) {
        cJSON_SetNumberValue(item, value);
    } else {
        cJSON_DeleteItemFromObject(profile, key);
        cJSON_AddNumberToObject(profile, key, value);
    }

    int result = Save_Profiles_Locked() == 0;
    g_rec_mutex_unlock(&profiles_lock);
    return result;
}

int Timelapse_Save_Profiles(void) {
    g_rec_mutex_lock(&profiles_lock);
    int result = Save_Profiles_Locked();
    g_rec_mutex_unlock(&profiles_lock);
    return result;
}

/*-----------------------------------------------------
 * Load / reset
 *-----------------------------------------------------*/

/* Main thread. */
static int Timelapse_Load_Profiles(void) {
    char profiles_path[1024];
    if (!storage_profiles_path(profiles_path, sizeof(profiles_path))) {
        LOG_WARN("%s: Failed to build profiles path\n", __func__);
        return -1;
    }

    LOG_TRACE("%s: Loading profiles from %s\n", __func__, profiles_path);

    FILE *file = fopen(profiles_path, "r");
    if (!file) {
        LOG("%s: No stored profiles yet\n", __func__);
        g_rec_mutex_lock(&profiles_lock);
        if (!TimelapseProfiles) {
            TimelapseProfiles = cJSON_CreateArray();
        }
        int result = Save_Profiles_Locked();
        g_rec_mutex_unlock(&profiles_lock);
        return result;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 0) {
        LOG_WARN("%s: Failed to size %s\n", __func__, profiles_path);
        fclose(file);
        return -1;
    }

    char *data = (char *)malloc(length + 1);
    if (!data) {
        LOG_WARN("%s: Memory allocation failed\n", __func__);
        fclose(file);
        return -1;
    }

    size_t read = fread(data, 1, length, file);
    fclose(file);
    data[read] = '\0';
    LOG_TRACE("%s: Read %zu bytes\n", __func__, read);

    cJSON* fileList = cJSON_Parse(data);
    free(data);
    if (!fileList) {
        LOG_WARN("%s: Unable to parse profiles.json\n", __func__);
        return 0;
    }

    cJSON *profile;
    cJSON_ArrayForEach(profile, fileList) {
        cJSON *id_obj = cJSON_GetObjectItem(profile, "id");
        if (id_obj && id_obj->valuestring) {
            Ensure_Directory_Exists(id_obj->valuestring);
        }
        /* Activate takes ownership of the duplicate, including on failure. */
        Timelapse_Activate_Profile(cJSON_Duplicate(profile, 1));
    }
    cJSON_Delete(fileList);
    return 1;
}

/* Main thread, profiles_lock held. */
static void Teardown_All_Locked(void) {
    if (timelapse_subscriptions) {
        g_hash_table_remove_all(timelapse_subscriptions);
    }
    if (timelapse_timers) {
        g_hash_table_remove_all(timelapse_timers);
    }
    if (TimelapseProfiles) {
        cJSON_Delete(TimelapseProfiles);
        TimelapseProfiles = NULL;
    }
}

static int Reset_Task(void* arg) {
    (void)arg;
    g_rec_mutex_lock(&profiles_lock);
    Teardown_All_Locked();
    TimelapseProfiles = cJSON_CreateArray();
    g_rec_mutex_unlock(&profiles_lock);

    /* Outside the teardown so the reloaded profiles get fresh timers and
     * subscriptions - the previous version tore them down after reloading and
     * left every profile without a capture timer until the next restart. */
    Timelapse_Load_Profiles();
    return 1;
}

static int Pause_Task(void* arg) {
    (void)arg;
    g_rec_mutex_lock(&profiles_lock);
    Teardown_All_Locked();
    TimelapseProfiles = cJSON_CreateArray();
    g_rec_mutex_unlock(&profiles_lock);
    return 1;
}

void Timelapse_Pause(void) {
    if (timelapse_initialized) {
        Run_On_Main_Thread(Pause_Task, NULL);
    }
}

void Timelapse_Reset(void) {
    /* /reset is reachable before the services start (that endpoint exists so a
       corrupt SD card can be wiped). Nothing to reset in that state, and the
       timer/subscription tables do not exist yet. */
    if (!timelapse_initialized) {
        LOG("%s: Services not started, nothing to reset\n", __func__);
        return;
    }
    Run_On_Main_Thread(Reset_Task, NULL);
}

/*-----------------------------------------------------
 * HTTP endpoint
 *-----------------------------------------------------*/

/* Shared by POST and PUT: both replace the profile wholesale. */
static void Handle_Profile_Write(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request,
                                 const char* what) {
    const char* contentType = ACAP_HTTP_Get_Content_Type(request);
    if (!contentType || strcmp(contentType, "application/json") != 0) {
        ACAP_HTTP_Respond_Error(response, 415, "Unsupported Media Type - Use application/json");
        return;
    }

    if (!request->postData || request->postDataLength == 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing request data");
        return;
    }

    cJSON* profile = cJSON_Parse(request->postData);
    if (!profile) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON data");
        return;
    }

    cJSON* idItem = cJSON_GetObjectItem(profile, "id");
    if (!idItem || !idItem->valuestring || !strlen(idItem->valuestring)) {
        cJSON_Delete(profile);
        ACAP_HTTP_Respond_Error(response, 400, "Invalid profile. Missing id");
        return;
    }

    cJSON* nameItem = cJSON_GetObjectItem(profile, "name");
    if (!nameItem || !nameItem->valuestring || !strlen(nameItem->valuestring)) {
        cJSON_Delete(profile);
        ACAP_HTTP_Respond_Error(response, 400, "Invalid profile. Missing name");
        return;
    }

    cJSON* resolutionItem = cJSON_GetObjectItem(profile, "resolution");
    if (!resolutionItem || !resolutionItem->valuestring || !strlen(resolutionItem->valuestring)) {
        cJSON_Delete(profile);
        ACAP_HTTP_Respond_Error(response, 400, "Invalid profile. Missing resolution");
        return;
    }

    char id[PROFILE_ID_MAX];
    g_strlcpy(id, idItem->valuestring, sizeof(id));

    if (!cJSON_GetObjectItem(profile, "fps")) {
        cJSON_AddNumberToObject(profile, "fps", 10);
    }
    if (!cJSON_GetObjectItem(profile, "archived")) {
        cJSON_AddNumberToObject(profile, "archived", 10);
    }
    ensure_archive_defaults(profile);

    cJSON* new_fps_item = cJSON_GetObjectItem(profile, "fps");
    int new_fps = (new_fps_item && new_fps_item->type == cJSON_Number) ? new_fps_item->valueint : 10;

    /* Read the previous fps through the locked accessor; the old profile object
     * is freed by the activation below, so nothing may point into it. */
    int had_existing_profile = 0;
    int old_fps = 10;
    cJSON* existing = Timelapse_Get_Profile(id);
    if (existing) {
        had_existing_profile = 1;
        cJSON* old_fps_item = cJSON_GetObjectItem(existing, "fps");
        if (old_fps_item && old_fps_item->type == cJSON_Number) {
            old_fps = old_fps_item->valueint;
        }
        cJSON_Delete(existing);
    }

    /* Ownership of profile passes to Timelapse_Activate_Profile either way. */
    if (!Timelapse_Activate_Profile(profile)) {
        ACAP_HTTP_Respond_Error(response, 500, "Failed to store timelapse profile");
        return;
    }

    Ensure_Directory_Exists(id);
    Timelapse_Save_Profiles();
    if (had_existing_profile && old_fps != new_fps) {
        Queue_Reencode_Export(id, old_fps, new_fps);
    }
    ACAP_HTTP_Respond_Text(response, what);
}

static void HTTP_Endpoint_Timelapse(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {

    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        LOG_WARN("Invalid Request Method\n");
        ACAP_HTTP_Respond_Error(response, 400, "Invalid Request Method");
        return;
    }

    LOG_TRACE("%s: Method=%s\n", __func__, method);

    if (strcmp(method, "POST") == 0) {
        Handle_Profile_Write(response, request, "Timelapse added successfully");
        return;
    }

    if (strcmp(method, "PUT") == 0) {
        Handle_Profile_Write(response, request, "Timelapse updated successfully");
        return;
    }

    if (strcmp(method, "DELETE") == 0) {
        const char* id = ACAP_HTTP_Request_Param(request, "id");
        LOG_TRACE("%s: DELETE %s\n", __func__, id);
        if (!id) {
            LOG_WARN("%s: DELETE missing id parameter\n", __func__);
            ACAP_HTTP_Respond_Error(response, 400, "Missing 'id' parameter");
            return;
        }

        cJSON* removed_profile = Timelapse_Get_Profile(id);
        if (!Timelapse_Remove_Profile_By_Id(id)) {
            LOG_WARN("%s: DELETE failed on %s\n", __func__, id);
            cJSON_Delete(removed_profile);
            ACAP_HTTP_Respond_Error(response, 500, "Failed to delete timelapse profile");
            return;
        }

        Timelapse_Save_Profiles();

        // Deactivate timers/subscriptions before deleting media so no main-context capture
        // can recreate the profile directory after the purge. Restore the profile if media
        // cleanup fails, leaving the user with a retryable and internally consistent state.
        if (!Recordings_Delete_Profile_Media(id)) {
            LOG_WARN("%s: DELETE media purge failed on %s\n", __func__, id);
            if (removed_profile) {
                Timelapse_Activate_Profile(removed_profile);
                removed_profile = NULL;
                Timelapse_Save_Profiles();
            }
            ACAP_HTTP_Respond_Error(response, 500, "Failed to delete recording media");
            return;
        }

        cJSON_Delete(removed_profile);
        ACAP_HTTP_Respond_Text(response, "Timelapse deleted successfully");
        LOG_TRACE("%s: DELETE Success\n", __func__);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        cJSON* profiles = Timelapse_Get_Profiles();
        ACAP_HTTP_Respond_JSON(response, profiles);
        cJSON_Delete(profiles);
        return;
    }

    ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
}

int
Timelapse_Init(Timelapse_Callback callback) {
    if (timelapse_initialized) {
        LOG_WARN("%s: Already initialized\n", __func__);
        return 0;
    }

    /* profiles_lock is statically allocated, which GLib defines as already
       initialised. Recorded here so Run_On_Main_Thread() can tell the GLib main
       thread from the FastCGI and media worker threads. */
    main_thread = g_thread_self();

    Timelapse_ServiceCallBack = callback;
    ACAP_EVENTS_Unsubscribe(0);

    timelapse_timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, Timer_Source_Dispose);
    timelapse_subscriptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, Event_Subscription_Dispose);

    ACAP_HTTP_Node("timelapse", HTTP_Endpoint_Timelapse);
    ACAP_EVENTS_SetCallback(Timelapse_Event_Callback);
    timelapse_initialized = 1;
    return Timelapse_Load_Profiles();
}
