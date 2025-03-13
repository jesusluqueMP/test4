/* GStreamer
 * Copyright (C) 2023 Your Name <your.email@example.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/* Define GST_USE_UNSTABLE_API to avoid warnings about unstable API */
#define GST_USE_UNSTABLE_API

/* Define PACKAGE for plugin registration */
#ifndef PACKAGE
#define PACKAGE "spoutsrc"
#endif

/**
 * SECTION:element-spoutsrc
 * @title: spoutsrc
 * @short_description: Spout DirectX texture sharing source element
 *
 * spoutsrc captures frames from Spout senders, which are applications
 * sharing DirectX textures via Spout's shared memory framework.
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 spoutsrc sender-name=SenderName ! queue ! d3d11videosink
 * ```
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstspoutsrc.h"
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11utils.h>
#include <gst/d3d11/gstd3d11format.h>
#include <mutex>
#include <string>

// DirectX headers needed for DXGI format definitions
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

// Include Spout SDK headers
#include "SpoutDX.h"

GST_DEBUG_CATEGORY_STATIC (gst_spout_src_debug);
#define GST_CAT_DEFAULT gst_spout_src_debug

/* Memory:D3D11Memory caps feature indicates the buffer contains D3D11 GPU memory */
static GstStaticCaps pad_template_caps =
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, GST_SPOUT_SRC_FORMATS));

enum
{
  PROP_0,
  PROP_SENDER_NAME,
  PROP_WAIT_TIMEOUT,
  PROP_ADAPTER,
  PROP_PROCESSING_DEADLINE,
  PROP_FORCE_RECONNECT,
};

#define DEFAULT_SENDER_NAME        ""
#define DEFAULT_WAIT_TIMEOUT       16    /* ms */
#define DEFAULT_ADAPTER           -1     /* Default adapter */
#define DEFAULT_PROCESSING_DEADLINE (20 * GST_MSECOND)
#define DEFAULT_FORCE_RECONNECT   FALSE
#define DEFAULT_FRAMERATE         30.0   /* Default framerate if sender doesn't provide one */

/* Private data structure */
struct GstSpoutSrcPrivate
{
  /* GStreamer D3D11 Device */
  GstD3D11Device *device = nullptr;
  
  /* Spout SDK object */
  spoutDX *spout = nullptr;
  
  /* Texture information */
  ID3D11Texture2D *shared_texture = nullptr;
  ID3D11ShaderResourceView *texture_srv = nullptr;
  HANDLE shared_handle = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  GstVideoInfo video_info;
  
  /* Caps negotiation */
  GstCaps *caps = nullptr;
  
  /* Buffer pool for texture reuse */
  GstBufferPool *pool = nullptr;
  
  /* Thread safety */
  std::mutex lock;
  gboolean flushing = FALSE;
  
  /* Properties */
  std::string sender_name = DEFAULT_SENDER_NAME;
  guint wait_timeout = DEFAULT_WAIT_TIMEOUT;
  gint adapter = DEFAULT_ADAPTER;
  GstClockTime processing_deadline = DEFAULT_PROCESSING_DEADLINE;
  gboolean force_reconnect = DEFAULT_FORCE_RECONNECT;
  
  /* Connection state */
  gboolean connected = FALSE;
  gboolean first_frame = TRUE;
  guint reconnect_attempts = 0;
  std::string connected_sender_name; // Track the name of the connected sender
  
  /* Timing */
  GstClockTime prev_pts = GST_CLOCK_TIME_NONE;
  guint64 frame_number = 0;
  double current_fps = DEFAULT_FRAMERATE;
  GstClockTime last_receive_time = GST_CLOCK_TIME_NONE;
};

struct _GstSpoutSrc
{
  GstBaseSrc parent;
  
  GstSpoutSrcPrivate *priv;
};

static void gst_spout_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_spout_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_spout_src_finalize (GObject * object);

static GstClock *gst_spout_src_provide_clock (GstElement * elem);
static void gst_spout_src_set_context (GstElement * elem, GstContext * context);

static gboolean gst_spout_src_start (GstBaseSrc * src);
static gboolean gst_spout_src_stop (GstBaseSrc * src);
static gboolean gst_spout_src_unlock (GstBaseSrc * src);
static gboolean gst_spout_src_unlock_stop (GstBaseSrc * src);
static gboolean gst_spout_src_query (GstBaseSrc * src, GstQuery * query);
static GstCaps *gst_spout_src_get_caps (GstBaseSrc * src, GstCaps * filter);
static GstCaps *gst_spout_src_fixate (GstBaseSrc * src, GstCaps * caps);
static gboolean gst_spout_src_decide_allocation (GstBaseSrc * src, GstQuery * query);
static GstFlowReturn gst_spout_src_create (GstBaseSrc * src, guint64 offset,
    guint size, GstBuffer ** buf);

/* Helper functions */
static gboolean gst_spout_src_connect (GstSpoutSrc * self);
static void gst_spout_src_disconnect (GstSpoutSrc * self);
static GstVideoFormat gst_spout_src_dxgi_format_to_gst (DXGI_FORMAT dxgi_format);
static GstFlowReturn gst_spout_src_copy_texture_to_buffer (GstSpoutSrc * self, GstBuffer * buffer);

#define gst_spout_src_parent_class parent_class
G_DEFINE_TYPE (GstSpoutSrc, gst_spout_src, GST_TYPE_BASE_SRC);

static void
gst_spout_src_class_init (GstSpoutSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS (klass);
  GstCaps *caps;

  gobject_class->set_property = gst_spout_src_set_property;
  gobject_class->get_property = gst_spout_src_get_property;
  gobject_class->finalize = gst_spout_src_finalize;

  /* Install properties */
  g_object_class_install_property (gobject_class, PROP_SENDER_NAME,
      g_param_spec_string ("sender-name", "Sender Name",
          "Connect to this specific Spout sender (empty = autoconnect to active sender)",
          DEFAULT_SENDER_NAME, (GParamFlags) (G_PARAM_READWRITE | 
          G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
  
  g_object_class_install_property (gobject_class, PROP_WAIT_TIMEOUT,
      g_param_spec_uint ("wait-timeout", "Wait Timeout",
          "Timeout in milliseconds to wait for a frame", 
          0, G_MAXUINT, DEFAULT_WAIT_TIMEOUT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
          
  g_object_class_install_property (gobject_class, PROP_ADAPTER,
      g_param_spec_int ("adapter", "Adapter",
          "DXGI Adapter index to use (-1 = default)",
          -1, G_MAXINT, DEFAULT_ADAPTER,
          (GParamFlags) (G_PARAM_READWRITE | 
          G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
          
  g_object_class_install_property (gobject_class, PROP_PROCESSING_DEADLINE,
      g_param_spec_uint64 ("processing-deadline", "Processing deadline",
          "Maximum processing time for a buffer in nanoseconds", 
          0, G_MAXUINT64, DEFAULT_PROCESSING_DEADLINE,
          (GParamFlags) (G_PARAM_READWRITE | 
          G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property (gobject_class, PROP_FORCE_RECONNECT,
      g_param_spec_boolean ("force-reconnect", "Force Reconnect",
          "Force reconnection to the sender on each frame (useful for tricky senders)",
          DEFAULT_FORCE_RECONNECT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /* Set element metadata */
  gst_element_class_set_static_metadata (element_class,
      "Spout Source", "Source/Video",
      "Receives DirectX textures from Spout senders",
      "jesus luque <jluque@mediapro.tv>");

  /* Add pad template with memory:D3D11Memory caps */
  caps = gst_static_caps_get (&pad_template_caps);
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  /* Set element functions */
  element_class->provide_clock = GST_DEBUG_FUNCPTR (gst_spout_src_provide_clock);
  element_class->set_context = GST_DEBUG_FUNCPTR (gst_spout_src_set_context);

  /* Set source functions */
  basesrc_class->start = GST_DEBUG_FUNCPTR (gst_spout_src_start);
  basesrc_class->stop = GST_DEBUG_FUNCPTR (gst_spout_src_stop);
  basesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_spout_src_unlock);
  basesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_spout_src_unlock_stop);
  basesrc_class->query = GST_DEBUG_FUNCPTR (gst_spout_src_query);
  basesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_spout_src_get_caps);
  basesrc_class->fixate = GST_DEBUG_FUNCPTR (gst_spout_src_fixate);
  basesrc_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_spout_src_decide_allocation);
  basesrc_class->create = GST_DEBUG_FUNCPTR (gst_spout_src_create);

  /* Initialize debug category */
  GST_DEBUG_CATEGORY_INIT (gst_spout_src_debug, "spoutsrc", 0, "Spout Source");
}

static void
gst_spout_src_init (GstSpoutSrc * self)
{
  /* Configure base source properties */
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (self), TRUE);

  /* Allocate private data */
  self->priv = new GstSpoutSrcPrivate ();

  /* This is a live source that needs a clock */
  GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
  GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_REQUIRE_CLOCK);
}

static void
gst_spout_src_finalize (GObject * object)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (object);

  /* Free private data */
  delete self->priv;
  self->priv = nullptr;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_spout_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (object);
  GstSpoutSrcPrivate *priv = self->priv;
  std::unique_lock<std::mutex> lock(priv->lock);

  switch (prop_id) {
    case PROP_SENDER_NAME: {
      const gchar *sender_name = g_value_get_string (value);
      priv->sender_name.clear();
      if (sender_name)
        priv->sender_name = sender_name;
      else
        priv->sender_name = DEFAULT_SENDER_NAME;
      GST_DEBUG_OBJECT (self, "Set sender name to '%s'", priv->sender_name.c_str());
      break;
    }
    case PROP_WAIT_TIMEOUT:
      priv->wait_timeout = g_value_get_uint (value);
      GST_DEBUG_OBJECT (self, "Set wait timeout to %d ms", priv->wait_timeout);
      break;
    case PROP_ADAPTER:
      priv->adapter = g_value_get_int (value);
      GST_DEBUG_OBJECT (self, "Set adapter to %d", priv->adapter);
      break;
    case PROP_PROCESSING_DEADLINE: {
      GstClockTime prev_val = priv->processing_deadline;
      priv->processing_deadline = g_value_get_uint64 (value);
      GST_DEBUG_OBJECT (self, "Set processing deadline to %" GST_TIME_FORMAT,
          GST_TIME_ARGS (priv->processing_deadline));
      
      /* Post latency message if the value changed */
      if (prev_val != priv->processing_deadline) {
        lock.unlock();
        gst_element_post_message (GST_ELEMENT_CAST (self),
            gst_message_new_latency (GST_OBJECT_CAST (self)));
      }
      break;
    }
    case PROP_FORCE_RECONNECT:
      priv->force_reconnect = g_value_get_boolean (value);
      GST_DEBUG_OBJECT (self, "Set force reconnect to %s", 
                       priv->force_reconnect ? "TRUE" : "FALSE");
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_spout_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (object);
  GstSpoutSrcPrivate *priv = self->priv;
  std::lock_guard<std::mutex> lock(priv->lock);

  switch (prop_id) {
    case PROP_SENDER_NAME:
      g_value_set_string (value, priv->sender_name.c_str());
      break;
    case PROP_WAIT_TIMEOUT:
      g_value_set_uint (value, priv->wait_timeout);
      break;
    case PROP_ADAPTER:
      g_value_set_int (value, priv->adapter);
      break;
    case PROP_PROCESSING_DEADLINE:
      g_value_set_uint64 (value, priv->processing_deadline);
      break;
    case PROP_FORCE_RECONNECT:
      g_value_set_boolean (value, priv->force_reconnect);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstClock *
gst_spout_src_provide_clock (GstElement * elem)
{
  /* Use system clock for this live source */
  return gst_system_clock_obtain ();
}

static void
gst_spout_src_set_context (GstElement * elem, GstContext * context)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (elem);
  GstSpoutSrcPrivate *priv = self->priv;

  /* Handle D3D11 device context */
  gst_d3d11_handle_set_context (elem, context, priv->adapter, &priv->device);

  GST_ELEMENT_CLASS (parent_class)->set_context (elem, context);
}

/* Helper function to convert DXGI_FORMAT to GstVideoFormat */
static GstVideoFormat
gst_spout_src_dxgi_format_to_gst (DXGI_FORMAT dxgi_format)
{
  switch (dxgi_format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return GST_VIDEO_FORMAT_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return GST_VIDEO_FORMAT_RGBA;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
      return GST_VIDEO_FORMAT_BGRx;
    /* DXGI_FORMAT_R8G8B8X8_UNORM is not defined in all DirectX headers
     * Use a numeric constant instead (122) */
    case 122: /* DXGI_FORMAT_R8G8B8X8_UNORM */
      return GST_VIDEO_FORMAT_RGBx;
    default:
      return GST_VIDEO_FORMAT_UNKNOWN;
  }
}

/* Safely disconnect from Spout and clean up resources */
static void
gst_spout_src_disconnect (GstSpoutSrc * self)
{
  GstSpoutSrcPrivate *priv = self->priv;
  std::lock_guard<std::mutex> lock(priv->lock);
  
  GST_DEBUG_OBJECT (self, "Disconnecting from Spout (current state: connected=%d, sender=%s)",
                    priv->connected, priv->connected_sender_name.c_str());
  
  /* Release texture resources */
  if (priv->texture_srv) {
    priv->texture_srv->Release();
    priv->texture_srv = nullptr;
  }
  
  if (priv->shared_texture) {
    priv->shared_texture->Release();
    priv->shared_texture = nullptr;
  }
  
  /* Release Spout receiver */
  if (priv->spout) {
    priv->spout->ReleaseReceiver();
  }
  
  priv->connected = FALSE;
  priv->first_frame = TRUE;
  priv->connected_sender_name.clear();
}

/* Connect to a Spout sender and setup texture sharing */
static gboolean
gst_spout_src_connect (GstSpoutSrc * self)
{
  GstSpoutSrcPrivate *priv = self->priv;
  std::lock_guard<std::mutex> lock(priv->lock);
  
  if (!priv->spout) {
    GST_DEBUG_OBJECT (self, "Creating new spoutDX instance");
    priv->spout = new spoutDX();
    if (!priv->spout) {
      GST_ERROR_OBJECT (self, "Failed to create spoutDX instance");
      return FALSE;
    }
  }
  
  /* Get the D3D11 device from GStreamer */
  ID3D11Device *d3d11_device = gst_d3d11_device_get_device_handle (priv->device);
  if (!d3d11_device) {
    GST_ERROR_OBJECT (self, "Failed to get D3D11 device handle");
    return FALSE;
  }
  
  /* Initialize Spout with our D3D11 device */
  if (!priv->spout->OpenDirectX11(d3d11_device)) {
    GST_ERROR_OBJECT (self, "Failed to initialize Spout DirectX11");
    return FALSE;
  }
  
  /* Get sender list for debugging */
  int senderCount = priv->spout->GetSenderCount();
  GST_INFO_OBJECT (self, "Found %d Spout senders", senderCount);
  
  for (int i = 0; i < senderCount; i++) {
    char senderName[256];
    if (priv->spout->GetSender(i, senderName, 256)) {
      GST_INFO_OBJECT (self, "Spout Sender %d: '%s'", i, senderName);
    }
  }
  
  if (senderCount == 0) {
    GST_DEBUG_OBJECT (self, "No Spout senders found, will wait for one to appear");
    return TRUE; // Not an error, just wait for sender to appear
  }
  
  /* Set receiver name if specified */
  if (!priv->sender_name.empty()) {
    /* Try to connect using the sender name */
    const char* senderName = priv->sender_name.c_str();
    GST_DEBUG_OBJECT (self, "Trying to connect to Spout sender '%s'", senderName);
    
    /* First release any existing receiver connection */
    priv->spout->ReleaseReceiver();
    
    /* Set the receiver to target the specified sender */
    priv->spout->SetReceiverName(senderName);
    
    /* Get the sender info */
    unsigned int width = 0;
    unsigned int height = 0;
    HANDLE shareHandle = NULL;
    DWORD format = 0;
    
    if (priv->spout->GetSenderInfo(senderName, width, height, shareHandle, format)) {
      GST_INFO_OBJECT (self, "Found sender info for '%s': %dx%d, format %d", 
                      senderName, width, height, format);
      
      /* Now try to receive a texture */
      ID3D11Texture2D* texture = NULL;
      if (priv->spout->ReceiveTexture(&texture)) {
        GST_INFO_OBJECT (self, "Successfully connected to sender '%s'", senderName);
        
        /* Now set up our local info based on the connection */
        priv->format = (DXGI_FORMAT)format;
        priv->connected_sender_name = senderName;
        
        /* Get or create caps based on sender info */
        GstVideoFormat video_format = gst_spout_src_dxgi_format_to_gst((DXGI_FORMAT)format);
        if (video_format == GST_VIDEO_FORMAT_UNKNOWN) {
          GST_WARNING_OBJECT (self, "Unsupported DXGI format %d, falling back to BGRA", format);
          video_format = GST_VIDEO_FORMAT_BGRA;
        }
        
        /* Get the sender frame rate if available */
        double fps = priv->spout->GetSenderFps();
        if (fps <= 0.0 || fps > 1000.0) {
          /* If the sender doesn't provide a valid framerate, use our default */
          fps = DEFAULT_FRAMERATE;
        }
        priv->current_fps = fps;
        
        GST_DEBUG_OBJECT (self, "Using sender framerate: %.2f fps", fps);
        
        /* Set up the video info with the framerate */
        gst_video_info_set_format(&priv->video_info, video_format, width, height);
        priv->video_info.fps_n = (int)(fps * 1000);
        priv->video_info.fps_d = 1000; /* Using 1000 as denominator for better precision */
        
        /* Create caps from video info and make them writable */
        GstCaps *new_caps = gst_video_info_to_caps(&priv->video_info);
        new_caps = gst_caps_make_writable(new_caps);
        
        /* Add D3D11 memory feature to the writable caps */
        GstCapsFeatures *features = gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, NULL);
        gst_caps_set_features(new_caps, 0, features);
        
        /* Replace the existing caps with the new one */
        gst_caps_replace(&priv->caps, new_caps);
        gst_caps_unref(new_caps);
        
        GST_DEBUG_OBJECT (self, "Created caps %" GST_PTR_FORMAT, priv->caps);
        
        /* Clean up the texture we just received */
        if (texture) {
          texture->Release();
        }
        
        /* Mark as connected and update last receive time */
        priv->connected = TRUE;
        priv->reconnect_attempts = 0;
        priv->last_receive_time = gst_util_get_timestamp();
        
        return TRUE;
      } else {
        GST_WARNING_OBJECT (self, "Found sender info but failed to connect to texture");
      }
    } else {
      GST_WARNING_OBJECT (self, "SetReceiverName to '%s' but couldn't get sender info", senderName);
    }
  } else {
    /* Try to connect to the active sender */
    GST_DEBUG_OBJECT (self, "No sender name specified, trying to connect to active sender");
    
    /* First release any existing receiver connection */
    priv->spout->ReleaseReceiver();
    
    /* Try to receive a texture - this should connect to the active sender */
    ID3D11Texture2D* texture = NULL;
    if (priv->spout->ReceiveTexture(&texture)) {
      const char* sender_name = priv->spout->GetSenderName();
      GST_INFO_OBJECT (self, "Successfully connected to active sender '%s'", sender_name);
      
      /* Store the connected sender name */
      priv->connected_sender_name = sender_name ? sender_name : "";
      
      /* Get the sender width and height */
      unsigned int width = priv->spout->GetSenderWidth();
      unsigned int height = priv->spout->GetSenderHeight();
      DXGI_FORMAT format = priv->spout->GetSenderFormat();
      
      /* Store the format */
      priv->format = format;
      
      /* Get or create caps based on sender info */
      GstVideoFormat video_format = gst_spout_src_dxgi_format_to_gst(format);
      if (video_format == GST_VIDEO_FORMAT_UNKNOWN) {
        GST_WARNING_OBJECT (self, "Unsupported DXGI format %d, falling back to BGRA", format);
        video_format = GST_VIDEO_FORMAT_BGRA;
      }
      
      /* Get the sender frame rate if available */
      double fps = priv->spout->GetSenderFps();
      if (fps <= 0.0 || fps > 1000.0) {
        /* If the sender doesn't provide a valid framerate, use our default */
        fps = DEFAULT_FRAMERATE;
      }
      priv->current_fps = fps;
      
      GST_DEBUG_OBJECT (self, "Using sender framerate: %.2f fps", fps);
      
      /* Set up the video info with the framerate */
      gst_video_info_set_format(&priv->video_info, video_format, width, height);
      priv->video_info.fps_n = (int)(fps * 1000);
      priv->video_info.fps_d = 1000; /* Using 1000 as denominator for better precision */
      
      /* Create caps from video info and make them writable */
      GstCaps *new_caps = gst_video_info_to_caps(&priv->video_info);
      new_caps = gst_caps_make_writable(new_caps);
      
      /* Add D3D11 memory feature to the writable caps */
      GstCapsFeatures *features = gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, NULL);
      gst_caps_set_features(new_caps, 0, features);
      
      /* Replace the existing caps with the new one */
      gst_caps_replace(&priv->caps, new_caps);
      gst_caps_unref(new_caps);
      
      GST_DEBUG_OBJECT (self, "Created caps %" GST_PTR_FORMAT, priv->caps);
      
      /* Clean up the texture we just received */
      if (texture) {
        texture->Release();
      }
      
      /* Mark as connected and update last receive time */
      priv->connected = TRUE;
      priv->reconnect_attempts = 0;
      priv->last_receive_time = gst_util_get_timestamp();
      
      return TRUE;
    } else {
      GST_WARNING_OBJECT (self, "Failed to connect to active sender");
    }
  }
  
  /* If we're here, connection failed */
  GST_WARNING_OBJECT (self, "Failed to connect to any Spout sender (%d attempts)", 
                     ++priv->reconnect_attempts);
  
  /* If this is our first few attempts, not a critical error */
  if (priv->reconnect_attempts < 5) {
    return TRUE;
  }
  
  return FALSE;
}

static gboolean
gst_spout_src_start (GstBaseSrc * src)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "start");

  /* Ensure we have a D3D11 device */
  if (!gst_d3d11_ensure_element_data (GST_ELEMENT_CAST (self),
          priv->adapter, &priv->device)) {
    GST_ERROR_OBJECT (self, "Failed to get D3D11 device");
    return FALSE;
  }

  /* Connect to Spout */
  if (!gst_spout_src_connect (self)) {
    GST_ERROR_OBJECT (self, "Failed to connect to Spout");
    /* Do not fail here - we'll retry in the create function */
  }

  /* Reset frame count and timing */
  priv->frame_number = 0;
  priv->prev_pts = GST_CLOCK_TIME_NONE;
  priv->first_frame = TRUE;
  priv->reconnect_attempts = 0;
  priv->last_receive_time = GST_CLOCK_TIME_NONE;

  return TRUE;
}

static gboolean
gst_spout_src_stop (GstBaseSrc * src)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  
  GST_DEBUG_OBJECT (self, "stop");
  
  /* Clean up texture resources */
  if (priv->texture_srv) {
    priv->texture_srv->Release();
    priv->texture_srv = nullptr;
  }
  
  if (priv->shared_texture) {
    priv->shared_texture->Release();
    priv->shared_texture = nullptr;
  }

  /* Release Spout resources */
  if (priv->spout) {
    priv->spout->ReleaseReceiver();
    priv->spout->CloseDirectX11();
    delete priv->spout;
    priv->spout = nullptr;
  }
  
  /* Release D3D11 device */
  gst_clear_object(&priv->device);
  
  /* Release buffer pool */
  if (priv->pool) {
    gst_buffer_pool_set_active(priv->pool, FALSE);
    gst_object_unref(priv->pool);
    priv->pool = nullptr;
  }
  
  /* Clear caps */
  gst_clear_caps(&priv->caps);
  
  /* Reset connection state */
  priv->connected = FALSE;
  priv->first_frame = TRUE;
  priv->reconnect_attempts = 0;
  priv->connected_sender_name.clear();

  return TRUE;
}

static gboolean
gst_spout_src_unlock (GstBaseSrc * src)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  std::lock_guard<std::mutex> lock(priv->lock);

  GST_DEBUG_OBJECT (self, "unlock");
  priv->flushing = TRUE;

  return TRUE;
}

static gboolean
gst_spout_src_unlock_stop (GstBaseSrc * src)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  std::lock_guard<std::mutex> lock(priv->lock);

  GST_DEBUG_OBJECT (self, "unlock_stop");
  priv->flushing = FALSE;

  return TRUE;
}

static gboolean
gst_spout_src_query (GstBaseSrc * src, GstQuery * query)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY: {
      std::lock_guard<std::mutex> lock(priv->lock);
      
      /* Report latency based on processing deadline */
      if (GST_CLOCK_TIME_IS_VALID (priv->processing_deadline)) {
        gst_query_set_latency (query, TRUE, priv->processing_deadline,
            GST_CLOCK_TIME_NONE);
      } else {
        gst_query_set_latency (query, TRUE, 0, 0);
      }
      
      ret = TRUE;
      break;
    }
    case GST_QUERY_CONTEXT:
      /* Handle D3D11 context query */
      ret = gst_d3d11_handle_context_query (GST_ELEMENT (self), query,
          priv->device);
      if (ret)
        break;
      
      /* Fall through for other context types */
    default:
      ret = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }

  return ret;
}

static GstCaps *
gst_spout_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  GstCaps *caps = NULL;

  GST_DEBUG_OBJECT (self, "get_caps");

  std::unique_lock<std::mutex> lock(priv->lock);
  
  /* If we're connected to a sender, return its caps */
  if (priv->caps) {
    caps = gst_caps_ref (priv->caps);
  } else {
    /* Otherwise return template caps */
    caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
  }
  
  lock.unlock();

  /* Apply filter if specified */
  if (filter) {
    GstCaps *intersection;
    intersection = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }

  GST_DEBUG_OBJECT (self, "Returning caps: %" GST_PTR_FORMAT, caps);
  return caps;
}

static GstCaps *
gst_spout_src_fixate (GstBaseSrc * src, GstCaps * caps)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  GstStructure *s;
  gint width, height;

  GST_DEBUG_OBJECT (self, "fixate: %" GST_PTR_FORMAT, caps);

  /* Make caps writable */
  caps = gst_caps_make_writable (caps);

  /* If we already know dimensions from a connected Spout stream, use those */
  std::lock_guard<std::mutex> lock(priv->lock);
  if (priv->connected && priv->spout) {
    width = priv->spout->GetSenderWidth();
    height = priv->spout->GetSenderHeight();
    
    if (width <= 0 || height <= 0) {
      /* Try to get width/height from video info */
      width = GST_VIDEO_INFO_WIDTH(&priv->video_info);
      height = GST_VIDEO_INFO_HEIGHT(&priv->video_info);
    }
  } else {
    /* Default size if not connected yet */
    width = 640;
    height = 480;
  }
  
  /* Make sure we have valid dimensions */
  if (width <= 0 || height <= 0) {
    width = 640;
    height = 480;
  }

  /* Get framerate from connected sender or use default */
  double fps = priv->current_fps;
  if (fps <= 0.0 || fps > 1000.0) {
    fps = DEFAULT_FRAMERATE;
  }

  /* For each structure in caps, fixate dimensions and framerate */
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    s = gst_caps_get_structure (caps, i);
    
    /* Fixate to either the connected dimensions or default */
    gst_structure_fixate_field_nearest_int (s, "width", width);
    gst_structure_fixate_field_nearest_int (s, "height", height);
    
    /* Fixate framerate using the sender's fps or default */
    if (gst_structure_has_field (s, "framerate")) {
      gst_structure_fixate_field_nearest_fraction (s, "framerate", 
                                                 (int)(fps + 0.5), 1);
    } else {
      /* Add framerate if not present */
      gst_structure_set (s, "framerate", GST_TYPE_FRACTION, (int)(fps + 0.5), 1, NULL);
    }
  }

  /* Let parent class handle the rest */
  return gst_caps_fixate (caps);
}

static gboolean
gst_spout_src_decide_allocation (GstBaseSrc * src, GstQuery * query)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  GstBufferPool *pool = NULL;
  GstCaps *caps;
  GstVideoInfo info;
  guint size, min, max;
  gboolean update_pool = FALSE;
  GstStructure *config;

  /* Get negotiated caps from the query */
  gst_query_parse_allocation (query, &caps, NULL);
  if (!caps) {
    GST_ERROR_OBJECT (self, "No caps in allocation query");
    return FALSE;
  }

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_ERROR_OBJECT (self, "Failed to parse caps into video info");
    return FALSE;
  }

  /* Calculate buffer size from video dimensions */
  size = GST_VIDEO_INFO_SIZE (&info);

  /* Parse existing pool parameters */
  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    update_pool = TRUE;
  } else {
    /* Suggest default values if no pool present */
    min = 2;
    max = 0;
  }

  /* If downstream doesn't provide a pool, create a D3D11 buffer pool */
  if (!pool) {
    GST_DEBUG_OBJECT (self, "Creating new D3D11 buffer pool");
    /* Create a D3D11 buffer pool that will allocate D3D11 memory buffers */
    pool = gst_d3d11_buffer_pool_new (priv->device);
    if (!pool) {
      GST_ERROR_OBJECT (self, "Failed to create D3D11 buffer pool");
      return FALSE;
    }
  }

  /* Configure the pool */
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min, max);
  
  /* Enable video meta for stride information */
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  
  /* Apply configuration */
  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (self, "Failed to set buffer pool config");
    gst_object_unref (pool);
    return FALSE;
  }

  /* Update the query with our pool */
  if (update_pool)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  /* Store pool for our use */
  if (priv->pool)
    gst_object_unref (priv->pool);
  priv->pool = pool;
  gst_object_ref (priv->pool);

  /* Activate the pool */
  if (!gst_buffer_pool_set_active (priv->pool, TRUE)) {
    GST_ERROR_OBJECT (self, "Failed to activate buffer pool");
    return FALSE;
  }

  return TRUE;
}

/* Helper function to copy DX texture to GStreamer buffer */
static GstFlowReturn
gst_spout_src_copy_texture_to_buffer (GstSpoutSrc * self, GstBuffer * buffer)
{
  GstSpoutSrcPrivate *priv = self->priv;
  GstMemory *mem;
  GstD3D11Memory *dmem;
  ID3D11Texture2D *texture = NULL;
  ID3D11DeviceContext *context = NULL;
  gboolean was_connected = FALSE;
  const char* sender_name = NULL;
  
  /* Check connection state before starting */
  {
    std::lock_guard<std::mutex> lock(priv->lock);
    was_connected = priv->connected && priv->spout;
    sender_name = priv->connected_sender_name.c_str();
    
    GST_LOG_OBJECT (self, "Connection state: connected=%d, sender=%s, spout=%p", 
                    priv->connected, sender_name, priv->spout);
  }
  
  if (!was_connected) {
    GST_INFO_OBJECT (self, "Not connected before copy attempt, trying to connect");
    if (!gst_spout_src_connect (self)) {
      GST_WARNING_OBJECT (self, "Failed to connect before texture copy");
      return GST_FLOW_ERROR;
    }
  }
  
  /* Get D3D11 memory from the buffer */
  mem = gst_buffer_peek_memory (buffer, 0);
  if (!gst_is_d3d11_memory (mem)) {
    GST_ERROR_OBJECT (self, "Not a D3D11 memory");
    return GST_FLOW_ERROR;
  }
  
  dmem = GST_D3D11_MEMORY_CAST (mem);
  texture = (ID3D11Texture2D *) gst_d3d11_memory_get_resource_handle (dmem);
  if (!texture) {
    GST_ERROR_OBJECT (self, "Failed to get D3D11 texture from memory");
    return GST_FLOW_ERROR;
  }
  
  /* Get the device context */
  ID3D11Device *device = gst_d3d11_device_get_device_handle (priv->device);
  if (!device) {
    GST_ERROR_OBJECT (self, "Failed to get D3D11 device handle");
    return GST_FLOW_ERROR;
  }
  device->GetImmediateContext(&context);
  
  GST_LOG_OBJECT (self, "Attempting to receive texture from Spout to texture %p", texture);
  
  /* If we're forcing reconnection on each frame, do it now */
  if (priv->force_reconnect) {
    if (!priv->sender_name.empty()) {
      priv->spout->SetReceiverName(priv->sender_name.c_str());
    } else {
      priv->spout->ReleaseReceiver();
    }
  }
  
  /* Use Spout to receive texture directly to our buffer's texture */
  bool spout_result = priv->spout->ReceiveTexture(&texture);
  
  if (!spout_result) {
    GST_WARNING_OBJECT (self, "Failed to receive texture from Spout");
    if (context) context->Release();
    
    /* Try to reconnect if connection was lost */
    {
      std::lock_guard<std::mutex> lock(priv->lock);
      
      GST_WARNING_OBJECT (self, "Lost connection to Spout sender '%s', attempting to reconnect",
                          priv->connected_sender_name.c_str());
      priv->connected = FALSE;
    }
    
    gst_spout_src_disconnect(self);
    gst_spout_src_connect(self);
    
    return GST_FLOW_ERROR;
  }
  
  /* Update last receive time */
  {
    std::lock_guard<std::mutex> lock(priv->lock);
    priv->last_receive_time = gst_util_get_timestamp();
  }
  
  GST_LOG_OBJECT (self, "Successfully received texture from Spout");
  
  /* Set or update caps based on the sender if needed */
  if (priv->spout->IsUpdated() || priv->first_frame) {
    std::unique_lock<std::mutex> lock(priv->lock);
    
    unsigned int width = priv->spout->GetSenderWidth();
    unsigned int height = priv->spout->GetSenderHeight();
    DXGI_FORMAT format = priv->spout->GetSenderFormat();
    
    /* Update connected sender name */
    sender_name = priv->spout->GetSenderName();
    if (sender_name) {
      priv->connected_sender_name = sender_name;
    }
    
    GST_DEBUG_OBJECT (self, "Updating caps from sender '%s': %dx%d format=%d", 
                      priv->connected_sender_name.c_str(), width, height, format);
    
    /* Get the sender frame rate if available */
    double fps = priv->spout->GetSenderFps();
    if (fps <= 0.0 || fps > 1000.0) {
      /* If the sender doesn't provide a valid framerate, use our default */
      fps = DEFAULT_FRAMERATE;
    }
    priv->current_fps = fps;
    
    GST_DEBUG_OBJECT (self, "Using sender framerate: %.2f fps", fps);
    
    /* Update our caps */
    GstVideoFormat video_format = gst_spout_src_dxgi_format_to_gst(format);
    if (video_format == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_WARNING_OBJECT (self, "Unsupported DXGI format %d, falling back to BGRA", format);
      video_format = GST_VIDEO_FORMAT_BGRA;
    }
    
    /* Set up the video info with framerate */
    gst_video_info_set_format(&priv->video_info, video_format, width, height);
    priv->video_info.fps_n = (int)(fps * 1000);
    priv->video_info.fps_d = 1000; /* Using 1000 as denominator for better precision */
    
    /* Create caps from video info and make them writable */
    GstCaps *new_caps = gst_video_info_to_caps(&priv->video_info);
    new_caps = gst_caps_make_writable(new_caps);
    
    /* Add D3D11 memory feature to the writable caps */
    GstCapsFeatures *features = gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, NULL);
    gst_caps_set_features(new_caps, 0, features);
    
    /* Replace the existing caps with the new one */
    gst_caps_replace(&priv->caps, new_caps);
    gst_caps_unref(new_caps);
    
    GST_DEBUG_OBJECT (self, "Updated caps: %" GST_PTR_FORMAT, priv->caps);
    
    /* Set the caps on the source pad */
    GstCaps *current_caps = gst_caps_ref(priv->caps);
    lock.unlock();
    
    gst_base_src_set_caps(GST_BASE_SRC(self), current_caps);
    gst_caps_unref(current_caps);
    
    /* Note: we've released the lock before calling set_caps to avoid deadlocks */
    lock.lock();
    priv->first_frame = FALSE;
    priv->connected = TRUE;
  }
  
  /* Release device context */
  if (context) {
    context->Release();
  }
  
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_spout_src_create (GstBaseSrc * src, guint64 offset, guint size,
    GstBuffer ** buf)
{
  GstSpoutSrc *self = GST_SPOUT_SRC (src);
  GstSpoutSrcPrivate *priv = self->priv;
  GstFlowReturn ret;
  GstClock *clock;
  GstClockTime base_time, clock_time, timestamp;
  gboolean connected = FALSE;
  GstBuffer *buffer = NULL;
  
  /* Check if we're flushing */
  {
    std::lock_guard<std::mutex> lock(priv->lock);
    if (priv->flushing) {
      GST_DEBUG_OBJECT (self, "Flushing, returning FLUSHING");
      return GST_FLOW_FLUSHING;
    }
  }
  
  /* Ensure we're connected to a Spout sender */
  {
    std::unique_lock<std::mutex> lock(priv->lock);
    connected = priv->connected && priv->spout;
    
    GST_LOG_OBJECT (self, "Connection status check: connected=%d spout=%p", 
                    priv->connected, priv->spout);
    
    /* If we just connected, update downstream with the new caps */
    if (connected && priv->caps) {
      const char* sender_name = priv->connected_sender_name.c_str();
      GstCaps *current_caps = gst_caps_ref(priv->caps);
      lock.unlock();
      
      GST_DEBUG_OBJECT (self, "Connected to sender '%s', setting caps: %" GST_PTR_FORMAT,
                        sender_name, current_caps);
      
      gst_base_src_set_caps(src, current_caps);
      gst_caps_unref(current_caps);
    }
    else {
      lock.unlock();
    }
  }
  
  if (!connected) {
    /* Try to connect or reconnect */
    gboolean result = gst_spout_src_connect (self);
    
    /* Re-check connection state after connection attempt */
    {
      std::lock_guard<std::mutex> lock(priv->lock);
      connected = priv->connected && priv->spout;
      
      GST_LOG_OBJECT (self, "Connection after connect attempt: connected=%d spout=%p", 
                     priv->connected, priv->spout);
    }
    
    if (!result && priv->reconnect_attempts >= 5) {
      GST_ERROR_OBJECT (self, "Failed to connect to Spout after multiple attempts");
      return GST_FLOW_ERROR;
    }
    
    /* Check if connection succeeded */
    if (!connected) {
      /* Wait a short time and provide a dummy black frame instead of returning empty-handed */
      GST_INFO_OBJECT (self, "No Spout sender available, waiting...");
      g_usleep(priv->wait_timeout * 1000); // Convert ms to µs
      
      if (!priv->pool) {
        GST_DEBUG_OBJECT (self, "No buffer pool available yet, deferring");
        return GST_FLOW_OK;  // Try again next time
      }
      
      /* Acquire a buffer from the pool */
      ret = gst_buffer_pool_acquire_buffer(priv->pool, &buffer, NULL);
      if (ret != GST_FLOW_OK) {
        GST_WARNING_OBJECT(self, "Failed to acquire buffer: %s", gst_flow_get_name(ret));
        return GST_FLOW_OK;  // Try again next time
      }
      
      /* Initialize buffer to black */
      GstMapInfo map;
      if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memset(map.data, 0, map.size);
        gst_buffer_unmap(buffer, &map);
      }
      
      /* Set timestamps for the dummy buffer */
      clock = gst_element_get_clock(GST_ELEMENT_CAST(self));
      if (clock) {
        clock_time = gst_clock_get_time(clock);
        base_time = GST_ELEMENT_CAST(self)->base_time;
        gst_object_unref(clock);
        
        if (clock_time > base_time)
          timestamp = clock_time - base_time;
        else
          timestamp = 0;
          
        GST_BUFFER_TIMESTAMP(buffer) = timestamp;
        
        /* Calculate duration based on current fps */
        double fps = DEFAULT_FRAMERATE;
        {
          std::lock_guard<std::mutex> lock(priv->lock);
          fps = priv->current_fps > 0 ? priv->current_fps : DEFAULT_FRAMERATE;
        }
        
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND, (int)fps);
      }
      
      *buf = buffer;
      return GST_FLOW_OK;
    }
  }
  
  /* Set caps if we haven't done so yet */
  {
    std::unique_lock<std::mutex> lock(priv->lock);
    if (priv->caps) {
      const char* sender_name = priv->connected_sender_name.c_str();
      GST_LOG_OBJECT (self, "Setting caps for sender '%s'", sender_name);
      
      GstCaps *current_caps = gst_caps_ref(priv->caps);
      lock.unlock();
      
      gst_base_src_set_caps(src, current_caps);
      gst_caps_unref(current_caps);
    }
    else {
      lock.unlock();
    }
  }
  
  /* Get a buffer from our pool */
  ret = gst_buffer_pool_acquire_buffer(priv->pool, &buffer, NULL);
  if (ret != GST_FLOW_OK) {
    GST_ERROR_OBJECT (self, "Failed to acquire buffer from pool: %s",
        gst_flow_get_name (ret));
    return ret;
  }
  
  /* Receive texture from Spout */
  ret = gst_spout_src_copy_texture_to_buffer(self, buffer);
  if (ret != GST_FLOW_OK) {
    gst_buffer_unref(buffer);
    GST_WARNING_OBJECT (self, "Failed to copy texture to buffer");
    
    /* Not a fatal error - we'll retry on the next frame */
    return GST_FLOW_OK;  // Try again next time
  }
  
  /* Set buffer timestamp */
  clock = gst_element_get_clock(GST_ELEMENT_CAST(self));
  if (clock) {
    clock_time = gst_clock_get_time(clock);
    base_time = GST_ELEMENT_CAST(self)->base_time;
    gst_object_unref(clock);
    
    if (clock_time > base_time)
      timestamp = clock_time - base_time;
    else
      timestamp = 0;
    
    GST_BUFFER_TIMESTAMP(buffer) = timestamp;
    
    /* Calculate duration if we have a previous timestamp */
    {
      std::lock_guard<std::mutex> lock(priv->lock);
      
      /* Get current fps for duration calculation */
      double fps = priv->current_fps > 0 ? priv->current_fps : DEFAULT_FRAMERATE;
      GstClockTime frame_duration = gst_util_uint64_scale_int(1, GST_SECOND, (int)fps);
      
      if (GST_CLOCK_TIME_IS_VALID(priv->prev_pts)) {
        /* Avoid potential underflow if timestamps are irregular */
        if (timestamp > priv->prev_pts) {
          GST_BUFFER_DURATION(buffer) = timestamp - priv->prev_pts;
        } else {
          /* Use calculated frame duration */
          GST_BUFFER_DURATION(buffer) = frame_duration;
        }
      } else {
        /* Use calculated frame duration */
        GST_BUFFER_DURATION(buffer) = frame_duration;
      }
      
      priv->prev_pts = timestamp;
      
      /* Set frame count */
      GST_BUFFER_OFFSET(buffer) = priv->frame_number++;
    }
  }
  
  *buf = buffer;
  return GST_FLOW_OK;
}

/* Plugin entry point */
static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "spoutsrc", GST_RANK_NONE,
      GST_TYPE_SPOUT_SRC);
}

/* Register the plugin with GStreamer. */
GST_PLUGIN_DEFINE(
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  spoutsrc,
  "Overon SpoutDX Source",
  plugin_init,
  "1.0.0",
  "GPL",
  PACKAGE,
  "https://www.overon.es"
)