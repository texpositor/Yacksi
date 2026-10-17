# YACSI - Cave Survey Instrument Android App

YACSI (Yet Another Cave Survey Instrument) is an Android application built with Kotlin and Jetpack Compose that connects to an ESP32-based cave survey device ("Yacksi") over Bluetooth Classic SPP.

## Features

- **Single-Screen Design**: Connect, control, stream data, and view logs all on one screen.
- **Bluetooth Classic SPP**: Connects to paired devices using standard SPP UUID (`00001101-0000-1000-8000-00805F9B34FB`), highlighting "Yacksi" as the default one-tap target.
- **Event-Driven Streaming**: Continuously listens for unsolicited data and command responses from the device in a background coroutine.
- **Parsed & Raw Data Display**:
  - Distance (`DIST_MM`), Bearing (`BEARING`), and Inclination (`INCL`) displayed in large, readable cards.
  - Raw accelerometer (`AX`, `AY`, `AZ`) and magnetometer (`MX`, `MY`, `MZ`) values in a diagnostics panel.
- **Run / Stop Control**: Sends `R` and `S` commands.
- **Calibration Workflows**:
  - **Accelerometer Calibration (A)**: Prompts user through 6 successive orientations with a "Continue to next position" button sending `\r`.
  - **Magnetometer Calibration (M)**: Continuous sampling during rotation with a "Finish calibration" button sending `S`.
  - **Erase Calibration (X)**: Clears stored magnetometer calibration with confirmation dialog.
- **Configuration Persistence**: Bluetooth connection and data state survive screen rotation (`ViewModel`).

## Build and Run

1. Open the project in Android Studio (Ladybug / Jellyfish or later).
2. Ensure your Android device (running Android 15+) is paired with your ESP32 "Yacksi" instrument via Bluetooth settings.
3. Build and run the app using Gradle or Android Studio's **Run** button.
4. Tap **Connect**, select **Yacksi** from the list, and start surveying!
