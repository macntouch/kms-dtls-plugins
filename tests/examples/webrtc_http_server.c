/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#include <gst/gst.h>
#include <libsoup/soup.h>
#include <nice/nice.h>
#include <string.h>

#define GST_CAT_DEFAULT webrtc_http_server
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define DEBUG_NAME "webrtc_http_server"

#define PORT 8080
#define MIME_TYPE "text/html"
#define HTML_FILE "webrtc_loopback.html"
#define PEMFILE "certkey.pem"

static GRand *rand;
static GHashTable *cookies;

typedef struct _MesiaSession {
  gint64 id;

  GMainContext *context;
  GMainLoop * loop;

  SoupServer *server;
  SoupMessage *msg;

  NiceAgent *agent;
  guint stream_id;

  GstElement *pipeline;
} MesiaSession;

static void
bus_msg (GstBus * bus, GstMessage * msg, gpointer pipe)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      gchar *error_file;

      error_file = g_strdup_printf ("error-%s", GST_OBJECT_NAME (pipe));
      GST_ERROR ("Error: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, error_file);
      g_free (error_file);

      /* TODO: free mediaSession*/
      break;
    }
    case GST_MESSAGE_WARNING:{
      gchar *warn_file = g_strdup_printf ("warning-%s", GST_OBJECT_NAME (pipe));

      GST_WARNING ("Warning: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, warn_file);
      g_free (warn_file);
      break;
    }
    default:
      break;
  }
}

static void
create_pipeline (MesiaSession *mediaSession)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  GstElement *rtpvp8pay = gst_element_factory_make ("rtpvp8pay", NULL);
  GstElement *rtpvp8depay = gst_element_factory_make ("rtpvp8depay", NULL);
  GstElement *vp8dec = gst_element_factory_make ("vp8dec", NULL);
  GstElement *vp8enc = gst_element_factory_make ("vp8enc", NULL);
  GstElement *dtlssrtpenc = gst_element_factory_make ("dtlssrtpenc", NULL);
  GstElement *dtlssrtpdec = gst_element_factory_make ("dtlssrtpdec", NULL);
  GstElement *nicesink = gst_element_factory_make ("nicesink", NULL);
  GstElement *nicesrc = gst_element_factory_make ("nicesrc", NULL);
  GstElement *capsfilter = gst_element_factory_make ("capsfilter", NULL);
  GstElement *clockoverlay = gst_element_factory_make ("clockoverlay", NULL);
  GstCaps *caps;

  g_object_set (G_OBJECT (pipeline), "async-handling", TRUE, NULL);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (bus_msg), pipeline);
  g_object_unref(bus);

  caps = gst_caps_new_simple("application/x-rtp",
                              "payload", G_TYPE_INT, 96,
                              NULL);
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  g_object_set (clockoverlay, "font-desc", "Sans 28", NULL);
  g_object_set(vp8enc, "deadline", GST_SECOND / 30, "target-bitrate", 256000,
                "keyframe-mode", 0, "end-usage", 2, NULL);

  g_object_set (G_OBJECT (dtlssrtpenc), "channel-id", GST_OBJECT_NAME (pipeline), NULL);
  g_object_set (G_OBJECT (dtlssrtpenc), "is-client", FALSE, NULL);
  g_object_set (G_OBJECT (dtlssrtpdec), "channel-id", GST_OBJECT_NAME (pipeline), NULL);
  g_object_set (G_OBJECT (dtlssrtpdec), "is-client", FALSE, NULL);
  g_object_set (G_OBJECT (dtlssrtpdec), "certificate-pem-file", PEMFILE, NULL);

  g_object_set (G_OBJECT (nicesink), "agent", mediaSession->agent, "stream", mediaSession->stream_id, "component", 1, NULL);
  g_object_set (G_OBJECT (nicesrc), "agent", mediaSession->agent, "stream", mediaSession->stream_id, "component", 1, NULL);

  gst_bin_add_many (GST_BIN (pipeline), nicesrc, dtlssrtpdec, rtpvp8depay, rtpvp8pay, dtlssrtpenc, nicesink,
                    capsfilter, vp8dec, vp8enc, clockoverlay, NULL);
  gst_element_link_many (nicesrc, dtlssrtpdec, capsfilter, rtpvp8depay, vp8dec, clockoverlay,
                         vp8enc, rtpvp8pay, dtlssrtpenc, nicesink, NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL,
                                GST_OBJECT_NAME(pipeline));

  mediaSession->pipeline = pipeline;
}

static gchar*
generate_fingerprint (const gchar *pem_file)
{
  GTlsCertificate *cert;
  GError *error = NULL;
  GByteArray *ba;
  gssize length;
  int size;
  gchar *fingerprint;
  gchar *fingerprint_colon;
  int i, j;

  cert = g_tls_certificate_new_from_file (pem_file, &error);
  g_object_get (cert, "certificate", &ba, NULL);
  fingerprint = g_compute_checksum_for_data (G_CHECKSUM_SHA256, ba->data, ba->len);
  g_object_unref (cert);

  length = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  size = (int)(length*2 + length) * sizeof (gchar);
  fingerprint_colon = g_malloc0 (size);

  j = 0;
  for (i=0; i< length*2; i+=2) {
    fingerprint_colon[j] = g_ascii_toupper (fingerprint[i]);
    fingerprint_colon[++j] = g_ascii_toupper (fingerprint[i+1]);
    fingerprint_colon[++j] = ':';
    j++;
  };
  fingerprint_colon[size-1] = '\0';
  g_free (fingerprint);

  return fingerprint_colon;
}

static void
gathering_done (NiceAgent *agent, guint stream_id, MesiaSession *mediaSession)
{
  SoupServer *server = mediaSession->server;
  SoupMessage *msg = mediaSession->msg;
  GString *sdpStr;
  GSList *candidates;
  GSList *walk;
  NiceCandidate *lowest_prio_cand = NULL;
  gchar addr[NICE_ADDRESS_STRING_LEN+1];
  gchar *ufrag, *pwd;
  gchar *fingerprint;
  gchar *line;

  nice_agent_get_local_credentials (mediaSession->agent, mediaSession->stream_id, &ufrag, &pwd);
  candidates = nice_agent_get_local_candidates (mediaSession->agent, mediaSession->stream_id, 1);

  for (walk = candidates; walk; walk = walk->next) {
    NiceCandidate *cand = walk->data;

    if (!lowest_prio_cand ||
        lowest_prio_cand->priority < cand->priority)
      lowest_prio_cand = cand;
  }

  candidates = g_slist_concat (candidates,
                               nice_agent_get_local_candidates (mediaSession->agent, mediaSession->stream_id, 2));

  nice_address_to_string (&lowest_prio_cand->addr, addr);
  fingerprint = generate_fingerprint (PEMFILE);

  sdpStr = g_string_new ("");
  g_string_append_printf (sdpStr,
      "\"v=0\\r\\n\" +\n"
      "\"o=- 2750483185 0 IN IP4 %s\\r\\n\" +\n"
      "\"s=\\r\\n\" +\n"
      "\"t=0 0\\r\\n\" +\n"
      "\"a=ice-ufrag:%s\\r\\n\" +\n"
      "\"a=ice-pwd:%s\\r\\n\" +\n"
      "\"a=fingerprint:sha-256 %s\\r\\n\" +\n"
      "\"a=group:BUNDLE video\\r\\n\" +\n"
      "\"m=video %d RTP/SAVPF 96\\r\\n\" +\n"
      "\"c=IN IP4 %s\\r\\n\" +\n"
      "\"a=rtpmap:96 VP8/90000\\r\\n\" +\n"
      "\"a=sendrecv\\r\\n\" +\n"
      "\"a=mid:video\\r\\n\" +\n"
      "\"a=rtcp-mux\\r\\n\"",
      addr, ufrag, pwd, fingerprint, nice_address_get_port (&lowest_prio_cand->addr), addr);

  g_free (ufrag);
  g_free (pwd);
  g_free (fingerprint);

  for (walk = candidates; walk; walk = walk->next) {
    NiceCandidate *cand = walk->data;

    nice_address_to_string (&cand->addr, addr);
    g_string_append_printf (sdpStr,
        "+\n\"a=candidate:%s %d UDP %d %s %d typ host\\r\\n\"",
        cand->foundation, cand->component_id, cand->priority, addr,
        nice_address_get_port (&cand->addr));
  }
  g_slist_free_full (candidates, (GDestroyNotify) nice_candidate_free);

  line = g_strdup_printf("sdp = %s;\n", sdpStr->str);
  soup_message_body_append (msg->response_body, SOUP_MEMORY_TAKE, line,
                            strlen(line));
  g_string_free (sdpStr, FALSE);

  line = "</script>\n</body>\n</html>\n";
  soup_message_body_append (msg->response_body, SOUP_MEMORY_STATIC, line,
                            strlen(line));

  soup_message_set_status (msg, SOUP_STATUS_OK);
  soup_server_unpause_message (server, msg);

  g_object_unref (server);
  g_object_unref (msg);
}

static void
nice_agent_recv (NiceAgent *agent, guint stream_id, guint component_id,
                 guint len, gchar *buf, gpointer user_data)
{
  /* Nothing to do, this callback is only for negotiation */
}

static gpointer
loop_thread (gpointer loop)
{
  g_main_loop_run (loop); /* TODO: g_main_loop_quit */

  return NULL;
}

static gchar *
get_substring (const gchar *regex, const gchar *string)
{
  GRegex *gregex;
  GMatchInfo *match_info = NULL;
  gchar *ret = NULL;

  gregex = g_regex_new (regex,
      G_REGEX_MULTILINE | G_REGEX_NEWLINE_CRLF, 0, NULL);
  g_assert (gregex);

  g_regex_match (gregex, string, 0, &match_info);

  if (g_match_info_get_match_count (match_info) == 2)
    ret = g_match_info_fetch (match_info, 1);

  g_match_info_free (match_info);
  g_regex_unref (gregex);

  return ret;
}

static gboolean
configure_media_session (MesiaSession *mediaSession, const gchar *sdp)
{
  gboolean ret;
  gchar *ufrag;
  gchar *pwd;
  GRegex *regex;
  GMatchInfo *match_info = NULL;

  GST_DEBUG ("Process SDP:\n%s", sdp);

  ufrag = get_substring ("^a=ice-ufrag:([A-Za-z0-9\\+\\/]+)$", sdp);
  pwd = get_substring ("^a=ice-pwd:([A-Za-z0-9\\+\\/]+)$", sdp);
  /* TODO: get remote fingerprint and pt*/

  nice_agent_set_remote_credentials (mediaSession->agent, mediaSession->stream_id, ufrag, pwd);
  g_free (ufrag);
  g_free (pwd);

  regex = g_regex_new ("^a=candidate:(?<foundation>[0-9]+) (?<cid>[0-9]+)"
      " (udp|UDP) (?<prio>[0-9]+) (?<addr>[0-9.:a-zA-Z]+) (?<port>[0-9]+) typ host( generation [0-9]+)?$",
      G_REGEX_MULTILINE | G_REGEX_NEWLINE_CRLF, 0, NULL);
  g_assert (regex);
  g_regex_match (regex, sdp, 0, &match_info);

  while (g_match_info_matches (match_info)) {
    NiceCandidate *cand;
    GSList *candidates;

    gchar *foundation = g_match_info_fetch_named (match_info, "foundation");
    gchar *cid_str = g_match_info_fetch_named (match_info, "cid");
    gchar *prio_str = g_match_info_fetch_named (match_info, "prio");
    gchar *addr = g_match_info_fetch_named (match_info, "addr");
    gchar *port_str = g_match_info_fetch_named (match_info, "port");

    cand = nice_candidate_new (NICE_CANDIDATE_TYPE_HOST);
    cand->component_id = g_ascii_strtoll (cid_str, NULL, 10);
    cand->priority = g_ascii_strtoll (prio_str, NULL, 10);
    strncpy (cand->foundation, foundation, NICE_CANDIDATE_MAX_FOUNDATION);

    ret = nice_address_set_from_string (&cand->addr, addr);
    g_assert (ret);
    nice_address_set_port (&cand->addr, g_ascii_strtoll (port_str, NULL, 10));

    g_free (addr);
    g_free (foundation);
    g_free (cid_str);
    g_free (prio_str);
    g_free (port_str);

    candidates = g_slist_append (NULL, cand);
    ret = nice_agent_set_remote_candidates (mediaSession->agent, mediaSession->stream_id,
        cand->component_id, candidates);
    g_assert (ret);
    g_slist_free (candidates);
    nice_candidate_free (cand);

    g_match_info_next (match_info, NULL);
  }

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return TRUE;
}

static MesiaSession*
init_media_session (SoupServer *server, SoupMessage *msg, gint64 id)
{
  MesiaSession *mediaSession;
  GThread * lt;

  mediaSession = g_slice_new0 (MesiaSession); /* TODO: free */
  mediaSession->id = id;

  mediaSession->context = g_main_context_new ();
  mediaSession->loop = g_main_loop_new (mediaSession->context, TRUE); /* TODO: g_main_loop_unref */
  lt = g_thread_new ("ctx", loop_thread, mediaSession->loop);
  g_thread_unref (lt);

  mediaSession->server = g_object_ref (server);
  mediaSession->msg = g_object_ref (msg);

  mediaSession->agent = nice_agent_new (mediaSession->context, NICE_COMPATIBILITY_RFC5245);
  g_object_set (mediaSession->agent, "upnp", FALSE, NULL);
  g_object_set(G_OBJECT (mediaSession->agent),
               "stun-server", "77.72.174.167",
               "stun-server-port", 3478,
               NULL);

  mediaSession->stream_id = nice_agent_add_stream (mediaSession->agent, 2);
  nice_agent_attach_recv (mediaSession->agent, mediaSession->stream_id, 1, mediaSession->context,
                         nice_agent_recv, NULL);
  nice_agent_attach_recv (mediaSession->agent, mediaSession->stream_id, 2, mediaSession->context,
                         nice_agent_recv, NULL);
  g_signal_connect (mediaSession->agent, "candidate-gathering-done",
    G_CALLBACK (gathering_done), mediaSession);

  create_pipeline (mediaSession);

  nice_agent_gather_candidates (mediaSession->agent, mediaSession->stream_id);

  return mediaSession;
}

static void
server_callback (SoupServer *server, SoupMessage *msg, const char *path,
                 GHashTable *query, SoupClientContext *client,
                 gpointer user_data)
{
  gboolean ret;
  gchar *contents;
  gsize length;
  const char *cookie_str;
  SoupCookie *cookie = NULL;
  gint64 id, *id_ptr;
  char *header;
  MesiaSession *mediaSession = NULL;

  GST_DEBUG ("Request: %s", path);

  if (msg->method != SOUP_METHOD_GET) {
    soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);
    GST_DEBUG ("Not implemented");
    return;
  }

  if (g_strcmp0 (path, "/") != 0) {
    soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
    GST_DEBUG ("Not found");
    return;
  }

  cookie_str = soup_message_headers_get_list (msg->request_headers, "Cookie");
  if (cookie_str != NULL) {
    gchar **tokens, **token;

    tokens = g_strsplit (cookie_str, ";", 0);
    for (token = tokens; *token != NULL; token++) {
      cookie = soup_cookie_parse (*token, NULL);

      if (cookie != NULL) {
        if (g_strcmp0 (cookie->name, "id") == 0) {
          id = g_ascii_strtoll (cookie->value, NULL, 0);
          if (id != G_GINT64_CONSTANT (0)) {
            GST_DEBUG ("Found id: %"G_GINT64_FORMAT, id);
            mediaSession = g_hash_table_lookup (cookies, &id);
            break;
          }
        }
        soup_cookie_free (cookie);
        cookie = NULL;
      }
    }
    g_strfreev (tokens);
  }

  if (cookie == NULL) {
    gchar *id_str;
    const gchar *host;

    id = g_rand_double_range (rand, (double) G_MININT64, (double) G_MAXINT64);
    id_str = g_strdup_printf ("%" G_GINT64_FORMAT, id);
    host = soup_message_headers_get_one(msg->request_headers, "Host");
    if (host == NULL) {
      host = "localhost";
    }

    cookie = soup_cookie_new ("id", id_str, host, path, -1);
    g_free (id_str);
  }

  if (query != NULL) {
    gchar * sdp = g_hash_table_lookup(query, "sdp");
    if (sdp != NULL && mediaSession != NULL) {
      if (configure_media_session (mediaSession, sdp)) {
        soup_message_set_status (msg, SOUP_STATUS_OK);
      } else {
        soup_message_set_status (msg, SOUP_STATUS_NOT_ACCEPTABLE);
      }
      soup_message_set_response (msg, MIME_TYPE, SOUP_MEMORY_STATIC, "", 0);
      return;
    }
  }

  g_hash_table_remove (cookies, &id);

  header = soup_cookie_to_cookie_header (cookie);
  if (header != NULL) {
    soup_message_headers_append (msg->response_headers, "Set-Cookie", header);
    g_free(header);
  } else {
    GST_WARNING ("Null cookie");
  }
  soup_cookie_free (cookie);

  mediaSession = init_media_session (server, msg, id);
  id_ptr = g_malloc (sizeof(gint64));
  *id_ptr = id;
  g_hash_table_insert (cookies, id_ptr, mediaSession);

  ret = g_file_get_contents (HTML_FILE, &contents, &length, NULL);
  if (!ret) {
    soup_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
    GST_ERROR ("Error loading %s file", HTML_FILE);
    return;
  }

  soup_message_set_response (msg, MIME_TYPE, SOUP_MEMORY_STATIC, "", 0);
  soup_message_body_append (msg->response_body, SOUP_MEMORY_TAKE, contents, length);
  soup_server_pause_message (server, msg);
}

static void
destroy_media_session (gpointer data)
{
  GST_WARNING ("TODO: implement");
}

int
main (int argc, char ** argv)
{
  SoupServer *server;

  gst_init (&argc, &argv);
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, DEBUG_NAME, 0, DEBUG_NAME);

  cookies = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, destroy_media_session);
  rand = g_rand_new ();

  GST_INFO ("Start Kurento WebRTC HTTP server");
  server = soup_server_new (SOUP_SERVER_PORT, PORT,
                            NULL);
  soup_server_add_handler (server, "/", server_callback, NULL, NULL);
  soup_server_run (server);

  g_hash_table_destroy (cookies);
  g_rand_free (rand);

  return 0;
}
