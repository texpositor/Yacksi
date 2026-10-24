#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Preferences.h>

#define RXD2 33
#define TXD2 32
#define ENA_PIN 25
#define CALIBRATION_LED_PIN 4

#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23
#define ADXL355_CS 5
#define RM3100_CS 15

#define ADXL355_DEVID_AD 0x00
#define ADXL355_DEVID_MST 0x01
#define ADXL355_PARTID 0x02
#define ADXL355_STATUS 0x04
#define ADXL355_XDATA3 0x08
#define ADXL355_FIFO_ENTRIES 0x05
#define ADXL355_POWER_CTL 0x2D
#define ADXL355_RANGE 0x2C
#define ADXL355_FILTER 0x28
#define ACCEL_CAL_MAGIC 0x41445843UL
#define ACCEL_CAL_SAMPLES 100
#define MAG_CAL_MIN_SAMPLES 300
#define MAG_CAL_MAX_SAMPLES 1000
#define MAG_CAL_SAMPLE_INTERVAL_MS 30
#define ACCEL_CAL_SAMPLE_INTERVAL_MS 10

// Heeb (DistoX-style) calibration: groups of shots at a fixed target, rolling
// the instrument about the laser axis between the shots of each group.
#define HEEB_GROUPS 14
#define HEEB_ROLLS 4
#define HEEB_MAX_SHOTS (HEEB_GROUPS * HEEB_ROLLS)
#define HEEB_MIN_GROUPS 8
#define HEEB_SAMPLES_PER_SHOT 24
#define HEEB_SETTLE_MS 150
// Change in tilt between the first and second half of a shot's samples that
// counts as the instrument having moved; sensor noise averages out well below this.
#define HEEB_MAX_SHOT_MOVEMENT_DEG 1.5f
#ifndef HEEB_MAX_ITERATIONS
#define HEEB_MAX_ITERATIONS 200
#endif
#define HEEB_CONVERGENCE 1.0e-6
#define HEEB_CAL_MAGIC 0x48454231UL
#define HEEB_SHOTS_MAGIC 0x48455331UL

#define RM3100_REVID_REG 0x36
#define RM3100_POLL_REG 0x00
#define RM3100_CMM_REG 0x01
#define RM3100_STATUS_REG 0x34
#define RM3100_CCX1_REG 0x04
#define RM3100_CCX0_REG 0x05
#define RM3100_MX2_REG 0x24
#define RM3100_TMRC_REG 0x0B

extern BluetoothSerial SerialBT;

class MessageOutput : public Print {
public:
  explicit MessageOutput(bool sendBluetooth) : sendBluetooth_(sendBluetooth) {}

  size_t write(uint8_t value) override {
    const size_t bytesWritten = Serial.write(value);
    if (sendBluetooth_) SerialBT.write(value);
    return bytesWritten;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    const size_t bytesWritten = Serial.write(buffer, size);
    if (sendBluetooth_) SerialBT.write(buffer, size);
    return bytesWritten;
  }

private:
  bool sendBluetooth_;
};

inline MessageOutput sendMessage(bool sendBluetooth) {
  return MessageOutput(sendBluetooth);
}

struct AccelerometerCalibration {
  uint32_t magic;
  float offset[3];
  float scale[3];
};

// Full linear calibration of both sensors in raw counts:
// g = aG * rawAccel + bG, m = aM * rawMag + bM, both in the laser frame
// (X = laser, Y = left, Z = up) and scaled to unit length.
struct HeebCalibration {
  uint32_t magic;
  float aG[3][3];
  float bG[3];
  float aM[3][3];
  float bM[3];
  float rmsError;
  float maxError;
  float dip;
};

extern AccelerometerCalibration accelerometerCalibration;
extern Preferences calibrationPreferences;
extern float magOffset[3];
extern float magMatrix[3][3];
extern bool magCalibrationValid;
extern HeebCalibration heebCalibration;
extern bool heebCalibrationValid;
extern bool reading;
extern uint8_t laserCycleState;

void turnLaserOff();
void turnLaserOn();

void adxl355_writeRegister(uint8_t reg, uint8_t value);
uint8_t adxl355_readRegister(uint8_t reg);
void initADXL355();
int32_t readADXL355Axis(uint8_t startReg);
void readAccelerometer(int32_t readings[3]);

uint8_t rm3100_readRegister(uint8_t reg);
bool rm3100_dataReady();
int32_t readRM3100Axis(uint8_t startReg);
void readMagnetometer(int32_t readings[3]);
void rm3100_writeRegister(uint8_t reg, uint8_t value);
void initRM3100();

bool loadAccelerometerCalibration();
bool saveAccelerometerCalibration(const AccelerometerCalibration& calibration);
void startAccelerometerCalibration();
void serviceAccelerometerCalibration();
void finishAccelerometerCalibration();
void roughCalibratedAccel(const float raw[3], float out[3]);
bool isAccelerometerCalibrationActive();
bool processAccelerometerCalibrationInput(char input);

bool loadMagCalibration();
bool saveMagCalibration(const float offset[3], const float matrix[3][3]);
void eraseMagCalibration();
void startMagnetometerCalibration();
void finishMagnetometerCalibration();
void serviceMagnetometerCalibration();
void roughCalibratedMag(const float raw[3], float out[3]);
bool isMagnetometerCalibrationActive();
bool processMagnetometerCalibrationInput(char input);

void startHeebCalibration();
void serviceHeebCalibration();
bool isHeebCalibrationActive();
bool processHeebCalibrationInput(char input);
bool loadHeebCalibration();
void refitStoredHeebShots();
void dumpHeebCalibration();
void eraseHeebCalibration();

// Applies the Heeb calibration when valid, otherwise the rough G/M calibrations.
void calibratedVectors(const float rawAccel[3], const float rawMag[3],
                       float g[3], float m[3]);
// Returns false when the orientation is undefined (e.g. no field data).
bool laserBearingInclination(const float g[3], const float m[3],
                             float& bearing, float& inclination);

void handleCommand(char command);
