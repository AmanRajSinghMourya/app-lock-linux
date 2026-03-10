import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(const FingerprintApp());
}

class FingerprintApp extends StatelessWidget {
  const FingerprintApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'App Lock Test',
      theme: ThemeData(useMaterial3: true),
      home: const FingerprintPage(),
    );
  }
}

class FingerprintPage extends StatefulWidget {
  const FingerprintPage({super.key});

  @override
  State<FingerprintPage> createState() => _FingerprintPageState();
}

class _FingerprintPageState extends State<FingerprintPage> {
  final FingerprintBridge _bridge = const FingerprintBridge();

  String _status = 'Press the button to test fingerprint auth.';
  bool _busy = false;

  Future<void> _authenticate() async {
    if (_busy) {
      return;
    }

    setState(() {
      _busy = true;
      _status = 'Checking fingerprint availability...';
    });

    final FingerprintCommandResult availability = await _bridge
        .canAuthenticate();
    if (!mounted) {
      return;
    }

    if (!availability.ok) {
      setState(() {
        _busy = false;
        _status = availability.message;
      });
      return;
    }

    setState(() {
      _status = 'Touch the fingerprint reader.';
    });

    final ValueNotifier<bool> cancelling = ValueNotifier<bool>(false);
    final dialogFuture = showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (BuildContext dialogContext) {
        return AlertDialog(
          title: const Text('Fingerprint authentication'),
          content: const Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              CircularProgressIndicator(),
              SizedBox(height: 16),
              Text('Touch the fingerprint reader.'),
            ],
          ),
          actions: <Widget>[
            ValueListenableBuilder<bool>(
              valueListenable: cancelling,
              builder:
                  (BuildContext context, bool isCancelling, Widget? child) {
                    return TextButton(
                      onPressed: isCancelling
                          ? null
                          : () async {
                              cancelling.value = true;
                              await _bridge.cancelAuthentication();
                            },
                      child: Text(isCancelling ? 'Cancelling...' : 'Cancel'),
                    );
                  },
            ),
          ],
        );
      },
    );

    await Future<void>.delayed(Duration.zero);
    final FingerprintCommandResult authentication = await _bridge
        .authenticate();
    if (!mounted) {
      cancelling.dispose();
      return;
    }

    final NavigatorState navigator = Navigator.of(context, rootNavigator: true);
    if (navigator.canPop()) {
      navigator.pop();
    }
    await dialogFuture;
    cancelling.dispose();

    setState(() {
      _busy = false;
      _status = authentication.message;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              FilledButton(
                onPressed: _busy ? null : _authenticate,
                child: Text(_busy ? 'Waiting...' : 'Test Fingerprint'),
              ),
              const SizedBox(height: 16),
              SizedBox(
                width: 320,
                child: Text(_status, textAlign: TextAlign.center),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class FingerprintBridge {
  const FingerprintBridge();

  static const MethodChannel _channel = MethodChannel(
    'app_lock_linux/fingerprint',
  );

  Future<FingerprintCommandResult> canAuthenticate() {
    return _invoke('canAuthenticate');
  }

  Future<FingerprintCommandResult> authenticate() {
    return _invoke('authenticate');
  }

  Future<FingerprintCommandResult> cancelAuthentication() {
    return _invoke('cancelAuthentication');
  }

  Future<FingerprintCommandResult> _invoke(String method) async {
    if (!Platform.isLinux) {
      return FingerprintCommandResult(
        ok: false,
        message:
            'Fingerprint test is implemented only for Linux. Current host: ${Platform.operatingSystem}.',
      );
    }

    try {
      final Object? raw = await _channel.invokeMethod<Object?>(method);
      if (raw is Map<Object?, Object?>) {
        final bool available = raw['available'] == true;
        final bool authenticated = raw['authenticated'] == true;
        final bool cancelRequested = raw['cancelRequested'] == true;
        final bool success = switch (method) {
          'canAuthenticate' => available,
          'authenticate' => authenticated,
          'cancelAuthentication' => cancelRequested,
          _ => false,
        };
        final Object? message = raw['message'];
        return FingerprintCommandResult(
          ok: success,
          message: message is String ? message : 'No response message.',
        );
      }

      return const FingerprintCommandResult(
        ok: false,
        message: 'Unexpected native response.',
      );
    } on MissingPluginException {
      return const FingerprintCommandResult(
        ok: false,
        message: 'Fingerprint plugin is not registered.',
      );
    } on PlatformException catch (error) {
      return FingerprintCommandResult(
        ok: false,
        message: error.message ?? error.code,
      );
    } catch (error) {
      return FingerprintCommandResult(ok: false, message: error.toString());
    }
  }
}

class FingerprintCommandResult {
  const FingerprintCommandResult({required this.ok, required this.message});

  final bool ok;
  final String message;
}
