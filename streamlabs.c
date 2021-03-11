#include <gst/gst.h>
#include <glib.h>
#include <gst/video/video.h>
#include <string.h>

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
  GstElement *pipeline, *videosource[3], *timeoverlay[3], *mixer, *tee, *playback_queue, *playback_convert, *playback_sink;
  GstElement *network_queue, *rtmpsink,  *flvmux, *x264enc;

  guint sourceid;        /* To control the GSource */

  GMainLoop *main_loop;  /* GLib's Main Loop */
} CustomData;

/* This function is called when an error message is posted on the bus */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  g_main_loop_quit (data->main_loop);
}

static gboolean
link_elements_with_filter (GstElement *element1, GstElement *element2)
{
    gboolean link_ok;
    GstCaps *caps;

    caps = gst_caps_new_simple ("video/x-raw",
                                "format", G_TYPE_STRING, "I420",
                                "width", G_TYPE_INT, 640,
                                "height", G_TYPE_INT, 360,
                                "framerate", GST_TYPE_FRACTION, 30, 1,
                                NULL);

    link_ok = gst_element_link_filtered (element1, element2, caps);
    gst_caps_unref (caps);

    if (!link_ok) {
        g_warning ("Failed to link element1 and element2!");
    }

    return link_ok;
}


int main(int argc, char *argv[]) {

  GOptionContext *ctx;
  GError *err = NULL;
  gchar *rtmplink = NULL;
  GOptionEntry entries[] = {
    { "rtmplink", 'r', 0, G_OPTION_ARG_STRING, &rtmplink},
    { NULL }
  };

  ctx = g_option_context_new ("Publish to Twitch");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    g_print ("Failed to initialize: %s\n", err->message);
    g_clear_error (&err);
    g_option_context_free (ctx);
    return 1;
  }
  g_option_context_free (ctx);

  if (!rtmplink){
	g_printerr("Please specify rtmp link to publish\n");
  	return -1;
  }
      
  CustomData data;

  GstPad *tee_playback_pad, *tee_network_pad;
  GstPad *queue_playback_pad, *queue_network_pad;

  GstCaps *playback_caps;
  GstBus *bus;

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Create the elements */
  GString *videotestsrc_str = g_string_new (NULL);
  GString *timeoverlay_str = g_string_new (NULL);

  for (int i = 0; i < 3; ++i){
      g_string_printf (videotestsrc_str, "videotestsrc%d", i);
      g_string_printf (timeoverlay_str, "timeoverlay%d", i);

      data.videosource[i] = gst_element_factory_make("videotestsrc", videotestsrc_str->str);
      data.timeoverlay[i] = gst_element_factory_make("timeoverlay", timeoverlay_str->str);

      g_object_set(G_OBJECT(data.videosource[i]), "is-live", TRUE, NULL);
  }

  g_string_free (timeoverlay_str, TRUE);
  g_string_free (videotestsrc_str, TRUE);


  data.mixer = gst_element_factory_make ("videomixer", "videomixer");
 
  data.tee = gst_element_factory_make ("tee", "tee");
  data.playback_queue = gst_element_factory_make ("queue", "playback_queue");
  g_object_set(G_OBJECT(data.playback_queue), "leaky", 2, NULL );
  g_object_set(G_OBJECT(data.playback_queue), "max-size-buffers", 5, NULL);
  data.playback_convert = gst_element_factory_make ("videoconvert", "playback_videoconvert");
  data.playback_sink = gst_element_factory_make ("glimagesink", "playback_sink");
  g_object_set(G_OBJECT(data.playback_sink), "sync", FALSE, NULL);

  data.network_queue = gst_element_factory_make("queue","network_queue");
  g_object_set(G_OBJECT(data.playback_queue), "leaky", 2, NULL );
  g_object_set(G_OBJECT(data.playback_queue), "max-size-buffers", 5, NULL);
  data.x264enc = gst_element_factory_make("x264enc", "x264enc");
  g_object_set(G_OBJECT(data.x264enc), "key-int-max", 15, NULL);
  g_object_set(G_OBJECT(data.x264enc), "bitrate", 3000, NULL);
  g_object_set(G_OBJECT(data.x264enc), "speed-preset", 1,NULL);
  g_object_set(G_OBJECT(data.x264enc), "tune", 4, NULL);
  data.flvmux = gst_element_factory_make ("flvmux", "flvmux");
  data.rtmpsink = gst_element_factory_make ("rtmpsink", "rtmpsink");
  g_object_set(G_OBJECT(data.rtmpsink), "location", rtmplink, NULL);
  g_object_set(G_OBJECT(data.rtmpsink), "sync", FALSE, NULL);

  /* Create the empty pipeline */
  data.pipeline = gst_pipeline_new ("pipeline");

  if (!data.pipeline || !data.mixer || !data.tee || !data.playback_queue || !data.playback_sink || !data.playback_convert ||
      !data.x264enc || !data.flvmux || !data.rtmpsink ) {
    g_printerr ("Not all elements could be created from pipeline.\n");
    return -1;
  }

  for (int i = 0; i < 3; ++i) {
      if (!data.videosource[i] || !data.timeoverlay[i] ){
          g_printerr ("Not all filters could be created.\n");
          return -1;
      }
  }

  /* Link all elements that can be automatically linked because they have "Always" pads */
  gst_bin_add_many (GST_BIN (data.pipeline), data.mixer, data.tee, data.playback_queue, data.playback_sink, data.playback_convert,
                    data.network_queue, data.x264enc, data.flvmux, data.rtmpsink, NULL);

  for (int i = 0; i < 3; ++i){
      gst_bin_add_many (GST_BIN (data.pipeline), data.videosource[i], data.timeoverlay[i], NULL);

      if (TRUE != link_elements_with_filter(data.videosource[i], data.timeoverlay[i])){
          g_printerr ("Can not link video source and time overlay.\n");
          return -1;
      }

      if (TRUE != gst_element_link(data.timeoverlay[i], data.mixer)){
          g_printerr ("Elements could not be linked.\n");
          gst_object_unref (data.pipeline);
          return -1;
      }
  }


  if (gst_element_link_many (data.mixer, data.tee, NULL) != TRUE ||
      gst_element_link_many (data.network_queue, data.x264enc, data.flvmux, data.rtmpsink, NULL) != TRUE ||
      gst_element_link_many (data.playback_queue, data.playback_convert, data.playback_sink, NULL) != TRUE ) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  /* Manually link the Tee, which has "Request" pads */
  tee_playback_pad = gst_element_get_request_pad (data.tee, "src_%u");
  queue_playback_pad = gst_element_get_static_pad (data.playback_queue, "sink");
  tee_network_pad = gst_element_get_request_pad (data.tee, "src_%u");
  queue_network_pad = gst_element_get_static_pad (data.network_queue, "sink");
  queue_playback_pad = gst_element_get_static_pad (data.playback_queue, "sink");

  if (gst_pad_link (tee_playback_pad, queue_playback_pad) != GST_PAD_LINK_OK ||
      gst_pad_link (tee_network_pad, queue_network_pad) != GST_PAD_LINK_OK) {
    g_printerr ("Tee could not be linked\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  gst_object_unref (queue_playback_pad);
  gst_object_unref (queue_network_pad);

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
  gst_object_unref (bus);

  g_object_set(G_OBJECT(data.mixer),"background",1,NULL);

  GstPad* sink_1 = gst_element_get_static_pad(data.mixer, "sink_1");
  GstPad* sink_2 = gst_element_get_static_pad(data.mixer, "sink_2");

  g_object_set(sink_1, "xpos", 640, NULL);
  g_object_set(sink_1, "ypos", 0, NULL);
  g_object_set(sink_2, "xpos", 360, NULL);
  g_object_set(sink_2, "ypos", 360, NULL);

  gst_object_unref(sink_1);
  gst_object_unref(sink_2);

  /* Start playing the pipeline */
  gst_element_set_state (data.pipeline, GST_STATE_PLAYING);

  /* Create a GLib Main Loop and set it to run */
  data.main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (data.main_loop);

  /* Release the request pads from the Tee, and unref them */
  gst_element_release_request_pad (data.tee, tee_playback_pad);
  gst_element_release_request_pad (data.tee, tee_network_pad);
  gst_object_unref (tee_playback_pad);
  gst_object_unref (tee_network_pad);


  /* Free resources */
  gst_element_set_state (data.pipeline, GST_STATE_NULL);
  gst_object_unref (data.pipeline);
  return 0;
}
