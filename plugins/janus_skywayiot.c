/*! \file   janus_skywayiot.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus skywayiot plugin
 * \details  This is a trivial skywayiot plugin for Janus, just used to
 * showcase the plugin interface. A peer attaching to this plugin will
 * receive back the same RTP packets and RTCP messages he sends: the
 * RTCP messages, of course, would be modified on the way by the gateway
 * to make sure they are coherent with the involved SSRCs. In order to
 * demonstrate how peer-provided messages can change the behaviour of a
 * plugin, this plugin implements a simple API based on three messages:
 *
 * 1. a message to enable/disable audio (that is, to tell the plugin
 * whether incoming audio RTP packets need to be sent back or discarded);
 * 2. a message to enable/disable video (that is, to tell the plugin
 * whether incoming video RTP packets need to be sent back or discarded);
 * 3. a message to cap the bitrate (which would modify incoming RTCP
 * REMB messages before sending them back, in order to trick the peer into
 * thinking the available bandwidth is different).
 *
 * \section echoapi Skyway IoT API
 *
 * There's a single unnamed request you can send and it's asynchronous,
 * which means all responses (successes and errors) will be delivered
 * as events with the same transaction.
 *
 * The request has to be formatted as follows. All the attributes are
 * optional, so any request can contain a subset of them:
 *
\verbatim
{
 "audio" : true|false,
 "video" : true|false,
 "bitrate" : <numeric bitrate value>,
 "record" : true|false,
 "filename" : <base path/filename to use for the recording>
}
\endverbatim
 *
 * \c audio instructs the plugin to do or do not bounce back audio
 * frames; \c video does the same for video; \c bitrate caps the
 * bandwidth to force on the browser encoding side (e.g., 128000 for
 * 128kbps).
 *
 * The first request must be sent together with a JSEP offer to
 * negotiate a PeerConnection: a JSEP answer will be provided with
 * the asynchronous response notification. Subsequent requests (e.g., to
 * dynamically manipulate the bitrate while testing) have to be sent
 * without any JSEP payload attached.
 *
 * A successful request will result in an \c ok event:
 *
\verbatim
{
 "skywayiot" : "event",
 "result": "ok"
}
\endverbatim
 *
 * An error instead will provide both an error code and a more verbose
 * description of the cause of the issue:
 *
\verbatim
{
 "skywayiot" : "event",
 "error_code" : <numeric ID, check Macros below>,
 "error" : "<error description as a string>"
}
\endverbatim
 *
 * If the plugin detects a loss of the associated PeerConnection, a
 * "done" notification is triggered to inform the application the Echo
 * Test session is over:
 *
\verbatim
{
 "skywayiot" : "event",
 "result": "done"
}
\endverbatim
 *
 * \ingroup plugins
 * \ref plugins
 */

#include "plugin.h"

#include <jansson.h>
#include <netdb.h>

#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../record.h"
#include "../rtcp.h"
#include "../utils.h"


/* Plugin information */
#define JANUS_SKYWAYIOT_VERSION   4
#define JANUS_SKYWAYIOT_VERSION_STRING "0.0.4"
#define JANUS_SKYWAYIOT_DESCRIPTION  "This is a SkyWay IoT plugin for Janus gateway."
#define JANUS_SKYWAYIOT_NAME    "JANUS SkyWay IoT plugin"
#define JANUS_SKYWAYIOT_AUTHOR   "Kensaku Komatsu"
#define JANUS_SKYWAYIOT_PACKAGE   "janus.plugin.skywayiot"

/* Plugin methods */
janus_plugin *create(void);
int janus_skywayiot_init(janus_callbacks *callback, const char *config_path);
void janus_skywayiot_destroy(void);
int janus_skywayiot_get_api_compatibility(void);
int janus_skywayiot_get_version(void);
const char *janus_skywayiot_get_version_string(void);
const char *janus_skywayiot_get_description(void);
const char *janus_skywayiot_get_name(void);
const char *janus_skywayiot_get_author(void);
const char *janus_skywayiot_get_package(void);
void janus_skywayiot_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_skywayiot_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void janus_skywayiot_setup_media(janus_plugin_session *handle);
void janus_skywayiot_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_skywayiot_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_skywayiot_incoming_data(janus_plugin_session *handle, char *buf, int len);
void janus_skywayiot_slow_link(janus_plugin_session *handle, int uplink, int video);
void janus_skywayiot_hangup_media(janus_plugin_session *handle);
void janus_skywayiot_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_skywayiot_query_session(janus_plugin_session *handle);

/* Plugin setup */
static janus_plugin janus_skywayiot_plugin =
 JANUS_PLUGIN_INIT (
  .init = janus_skywayiot_init,
  .destroy = janus_skywayiot_destroy,

  .get_api_compatibility = janus_skywayiot_get_api_compatibility,
  .get_version = janus_skywayiot_get_version,
  .get_version_string = janus_skywayiot_get_version_string,
  .get_description = janus_skywayiot_get_description,
  .get_name = janus_skywayiot_get_name,
  .get_author = janus_skywayiot_get_author,
  .get_package = janus_skywayiot_get_package,

  .create_session = janus_skywayiot_create_session,
  .handle_message = janus_skywayiot_handle_message,
  .setup_media = janus_skywayiot_setup_media,
  .incoming_rtp = janus_skywayiot_incoming_rtp,
  .incoming_rtcp = janus_skywayiot_incoming_rtcp,
  .incoming_data = janus_skywayiot_incoming_data,
  .slow_link = janus_skywayiot_slow_link,
  .hangup_media = janus_skywayiot_hangup_media,
  .destroy_session = janus_skywayiot_destroy_session,
  .query_session = janus_skywayiot_query_session,
 );

/* Plugin creator */
janus_plugin *create(void) {
 JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_SKYWAYIOT_NAME);
 return &janus_skywayiot_plugin;
}


/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static GThread *watchdog;
static void *janus_skywayiot_handler(void *data);
static int create_data_receiver(char *data_recv_addr, int data_recv_port);
static int create_data_sender(char *data_send_dest,   int data_send_port);
static int create_media_sender(char *media_recv_addr, int media_recv_port);

static void *thread_receive_external_data(void *data);

static void relay_extern_data(gpointer handle, gpointer session, gpointer data);

typedef struct data_with_handleid {
 guint64 handle_id;
 char *data;
 int data_len;
} data_with_handleid;

typedef struct janus_skywayiot_message {
 janus_plugin_session *handle;
 char *transaction;
 json_t *message;
 json_t *jsep;
} janus_skywayiot_message;
static GAsyncQueue *messages = NULL;
static janus_skywayiot_message exit_message;

typedef struct janus_skywayiot_session {
 janus_plugin_session *handle;
 gboolean has_audio;
 gboolean has_video;
 gboolean has_data;
 gboolean audio_active;
 gboolean video_active;
 uint64_t bitrate;
 janus_recorder *arc; /* The Janus recorder instance for this user's audio, if enabled */
 janus_recorder *vrc; /* The Janus recorder instance for this user's video, if enabled */
 janus_recorder *drc; /* The Janus recorder instance for this user's data, if enabled */
 janus_mutex rec_mutex; /* Mutex to protect the recorders from race conditions */
 guint16 slowlink_count;
 volatile gint hangingup;
 gint64 destroyed; /* Time at which this session was marked as destroyed */
} janus_skywayiot_session;
static GHashTable *sessions;
static GList *old_sessions;
static janus_mutex sessions_mutex;

int data_recv_fd;  /* socket for external data stream */
int data_send_fd;  /* socket for external data stream */
int media_send_fd; /* socket for external media stream */

struct sockaddr_in g_data_sender;
struct sockaddr_in g_media_sender;

static void janus_skywayiot_message_free(janus_skywayiot_message *msg) {
 if(!msg || msg == &exit_message)
  return;

 msg->handle = NULL;

 g_free(msg->transaction);
 msg->transaction = NULL;
 if(msg->message)
  json_decref(msg->message);
 msg->message = NULL;
 if(msg->jsep)
  json_decref(msg->jsep);
 msg->jsep = NULL;

 g_free(msg);
}


/* Error codes */
#define JANUS_SKYWAYIOT_ERROR_NO_MESSAGE   411
#define JANUS_SKYWAYIOT_ERROR_INVALID_JSON  412
#define JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT 413


/* SkywayIoT watchdog/garbage collector (sort of) */
void *janus_skywayiot_watchdog(void *data);
void *janus_skywayiot_watchdog(void *data) {
 JANUS_LOG(LOG_INFO, "SkywayIoT watchdog started\n");
 gint64 now = 0;
 while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
  janus_mutex_lock(&sessions_mutex);
  /* Iterate on all the sessions */
  now = janus_get_monotonic_time();
  if(old_sessions != NULL) {
   GList *sl = old_sessions;
   JANUS_LOG(LOG_HUGE, "Checking %d old SkywayIoT sessions...\n", g_list_length(old_sessions));
   while(sl) {
    janus_skywayiot_session *session = (janus_skywayiot_session *)sl->data;
    if(!session) {
     sl = sl->next;
     continue;
    }
    if(now-session->destroyed >= 5*G_USEC_PER_SEC) {
     /* We're lazy and actually get rid of the stuff only after a few seconds */
     JANUS_LOG(LOG_VERB, "Freeing old SkywayIoT session\n");
     GList *rm = sl->next;
     old_sessions = g_list_delete_link(old_sessions, sl);
     sl = rm;
     session->handle = NULL;
     g_free(session);
     session = NULL;
     continue;
    }
    sl = sl->next;
   }
  }
  janus_mutex_unlock(&sessions_mutex);
  g_usleep(500000);
 }
 JANUS_LOG(LOG_INFO, "SkywayIoT watchdog stopped\n");
 return NULL;
}


/* Plugin implementation */
int janus_skywayiot_init(janus_callbacks *callback, const char *config_path) {
 if(g_atomic_int_get(&stopping)) {
  /* Still stopping from before */
  return -1;
 }
 if(callback == NULL || config_path == NULL) {
  /* Invalid arguments */
  return -1;
 }

 /* Read configuration */
 char filename[255];
 g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_SKYWAYIOT_PACKAGE);
 JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
 janus_config *config = janus_config_parse(filename);

 GList *cl = NULL;
 if(config != NULL) {
  cl = janus_config_get_categories(config);
 }

 while(cl != NULL) {
  janus_config_category *cat = (janus_config_category *)cl->data;
  if(cat->name == NULL || strcasecmp(cat->name, "external-interface") != 0) {
   cl = cl->next;
   continue;
  }

  JANUS_LOG(LOG_INFO, "config:: name of category '%s'\n", cat->name);

  janus_config_item *data_recv_port = janus_config_get_item(cat, "data_recv_port");
  janus_config_item *data_recv_addr = janus_config_get_item(cat, "data_recv_addr");

  janus_config_item *data_send_port = janus_config_get_item(cat, "data_send_port");
  janus_config_item *data_send_dest = janus_config_get_item(cat, "data_send_dest");

  janus_config_item *media_send_port = janus_config_get_item(cat, "media_send_port");
  janus_config_item *media_send_dest = janus_config_get_item(cat, "media_send_dest");

  if(data_recv_port == NULL || data_recv_port->value == NULL
    || data_recv_addr == NULL || data_recv_addr->value == NULL
    || data_send_port == NULL || data_send_port->value == NULL
    || data_send_dest == NULL || data_send_dest->value == NULL
    || media_send_port == NULL || media_send_port->value == NULL
    || media_send_dest == NULL || media_send_dest->value == NULL) {
   JANUS_LOG(LOG_WARN, "  -- Invalid dataport, mediaport, listenaddr, we'll skip opening '%s'. \n", cat->name);
   cl = cl->next;
   continue;
  } else {
  create_data_receiver( (char *)data_recv_addr->value, atoi(data_recv_port->value) );
  create_data_sender( (char *)data_send_dest->value, atoi(data_send_port->value) );
  create_media_sender( (char *)media_send_dest->value, atoi(media_send_port->value) );

  cl = cl->next;
  }
 }
 janus_config_print(config);
 /* This plugin actually has nothing to configure... */
 janus_config_destroy(config);
 config = NULL;

 sessions = g_hash_table_new(NULL, NULL);
 janus_mutex_init(&sessions_mutex);
 messages = g_async_queue_new_full((GDestroyNotify) janus_skywayiot_message_free);
 /* This is the callback we'll need to invoke to contact the gateway */
 gateway = callback;
 g_atomic_int_set(&initialized, 1);

 GError *error = NULL;
 /* Start the sessions watchdog */
 watchdog = g_thread_try_new("skywayiot watchdog", &janus_skywayiot_watchdog, NULL, &error);
 if(error != NULL) {
  g_atomic_int_set(&initialized, 0);
  JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SkywayIoT watchdog thread...\n", error->code, error->message ? error->message : "??");
  return -1;
 }
 /* Launch the thread that will handle incoming messages */
 handler_thread = g_thread_try_new("skywayiot handler", janus_skywayiot_handler, NULL, &error);
 if(error != NULL) {
  g_atomic_int_set(&initialized, 0);
  JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SkywayIoT handler thread...\n", error->code, error->message ? error->message : "??");
  return -1;
 }
 JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_SKYWAYIOT_NAME);
 return 0;
}

void janus_skywayiot_destroy(void) {
 if(!g_atomic_int_get(&initialized))
  return;
 g_atomic_int_set(&stopping, 1);

 g_async_queue_push(messages, &exit_message);
 if(handler_thread != NULL) {
  g_thread_join(handler_thread);
  handler_thread = NULL;
 }
 if(watchdog != NULL) {
  g_thread_join(watchdog);
  watchdog = NULL;
 }

 /* FIXME We should destroy the sessions cleanly */
 janus_mutex_lock(&sessions_mutex);
 g_hash_table_destroy(sessions);
 janus_mutex_unlock(&sessions_mutex);
 g_async_queue_unref(messages);
 messages = NULL;
 sessions = NULL;

 g_atomic_int_set(&initialized, 0);
 g_atomic_int_set(&stopping, 0);
 JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_SKYWAYIOT_NAME);
}

int janus_skywayiot_get_api_compatibility(void) {
 /* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
 return JANUS_PLUGIN_API_VERSION;
}

int janus_skywayiot_get_version(void) {
 return JANUS_SKYWAYIOT_VERSION;
}

const char *janus_skywayiot_get_version_string(void) {
 return JANUS_SKYWAYIOT_VERSION_STRING;
}

const char *janus_skywayiot_get_description(void) {
 return JANUS_SKYWAYIOT_DESCRIPTION;
}

const char *janus_skywayiot_get_name(void) {
 return JANUS_SKYWAYIOT_NAME;
}

const char *janus_skywayiot_get_author(void) {
 return JANUS_SKYWAYIOT_AUTHOR;
}

const char *janus_skywayiot_get_package(void) {
 return JANUS_SKYWAYIOT_PACKAGE;
}

void janus_skywayiot_create_session(janus_plugin_session *handle, int *error) {
 if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
  *error = -1;
  return;
 }
 janus_skywayiot_session *session = (janus_skywayiot_session *)g_malloc0(sizeof(janus_skywayiot_session));
 session->handle = handle;
 session->has_audio = FALSE;
 session->has_video = FALSE;
 session->has_data = FALSE;
 session->audio_active = TRUE;
 session->video_active = TRUE;
 janus_mutex_init(&session->rec_mutex);
 session->bitrate = 0; /* No limit */
 session->destroyed = 0;
 g_atomic_int_set(&session->hangingup, 0);
 handle->plugin_handle = session;
 janus_mutex_lock(&sessions_mutex);
 g_hash_table_insert(sessions, handle, session);
 janus_mutex_unlock(&sessions_mutex);

 return;
}

void janus_skywayiot_destroy_session(janus_plugin_session *handle, int *error) {
 if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
  *error = -1;
  return;
 }
 janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
 if(!session) {
  JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
  *error = -2;
  return;
 }
 JANUS_LOG(LOG_VERB, "Removing SkyWay IoT session...\n");
 janus_mutex_lock(&sessions_mutex);
 if(!session->destroyed) {
  session->destroyed = janus_get_monotonic_time();
  g_hash_table_remove(sessions, handle);
  /* Cleaning up and removing the session is done in a lazy way */
  old_sessions = g_list_append(old_sessions, session);
 }
 janus_mutex_unlock(&sessions_mutex);
 return;
}

json_t *janus_skywayiot_query_session(janus_plugin_session *handle) {
 if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
  return NULL;
 }
 janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
 if(!session) {
  JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
  return NULL;
 }
 /* In the echo test, every session is the same: we just provide some configure info */
 json_t *info = json_object();
 json_object_set_new(info, "audio_active", session->audio_active ? json_true() : json_false());
 json_object_set_new(info, "video_active", session->video_active ? json_true() : json_false());
 json_object_set_new(info, "bitrate", json_integer(session->bitrate));
 if(session->arc || session->vrc || session->drc) {
  json_t *recording = json_object();
  if(session->arc && session->arc->filename)
   json_object_set_new(recording, "audio", json_string(session->arc->filename));
  if(session->vrc && session->vrc->filename)
   json_object_set_new(recording, "video", json_string(session->vrc->filename));
  if(session->drc && session->drc->filename)
   json_object_set_new(recording, "data", json_string(session->drc->filename));
  json_object_set_new(info, "recording", recording);
 }
 json_object_set_new(info, "slowlink_count", json_integer(session->slowlink_count));
 json_object_set_new(info, "destroyed", json_integer(session->destroyed));
 return info;
}

struct janus_plugin_result *janus_skywayiot_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
 if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);

 janus_skywayiot_message *msg = g_malloc0(sizeof(janus_skywayiot_message));
 msg->handle = handle;
 msg->transaction = transaction;
 msg->message = message;
 msg->jsep = jsep;
 g_async_queue_push(messages, msg);

 /* All the requests to this plugin are handled asynchronously: we add a comment
  * (a JSON object with a "hint" string in it, that's what the core expects),
  * but we don't have to: other plugins don't put anything in there */
 return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, "I'm taking my time!", NULL);
}

typedef struct my_struct {
 guint64 handle_id;
 char* mesg;
} my_struct;

static void show(gpointer key, gpointer value, gpointer data) {
 janus_plugin_session *handle = (janus_plugin_session *)key;
  janus_skywayiot_session *session = (janus_skywayiot_session *)value;

 guint64 handle_id = (guint64)handle;
 my_struct *search = (my_struct *)data;

 printf("[%ld, %ld] %s\n", handle_id, search->handle_id, search->mesg );

 if(search->handle_id == handle_id) {
  gboolean has_audio = session->has_audio;
  gboolean has_video = session->has_video;
  gboolean has_data = session->has_data;

   printf("[%ld]matched!!!  has_video => %d, has_audio => %d, has_data => %d\n", handle_id, has_video, has_audio, has_data);
 }


}

void janus_skywayiot_setup_media(janus_plugin_session *handle) {
 JANUS_LOG(LOG_INFO, "WebRTC media is now available\n");
 if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return;
 janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
 if(!session) {
  JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
  return;
 }
 if(session->destroyed)
  return;

 gboolean has_audio = session->has_audio;
 gboolean has_video = session->has_video;
 gboolean has_data = session->has_data;

 char *mesg = g_strdup("abc");
 my_struct search = {
  handle_id: (guint64)handle,
 mesg: mesg
 };

 g_hash_table_foreach(sessions, show, &search);

 JANUS_LOG(LOG_INFO, "[%ld, %ld] WebRTC media : has_audio[%d], has_video[%d], has_data[%d]\n", (guint64)handle, (guint64)session, has_audio, has_video, has_data);
 g_atomic_int_set(&session->hangingup, 0);
 /* We really don't care, as we only send RTP/RTCP we get in the first place back anyway */
}

void janus_skywayiot_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
 if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return;


 /* Simple echo test */
 if(gateway) {
  /* Honour the audio/video active flags */
  janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
  if(!session) {
   JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
   return;
  }
  if(session->destroyed)
   return;
  if((!video && session->audio_active) || (video && session->video_active)) {
  socklen_t addrlen = sizeof(g_media_sender);

  if((void *)&g_media_sender != NULL) {
   sendto(media_send_fd, buf, len, 0, (struct sockaddr *)&g_media_sender, addrlen);
  }
  }
 }
}

void janus_skywayiot_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
 if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return;
 /* Simple echo test */
 if(gateway) {
  janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
  if(!session) {
   JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
   return;
  }
  if(session->destroyed)
   return;
  if(session->bitrate > 0)
   janus_rtcp_cap_remb(buf, len, session->bitrate);
  gateway->relay_rtcp(handle, video, buf, len);
 }
}

void janus_skywayiot_incoming_data(janus_plugin_session *handle, char *buf, int len) {
 if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return;
 /* Simple echo test */
 if(gateway) {
  janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
  if(!session) {
   JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
   return;
  }
  if(session->destroyed)
   return;
  if(buf == NULL || len <= 0)
   return;

  char* ext_data;
 int id_len = sizeof(guint64);
 guint64 handle_id = (guint64)handle;
 socklen_t addrlen = sizeof(g_data_sender);

  ext_data = (char *)malloc( id_len + len );
 memcpy(ext_data, &handle_id, id_len);
 memcpy(ext_data + id_len, buf, len);

 if((void *)&g_data_sender != NULL) {
  sendto(data_send_fd, ext_data, (id_len + len), 0, (struct sockaddr *)&g_data_sender, addrlen);
 }
 g_free(ext_data);
 }
}

void janus_skywayiot_slow_link(janus_plugin_session *handle, int uplink, int video) {
 /* The core is informing us that our peer got or sent too many NACKs, are we pushing media too hard? */
 if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return;
 janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
 if(!session) {
  JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
  return;
 }
 if(session->destroyed)
  return;
 session->slowlink_count++;
 if(uplink && !video && !session->audio_active) {
  /* We're not relaying audio and the peer is expecting it, so NACKs are normal */
  JANUS_LOG(LOG_VERB, "Getting a lot of NACKs (slow uplink) for audio, but that's expected, a configure disabled the audio forwarding\n");
 } else if(uplink && video && !session->video_active) {
  /* We're not relaying video and the peer is expecting it, so NACKs are normal */
  JANUS_LOG(LOG_VERB, "Getting a lot of NACKs (slow uplink) for video, but that's expected, a configure disabled the video forwarding\n");
 } else {
  /* Slow uplink or downlink, maybe we set the bitrate cap too high? */
  if(video) {
   /* Halve the bitrate, but don't go too low... */
   session->bitrate = session->bitrate > 0 ? session->bitrate : 512*1024;
   session->bitrate = session->bitrate/2;
   if(session->bitrate < 64*1024)
    session->bitrate = 64*1024;
   JANUS_LOG(LOG_WARN, "Getting a lot of NACKs (slow %s) for %s, forcing a lower REMB: %"SCNu64"\n",
    uplink ? "uplink" : "downlink", video ? "video" : "audio", session->bitrate);
   /* ... and send a new REMB back */
   char rtcpbuf[24];
   janus_rtcp_remb((char *)(&rtcpbuf), 24, session->bitrate);
   gateway->relay_rtcp(handle, 1, rtcpbuf, 24);
   /* As a last thing, notify the user about this */
   json_t *event = json_object();
   json_object_set_new(event, "skywayiot", json_string("event"));
   json_t *result = json_object();
   json_object_set_new(result, "status", json_string("slow_link"));
   json_object_set_new(result, "bitrate", json_integer(session->bitrate));
   json_object_set_new(event, "result", result);
   gateway->push_event(session->handle, &janus_skywayiot_plugin, NULL, event, NULL);
   /* We don't need the event anymore */
   json_decref(event);
  }
 }
}

void janus_skywayiot_hangup_media(janus_plugin_session *handle) {
 JANUS_LOG(LOG_INFO, "No WebRTC media anymore\n");
 if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
  return;
 janus_skywayiot_session *session = (janus_skywayiot_session *)handle->plugin_handle;
 if(!session) {
  JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
  return;
 }
 if(session->destroyed)
  return;
 if(g_atomic_int_add(&session->hangingup, 1))
  return;
 /* Send an event to the browser and tell it's over */
 json_t *event = json_object();
 json_object_set_new(event, "skywayiot", json_string("event"));
 json_object_set_new(event, "result", json_string("done"));
 int ret = gateway->push_event(handle, &janus_skywayiot_plugin, NULL, event, NULL);
 JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
 json_decref(event);
 /* Get rid of the recorders, if available */
 janus_mutex_lock(&session->rec_mutex);
 if(session->arc) {
  janus_recorder_close(session->arc);
  JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
  janus_recorder_free(session->arc);
 }
 session->arc = NULL;
 if(session->vrc) {
  janus_recorder_close(session->vrc);
  JANUS_LOG(LOG_INFO, "Closed video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
  janus_recorder_free(session->vrc);
 }
 session->vrc = NULL;
 if(session->drc) {
  janus_recorder_close(session->drc);
  JANUS_LOG(LOG_INFO, "Closed data recording %s\n", session->drc->filename ? session->drc->filename : "??");
  janus_recorder_free(session->drc);
 }
 session->drc = NULL;
 janus_mutex_unlock(&session->rec_mutex);
 /* Reset controls */
 session->has_audio = FALSE;
 session->has_video = FALSE;
 session->has_data = FALSE;
 session->audio_active = TRUE;
 session->video_active = TRUE;
 session->bitrate = 0;
}

/* Thread to handle incoming messages */
static void *janus_skywayiot_handler(void *data) {
 JANUS_LOG(LOG_VERB, "Joining SkywayIoT handler thread\n");
 janus_skywayiot_message *msg = NULL;
 int error_code = 0;
 char *error_cause = g_malloc0(512);
 json_t *root = NULL;
 while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
  msg = g_async_queue_pop(messages);
  if(msg == NULL)
   continue;
  if(msg == &exit_message)
   break;
  if(msg->handle == NULL) {
   janus_skywayiot_message_free(msg);
   continue;
  }
  janus_skywayiot_session *session = NULL;
  janus_mutex_lock(&sessions_mutex);
  if(g_hash_table_lookup(sessions, msg->handle) != NULL ) {
   session = (janus_skywayiot_session *)msg->handle->plugin_handle;
  }
  janus_mutex_unlock(&sessions_mutex);
  if(!session) {
   JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
   janus_skywayiot_message_free(msg);
   continue;
  }
  if(session->destroyed) {
   janus_skywayiot_message_free(msg);
   continue;
  }
  /* Handle request */
  error_code = 0;
  root = msg->message;
  if(msg->message == NULL) {
   JANUS_LOG(LOG_ERR, "No message??\n");
   error_code = JANUS_SKYWAYIOT_ERROR_NO_MESSAGE;
   g_snprintf(error_cause, 512, "%s", "No message??");
   goto error;
  }
  if(!json_is_object(root)) {
   JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_JSON;
   g_snprintf(error_cause, 512, "JSON error: not an object");
   goto error;
  }
  /* Parse request */
  const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
  const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
  json_t *audio = json_object_get(root, "audio");
  if(audio && !json_is_boolean(audio)) {
   JANUS_LOG(LOG_ERR, "Invalid element (audio should be a boolean)\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT;
   g_snprintf(error_cause, 512, "Invalid value (audio should be a boolean)");
   goto error;
  }
  json_t *video = json_object_get(root, "video");
  if(video && !json_is_boolean(video)) {
   JANUS_LOG(LOG_ERR, "Invalid element (video should be a boolean)\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT;
   g_snprintf(error_cause, 512, "Invalid value (video should be a boolean)");
   goto error;
  }
  json_t *bitrate = json_object_get(root, "bitrate");
  if(bitrate && (!json_is_integer(bitrate) || json_integer_value(bitrate) < 0)) {
   JANUS_LOG(LOG_ERR, "Invalid element (bitrate should be a positive integer)\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT;
   g_snprintf(error_cause, 512, "Invalid value (bitrate should be a positive integer)");
   goto error;
  }
  json_t *record = json_object_get(root, "record");
  if(record && !json_is_boolean(record)) {
   JANUS_LOG(LOG_ERR, "Invalid element (record should be a boolean)\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT;
   g_snprintf(error_cause, 512, "Invalid value (record should be a boolean)");
   goto error;
  }
  json_t *recfile = json_object_get(root, "filename");
  if(recfile && !json_is_string(recfile)) {
   JANUS_LOG(LOG_ERR, "Invalid element (filename should be a string)\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT;
   g_snprintf(error_cause, 512, "Invalid value (filename should be a string)");
   goto error;
  }
  /* Enforce request */
  if(audio) {
   session->audio_active = json_is_true(audio);
   JANUS_LOG(LOG_VERB, "Setting audio property: %s\n", session->audio_active ? "true" : "false");
  }
  if(video) {
   if(!session->video_active && json_is_true(video)) {
    /* Send a PLI */
    JANUS_LOG(LOG_VERB, "Just (re-)enabled video, sending a PLI to recover it\n");
    char buf[12];
    memset(buf, 0, 12);
    janus_rtcp_pli((char *)&buf, 12);
    gateway->relay_rtcp(session->handle, 1, buf, 12);
   }
   session->video_active = json_is_true(video);
   JANUS_LOG(LOG_VERB, "Setting video property: %s\n", session->video_active ? "true" : "false");
  }
  if(bitrate) {
   session->bitrate = json_integer_value(bitrate);
   JANUS_LOG(LOG_VERB, "Setting video bitrate: %"SCNu64"\n", session->bitrate);
   if(session->bitrate > 0) {
    /* FIXME Generate a new REMB (especially useful for Firefox, which doesn't send any we can cap later) */
    char buf[24];
    memset(buf, 0, 24);
    janus_rtcp_remb((char *)&buf, 24, session->bitrate);
    JANUS_LOG(LOG_VERB, "Sending REMB\n");
    gateway->relay_rtcp(session->handle, 1, buf, 24);
    /* FIXME How should we handle a subsequent "no limit" bitrate? */
   }
  }
  if(record) {
   if(msg_sdp) {
    session->has_audio = (strstr(msg_sdp, "m=audio") != NULL);
    session->has_video = (strstr(msg_sdp, "m=video") != NULL);
    session->has_data = (strstr(msg_sdp, "DTLS/SCTP") != NULL);
   }
   gboolean recording = json_is_true(record);
   const char *recording_base = json_string_value(recfile);
   JANUS_LOG(LOG_VERB, "Recording %s (base filename: %s)\n", recording ? "enabled" : "disabled", recording_base ? recording_base : "not provided");
   janus_mutex_lock(&session->rec_mutex);
   if(!recording) {
    /* Not recording (anymore?) */
    if(session->arc) {
     janus_recorder_close(session->arc);
     JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
     janus_recorder_free(session->arc);
    }
    session->arc = NULL;
    if(session->vrc) {
     janus_recorder_close(session->vrc);
     JANUS_LOG(LOG_INFO, "Closed video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
     janus_recorder_free(session->vrc);
    }
    session->vrc = NULL;
    if(session->drc) {
     janus_recorder_close(session->drc);
     JANUS_LOG(LOG_INFO, "Closed data recording %s\n", session->drc->filename ? session->drc->filename : "??");
     janus_recorder_free(session->drc);
    }
    session->drc = NULL;
   } else {
    /* We've started recording, send a PLI and go on */
    char filename[255];
    gint64 now = janus_get_real_time();
    if(session->has_audio) {
     /* FIXME We assume we're recording Opus, here */
     memset(filename, 0, 255);
     if(recording_base) {
      /* Use the filename and path we have been provided */
      g_snprintf(filename, 255, "%s-audio", recording_base);
      session->arc = janus_recorder_create(NULL, "opus", filename);
      if(session->arc == NULL) {
       /* FIXME We should notify the fact the recorder could not be created */
       JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this SkywayIoT user!\n");
      }
     } else {
      /* Build a filename */
      g_snprintf(filename, 255, "skywayiot-%p-%"SCNi64"-audio", session, now);
      session->arc = janus_recorder_create(NULL, "opus", filename);
      if(session->arc == NULL) {
       /* FIXME We should notify the fact the recorder could not be created */
       JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this SkywayIoT user!\n");
      }
     }
    }
    if(session->has_video) {
     /* FIXME We assume we're recording VP8, here */
     memset(filename, 0, 255);
     if(recording_base) {
      /* Use the filename and path we have been provided */
      g_snprintf(filename, 255, "%s-video", recording_base);
      session->vrc = janus_recorder_create(NULL, "vp8", filename);
      if(session->vrc == NULL) {
       /* FIXME We should notify the fact the recorder could not be created */
       JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this SkywayIoT user!\n");
      }
     } else {
      /* Build a filename */
      g_snprintf(filename, 255, "skywayiot-%p-%"SCNi64"-video", session, now);
      session->vrc = janus_recorder_create(NULL, "vp8", filename);
      if(session->vrc == NULL) {
       /* FIXME We should notify the fact the recorder could not be created */
       JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this SkywayIoT user!\n");
      }
     }
     /* Send a PLI */
     JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
     char buf[12];
     memset(buf, 0, 12);
     janus_rtcp_pli((char *)&buf, 12);
     gateway->relay_rtcp(session->handle, 1, buf, 12);
    }
    if(session->has_data) {
     memset(filename, 0, 255);
     if(recording_base) {
      /* Use the filename and path we have been provided */
      g_snprintf(filename, 255, "%s-data", recording_base);
      session->drc = janus_recorder_create(NULL, "text", filename);
      if(session->drc == NULL) {
       /* FIXME We should notify the fact the recorder could not be created */
       JANUS_LOG(LOG_ERR, "Couldn't open a text data recording file for this SkywayIoT user!\n");
      }
     } else {
      /* Build a filename */
      g_snprintf(filename, 255, "skywayiot-%p-%"SCNi64"-data", session, now);
      session->drc = janus_recorder_create(NULL, "text", filename);
      if(session->drc == NULL) {
       /* FIXME We should notify the fact the recorder could not be created */
       JANUS_LOG(LOG_ERR, "Couldn't open a text data recording file for this SkywayIoT user!\n");
      }
     }
    }
   }
   janus_mutex_unlock(&session->rec_mutex);
  }
  /* Any SDP to handle? */
  if(msg_sdp) {
   JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
   session->has_audio = (strstr(msg_sdp, "m=audio") != NULL);
   session->has_video = (strstr(msg_sdp, "m=video") != NULL);
   session->has_data = (strstr(msg_sdp, "DTLS/SCTP") != NULL);
  }

  if(!audio && !video && !bitrate && !record && !msg_sdp) {
   JANUS_LOG(LOG_ERR, "No supported attributes (audio, video, bitrate, record, jsep) found\n");
   error_code = JANUS_SKYWAYIOT_ERROR_INVALID_ELEMENT;
   g_snprintf(error_cause, 512, "Message error: no supported attributes (audio, video, bitrate, record, jsep) found");
   goto error;
  }

  /* Prepare JSON event */
  json_t *event = json_object();
  json_object_set_new(event, "skywayiot", json_string("event"));
  json_object_set_new(event, "result", json_string("ok"));
  if(!msg_sdp) {
   int ret = gateway->push_event(msg->handle, &janus_skywayiot_plugin, msg->transaction, event, NULL);
   JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
   json_decref(event);
  } else {
   /* Forward the same offer to the gateway, to start the echo test */
   const char *type = NULL;
   if(!strcasecmp(msg_sdp_type, "offer"))
    type = "answer";
   if(!strcasecmp(msg_sdp_type, "answer"))
    type = "offer";
   /* Any media direction that needs to be fixed? */
   char *sdp = g_strdup(msg_sdp);
   if(strstr(sdp, "a=recvonly")) {
    /* Turn recvonly to inactive, as we simply bounce media back */
    sdp = janus_string_replace(sdp, "a=recvonly", "a=inactive");
   } else if(strstr(sdp, "a=sendonly")) {
    /* Turn sendonly to recvonly */
    sdp = janus_string_replace(sdp, "a=sendonly", "a=recvonly");
    /* FIXME We should also actually not echo this media back, though... */
   }
   /* Make also sure we get rid of ULPfec, red, etc. */
   if(strstr(sdp, "ulpfec")) {
    /* FIXME This really needs some better code */
    sdp = janus_string_replace(sdp, "a=rtpmap:116 red/90000\r\n", "");
    sdp = janus_string_replace(sdp, "a=rtpmap:117 ulpfec/90000\r\n", "");
    sdp = janus_string_replace(sdp, "a=rtpmap:96 rtx/90000\r\n", "");
    sdp = janus_string_replace(sdp, "a=fmtp:96 apt=100\r\n", "");
    sdp = janus_string_replace(sdp, "a=rtpmap:97 rtx/90000\r\n", "");
    sdp = janus_string_replace(sdp, "a=fmtp:97 apt=101\r\n", "");
    sdp = janus_string_replace(sdp, "a=rtpmap:98 rtx/90000\r\n", "");
    sdp = janus_string_replace(sdp, "a=fmtp:98 apt=116\r\n", "");
    sdp = janus_string_replace(sdp, " 116", "");
    sdp = janus_string_replace(sdp, " 117", "");
    sdp = janus_string_replace(sdp, " 96", "");
    sdp = janus_string_replace(sdp, " 97", "");
    sdp = janus_string_replace(sdp, " 98", "");
   }
   json_t *jsep = json_pack("{ssss}", "type", type, "sdp", sdp);
   /* How long will the gateway take to push the event? */
   g_atomic_int_set(&session->hangingup, 0);
   gint64 start = janus_get_monotonic_time();
   int res = gateway->push_event(msg->handle, &janus_skywayiot_plugin, msg->transaction, event, jsep);
   JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n",
    res, janus_get_monotonic_time()-start);
   g_free(sdp);
   /* We don't need the event and jsep anymore */
   json_decref(event);
   json_decref(jsep);
  }
  janus_skywayiot_message_free(msg);
  continue;

error:
  {
   /* Prepare JSON error event */
   json_t *event = json_object();
   json_object_set_new(event, "skywayiot", json_string("event"));
   json_object_set_new(event, "error_code", json_integer(error_code));
   json_object_set_new(event, "error", json_string(error_cause));
   int ret = gateway->push_event(msg->handle, &janus_skywayiot_plugin, msg->transaction, event, NULL);
   JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
   janus_skywayiot_message_free(msg);
   /* We don't need the event anymore */
   json_decref(event);
  }
 }
 g_free(error_cause);
 JANUS_LOG(LOG_VERB, "Leaving SkywayIoT handler thread\n");
 return NULL;
}

static int create_data_receiver(char *addr, int port) {
 JANUS_LOG(LOG_INFO, "create data receiver: listener address %s, port %d\n", addr, port);

 /* create a UDP socket for data receiver (it will be transfered via WebRTC DataChannel  */
 if ((data_recv_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
  JANUS_LOG(LOG_WARN, "cannot create socket for data receiver\n");
  return -1;
 }

 struct sockaddr_in data_sockaddr;      /* sockaddr for data channel */
 /* bind the socket to any valid IP address and a specific port */
 memset((char *)&data_sockaddr, 0, sizeof(data_sockaddr));
 data_sockaddr.sin_family = AF_INET;
 data_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
 data_sockaddr.sin_port = htons(port);

 if (bind(data_recv_fd, (struct sockaddr *)&data_sockaddr, sizeof(data_sockaddr)) < 0) {
  JANUS_LOG(LOG_WARN, "bind failed for data receiver\n");
  return -1;
 }

 JANUS_LOG(LOG_INFO, "succeed to create socket for data receiver\n");

 /* create thread to receive udp datagram for each channel */
 GError *error = NULL;
 g_thread_try_new("skywayiot_data_thread", &thread_receive_external_data, NULL, &error);
 if(error != NULL) {
  JANUS_LOG(LOG_WARN, "Got error %d (%s) while launching the data channel ext interface thread...\n", error->code, error->message ? error->message : "??");
  return -1;
 }
 return 0;
}

static int create_data_sender(char *addr, int port) {
 JANUS_LOG(LOG_INFO, "create data sender: destination address %s, port %d\n", addr, port);

 /* create a UDP socket for data sender (it was received via WebRTC DataChannel  */
 if ((data_send_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
  JANUS_LOG(LOG_WARN, "cannot create socket for data sender\n");
  return -1;
 }

 /* bind the socket to any valid IP address and a specific port */
 struct hostent *server;

 server = gethostbyname(addr);

 memset((char *)&g_data_sender, 0, sizeof(g_data_sender));
 g_data_sender.sin_family = AF_INET;
 bcopy((char *)server->h_addr, (char *)&g_data_sender.sin_addr.s_addr, server->h_length);
 g_data_sender.sin_port = htons(port);

 JANUS_LOG(LOG_INFO, "succeed to create socket for data sender\n");
 return 0;
}

static int create_media_sender(char *addr, int port) {
 JANUS_LOG(LOG_INFO, "create media sender: destination address %s, port %d\n", addr, port);

 /* create a UDP socket for data sender (it was received via WebRTC DataChannel  */
 if ((media_send_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
  JANUS_LOG(LOG_WARN, "cannot create socket for media sender\n");
  return -1;
 }

 /* bind the socket to any valid IP address and a specific port */
 struct hostent *server;

 server = gethostbyname(addr);

 memset((char *)&g_media_sender, 0, sizeof(g_media_sender));
 g_media_sender.sin_family = AF_INET;
 bcopy((char *)server->h_addr, (char *)&g_media_sender.sin_addr.s_addr, server->h_length);
 g_media_sender.sin_port = htons(port);

 JANUS_LOG(LOG_INFO, "succeed to create socket for media sender\n");
 return 0;
}





static void *thread_receive_external_data(void *data /* to avoid warning */) {
 char buff[1500];
 int bytes_recv;


 guint64 handle_id;
 int handle_id_len = sizeof(handle_id);
 int data_len = 0;
 struct sockaddr_in addr;
 socklen_t addr_len = sizeof(addr);

 data_with_handleid parsed = {
  handle_id: 0,
  data:      (char *) NULL,
  data_len:  0
 };

 while(1 /* fixme: detect plugin termination */ ) {
  bytes_recv = recvfrom(data_recv_fd, buff,  1500, 0, (struct sockaddr *)&addr, &addr_len);

  if(bytes_recv > handle_id_len) {
   memcpy(&handle_id, buff, (size_t)handle_id_len);

   data_len = bytes_recv - handle_id_len;

   parsed.handle_id = handle_id;
   parsed.data = buff + handle_id_len;
   parsed.data_len = data_len;

   g_hash_table_foreach(sessions, &relay_extern_data, &parsed);
  }
 }

 return NULL;
}

static void relay_extern_data(gpointer handle, gpointer session, gpointer data) {
 data_with_handleid *_data = (data_with_handleid *)data;

 guint64 handle_id = (guint64)handle;

 if(_data->handle_id == 0xffffffffffffffff || handle_id == _data->handle_id) {
  gateway->relay_data(handle, (void *)_data->data, _data->data_len);
 }
}

