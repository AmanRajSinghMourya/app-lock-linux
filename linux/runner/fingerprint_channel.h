#ifndef RUNNER_FINGERPRINT_CHANNEL_H_
#define RUNNER_FINGERPRINT_CHANNEL_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FingerprintChannel, fingerprint_channel, FINGERPRINT,
                     CHANNEL, GObject)

void fingerprint_channel_register_with_messenger(FlBinaryMessenger* messenger);

G_END_DECLS

#endif  // RUNNER_FINGERPRINT_CHANNEL_H_
