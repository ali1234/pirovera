#include <string.h>
#include <stdint.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>
#include <pthread.h>

#include "net.h"
#include "control.h"

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

/*
 * These macros provide a way to store the native pointer to CustomData, which might be 32 or 64 bits, into
 * a jlong, which is always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

/* Do not allow seeks to be performed closer than this distance. It is visually useless, and will probably
 * confuse some demuxers. */
#define SEEK_MIN_DELAY (500 * GST_MSECOND)

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
  jobject app;                  /* Application instance, used to call its methods. A global reference is kept. */
  GstElement *pipeline;         /* The running pipeline */
  GMainContext *context;        /* GLib context used to run the main loop */
  GMainLoop *main_loop;         /* GLib main loop */
  gboolean initialized;         /* To avoid informing the UI multiple times about the initialization */
  ANativeWindow *native_window; /* The Android native window where video will be rendered */
  GstState state;               /* Current pipeline state */
  GstState target_state;        /* Desired pipeline state, to be set once buffering is complete */
  gboolean is_live;             /* Live streams do not use buffering */
} CustomData;

/* playbin2 flags */
typedef enum {
  GST_PLAY_FLAG_TEXT = (1 << 2)  /* We want subtitle output */
} GstPlayFlags;

/* These global variables cache values which are not changing during execution */
static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID custom_data_field_id;
static jmethodID on_gstreamer_initialized_method_id;
static jmethodID on_media_size_changed_method_id;

/*
 * Private methods
 */

/* Register this thread with the VM */
static JNIEnv *attach_current_thread (void) {
  JNIEnv *env;
  JavaVMAttachArgs args;

  GST_DEBUG ("Attaching thread %p", g_thread_self ());
  args.version = JNI_VERSION_1_4;
  args.name = NULL;
  args.group = NULL;

  if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
    GST_ERROR ("Failed to attach current thread");
    return NULL;
  }

  return env;
}

/* Unregister this thread from the VM */
static void detach_current_thread (void *env) {
  GST_DEBUG ("Detaching thread %p", g_thread_self ());
  (*java_vm)->DetachCurrentThread (java_vm);
}

/* Retrieve the JNI environment for this thread */
static JNIEnv *get_jni_env (void) {
  JNIEnv *env;

  if ((env = pthread_getspecific (current_jni_env)) == NULL) {
    env = attach_current_thread ();
    pthread_setspecific (current_jni_env, env);
  }

  return env;
}

/* Forward declaration for the delayed seek callback */
static gboolean delayed_seek_cb (CustomData *data);

/* Retrieve errors from the bus and show them on the UI */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  gst_element_set_state (data->pipeline, GST_STATE_NULL);
}

/* Called when the End Of the Stream is reached. Just move to the beginning of the media and pause. */
static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  data->target_state = GST_STATE_PAUSED;
  data->is_live |= (gst_element_set_state (data->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_NO_PREROLL);
}

/* Called when buffering messages are received. We inform the UI about the current buffering level and
 * keep the pipeline paused until 100% buffering is reached. At that point, set the desired state. */
static void buffering_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  gint percent;

  if (data->is_live)
    return;

  gst_message_parse_buffering (msg, &percent);
  if (percent < 100 && data->target_state >= GST_STATE_PAUSED) {
    gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
  } else if (data->target_state >= GST_STATE_PLAYING) {
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
  }
}

/* Called when the clock is lost */
static void clock_lost_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  if (data->target_state >= GST_STATE_PLAYING) {
    gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
  }
}

/* Retrieve the video sink's Caps and tell the application about the media size */
static void check_media_size (CustomData *data) {
  JNIEnv *env = get_jni_env ();
  GstElement *video_sink;
  GstPad *video_sink_pad;
  GstCaps *caps;
  GstVideoInfo info;

  /* Retrieve the Caps at the entrance of the video sink */
  g_object_get (data->pipeline, "video-sink", &video_sink, NULL);
  video_sink_pad = gst_element_get_static_pad (video_sink, "sink");
  caps = gst_pad_get_current_caps (video_sink_pad);

  if (gst_video_info_from_caps(&info, caps)) {
    info.width = info.width * info.par_n / info.par_d;
    GST_DEBUG ("Media size is %dx%d, notifying application", info.width, info.height);

    (*env)->CallVoidMethod (env, data->app, on_media_size_changed_method_id, (jint)info.width, (jint)info.height);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
  }

  gst_caps_unref(caps);
  gst_object_unref (video_sink_pad);
  gst_object_unref(video_sink);
}

/* Notify UI about pipeline state changes */
static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  /* Only pay attention to messages coming from the pipeline, not its children */
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
    data->state = new_state;

    if (new_state == GST_STATE_NULL || new_state == GST_STATE_READY)
      data->is_live = FALSE;

    /* The Ready to Paused state change is particularly interesting: */
    if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
      /* By now the sink already knows the media size */
      check_media_size(data);
    }
  }
}

/* Check if all conditions are met to report GStreamer as initialized.
 * These conditions will change depending on the application */
static void check_initialization_complete (CustomData *data) {
  JNIEnv *env = get_jni_env ();
  if (!data->initialized && data->native_window && data->main_loop) {
    GST_DEBUG ("Initialization complete, notifying application. native_window:%p main_loop:%p", data->native_window, data->main_loop);

    /* The main loop is running and we received a native window, inform the sink about it */
    gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->pipeline), (guintptr)data->native_window);

    (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
    data->initialized = TRUE;
  }
}

static void source_setup (GstElement *pipeline, GstElement *source, CustomData *data) {
  g_print ("Source has been created. Configuring.\n");
  g_object_set (source, "latency", 50, NULL);
}

/* Main method for the native code. This is executed on its own thread. */
static void *app_function (void *userdata) {
  JavaVMAttachArgs args;
  GstBus *bus;
  CustomData *data = (CustomData *)userdata;
  GSource *timeout_source;
  GSource *bus_source;
  GError *error = NULL;
  guint flags;

  GST_DEBUG ("Creating pipeline in CustomData at %p", data);

  /* Create our own GLib Main Context and make it the default one */
  data->context = g_main_context_new ();
  g_main_context_push_thread_default(data->context);

  net_start(data->context);

  /* Build pipeline */
  data->pipeline = gst_parse_launch("playbin", &error);
  if (error) {
    g_clear_error (&error);
    return NULL;
  }

  /* Disable subtitles */
  g_object_get (data->pipeline, "flags", &flags, NULL);
  flags &= ~GST_PLAY_FLAG_TEXT;
  g_object_set (data->pipeline, "flags", flags, NULL);

  /* Set the pipeline to READY, so it can already accept a window handle, if we have one */
  data->target_state = GST_STATE_READY;
  gst_element_set_state(data->pipeline, GST_STATE_READY);

  /* Source setup callback so we can adjust latency. */
  g_signal_connect (data->pipeline, "source-setup", G_CALLBACK (source_setup), data);

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (data->pipeline);
  bus_source = gst_bus_create_watch (bus);
  g_source_set_callback (bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
  g_source_attach (bus_source, data->context);
  g_source_unref (bus_source);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::state-changed", (GCallback)state_changed_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::buffering", (GCallback)buffering_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::clock-lost", (GCallback)clock_lost_cb, data);
  gst_object_unref (bus);

  /* Create a GLib Main Loop and set it to run */
  GST_DEBUG ("Entering main loop... (CustomData:%p)", data);
  data->main_loop = g_main_loop_new (data->context, FALSE);
  check_initialization_complete (data);
  g_main_loop_run (data->main_loop);
  GST_DEBUG ("Exited main loop");
  g_main_loop_unref (data->main_loop);
  data->main_loop = NULL;

  /* Free resources */
  net_stop();
  g_main_context_pop_thread_default(data->context);
  g_main_context_unref (data->context);
  data->target_state = GST_STATE_NULL;
  gst_element_set_state (data->pipeline, GST_STATE_NULL);
  gst_object_unref (data->pipeline);

  return NULL;
}

/*
 * Java Bindings
 */

/* Instruct the native code to create its internal data structure, pipeline and thread */
static void gst_native_init (JNIEnv* env, jobject thiz) {
  CustomData *data = g_new0 (CustomData, 1);
  SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
  GST_DEBUG_CATEGORY_INIT (debug_category, "pirovera", 0, "Pi Rover");
  gst_debug_set_threshold_for_name("pirovera", GST_LEVEL_DEBUG);
  GST_DEBUG ("Created CustomData at %p", data);
  data->app = (*env)->NewGlobalRef (env, thiz);
  GST_DEBUG ("Created GlobalRef for app object at %p", data->app);
  pthread_create (&gst_app_thread, NULL, &app_function, data);
}

/* Quit the main loop, remove the native thread and free resources */
static void gst_native_finalize (JNIEnv* env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data) return;
  GST_DEBUG ("Quitting main loop...");
  g_main_loop_quit (data->main_loop);
  GST_DEBUG ("Waiting for thread to finish...");
  pthread_join (gst_app_thread, NULL);
  GST_DEBUG ("Deleting GlobalRef for app object at %p", data->app);
  (*env)->DeleteGlobalRef (env, data->app);
  GST_DEBUG ("Freeing CustomData at %p", data);
  g_free (data);
  SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
  GST_DEBUG ("Done finalizing");
}

/* Set playbin2's URI */
void gst_native_set_uri (JNIEnv* env, jobject thiz, jstring uri) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data || !data->pipeline) return;
  const jbyte *char_uri = (*env)->GetStringUTFChars (env, uri, NULL);
  GST_DEBUG ("Setting URI to %s", char_uri);
  if (data->target_state >= GST_STATE_READY)
    gst_element_set_state (data->pipeline, GST_STATE_READY);
  g_object_set(data->pipeline, "uri", char_uri, NULL);
  (*env)->ReleaseStringUTFChars (env, uri, char_uri);
  data->is_live |= (gst_element_set_state (data->pipeline, data->target_state) == GST_STATE_CHANGE_NO_PREROLL);
}

/* Set pipeline to PLAYING state */
static void gst_native_play (JNIEnv* env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data) return;
  GST_DEBUG ("Setting state to PLAYING");
  data->target_state = GST_STATE_PLAYING;
  data->is_live |= (gst_element_set_state (data->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_NO_PREROLL);
}

/* Set pipeline to PAUSED state */
static void gst_native_pause (JNIEnv* env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data) return;
  GST_DEBUG ("Setting state to PAUSED");
  data->target_state = GST_STATE_PAUSED;
  data->is_live |= (gst_element_set_state (data->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_NO_PREROLL);
}

/* Static class initializer: retrieve method and field IDs */
static jboolean gst_native_class_init (JNIEnv* env, jclass klass) {
  custom_data_field_id = (*env)->GetFieldID (env, klass, "native_custom_data", "J");
  on_gstreamer_initialized_method_id = (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
  on_media_size_changed_method_id = (*env)->GetMethodID (env, klass, "onMediaSizeChanged", "(II)V");

  if (!on_gstreamer_initialized_method_id || !on_media_size_changed_method_id) {
    /* We emit this message through the Android log instead of the GStreamer log because the later
     * has not been initialized yet.
     */
    __android_log_print (ANDROID_LOG_ERROR, "pirovera", "The calling class does not implement all necessary interface methods");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

static void gst_native_surface_init (JNIEnv *env, jobject thiz, jobject surface) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data) return;
  ANativeWindow *new_native_window = ANativeWindow_fromSurface(env, surface);
  GST_DEBUG ("Received surface %p (native window %p)", surface, new_native_window);

  if (data->native_window) {
    ANativeWindow_release (data->native_window);
    if (data->native_window == new_native_window) {
      GST_DEBUG ("New native window is the same as the previous one %p", data->native_window);
      if (data->pipeline) {
        gst_video_overlay_expose(GST_VIDEO_OVERLAY (data->pipeline));
        gst_video_overlay_expose(GST_VIDEO_OVERLAY (data->pipeline));
      }
      return;
    } else {
      GST_DEBUG ("Released previous native window %p", data->native_window);
      data->initialized = FALSE;
    }
  }
  data->native_window = new_native_window;

  check_initialization_complete (data);
}

static void gst_native_surface_finalize (JNIEnv *env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  if (!data) return;
  GST_DEBUG ("Releasing Native Window %p", data->native_window);

  if (data->pipeline) {
    gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->pipeline), (guintptr)NULL);
    gst_element_set_state (data->pipeline, GST_STATE_READY);
  }

  ANativeWindow_release (data->native_window);
  data->native_window = NULL;
  data->initialized = FALSE;
}

#define DZ (10000)

static unsigned short motor_speed(int n) {
  unsigned short tmp = 0;
  if (n < -DZ) {
    tmp = 0x8000;
    n = -n;
  }
  if (n < DZ) n = 0;
  else n = (n - 8000) * 1.31;
  n = n / 7;
  tmp |= n;
  return tmp;
}

static void gst_native_set_left (JNIEnv *env, jobject thiz, int n) {
  control_set_left(motor_speed(n));
}

static void gst_native_set_right (JNIEnv *env, jobject thiz, int n) {
  control_set_right(motor_speed(n));
}

static void gst_native_set_headlights (JNIEnv *env, jobject thiz, uint8_t n) {
  control_set_headlights(n);
}

static void gst_native_set_taillights (JNIEnv *env, jobject thiz, uint8_t n) {
  control_set_taillights(n);
}

static void gst_native_set_hazardlights (JNIEnv *env, jobject thiz, uint8_t n) {
  control_set_hazardlights(n);
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
  { "nativeInit", "()V", (void *) gst_native_init},
  { "nativeFinalize", "()V", (void *) gst_native_finalize},
  { "nativeSetUri", "(Ljava/lang/String;)V", (void *) gst_native_set_uri},
  { "nativePlay", "()V", (void *) gst_native_play},
  { "nativePause", "()V", (void *) gst_native_pause},
  { "nativeSurfaceInit", "(Ljava/lang/Object;)V", (void *) gst_native_surface_init},
  { "nativeSurfaceFinalize", "()V", (void *) gst_native_surface_finalize},
  { "nativeSetLeft", "(I)V", (void *) gst_native_set_left},
  { "nativeSetRight", "(I)V", (void *) gst_native_set_right},
  { "nativeSetHeadlights", "(Z)V", (void *) gst_native_set_headlights},
  { "nativeSetTaillights", "(Z)V", (void *) gst_native_set_taillights},
  { "nativeSetHazardlights", "(Z)V", (void *) gst_native_set_hazardlights},
  { "nativeClassInit", "()Z", (void *) gst_native_class_init}
};

/* Library initializer */
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  JNIEnv *env = NULL;

  java_vm = vm;

  if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
    __android_log_print (ANDROID_LOG_ERROR, "pirovera", "Could not retrieve JNIEnv");
    return 0;
  }
  jclass klass = (*env)->FindClass (env, "com/robotfuzz/al/pirovera/PiRover");
  (*env)->RegisterNatives (env, klass, native_methods, G_N_ELEMENTS(native_methods));

  pthread_key_create (&current_jni_env, detach_current_thread);

  return JNI_VERSION_1_4;
}
