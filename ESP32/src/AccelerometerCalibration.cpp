#include "Calibration.h"

#include <limits.h>

namespace {
bool accelCalibrationActive = false;
bool accelPositionSampling = false;
int accelCalibrationPosition = 0;
int accelPositionSampleCount = 0;
int64_t accelPositionSums[3] = {0, 0, 0};
int32_t accelMinimum[3];
int32_t accelMaximum[3];
unsigned long lastAccelSampleTime = 0;
unsigned long accelCalibrationStartTime = 0;
}

bool isAccelerometerCalibrationActive() {
  return accelCalibrationActive;
}

bool processAccelerometerCalibrationInput(char input) {
  if (!accelCalibrationActive) return false;

  if (input == 'q' || input == 'Q') {
    accelCalibrationActive = false;
    accelPositionSampling = false;
    turnLaserOff();
    sendMessage(true).println("Accelerometer calibration stopped; saved calibration unchanged.");
  } else if (input == '\r' || input == '\n') {
    if (millis() - accelCalibrationStartTime >= 150 && !accelPositionSampling) {
      accelPositionSampling = true;
      accelPositionSampleCount = 0;
      accelPositionSums[0] = accelPositionSums[1] = accelPositionSums[2] = 0;
      lastAccelSampleTime = millis() - ACCEL_CAL_SAMPLE_INTERVAL_MS;
      playSound(Sound::Beep);
      sendMessage(true).println("Sampling position...");
    }
  }
  return true;
}
bool loadAccelerometerCalibration() {
  if (!calibrationPreferences.begin("adxl355", true)) {
    sendMessage(true).println("Could not open accelerometer calibration storage");
    return false;
  }

  AccelerometerCalibration storedCalibration;
  const size_t storedLength = calibrationPreferences.getBytesLength("cal");
  const size_t bytesRead = storedLength == sizeof(storedCalibration)
    ? calibrationPreferences.getBytes("cal", &storedCalibration, sizeof(storedCalibration))
    : 0;
  calibrationPreferences.end();

  if (bytesRead != sizeof(storedCalibration) ||
      storedCalibration.magic != ACCEL_CAL_MAGIC) {
    sendMessage(true).println("No saved accelerometer calibration; using raw readings");
    return false;
  }

  for (int axis = 0; axis < 3; ++axis) {
    if (!isfinite(storedCalibration.offset[axis]) ||
        !isfinite(storedCalibration.scale[axis]) ||
        storedCalibration.scale[axis] <= 0.0f) {
      sendMessage(true).println("Saved accelerometer calibration is invalid; using raw readings");
      return false;
    }
  }

  accelerometerCalibration = storedCalibration;
  sendMessage(true).println("Loaded accelerometer calibration from non-volatile storage");
  return true;
}

bool saveAccelerometerCalibration(const AccelerometerCalibration& calibration) {
  if (!calibrationPreferences.begin("adxl355", false)) {
    sendMessage(true).println("Could not open accelerometer calibration storage");
    return false;
  }
  const size_t bytesWritten = calibrationPreferences.putBytes(
    "cal", &calibration, sizeof(calibration));
  calibrationPreferences.end();
  return bytesWritten == sizeof(calibration);
}

void readAccelerometer(int32_t readings[3]) {
  readings[0] = readADXL355Axis(ADXL355_XDATA3);
  readings[1] = readADXL355Axis(ADXL355_XDATA3 + 3);
  readings[2] = readADXL355Axis(ADXL355_XDATA3 + 6);
}

void startAccelerometerCalibration() {
  if (isMagnetometerCalibrationActive() || isHeebCalibrationActive()) {
    sendMessage(true).println("Finish other calibration before starting accelerometer calibration.");
    return;
  }
  reading = false;
  laserCycleState = 0;
  turnLaserOn();
  accelCalibrationActive = true;
  accelCalibrationStartTime = millis();
  accelPositionSampling = false;
  accelCalibrationPosition = 0;
  for (int axis = 0; axis < 3; ++axis) {
    accelMinimum[axis] = INT32_MAX;
    accelMaximum[axis] = INT32_MIN;
  }
  sendMessage(true).println("--- Accelerometer calibration ---");
  sendMessage(true).println("Place the instrument on a stable surface; press Enter at each position.");
  sendMessage(true).println("Type q to cancel.");
  sendMessage(true).println("Position 1/6: X axis pointing up");
}

void finishAccelerometerCalibration() {
  AccelerometerCalibration newCalibration;
  newCalibration.magic = ACCEL_CAL_MAGIC;
  float averageHalfRange = 0.0f;
  float halfRange[3];
  for (int axis = 0; axis < 3; ++axis) {
    halfRange[axis] = (accelMaximum[axis] - accelMinimum[axis]) / 2.0f;
    if (halfRange[axis] <= 0.0f) {
      sendMessage(true).println("Accelerometer calibration failed: an axis had no measurable range.");
      playSound(Sound::Error);
      accelCalibrationActive = false;
      turnLaserOff();
      return;
    }
    newCalibration.offset[axis] =
      (accelMaximum[axis] + accelMinimum[axis]) / 2.0f;
    averageHalfRange += halfRange[axis] / 3.0f;
  }
  for (int axis = 0; axis < 3; ++axis) {
    newCalibration.scale[axis] = averageHalfRange / halfRange[axis];
  }

  if (!saveAccelerometerCalibration(newCalibration)) {
    sendMessage(true).println("Could not save accelerometer calibration.");
    playSound(Sound::Error);
    accelCalibrationActive = false;
    turnLaserOff();
    return;
  }

  accelerometerCalibration = newCalibration;
  accelCalibrationActive = false;
  turnLaserOff();
  playSound(Sound::Success);
  sendMessage(true).println("Accelerometer calibration complete and saved.");
  for (int axis = 0; axis < 3; ++axis) {
    sendMessage(true).print("Axis ");
    sendMessage(true).print("XYZ"[axis]);
    sendMessage(true).print(" offset: ");
    sendMessage(true).print(newCalibration.offset[axis], 1);
    sendMessage(true).print(", scale: ");
    sendMessage(true).println(newCalibration.scale[axis], 6);
  }
}

void serviceAccelerometerCalibration() {
  if (!accelCalibrationActive || !accelPositionSampling ||
      millis() - lastAccelSampleTime < ACCEL_CAL_SAMPLE_INTERVAL_MS) {
    return;
  }

  lastAccelSampleTime = millis();
  int32_t readings[3];
  readAccelerometer(readings);
  for (int axis = 0; axis < 3; ++axis) {
    accelPositionSums[axis] += readings[axis];
  }
  ++accelPositionSampleCount;
  if (accelPositionSampleCount < ACCEL_CAL_SAMPLES) return;

  for (int axis = 0; axis < 3; ++axis) {
    const int32_t average = static_cast<int32_t>(
      accelPositionSums[axis] / ACCEL_CAL_SAMPLES);
    if (average < accelMinimum[axis]) accelMinimum[axis] = average;
    if (average > accelMaximum[axis]) accelMaximum[axis] = average;
  }

  accelPositionSampling = false;
  ++accelCalibrationPosition;
  if (accelCalibrationPosition == 6) {
    finishAccelerometerCalibration();
    return;
  }
  playSound(Sound::Success);

  const char* positions[] = {
    "X axis pointing up", "X axis pointing down",
    "Y axis pointing up", "Y axis pointing down",
    "Z axis pointing up", "Z axis pointing down"
  };
  sendMessage(true).print("Position ");
  sendMessage(true).print(accelCalibrationPosition + 1);
  sendMessage(true).print("/6: ");
  sendMessage(true).println(positions[accelCalibrationPosition]);
}

// Rough per-axis offset/scale correction; the Heeb calibration supersedes it
// and uses it only as a starting point.
void roughCalibratedAccel(const float raw[3], float out[3]) {
  for (int axis = 0; axis < 3; ++axis) {
    out[axis] = (raw[axis] - accelerometerCalibration.offset[axis]) *
                accelerometerCalibration.scale[axis];
  }
}
