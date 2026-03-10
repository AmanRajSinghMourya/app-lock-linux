#include "fingerprint_channel.h"

#include <flutter_linux/flutter_linux.h>
#include <gio/gio.h>

#include <cstring>

namespace {

constexpr char kFingerprintChannelName[] = "app_lock_linux/fingerprint";
constexpr char kFprintServiceName[] = "net.reactivated.Fprint";
constexpr char kManagerPath[] = "/net/reactivated/Fprint/Manager";
constexpr char kManagerInterface[] = "net.reactivated.Fprint.Manager";
constexpr char kDeviceInterface[] = "net.reactivated.Fprint.Device";

struct AvailabilityRequest {
  FingerprintChannel* owner;
  FlMethodCall* method_call;
  GDBusConnection* connection;
};

struct AuthRequest {
  FingerprintChannel* owner;
  FlMethodCall* method_call;
  GDBusConnection* connection;
  gchar* device_path;
  guint verify_status_subscription_id;
  gboolean claim_done;
  gboolean verify_started;
  gboolean cancel_requested;
  gboolean cleanup_started;
  gboolean result_ready;
  gboolean authenticated;
  gchar* username;
  gchar* result_status;
  gchar* result_message;
};

}  // namespace

struct _FingerprintChannel {
  GObject parent_instance;
  AuthRequest* active_auth_request;
};

G_DEFINE_TYPE(FingerprintChannel, fingerprint_channel, g_object_get_type())

static FlMethodResponse* build_response(gboolean available,
                                        gboolean authenticated,
                                        gboolean cancel_requested,
                                        const gchar* status,
                                        const gchar* message) {
  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "available", fl_value_new_bool(available));
  fl_value_set_string_take(result, "authenticated",
                           fl_value_new_bool(authenticated));
  fl_value_set_string_take(result, "cancelRequested",
                           fl_value_new_bool(cancel_requested));
  fl_value_set_string_take(result, "status", fl_value_new_string(status));
  fl_value_set_string_take(result, "message", fl_value_new_string(message));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void respond(FlMethodCall* method_call, FlMethodResponse* response) {
  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error)) {
    g_warning("Failed to respond on fingerprint channel: %s", error->message);
  }
}

static const gchar* current_username() {
  const gchar* username = g_get_user_name();
  return username != nullptr ? username : "";
}

static gchar* describe_fprint_error(GError* error,
                                    const gchar* fallback_message) {
  if (error == nullptr) {
    return g_strdup(fallback_message);
  }

  g_autofree gchar* remote_error = g_dbus_error_get_remote_error(error);
  if (remote_error != nullptr) {
    if (g_str_has_suffix(remote_error,
                         ".net.reactivated.Fprint.Error.NoEnrolledPrints") ||
        g_str_has_suffix(remote_error, ".NoEnrolledPrints")) {
      return g_strdup("No fingerprints are enrolled for the current user.");
    }

    if (g_str_has_suffix(remote_error,
                         ".net.reactivated.Fprint.Error.PermissionDenied") ||
        g_str_has_suffix(remote_error, ".PermissionDenied")) {
      return g_strdup(
          "Fingerprint permission was denied by the Linux fingerprint service.");
    }

    if (g_str_has_suffix(remote_error,
                         ".net.reactivated.Fprint.Error.AlreadyInUse") ||
        g_str_has_suffix(remote_error, ".AlreadyInUse")) {
      return g_strdup("Fingerprint reader is already in use.");
    }

    if (g_str_has_suffix(remote_error,
                         ".net.reactivated.Fprint.Error.NoSuchDevice") ||
        g_str_has_suffix(remote_error, ".NoSuchDevice")) {
      return g_strdup("No fingerprint reader is available.");
    }

    if (g_str_has_suffix(remote_error,
                         ".net.reactivated.Fprint.Error.ClaimDevice") ||
        g_str_has_suffix(remote_error, ".ClaimDevice")) {
      return g_strdup("Fingerprint reader could not be claimed.");
    }
  }

  return g_strdup_printf("%s: %s", fallback_message, error->message);
}

static void free_availability_request(AvailabilityRequest* request) {
  if (request == nullptr) {
    return;
  }

  if (request->owner != nullptr) {
    g_object_unref(request->owner);
  }
  if (request->method_call != nullptr) {
    g_object_unref(request->method_call);
  }
  if (request->connection != nullptr) {
    g_object_unref(request->connection);
  }
  delete request;
}

static void complete_availability_request(AvailabilityRequest* request,
                                          gboolean available,
                                          const gchar* status,
                                          const gchar* message) {
  g_autoptr(FlMethodResponse) response =
      build_response(available, FALSE, FALSE, status, message);
  respond(request->method_call, response);
  free_availability_request(request);
}

static void auth_request_clear_from_owner(AuthRequest* request) {
  if (request->owner != nullptr &&
      request->owner->active_auth_request == request) {
    request->owner->active_auth_request = nullptr;
  }
}

static void auth_request_unsubscribe(AuthRequest* request) {
  if (request->connection != nullptr && request->verify_status_subscription_id != 0) {
    g_dbus_connection_signal_unsubscribe(request->connection,
                                         request->verify_status_subscription_id);
    request->verify_status_subscription_id = 0;
  }
}

static void free_auth_request(AuthRequest* request) {
  if (request == nullptr) {
    return;
  }

  auth_request_unsubscribe(request);
  auth_request_clear_from_owner(request);
  if (request->owner != nullptr) {
    g_object_unref(request->owner);
  }
  if (request->method_call != nullptr) {
    g_object_unref(request->method_call);
  }
  if (request->connection != nullptr) {
    g_object_unref(request->connection);
  }
  g_free(request->device_path);
  g_free(request->username);
  g_free(request->result_status);
  g_free(request->result_message);
  delete request;
}

static void set_auth_result(AuthRequest* request, gboolean authenticated,
                            const gchar* status, const gchar* message) {
  if (request->result_ready) {
    return;
  }

  request->result_ready = TRUE;
  request->authenticated = authenticated;
  g_free(request->result_status);
  g_free(request->result_message);
  request->result_status = g_strdup(status);
  request->result_message = g_strdup(message);
}

static void complete_auth_request(AuthRequest* request) {
  const gchar* status =
      request->result_status == nullptr ? "error" : request->result_status;
  const gchar* message = request->result_message == nullptr
                             ? "Fingerprint authentication failed."
                             : request->result_message;
  g_autoptr(FlMethodResponse) response = build_response(
      FALSE, request->authenticated, request->cancel_requested, status, message);
  respond(request->method_call, response);
  free_auth_request(request);
}

static void start_auth_release(AuthRequest* request);

static void on_auth_release_finished(GObject* object, GAsyncResult* result,
                                     gpointer user_data) {
  (void)object;
  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) value =
      g_dbus_connection_call_finish(request->connection, result, &error);
  (void)value;
  request->claim_done = FALSE;
  complete_auth_request(request);
}

static void start_auth_release(AuthRequest* request) {
  if (!request->claim_done || request->connection == nullptr ||
      request->device_path == nullptr) {
    complete_auth_request(request);
    return;
  }

  g_dbus_connection_call(request->connection, kFprintServiceName,
                         request->device_path, kDeviceInterface, "Release",
                         nullptr, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE,
                         -1, nullptr, on_auth_release_finished, request);
}

static void on_auth_verify_stop_finished(GObject* object, GAsyncResult* result,
                                         gpointer user_data) {
  (void)object;
  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) value =
      g_dbus_connection_call_finish(request->connection, result, &error);
  (void)value;
  request->verify_started = FALSE;
  start_auth_release(request);
}

static void begin_auth_cleanup(AuthRequest* request) {
  if (request->cleanup_started) {
    return;
  }

  request->cleanup_started = TRUE;
  auth_request_unsubscribe(request);

  if (request->verify_started && request->connection != nullptr &&
      request->device_path != nullptr) {
    g_dbus_connection_call(request->connection, kFprintServiceName,
                           request->device_path, kDeviceInterface, "VerifyStop",
                           nullptr, G_VARIANT_TYPE_UNIT,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           on_auth_verify_stop_finished, request);
    return;
  }

  start_auth_release(request);
}

static void on_verify_status_signal(GDBusConnection* connection,
                                    const gchar* sender_name,
                                    const gchar* object_path,
                                    const gchar* interface_name,
                                    const gchar* signal_name,
                                    GVariant* parameters, gpointer user_data) {
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;

  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  if (request->cleanup_started || request->result_ready) {
    return;
  }

  const gchar* status = nullptr;
  gboolean done = FALSE;
  g_variant_get(parameters, "(&sb)", &status, &done);
  if (!done) {
    return;
  }

  if (request->cancel_requested) {
    set_auth_result(request, FALSE, "cancelled",
                    "Fingerprint verification was cancelled.");
  } else if (g_strcmp0(status, "verify-match") == 0) {
    set_auth_result(request, TRUE, "authenticated",
                    "Fingerprint verified successfully.");
  } else if (g_strcmp0(status, "verify-no-match") == 0) {
    set_auth_result(request, FALSE, "failed", "Fingerprint did not match.");
  } else if (g_strcmp0(status, "verify-disconnected") == 0) {
    set_auth_result(request, FALSE, "failed",
                    "Fingerprint reader was disconnected.");
  } else {
    g_autofree gchar* message =
        g_strdup_printf("Fingerprint verification failed: %s.", status);
    set_auth_result(request, FALSE, "failed", message);
  }

  begin_auth_cleanup(request);
}

static void on_auth_verify_start_finished(GObject* object, GAsyncResult* result,
                                          gpointer user_data) {
  (void)object;
  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) value =
      g_dbus_connection_call_finish(request->connection, result, &error);
  (void)value;

  if (error != nullptr) {
    g_autofree gchar* message = describe_fprint_error(
        error, "Failed to start fingerprint verification");
    set_auth_result(request, FALSE, "failed", message);
    begin_auth_cleanup(request);
    return;
  }

  request->verify_started = TRUE;
  if (request->cancel_requested) {
    set_auth_result(request, FALSE, "cancelled",
                    "Fingerprint verification was cancelled.");
    begin_auth_cleanup(request);
  }
}

static void on_auth_claim_finished(GObject* object, GAsyncResult* result,
                                   gpointer user_data) {
  (void)object;
  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) value =
      g_dbus_connection_call_finish(request->connection, result, &error);
  (void)value;

  if (error != nullptr) {
    g_autofree gchar* message =
        describe_fprint_error(error, "Failed to claim the fingerprint device");
    set_auth_result(request, FALSE, "failed", message);
    begin_auth_cleanup(request);
    return;
  }

  request->claim_done = TRUE;
  if (request->cancel_requested) {
    set_auth_result(request, FALSE, "cancelled",
                    "Fingerprint verification was cancelled.");
    begin_auth_cleanup(request);
    return;
  }

  g_dbus_connection_call(request->connection, kFprintServiceName,
                         request->device_path, kDeviceInterface, "VerifyStart",
                         g_variant_new("(s)", "any"), G_VARIANT_TYPE_UNIT,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                         on_auth_verify_start_finished, request);
}

static void on_auth_default_device_finished(GObject* object,
                                            GAsyncResult* result,
                                            gpointer user_data) {
  (void)object;
  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) value =
      g_dbus_connection_call_finish(request->connection, result, &error);

  if (error != nullptr || value == nullptr) {
    g_autofree gchar* message = describe_fprint_error(
        error, "No fingerprint reader is available");
    set_auth_result(request, FALSE, "unavailable", message);
    begin_auth_cleanup(request);
    return;
  }

  const gchar* device_path = nullptr;
  g_variant_get(value, "(&o)", &device_path);
  request->device_path = g_strdup(device_path);
  request->verify_status_subscription_id = g_dbus_connection_signal_subscribe(
      request->connection, kFprintServiceName, kDeviceInterface, "VerifyStatus",
      request->device_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      on_verify_status_signal, request, nullptr);

  if (request->cancel_requested) {
    set_auth_result(request, FALSE, "cancelled",
                    "Fingerprint verification was cancelled.");
    begin_auth_cleanup(request);
    return;
  }

  g_dbus_connection_call(request->connection, kFprintServiceName,
                         request->device_path, kDeviceInterface, "Claim",
                         g_variant_new("(s)", request->username),
                         G_VARIANT_TYPE_UNIT,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                         on_auth_claim_finished, request);
}

static void on_auth_bus_ready(GObject* object, GAsyncResult* result,
                              gpointer user_data) {
  (void)object;
  AuthRequest* request = static_cast<AuthRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  request->connection = g_bus_get_finish(result, &error);

  if (request->connection == nullptr) {
    g_autofree gchar* message =
        describe_fprint_error(error, "Failed to connect to the system bus");
    set_auth_result(request, FALSE, "failed", message);
    begin_auth_cleanup(request);
    return;
  }

  if (request->cancel_requested) {
    set_auth_result(request, FALSE, "cancelled",
                    "Fingerprint verification was cancelled.");
    begin_auth_cleanup(request);
    return;
  }

  g_dbus_connection_call(request->connection, kFprintServiceName, kManagerPath,
                         kManagerInterface, "GetDefaultDevice", nullptr,
                         G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                         nullptr, on_auth_default_device_finished, request);
}

static void start_authenticate(FingerprintChannel* self,
                               FlMethodCall* method_call) {
  if (self->active_auth_request != nullptr) {
    g_autoptr(FlMethodResponse) response = build_response(
        FALSE, FALSE, FALSE, "busy",
        "A fingerprint verification request is already running.");
    respond(method_call, response);
    return;
  }

  AuthRequest* request = new AuthRequest{
      FINGERPRINT_CHANNEL(g_object_ref(self)),
      FL_METHOD_CALL(g_object_ref(method_call)),
      nullptr,
      nullptr,
      0,
      FALSE,
      FALSE,
      FALSE,
      FALSE,
      FALSE,
      FALSE,
      g_strdup(current_username()),
      nullptr,
      nullptr,
  };
  self->active_auth_request = request;

  g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, on_auth_bus_ready, request);
}

static void on_availability_default_device_finished(GObject* object,
                                                    GAsyncResult* result,
                                                    gpointer user_data) {
  (void)object;
  AvailabilityRequest* request =
      static_cast<AvailabilityRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GVariant) value =
      g_dbus_connection_call_finish(request->connection, result, &error);

  if (error != nullptr || value == nullptr) {
    g_autofree gchar* message =
        describe_fprint_error(error, "No fingerprint reader is available");
    complete_availability_request(request, FALSE, "unavailable", message);
    return;
  }

  complete_availability_request(request, TRUE, "available",
                                "Fingerprint reader is available.");
}

static void on_availability_bus_ready(GObject* object, GAsyncResult* result,
                                      gpointer user_data) {
  (void)object;
  AvailabilityRequest* request =
      static_cast<AvailabilityRequest*>(user_data);
  g_autoptr(GError) error = nullptr;
  request->connection = g_bus_get_finish(result, &error);

  if (request->connection == nullptr) {
    g_autofree gchar* message =
        describe_fprint_error(error, "Failed to connect to the system bus");
    complete_availability_request(request, FALSE, "failed", message);
    return;
  }

  g_dbus_connection_call(request->connection, kFprintServiceName, kManagerPath,
                         kManagerInterface, "GetDefaultDevice", nullptr,
                         G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                         nullptr, on_availability_default_device_finished,
                         request);
}

static void start_can_authenticate(FingerprintChannel* self,
                                   FlMethodCall* method_call) {
  AvailabilityRequest* request = new AvailabilityRequest{
      FINGERPRINT_CHANNEL(g_object_ref(self)),
      FL_METHOD_CALL(g_object_ref(method_call)),
      nullptr,
  };

  g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, on_availability_bus_ready, request);
}

static void cancel_authentication(FingerprintChannel* self,
                                  FlMethodCall* method_call) {
  AuthRequest* request = self->active_auth_request;
  if (request == nullptr) {
    g_autoptr(FlMethodResponse) response = build_response(
        FALSE, FALSE, FALSE, "idle",
        "No fingerprint verification is in progress.");
    respond(method_call, response);
    return;
  }

  request->cancel_requested = TRUE;
  set_auth_result(request, FALSE, "cancelled",
                  "Fingerprint verification was cancelled.");
  if (request->claim_done || request->verify_started) {
    begin_auth_cleanup(request);
  }

  g_autoptr(FlMethodResponse) response = build_response(
      FALSE, FALSE, TRUE, "cancelling",
      "Cancellation requested for the active fingerprint verification.");
  respond(method_call, response);
}

static void fingerprint_channel_handle_method_call(FingerprintChannel* self,
                                                   FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "canAuthenticate") == 0) {
    start_can_authenticate(self, method_call);
    return;
  }

  if (strcmp(method, "authenticate") == 0) {
    start_authenticate(self, method_call);
    return;
  }

  if (strcmp(method, "cancelAuthentication") == 0) {
    cancel_authentication(self, method_call);
    return;
  }

  g_autoptr(FlMethodResponse) response =
      FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  respond(method_call, response);
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  (void)channel;
  FingerprintChannel* plugin = FINGERPRINT_CHANNEL(user_data);
  fingerprint_channel_handle_method_call(plugin, method_call);
}

static void fingerprint_channel_dispose(GObject* object) {
  FingerprintChannel* self = FINGERPRINT_CHANNEL(object);
  if (self->active_auth_request != nullptr) {
    self->active_auth_request->cancel_requested = TRUE;
    set_auth_result(self->active_auth_request, FALSE, "cancelled",
                    "Fingerprint verification was cancelled.");
    if (self->active_auth_request->claim_done ||
        self->active_auth_request->verify_started) {
      begin_auth_cleanup(self->active_auth_request);
    }
  }

  G_OBJECT_CLASS(fingerprint_channel_parent_class)->dispose(object);
}

static void fingerprint_channel_class_init(FingerprintChannelClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fingerprint_channel_dispose;
}

static void fingerprint_channel_init(FingerprintChannel* self) {
  self->active_auth_request = nullptr;
}

void fingerprint_channel_register_with_messenger(FlBinaryMessenger* messenger) {
  FingerprintChannel* plugin = FINGERPRINT_CHANNEL(
      g_object_new(fingerprint_channel_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      messenger, kFingerprintChannelName, FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
