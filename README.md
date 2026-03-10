# Linux App Lock Probe

This sample app reproduces the Ente auth Linux fingerprint flow in a smaller Flutter project.

What it mirrors:

- `canAuthenticate` checks that the Linux `fprintd` service can see a fingerprint reader
- `authenticate` uses the Linux `fprintd` D-Bus device API directly
- Flutter first checks availability, then triggers fingerprint verification

What is different from the current Ente auth Linux path:

- The Linux runner uses async D-Bus calls, so the Flutter UI stays responsive during verification.
- The app shows a small Flutter dialog while verification is running.
- The dialog has a cancel action so the user is not stuck waiting forever.
- This is the better prototype to test before moving the logic into Ente auth.

Important Linux caveat:

- This is not a real OS biometric system prompt like macOS, iOS, Android, or Windows.
- On Linux desktop with `fprintd`, there typically is no standard desktop biometric dialog for Flutter to summon.
- The Flutter dialog is intentional: it gives a natural prompt without blocking the UI thread.
- Enrollment and permission failures are surfaced during the auth attempt so the app does not falsely report a real reader as missing.

Requirements on the Linux machine where you run it:

- `fprintd` installed and running
- A fingerprint reader supported by `fprintd`
- Fingerprints enrolled for the current user

Run on Linux:

```bash
flutter pub get
flutter run -d linux
```

Relevant Ente references:

- `/Users/amanraj/development/ente/mobile/packages/lock_screen/lib/local_authentication_service.dart`
- `/Users/amanraj/development/ente/mobile/packages/lock_screen/lib/auth_util.dart`
