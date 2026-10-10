#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <math.h>
#include <ctype.h>
#include <string.h>

// --- M01 Laser Module Pins ---
#define RXD2 33
#define TXD2 32
#define ENA_PIN 25

// --- ADXL355 & RM3100 SPI Pins ---
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23
#define ADXL355_CS 5
#define RM3100_CS  15

// --- ADXL355 Register Definitions ---
#define ADXL355_DEVID_AD    0x00
#define ADXL355_DEVID_MST   0x01
#define ADXL355_PARTID      0x02
#define ADXL355_STATUS      0x04
#define ADXL355_XDATA3      0x08
#define ADXL355_FIFO_ENTRIES 0x05
#define ADXL355_POWER_CTL   0x2D
#define ADXL355_RANGE       0x2C
#define ADXL355_FILTER      0x28
#define ACCEL_CAL_MAGIC     0x41445843UL
#define ACCEL_CAL_SAMPLES   100
#define MAG_CAL_MIN_SAMPLES 300
#define MAG_CAL_MAX_SAMPLES 1000
#define MAG_CAL_SAMPLE_INTERVAL_MS 30
#define ACCEL_CAL_SAMPLE_INTERVAL_MS 10

// --- RM3100 Register Definitions ---
#define RM3100_REVID_REG    0x36
#define RM3100_POLL_REG     0x00
#define RM3100_CMM_REG      0x01
#define RM3100_STATUS_REG   0x34
#define RM3100_CCX1_REG     0x04
#define RM3100_CCX0_REG     0x05
#define RM3100_MX2_REG      0x24
#define RM3100_TMRC_REG     0x0B

// --- M01 Laser Commands ---
uint8_t measureCmd[] = {0xAA, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x21};
uint8_t laserOnCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x01, 0xC1};
uint8_t laserOffCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x00, 0xC0};

void turnLaserOff() {
  Serial1.write(laserOffCmd, sizeof(laserOffCmd));
}

static inline uint32_t bcd32(const uint8_t* b){
  uint32_t v=0; for(int i=0;i<4;i++){ v=v*100 + ((b[i]>>4)&0x0F)*10 + (b[i]&0x0F); } return v;
}

struct AccelerometerCalibration {
  uint32_t magic;
  float offset[3];
  float scale[3];
};

AccelerometerCalibration accelerometerCalibration = {
  ACCEL_CAL_MAGIC, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}
};
Preferences calibrationPreferences;
float magOffset[3] = {0.0f, 0.0f, 0.0f};
float magMatrix[3][3] = {
  {1.0f, 0.0f, 0.0f},
  {0.0f, 1.0f, 0.0f},
  {0.0f, 0.0f, 1.0f}
};
bool magCalibrationValid = false;
bool streaming = false;
bool magCalibrationActive = false;
int32_t magSamples[MAG_CAL_MAX_SAMPLES][3];
size_t magSampleCount = 0;
unsigned long lastMagSampleTime = 0;
unsigned long magCalibrationStartTime = 0;

bool accelCalibrationActive = false;
bool accelPositionSampling = false;
int accelCalibrationPosition = 0;
int accelPositionSampleCount = 0;
int64_t accelPositionSums[3] = {0, 0, 0};
int32_t accelMinimum[3];
int32_t accelMaximum[3];
unsigned long lastAccelSampleTime = 0;
unsigned long accelCalibrationStartTime = 0;

uint8_t laserState = 0;
unsigned long laserDeadline = 0;
unsigned long nextLaserCycle = 0;
uint16_t lastDistanceMm = 0;

// ============================================================
// ADXL355 SPI Functions (uses (reg << 1) | RW format)
// ============================================================
void adxl355_writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((reg << 1) & 0xFE);  // Write: LSB = 0
  SPI.transfer(value);
  digitalWrite(ADXL355_CS, HIGH);
}

uint8_t adxl355_readRegister(uint8_t reg) {
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((reg << 1) | 0x01);  // Read: LSB = 1
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(ADXL355_CS, HIGH);
  return value;
}

void initADXL355() {
  // Read device ID first to verify communication
  uint8_t devid = adxl355_readRegister(ADXL355_DEVID_AD);
  Serial.print("ADXL355 DEVID_AD: 0x"); Serial.println(devid, HEX);
  
  // Set to standby mode before configuration
  adxl355_writeRegister(ADXL355_POWER_CTL, 0x00);
  delay(20);
  
  // Set measurement mode (0x06 = measurement mode)
  adxl355_writeRegister(ADXL355_POWER_CTL, 0x06);
  delay(20);
  
  // Set output data rate (ODR) - 0x04 gives 125 Hz, 0x05 gives 250 Hz, 0x06 gives 500 Hz
  // The FILTER register ODR bits [3:0] control this
  // Leaving default should be fine, but explicitly setting is safer
  // adxl355_writeRegister(ADXL355_FILTER, 0x04); // 125 Hz ODR
  
  Serial.println("ADXL355 Initialized");
}

int32_t readADXL355Axis(uint8_t startReg) {
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((startReg << 1) | 0x01); // Read command
  
  uint8_t b0 = SPI.transfer(0x00);
  uint8_t b1 = SPI.transfer(0x00);
  uint8_t b2 = SPI.transfer(0x00);
  digitalWrite(ADXL355_CS, HIGH);

  // Combine into 20-bit signed integer
  int32_t val = ((int32_t)b0 << 12) | ((int32_t)b1 << 4) | (b2 >> 4);
  // Sign extend from 20 bits
  if (val & 0x80000) { val |= 0xFFF00000; }
  return val;
}

bool loadAccelerometerCalibration() {
  if (!calibrationPreferences.begin("adxl355", true)) {
    Serial.println("Could not open accelerometer calibration storage");
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
    Serial.println("No saved accelerometer calibration; using raw readings");
    return false;
  }

  for (int axis = 0; axis < 3; ++axis) {
    if (!isfinite(storedCalibration.offset[axis]) ||
        !isfinite(storedCalibration.scale[axis]) ||
        storedCalibration.scale[axis] <= 0.0f) {
      Serial.println("Saved accelerometer calibration is invalid; using raw readings");
      return false;
    }
  }

  accelerometerCalibration = storedCalibration;
  Serial.println("Loaded accelerometer calibration from non-volatile storage");
  return true;
}

bool saveAccelerometerCalibration(const AccelerometerCalibration& calibration) {
  if (!calibrationPreferences.begin("adxl355", false)) {
    Serial.println("Could not open accelerometer calibration storage");
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
  if (magCalibrationActive) {
    Serial.println("Finish magnetometer calibration before starting accelerometer calibration.");
    return;
  }
  streaming = false;
  laserState = 0;
  turnLaserOff();
  accelCalibrationActive = true;
  accelCalibrationStartTime = millis();
  accelPositionSampling = false;
  accelCalibrationPosition = 0;
  for (int axis = 0; axis < 3; ++axis) {
    accelMinimum[axis] = INT32_MAX;
    accelMaximum[axis] = INT32_MIN;
  }
  Serial.println("--- Accelerometer calibration ---");
  Serial.println("Place the instrument on a stable surface; press Enter at each position.");
  Serial.println("Type q to cancel.");
  Serial.println("Position 1/6: X axis pointing up");
}

void finishAccelerometerCalibration() {
  AccelerometerCalibration newCalibration;
  newCalibration.magic = ACCEL_CAL_MAGIC;
  float averageHalfRange = 0.0f;
  float halfRange[3];
  for (int axis = 0; axis < 3; ++axis) {
    halfRange[axis] = (accelMaximum[axis] - accelMinimum[axis]) / 2.0f;
    if (halfRange[axis] <= 0.0f) {
      Serial.println("Accelerometer calibration failed: an axis had no measurable range.");
      accelCalibrationActive = false;
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
    Serial.println("Could not save accelerometer calibration.");
    accelCalibrationActive = false;
    return;
  }

  accelerometerCalibration = newCalibration;
  accelCalibrationActive = false;
  Serial.println("Accelerometer calibration complete and saved.");
  for (int axis = 0; axis < 3; ++axis) {
    Serial.print("Axis ");
    Serial.print("XYZ"[axis]);
    Serial.print(" offset: ");
    Serial.print(newCalibration.offset[axis], 1);
    Serial.print(", scale: ");
    Serial.println(newCalibration.scale[axis], 6);
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

  const char* positions[] = {
    "X axis pointing up", "X axis pointing down",
    "Y axis pointing up", "Y axis pointing down",
    "Z axis pointing up", "Z axis pointing down"
  };
  Serial.print("Position ");
  Serial.print(accelCalibrationPosition + 1);
  Serial.print("/6: ");
  Serial.println(positions[accelCalibrationPosition]);
}

float calculateInclinationX(const int32_t rawX, const int32_t rawY,
                            const int32_t rawZ) {
  const float x = (rawX - accelerometerCalibration.offset[0]) *
                  accelerometerCalibration.scale[0];
  const float y = (rawY - accelerometerCalibration.offset[1]) *
                  accelerometerCalibration.scale[1];
  const float z = (rawZ - accelerometerCalibration.offset[2]) *
                  accelerometerCalibration.scale[2];
  return atan2f(x, sqrtf(y * y + z * z)) * (180.0f / PI);
}

void startMagnetometerCalibration();
void finishMagnetometerCalibration();
void eraseMagCalibration();

void startStreaming() {
  if (magCalibrationActive || accelCalibrationActive) {
    Serial.println("Finish calibration before starting streaming.");
    return;
  }
  streaming = true;
  laserState = 0;
  nextLaserCycle = millis();
  Serial.println("STREAMING=1,DIST_MM,AX,AY,AZ,MX,MY,MZ,BEARING,INCL");
}

void stopStreaming() {
  streaming = false;
  laserState = 0;
  turnLaserOff();
  Serial.println("STREAMING=0");
}

void handleCommand(char command) {
  switch (toupper(static_cast<unsigned char>(command))) {
    case 'R':
      startStreaming();
      break;
    case 'S':
      if (magCalibrationActive) finishMagnetometerCalibration();
      stopStreaming();
      break;
    case 'M':
      startMagnetometerCalibration();
      break;
    case 'A':
      startAccelerometerCalibration();
      break;
    case 'X':
      eraseMagCalibration();
      break;
    case 'H':
    case '?':
      Serial.println("Commands: R=stream, S=stop, M=mag calibration, A=accelerometer calibration, X=erase mag calibration, H=help");
      break;
    default:
      Serial.println("Unknown command. Use R, S, M, A, X, or H.");
      break;
  }
}

void processSerialCharacter(char input) {
  if (magCalibrationActive) {
    if (input == '\r' || input == '\n') {
      if (millis() - magCalibrationStartTime < 150) return;
      finishMagnetometerCalibration();
      return;
    }
    finishMagnetometerCalibration();
    const char command = static_cast<char>(toupper(static_cast<unsigned char>(input)));
    if (command == 'S' || command == 'R' || command == 'X' ||
        command == 'A' || command == 'H' || command == '?') {
      handleCommand(command);
    }
    return;
  }

  if (accelCalibrationActive) {
    if (input == 'q' || input == 'Q' || input == 's' || input == 'S') {
      accelCalibrationActive = false;
      accelPositionSampling = false;
      Serial.println("Accelerometer calibration stopped; saved calibration unchanged.");
    } else if (input == '\r' || input == '\n') {
      if (millis() - accelCalibrationStartTime < 150) return;
      if (!accelPositionSampling) {
        accelPositionSampling = true;
        accelPositionSampleCount = 0;
        accelPositionSums[0] = accelPositionSums[1] = accelPositionSums[2] = 0;
        lastAccelSampleTime = millis() - ACCEL_CAL_SAMPLE_INTERVAL_MS;
        Serial.println("Sampling position...");
      }
    }
    return;
  }

  if (input == '\r' || input == '\n' || input == ' ' || input == '\t') return;
  handleCommand(input);
}

void pollSerialCommands() {
  while (Serial.available()) {
    processSerialCharacter(static_cast<char>(Serial.read()));
  }
}

// ============================================================
// RM3100 SPI Functions (uses reg | 0x80 format)
// ============================================================
void rm3100_writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg & 0x7F);  // Write: MSB = 0
  SPI.transfer(value);
  digitalWrite(RM3100_CS, HIGH);
}

uint8_t rm3100_readRegister(uint8_t reg) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg | 0x80);  // Read: MSB = 1
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(RM3100_CS, HIGH);
  return value;
}

void initRM3100() {
  // Check Device ID
  uint8_t revid = rm3100_readRegister(RM3100_REVID_REG);
  Serial.print("RM3100 REVID: 0x"); Serial.println(revid, HEX);

  // Reset CMM first (required before writing TMRC)
  rm3100_writeRegister(RM3100_CMM_REG, 0x00);
  delay(10);
  
  // Set Cycle Count (Default 200 = 0x00C8)
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(RM3100_CCX1_REG & 0x7F); // Write to CCX1
  SPI.transfer(0x00); SPI.transfer(0xC8); // X
  SPI.transfer(0x00); SPI.transfer(0xC8); // Y
  SPI.transfer(0x00); SPI.transfer(0xC8); // Z
  digitalWrite(RM3100_CS, HIGH);
  delay(10);
  
  // Set TMRC register for update rate (0x96 = ~37 Hz)
  rm3100_writeRegister(RM3100_TMRC_REG, 0x96);
  delay(10);

  // Set Continuous Measurement Mode: 0x79 = all 3 axes, DRDY after full sequence
  rm3100_writeRegister(RM3100_CMM_REG, 0x79);
  delay(10);
  
  Serial.println("RM3100 Initialized");
}

int32_t readRM3100Axis(uint8_t startReg) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(startReg | 0x80); // Read command
  uint8_t b0 = SPI.transfer(0x00);
  uint8_t b1 = SPI.transfer(0x00);
  uint8_t b2 = SPI.transfer(0x00);
  digitalWrite(RM3100_CS, HIGH);
  
  int32_t val = ((int32_t)b0 << 16) | ((int32_t)b1 << 8) | b2;
  if (val & 0x800000) { val |= 0xFF000000; }
  return val;
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
    Serial.println("WARNING: Could not open magnetometer calibration storage; using identity calibration.");
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
    Serial.println("Loaded magnetometer calibration from non-volatile storage.");
    return true;
  }

  Serial.println("WARNING: No valid magnetometer calibration; using zero offset and identity matrix.");
  return false;
}

bool saveMagCalibration(const float offset[3], const float matrix[3][3]) {
  if (!calibrationPreferences.begin("calib", false)) {
    Serial.println("Could not open magnetometer calibration storage.");
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
    Serial.println("Could not open magnetometer calibration storage for erase.");
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
    Serial.println("Warning: one or more magnetometer calibration keys could not be erased.");
  }
  magCalibrationValid = false;
  for (int axis = 0; axis < 3; ++axis) {
    magOffset[axis] = 0.0f;
    for (int column = 0; column < 3; ++column) {
      magMatrix[axis][column] = axis == column ? 1.0f : 0.0f;
    }
  }
  Serial.println("Magnetometer calibration erased; using identity calibration.");
  Serial.println("WARNING: Compass bearing unavailable until recalibrated.");
}

/*
 * Solve a linear system of equations using Gaussian elimination with partial pivoting.
 * Used by the magnetometer calibration routine to solve for the calibration matrix.
 * The input matrix is augmented, with the last column representing the constants.
 * Returns true if a solution was found, false if the system is singular or ill-conditioned.
 */

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

  // Jacobi rotations diagonalize the symmetric quadric before taking its root.
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

  // Fit the normalized algebraic quadric by least squares (normal equations).
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
  if (accelCalibrationActive) {
    Serial.println("Finish accelerometer calibration before starting magnetometer calibration.");
    return;
  }
  streaming = false;
  laserState = 0;
  turnLaserOff();
  magSampleCount = 0;
  magCalibrationActive = true;
  magCalibrationStartTime = millis();
  lastMagSampleTime = magCalibrationStartTime - MAG_CAL_SAMPLE_INTERVAL_MS;
  Serial.println("--- Magnetometer calibration ---");
  Serial.println("Slowly rotate the instrument through all orientations; aim for 300-500 samples.");
  Serial.println("Press any key or S to finish and fit the calibration.");
}

void finishMagnetometerCalibration() {
  magCalibrationActive = false;
  Serial.print("Collected magnetometer samples: ");
  Serial.println(static_cast<unsigned int>(magSampleCount));
  if (magSampleCount < MAG_CAL_MIN_SAMPLES) {
    Serial.print("Calibration failed: at least ");
    Serial.print(MAG_CAL_MIN_SAMPLES);
    Serial.println(" samples are required; existing calibration was not changed.");
    return;
  }

  float newOffset[3];
  float newMatrix[3][3] = {};
  if (!fitMagnetometerCalibration(newOffset, newMatrix)) {
    Serial.println("Magnetometer ellipsoid fit failed (insufficient orientation coverage or singular fit); existing calibration was not changed.");
    return;
  }
  if (!saveMagCalibration(newOffset, newMatrix)) {
    Serial.println("Could not save magnetometer calibration; existing calibration was not changed.");
    return;
  }

  for (int axis = 0; axis < 3; ++axis) {
    magOffset[axis] = newOffset[axis];
    for (int column = 0; column < 3; ++column) {
      magMatrix[axis][column] = newMatrix[axis][column];
    }
  }
  magCalibrationValid = true;
  Serial.print("Magnetometer hard-iron offset: ");
  Serial.print(magOffset[0], 3); Serial.print(", ");
  Serial.print(magOffset[1], 3); Serial.print(", ");
  Serial.println(magOffset[2], 3);
  Serial.println("Magnetometer soft-iron matrix:");
  for (int row = 0; row < 3; ++row) {
    Serial.print(magMatrix[row][0], 8); Serial.print(", ");
    Serial.print(magMatrix[row][1], 8); Serial.print(", ");
    Serial.println(magMatrix[row][2], 8);
  }
  Serial.println("Magnetometer calibration saved.");
}

void serviceMagnetometerCalibration() {
  if (!magCalibrationActive ||
      millis() - lastMagSampleTime < MAG_CAL_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastMagSampleTime = millis();
  if (magSampleCount >= MAG_CAL_MAX_SAMPLES) {
    finishMagnetometerCalibration();
    return;
  }

  magSamples[magSampleCount][0] = readRM3100Axis(RM3100_MX2_REG);
  magSamples[magSampleCount][1] = readRM3100Axis(RM3100_MX2_REG + 3);
  magSamples[magSampleCount][2] = readRM3100Axis(RM3100_MX2_REG + 6);
  ++magSampleCount;
  if (magSampleCount == MAG_CAL_MAX_SAMPLES) {
    Serial.println("Sample buffer full; fitting calibration.");
    finishMagnetometerCalibration();
  }
}

float getCompassBearing(float rawX, float rawY, float rawZ) {
  if (!magCalibrationValid) return NAN;

  // --- 1. Apply magnetometer calibration (hard-iron + soft-iron) ---
  const float raw[3] = {rawX, rawY, rawZ};
  float m[3] = {0.0f, 0.0f, 0.0f};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      m[row] += magMatrix[row][col] * (raw[col] - magOffset[col]);
    }
  }

  // RM3100 X is aligned with the laser. Its Y axis is reversed relative to
  // the ADXL355 frame; retain Z as measured so magnetic inclination agrees
  // with the accelerometer's positive-up axis during pitch compensation.
  m[1] = -m[1];

  // --- 2. Read and calibrate accelerometer ---
  int32_t accelRaw[3];
  readAccelerometer(accelRaw);
  float a[3];
  for (int i = 0; i < 3; ++i) {
    a[i] = (accelRaw[i] - accelerometerCalibration.offset[i]) *
           accelerometerCalibration.scale[i];
  }

  // --- 3. Normalize both vectors ---
  float normA = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
  float normM = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
  if (normA < 1e-6f || normM < 1e-6f) return NAN;
  for (int i = 0; i < 3; ++i) { a[i] /= normA; m[i] /= normM; }

  // --- 4. Build Earth-frame basis ---
  // Body frame: X=forward, Y=left, Z=up.
  // When level, the accelerometer reads +1g on Z, so "up" = a, "down" = -a.

  // East = normalize(down × magnetic field) = normalize((-a) × m)
  float e[3];
  e[0] = -a[1]*m[2] + a[2]*m[1];
  e[1] = -a[2]*m[0] + a[0]*m[2];
  e[2] = -a[0]*m[1] + a[1]*m[0];

  float normE = sqrtf(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
  if (normE < 1e-6f) return NAN;
  for (int i = 0; i < 3; ++i) e[i] /= normE;

  // North = East × down = e × (-a)
  float n[3];
  n[0] = -e[1]*a[2] + e[2]*a[1];
  n[1] = -e[2]*a[0] + e[0]*a[2];
  n[2] = -e[0]*a[1] + e[1]*a[0];

  // --- 5. Project the body's forward axis (X) onto the horizontal plane ---
  float forward[3] = {1.0f, 0.0f, 0.0f};
  float dot = forward[0]*(-a[0]) + forward[1]*(-a[1]) + forward[2]*(-a[2]);
  forward[0] -= dot * (-a[0]);
  forward[1] -= dot * (-a[1]);
  forward[2] -= dot * (-a[2]);
  float normF = sqrtf(forward[0]*forward[0] + forward[1]*forward[1] + forward[2]*forward[2]);
  if (normF < 1e-6f) return NAN;
  for (int i = 0; i < 3; ++i) forward[i] /= normF;

  // --- 6. Heading = signed angle from North to forward, about the down axis ---
  float cosAngle = n[0]*forward[0] + n[1]*forward[1] + n[2]*forward[2];
  float sinAngle = n[0]*(forward[1]*(-a[2]) - forward[2]*(-a[1]))
                 + n[1]*(forward[2]*(-a[0]) - forward[0]*(-a[2]))
                 + n[2]*(forward[0]*(-a[1]) - forward[1]*(-a[0]));

  float heading = atan2f(sinAngle, cosAngle) * (180.0f / PI);
  if (heading < 0.0f) heading += 360.0f;
  if (heading >= 360.0f) heading -= 360.0f;
  return heading;
}

// ============================================================
// Setup and Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // --- M01 Laser Setup ---
  pinMode(ENA_PIN, OUTPUT);
  digitalWrite(ENA_PIN, HIGH);
  Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println("--- M01 Laser Module Initializing ---");
  turnLaserOff();
  delay(200);
  while (Serial1.available()) Serial1.read();

  // --- SPI Bus Setup ---
  pinMode(ADXL355_CS, OUTPUT);
  pinMode(RM3100_CS, OUTPUT);
  digitalWrite(ADXL355_CS, HIGH);
  digitalWrite(RM3100_CS, HIGH);
  
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  initADXL355();
  initRM3100();
  loadAccelerometerCalibration();
  loadMagCalibration();
  
  Serial.println("--- System Ready ---");
  Serial.println("Commands: R=stream, S=stop, M=mag calibration, A=accelerometer calibration, X=erase mag calibration, H=help.");
}

void serviceStreaming() {
  if (!streaming) return;
  const unsigned long now = millis();
  if (laserState == 0) {
    if (now < nextLaserCycle) return;
    while (Serial1.available()) Serial1.read();
    Serial1.write(laserOnCmd, sizeof(laserOnCmd));
    laserDeadline = now + 500;
    laserState = 1;
    return;
  }

  if (laserState == 1) {
    if (now < laserDeadline) return;
    while (Serial1.available()) Serial1.read();
    Serial1.write(measureCmd, sizeof(measureCmd));
    laserDeadline = now + 2500;
    laserState = 2;
    return;
  }
  if (now < laserDeadline) return;

  if (Serial1.available() >= 13) {
    uint8_t response[13];
    for (int i = 0; i < 13; ++i) response[i] = Serial1.read();
    if (response[0] == 0xAA) {
      lastDistanceMm = static_cast<uint16_t>(bcd32(&response[6]));
    }
  }
  turnLaserOff();

  int32_t acceleration[3];
  readAccelerometer(acceleration);
  const int32_t ax = acceleration[0];
  const int32_t ay = acceleration[1];
  const int32_t az = acceleration[2];
  const int32_t mx = readRM3100Axis(RM3100_MX2_REG);
  const int32_t my = readRM3100Axis(RM3100_MX2_REG + 3);
  const int32_t mz = readRM3100Axis(RM3100_MX2_REG + 6);
  const float bearing = getCompassBearing(
    static_cast<float>(mx), static_cast<float>(my), static_cast<float>(mz));
  const float inclination = calculateInclinationX(ax, ay, az);

  Serial.print("DIST_MM="); Serial.print(lastDistanceMm);
  Serial.print(",AX="); Serial.print(ax);
  Serial.print(",AY="); Serial.print(ay);
  Serial.print(",AZ="); Serial.print(az);
  Serial.print(",MX="); Serial.print(mx);
  Serial.print(",MY="); Serial.print(my);
  Serial.print(",MZ="); Serial.print(mz);
  Serial.print(",BEARING="); Serial.print(bearing, 1);
  Serial.print(",INCL="); Serial.println(inclination, 1);

  laserState = 0;
  nextLaserCycle = millis() + 1000;
}

void loop() {
  pollSerialCommands();
  serviceAccelerometerCalibration();
  serviceMagnetometerCalibration();
  serviceStreaming();
}