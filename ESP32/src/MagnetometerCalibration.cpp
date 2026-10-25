#include "Calibration.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

namespace {
bool magCalibrationActive = false;
int32_t magSamples[MAG_CAL_MAX_SAMPLES][3];
size_t magSampleCount = 0;
unsigned long lastMagSampleTime = 0;
unsigned long magCalibrationStartTime = 0;
}

bool isMagnetometerCalibrationActive() {
  return magCalibrationActive;
}

bool processMagnetometerCalibrationInput(char input) {
  if (!magCalibrationActive) return false;

  if (input == '\r' || input == '\n') {
    if (millis() - magCalibrationStartTime < 150) return true;
    finishMagnetometerCalibration();
    return true;
  }

  finishMagnetometerCalibration();
  const char command = static_cast<char>(toupper(static_cast<unsigned char>(input)));
  if (command == 'R' || command == 'X' || command == 'A' || command == 'C' ||
      command == 'H' || command == '?') {
    handleCommand(command);
  }
  return true;
}

bool loadMagCalibration() {
  magCalibrationValid = false;
  for (int axis = 0; axis < 3; ++axis) {
    magOffset[axis] = 0.0f;
    for (int column = 0; column < 3; ++column) {
      magMatrix[axis][column] = axis == column ? 1.0f : 0.0f;
    }
  }

  if (!calibrationPreferences.begin("calib", true)) {
    sendMessage(true).println("WARNING: Could not open magnetometer calibration storage; using identity calibration.");
    return false;
  }

  bool valid = calibrationPreferences.getBool("mag_cal_valid", false);
  const char* offsetKeys[3] = {"mag_off_x", "mag_off_y", "mag_off_z"};
  const char* matrixKeys[3][3] = {
    {"mag_mat_00", "mag_mat_01", "mag_mat_02"},
    {"mag_mat_10", "mag_mat_11", "mag_mat_12"},
    {"mag_mat_20", "mag_mat_21", "mag_mat_22"}
  };
  float storedOffset[3];
  float storedMatrix[3][3];

  for (int axis = 0; axis < 3; ++axis) {
    if (!calibrationPreferences.isKey(offsetKeys[axis])) valid = false;
    storedOffset[axis] = calibrationPreferences.getFloat(offsetKeys[axis], NAN);
    if (!isfinite(storedOffset[axis])) valid = false;
    for (int column = 0; column < 3; ++column) {
      if (!calibrationPreferences.isKey(matrixKeys[axis][column])) valid = false;
      storedMatrix[axis][column] =
        calibrationPreferences.getFloat(matrixKeys[axis][column], NAN);
      if (!isfinite(storedMatrix[axis][column])) valid = false;
    }
  }
  calibrationPreferences.end();

  if (valid) {
    for (int axis = 0; axis < 3; ++axis) {
      magOffset[axis] = storedOffset[axis];
      for (int column = 0; column < 3; ++column) {
        magMatrix[axis][column] = storedMatrix[axis][column];
      }
    }
    magCalibrationValid = true;
    sendMessage(true).println("Loaded magnetometer calibration from non-volatile storage.");
    return true;
  }

  sendMessage(true).println("WARNING: No valid magnetometer calibration; using zero offset and identity matrix.");
  return false;
}

bool saveMagCalibration(const float offset[3], const float matrix[3][3]) {
  if (!calibrationPreferences.begin("calib", false)) {
    sendMessage(false).println("Could not open magnetometer calibration storage.");
    return false;
  }

  bool saved = calibrationPreferences.putBool("mag_cal_valid", false);
  const char* offsetKeys[3] = {"mag_off_x", "mag_off_y", "mag_off_z"};
  const char* matrixKeys[3][3] = {
    {"mag_mat_00", "mag_mat_01", "mag_mat_02"},
    {"mag_mat_10", "mag_mat_11", "mag_mat_12"},
    {"mag_mat_20", "mag_mat_21", "mag_mat_22"}
  };
  for (int axis = 0; axis < 3; ++axis) {
    saved = calibrationPreferences.putFloat(offsetKeys[axis], offset[axis]) > 0 && saved;
    for (int column = 0; column < 3; ++column) {
      saved = calibrationPreferences.putFloat(
        matrixKeys[axis][column], matrix[axis][column]) > 0 && saved;
    }
  }
  if (saved) saved = calibrationPreferences.putBool("mag_cal_valid", true);
  calibrationPreferences.end();
  return saved;
}

void eraseMagCalibration() {
  if (!calibrationPreferences.begin("calib", false)) {
    sendMessage(true).println("Could not open magnetometer calibration storage for erase.");
    return;
  }
  bool erased = true;
  const char* keys[] = {
    "mag_cal_valid", "mag_off_x", "mag_off_y", "mag_off_z"
  };
  for (size_t key = 0; key < sizeof(keys) / sizeof(keys[0]); ++key) {
    if (calibrationPreferences.isKey(keys[key]) &&
        !calibrationPreferences.remove(keys[key])) {
      erased = false;
    }
  }
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      char key[11] = "mag_mat_00";
      key[8] = static_cast<char>('0' + row);
      key[9] = static_cast<char>('0' + column);
      if (calibrationPreferences.isKey(key) &&
          !calibrationPreferences.remove(key)) {
        erased = false;
      }
    }
  }
  calibrationPreferences.end();
  if (!erased) {
    sendMessage(true).println("Warning: one or more magnetometer calibration keys could not be erased.");
  }
  magCalibrationValid = false;
  for (int axis = 0; axis < 3; ++axis) {
    magOffset[axis] = 0.0f;
    for (int column = 0; column < 3; ++column) {
      magMatrix[axis][column] = axis == column ? 1.0f : 0.0f;
    }
  }
  sendMessage(true).println("Magnetometer calibration erased; using identity calibration.");
  sendMessage(true).println("WARNING: Compass bearing unavailable until recalibrated.");
}

bool solveLinearSystem(float matrix[9][10], int size, float solution[9]) {
  for (int column = 0; column < size; ++column) {
    int pivotRow = column;
    float largest = fabsf(matrix[column][column]);
    for (int row = column + 1; row < size; ++row) {
      const float candidate = fabsf(matrix[row][column]);
      if (candidate > largest) {
        largest = candidate;
        pivotRow = row;
      }
    }
    if (!isfinite(largest) || largest < 1.0e-7f) return false;
    if (pivotRow != column) {
      for (int item = column; item <= size; ++item) {
        const float temporary = matrix[column][item];
        matrix[column][item] = matrix[pivotRow][item];
        matrix[pivotRow][item] = temporary;
      }
    }

    const float pivot = matrix[column][column];
    for (int item = column; item <= size; ++item) {
      matrix[column][item] /= pivot;
    }
    for (int row = 0; row < size; ++row) {
      if (row == column) continue;
      const float factor = matrix[row][column];
      for (int item = column; item <= size; ++item) {
        matrix[row][item] -= factor * matrix[column][item];
      }
    }
  }

  for (int row = 0; row < size; ++row) {
    solution[row] = matrix[row][size];
    if (!isfinite(solution[row])) return false;
  }
  return true;
}

bool symmetricSquareRoot(const float input[3][3], float output[3][3]) {
  float diagonalized[3][3];
  float eigenvectors[3][3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f}
  };
  memcpy(diagonalized, input, sizeof(diagonalized));

  for (int iteration = 0; iteration < 32; ++iteration) {
    int p = 0;
    int q = 1;
    float largest = fabsf(diagonalized[0][1]);
    if (fabsf(diagonalized[0][2]) > largest) {
      p = 0; q = 2; largest = fabsf(diagonalized[0][2]);
    }
    if (fabsf(diagonalized[1][2]) > largest) {
      p = 1; q = 2; largest = fabsf(diagonalized[1][2]);
    }
    if (largest < 1.0e-6f) break;

    const float angle = 0.5f * atan2f(
      2.0f * diagonalized[p][q],
      diagonalized[q][q] - diagonalized[p][p]);
    const float cosine = cosf(angle);
    const float sine = sinf(angle);
    float rotation[3][3] = {
      {1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f}
    };
    rotation[p][p] = cosine;
    rotation[q][q] = cosine;
    rotation[p][q] = sine;
    rotation[q][p] = -sine;

    float temporary[3][3] = {};
    float rotated[3][3] = {};
    float updatedVectors[3][3] = {};
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        for (int k = 0; k < 3; ++k) {
          temporary[row][column] += diagonalized[row][k] * rotation[k][column];
          updatedVectors[row][column] += eigenvectors[row][k] * rotation[k][column];
        }
      }
    }
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        for (int k = 0; k < 3; ++k) {
          rotated[row][column] += rotation[k][row] * temporary[k][column];
        }
      }
    }
    memcpy(diagonalized, rotated, sizeof(diagonalized));
    memcpy(eigenvectors, updatedVectors, sizeof(eigenvectors));
  }

  for (int eigen = 0; eigen < 3; ++eigen) {
    const float value = diagonalized[eigen][eigen];
    if (!isfinite(value) || value <= 0.0f) return false;
    const float root = sqrtf(value);
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        output[row][column] +=
          eigenvectors[row][eigen] * root * eigenvectors[column][eigen];
      }
    }
  }
  return true;
}

bool fitMagnetometerCalibration(float offset[3], float matrix[3][3]) {
  if (magSampleCount < MAG_CAL_MIN_SAMPLES) return false;

  float mean[3] = {0.0f, 0.0f, 0.0f};
  for (size_t sample = 0; sample < magSampleCount; ++sample) {
    for (int axis = 0; axis < 3; ++axis) {
      mean[axis] += static_cast<float>(magSamples[sample][axis]) /
                    static_cast<float>(magSampleCount);
    }
  }

  float sumSquared = 0.0f;
  for (size_t sample = 0; sample < magSampleCount; ++sample) {
    for (int axis = 0; axis < 3; ++axis) {
      const float centered = static_cast<float>(magSamples[sample][axis]) - mean[axis];
      sumSquared += centered * centered;
    }
  }
  const float normalization = sqrtf(
    sumSquared / (3.0f * static_cast<float>(magSampleCount)));
  if (!isfinite(normalization) || normalization <= 1.0e-6f) return false;

  float normal[9][10] = {};
  for (size_t sample = 0; sample < magSampleCount; ++sample) {
    const float x = (magSamples[sample][0] - mean[0]) / normalization;
    const float y = (magSamples[sample][1] - mean[1]) / normalization;
    const float z = (magSamples[sample][2] - mean[2]) / normalization;
    const float feature[9] = {
      x * x, y * y, z * z, 2.0f * y * z, 2.0f * x * z,
      2.0f * x * y, 2.0f * x, 2.0f * y, 2.0f * z
    };
    for (int row = 0; row < 9; ++row) {
      for (int column = 0; column < 9; ++column) {
        normal[row][column] += feature[row] * feature[column];
      }
      normal[row][9] += feature[row];
    }
  }

  float coefficients[9] = {};
  if (!solveLinearSystem(normal, 9, coefficients)) return false;
  float quadric[3][3] = {
    {coefficients[0], coefficients[5], coefficients[4]},
    {coefficients[5], coefficients[1], coefficients[3]},
    {coefficients[4], coefficients[3], coefficients[2]}
  };

  float centerSystem[9][10] = {};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      centerSystem[row][column] = quadric[row][column];
    }
    centerSystem[row][3] = -coefficients[6 + row];
  }
  float centerSolution[9] = {};
  if (!solveLinearSystem(centerSystem, 3, centerSolution)) return false;

  float center[3] = {centerSolution[0], centerSolution[1], centerSolution[2]};
  float centerQuadricCenter = 0.0f;
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      centerQuadricCenter += center[row] * quadric[row][column] * center[column];
    }
  }
  const float radiusScale = 1.0f + centerQuadricCenter;
  if (!isfinite(radiusScale) || radiusScale <= 0.0f) return false;

  float normalizedQuadric[3][3];
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      normalizedQuadric[row][column] = quadric[row][column] / radiusScale;
    }
  }
  float normalizedMatrix[3][3] = {};
  if (!symmetricSquareRoot(normalizedQuadric, normalizedMatrix)) return false;

  for (int axis = 0; axis < 3; ++axis) {
    offset[axis] = mean[axis] + normalization * center[axis];
    for (int column = 0; column < 3; ++column) {
      matrix[axis][column] = normalizedMatrix[axis][column] / normalization;
      if (!isfinite(matrix[axis][column])) return false;
    }
    if (!isfinite(offset[axis])) return false;
  }
  return true;
}

void startMagnetometerCalibration() {
  if (isAccelerometerCalibrationActive() || isHeebCalibrationActive()) {
    sendMessage(true).println("Finish other calibration before starting magnetometer calibration.");
    return;
  }
  sendMessage(true).println("Starting magnetometer calibration; Bluetooth will be unavailable until calibration ends.");
  reading = false;
  laserCycleState = 0;
  turnLaserOff();
  magSampleCount = 0;
  magCalibrationActive = true;
  magCalibrationStartTime = millis();
  lastMagSampleTime = magCalibrationStartTime - MAG_CAL_SAMPLE_INTERVAL_MS;
  sendMessage(false).println("--- Magnetometer calibration ---");
  sendMessage(false).println("Slowly rotate the instrument through all orientations; aim for 300-500 samples.");
  sendMessage(false).println("Press any key on the serial connection to finish and fit the calibration.");
}

bool endMagnetometerCalibration() {
  return true;
}

void finishMagnetometerCalibration() {
  magCalibrationActive = false;
  turnLaserOff();
  if (magSampleCount < MAG_CAL_MIN_SAMPLES) {
    const bool bluetoothAvailable = endMagnetometerCalibration();
    sendMessage(bluetoothAvailable).print("Collected magnetometer samples: ");
    sendMessage(bluetoothAvailable).println(static_cast<unsigned int>(magSampleCount));
    sendMessage(bluetoothAvailable).print("Calibration failed: at least ");
    sendMessage(bluetoothAvailable).print(MAG_CAL_MIN_SAMPLES);
    sendMessage(bluetoothAvailable).println(" samples are required; existing calibration was not changed.");
    playSound(Sound::Error);
    return;
  }

  float newOffset[3];
  float newMatrix[3][3] = {};
  if (!fitMagnetometerCalibration(newOffset, newMatrix)) {
    const bool bluetoothAvailable = endMagnetometerCalibration();
    sendMessage(bluetoothAvailable).print("Collected magnetometer samples: ");
    sendMessage(bluetoothAvailable).println(static_cast<unsigned int>(magSampleCount));
    sendMessage(bluetoothAvailable).println("Magnetometer ellipsoid fit failed (insufficient orientation coverage or singular fit); existing calibration was not changed.");
    playSound(Sound::Error);
    return;
  }
  if (!saveMagCalibration(newOffset, newMatrix)) {
    const bool bluetoothAvailable = endMagnetometerCalibration();
    sendMessage(bluetoothAvailable).print("Collected magnetometer samples: ");
    sendMessage(bluetoothAvailable).println(static_cast<unsigned int>(magSampleCount));
    sendMessage(bluetoothAvailable).println("Could not save magnetometer calibration; existing calibration was not changed.");
    playSound(Sound::Error);
    return;
  }

  for (int axis = 0; axis < 3; ++axis) {
    magOffset[axis] = newOffset[axis];
    for (int column = 0; column < 3; ++column) {
      magMatrix[axis][column] = newMatrix[axis][column];
    }
  }
  magCalibrationValid = true;
  const bool bluetoothAvailable = endMagnetometerCalibration();
  sendMessage(bluetoothAvailable).print("Collected magnetometer samples: ");
  sendMessage(bluetoothAvailable).println(static_cast<unsigned int>(magSampleCount));
  sendMessage(bluetoothAvailable).print("Magnetometer hard-iron offset: ");
  sendMessage(bluetoothAvailable).print(magOffset[0], 3); sendMessage(bluetoothAvailable).print(", ");
  sendMessage(bluetoothAvailable).print(magOffset[1], 3); sendMessage(bluetoothAvailable).print(", ");
  sendMessage(bluetoothAvailable).println(magOffset[2], 3);
  sendMessage(bluetoothAvailable).println("Magnetometer soft-iron matrix:");
  for (int row = 0; row < 3; ++row) {
    sendMessage(bluetoothAvailable).print(magMatrix[row][0], 8); sendMessage(bluetoothAvailable).print(", ");
    sendMessage(bluetoothAvailable).print(magMatrix[row][1], 8); sendMessage(bluetoothAvailable).print(", ");
    sendMessage(bluetoothAvailable).println(magMatrix[row][2], 8);
  }
  sendMessage(bluetoothAvailable).println("Magnetometer calibration saved.");
  playSound(Sound::Success);
}

void serviceMagnetometerCalibration() {
  const unsigned long now = millis();
  if (!magCalibrationActive ||
      now - lastMagSampleTime < MAG_CAL_SAMPLE_INTERVAL_MS) {
    return;
  }
  if (magSampleCount >= MAG_CAL_MAX_SAMPLES) {
    finishMagnetometerCalibration();
    return;
  }
  if (!rm3100_dataReady()) return;
  lastMagSampleTime = now;

  readMagnetometer(magSamples[magSampleCount]);
  ++magSampleCount;
  if (magSampleCount == MAG_CAL_MAX_SAMPLES) {
    sendMessage(false).println("Sample buffer full; fitting calibration.");
    finishMagnetometerCalibration();
  }
}

// Rough hard/soft-iron correction from the ellipsoid fit; the Heeb calibration
// supersedes it and uses it only as a starting point.
void roughCalibratedMag(const float raw[3], float out[3]) {
  for (int row = 0; row < 3; ++row) {
    out[row] = 0.0f;
    for (int col = 0; col < 3; ++col) {
      out[row] += magMatrix[row][col] * (raw[col] - magOffset[col]);
    }
  }
}
