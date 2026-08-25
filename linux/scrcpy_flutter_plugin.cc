#include "include/scrcpy_flutter_plugin/scrcpy_flutter_plugin.h"
#include "scrcpy_video_texture.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <map>
#include <memory>

#define SCRCPY_FLUTTER_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), scrcpy_flutter_plugin_get_type(), \
                              ScrcpyFlutterPlugin))

struct _ScrcpyFlutterPlugin {
  GObject parent_instance;
  FlPluginRegistrar* registrar;
  FlMethodChannel* channel;
  std::map<int64_t, ScrcpyVideoTexture*> textures;
};

G_DEFINE_TYPE(ScrcpyFlutterPlugin, scrcpy_flutter_plugin, g_object_get_type())

static FlMethodResponse* create_texture(ScrcpyFlutterPlugin* self) {
  FlTextureRegistrar* texture_registrar =
      fl_plugin_registrar_get_texture_registrar(self->registrar);

  ScrcpyVideoTexture* video_texture = scrcpy_video_texture_new();
  
  if (!fl_texture_registrar_register_texture(texture_registrar, FL_TEXTURE(video_texture))) {
    g_object_unref(video_texture);
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "TEXTURE_REGISTRATION_FAILED", "Failed to register texture with registrar", nullptr));
  }

  int64_t texture_id = fl_texture_get_id(FL_TEXTURE(video_texture));
  self->textures[texture_id] = video_texture;

  return FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_int(texture_id)));
}

static FlMethodResponse* dispose_texture(ScrcpyFlutterPlugin* self, FlValue* args) {
  if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Missing arguments map", nullptr));
  }

  FlValue* val = fl_value_lookup_string(args, "textureId");
  if (!val || fl_value_get_type(val) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Missing 'textureId' argument", nullptr));
  }
  int64_t texture_id = fl_value_get_int(val);

  auto it = self->textures.find(texture_id);
  if (it != self->textures.end()) {
    FlTextureRegistrar* texture_registrar =
        fl_plugin_registrar_get_texture_registrar(self->registrar);
    fl_texture_registrar_unregister_texture(texture_registrar, FL_TEXTURE(it->second));
    g_object_unref(it->second);
    self->textures.erase(it);
  }

  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static FlMethodResponse* set_texture_handle(ScrcpyFlutterPlugin* self, FlValue* args) {
  if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Missing arguments map", nullptr));
  }

  FlValue* val_id = fl_value_lookup_string(args, "textureId");
  FlValue* val_handle = fl_value_lookup_string(args, "handle");
  if (!val_id || !val_handle || 
      fl_value_get_type(val_id) != FL_VALUE_TYPE_INT || 
      fl_value_get_type(val_handle) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Missing 'textureId' or 'handle' argument", nullptr));
  }

  int64_t texture_id = fl_value_get_int(val_id);
  int64_t handle_addr = fl_value_get_int(val_handle);

  auto it = self->textures.find(texture_id);
  if (it != self->textures.end()) {
    scrcpy_video_texture_set_handle(it->second, reinterpret_cast<void*>(handle_addr));
  }

  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static FlMethodResponse* notify_frame_available(ScrcpyFlutterPlugin* self, FlValue* args) {
  if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Missing arguments map", nullptr));
  }

  FlValue* val = fl_value_lookup_string(args, "textureId");
  if (!val || fl_value_get_type(val) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Missing 'textureId' argument", nullptr));
  }
  int64_t texture_id = fl_value_get_int(val);

  auto it = self->textures.find(texture_id);
  if (it != self->textures.end()) {
    FlTextureRegistrar* texture_registrar =
        fl_plugin_registrar_get_texture_registrar(self->registrar);
    fl_texture_registrar_mark_texture_frame_available(texture_registrar, FL_TEXTURE(it->second));
  }

  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static void scrcpy_flutter_plugin_handle_method_call(
    ScrcpyFlutterPlugin* self,
    FlMethodCall* method_call) {
  
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;

  if (strcmp(method, "createTexture") == 0) {
    response = create_texture(self);
  } else if (strcmp(method, "disposeTexture") == 0) {
    response = dispose_texture(self, args);
  } else if (strcmp(method, "setTextureHandle") == 0) {
    response = set_texture_handle(self, args);
  } else if (strcmp(method, "notifyFrameAvailable") == 0) {
    response = notify_frame_available(self, args);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

static void scrcpy_flutter_plugin_dispose(GObject* object) {
  ScrcpyFlutterPlugin* self = SCRCPY_FLUTTER_PLUGIN(object);

  // Clean up remaining textures
  FlTextureRegistrar* texture_registrar =
      fl_plugin_registrar_get_texture_registrar(self->registrar);
  for (auto const& [texture_id, texture] : self->textures) {
    fl_texture_registrar_unregister_texture(texture_registrar, FL_TEXTURE(texture));
    g_object_unref(texture);
  }
  self->textures.clear();

  g_clear_object(&self->registrar);
  g_clear_object(&self->channel);

  G_OBJECT_CLASS(scrcpy_flutter_plugin_parent_class)->dispose(object);
}

static void scrcpy_flutter_plugin_class_init(ScrcpyFlutterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = scrcpy_flutter_plugin_dispose;
}

static void scrcpy_flutter_plugin_init(ScrcpyFlutterPlugin* self) {
  self->registrar = nullptr;
  self->channel = nullptr;
}

void scrcpy_flutter_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  ScrcpyFlutterPlugin* plugin = SCRCPY_FLUTTER_PLUGIN(
      g_object_new(scrcpy_flutter_plugin_get_type(), nullptr));

  plugin->registrar = FL_PLUGIN_REGISTRAR(g_object_ref(registrar));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  plugin->channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar), "scrcpy_flutter_plugin",
      FL_METHOD_CODEC(codec));

  fl_method_channel_set_method_call_handler(
      plugin->channel,
      [](FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data) {
        scrcpy_flutter_plugin_handle_method_call(SCRCPY_FLUTTER_PLUGIN(user_data), method_call);
      },
      plugin, nullptr);

  g_object_unref(plugin);
}
