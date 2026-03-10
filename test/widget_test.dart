import 'package:app_lock_linux/main.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('renders the fingerprint button', (WidgetTester tester) async {
    await tester.pumpWidget(const FingerprintApp());

    expect(find.text('Test Fingerprint'), findsOneWidget);
    expect(
      find.text('Press the button to test fingerprint auth.'),
      findsOneWidget,
    );
  });
}
