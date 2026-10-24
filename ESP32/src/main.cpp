#include <Arduino.h>
#include <SPI.h>
#include <ctype.h>
#include <math.h>
#include "Calibration.h"

BluetoothSerial SerialBT;

// --- M01 Laser Commands ---
uint8_t measureCmd[] = {0xAA, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x21};
uint8_t laserOnCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x01, 0xC1};
uint8_t laserOffCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x00, 0xC0};
uint8_t laserState = 0;

void turnLaserOff() {
  Serial1.write(laserOffCmd, sizeof(laserOffCmd));
  laserState = 0;
}

void turnLaserOn() {
  Serial1.write(laserOnCmd, sizeof(laserOnCmd));
  laserState = 1;
}

static inline uint32_t bcd32(const uint8_t* b){
  uint32_t v=0; for(int i=0;i<4;i++){ v=v*100 + ((b[i]>>4)&0x0F)*10 + (b[i]&0x0F); } return v;
}

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

bool reading = false;
uint8_t laserCycleState = 0;
unsigned long laserDeadline = 0;
unsigned long nextLaserCycle = 0;
uint16_t lastDistanceMm = 0;
const unsigned long LASER_SETTLE_MS = 100;
const unsigned long SENSOR_SETTLE_MS = 100;

// ============================================================
// ADXL355 SPI Functions (uses (reg << 1) | RW format)
// ============================================================
void adxl355_writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((reg << 1) & 0xFE);
  SPI.transfer(value);
  digitalWrite(ADXL355_CS, HIGH);
}

uint8_t adxl355_readRegister(uint8_t reg) {
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((reg << 1) | 0x01);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(ADXL355_CS, HIGH);
  return value;
}

void initADXL355() {
  uint8_t devid = adxl355_readRegister(ADXL355_DEVID_AD);
  sendMessage(true).print("ADXL355 DEVID_AD: 0x"); sendMessage(true).println(devid, HEX);

  adxl355_writeRegister(ADXL355_POWER_CTL, 0x00);
  delay(20);
  adxl355_writeRegister(ADXL355_POWER_CTL, 0x06);
  delay(20);

  sendMessage(true).println("ADXL355 Initialized");
}

int32_t readADXL355Axis(uint8_t startReg) {
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((startReg << 1) | 0x01);

  uint8_t b0 = SPI.transfer(0x00);
  uint8_t b1 = SPI.transfer(0x00);
  uint8_t b2 = SPI.transfer(0x00);
  digitalWrite(ADXL355_CS, HIGH);

  int32_t val = ((int32_t)b0 << 12) | ((int32_t)b1 << 4) | (b2 >> 4);
  if (val & 0x80000) { val |= 0xFFF00000; }
  return val;
}

uint8_t rm3100_readRegister(uint8_t reg) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg | 0x80);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(RM3100_CS, HIGH);
  return value;
}

bool rm3100_dataReady() {
  return (rm3100_readRegister(RM3100_STATUS_REG) & 0x80) != 0;
}


int32_t readRM3100Axis(uint8_t startReg) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(startReg | 0x80);
  uint8_t b0 = SPI.transfer(0x00);
  uint8_t b1 = SPI.transfer(0x00);
  uint8_t b2 = SPI.transfer(0x00);
  digitalWrite(RM3100_CS, HIGH);

  int32_t val = ((int32_t)b0 << 16) | ((int32_t)b1 << 8) | b2;
  if (val & 0x800000) { val |= 0xFF000000; }
  return val;
}

// Reads all three RM3100 axes in the ADXL355 frame. The RM3100 module's Y axis
// is reversed relative to the ADXL355, so it is negated here; without this the
// two sensors have opposite handedness and no rotation can align them.
void readMagnetometer(int32_t readings[3]) {
  readings[0] = readRM3100Axis(RM3100_MX2_REG);
  readings[1] = -readRM3100Axis(RM3100_MX2_REG + 3);
  readings[2] = readRM3100Axis(RM3100_MX2_REG + 6);
}

void startReading() {
  if (isMagnetometerCalibrationActive() || isAccelerometerCalibrationActive() ||
      isHeebCalibrationActive()) {
    sendMessage(true).println("Finish calibration before taking a reading.");
    return;
  }
  if (reading) {
    sendMessage(true).println("A reading is already in progress.");
    return;
  }
  reading = true;
  laserCycleState = 0;
  nextLaserCycle = millis();
}

void handleCommand(char command) {
  switch (toupper(static_cast<unsigned char>(command))) {
    case 'R': startReading(); break;
    case 'M': startMagnetometerCalibration(); break;
    case 'C': startHeebCalibration(); break;
    case 'F': refitStoredHeebShots(); break;
    case 'D': dumpHeebCalibration(); break;
    case 'E': eraseHeebCalibration(); break;
    case 'A': startAccelerometerCalibration(); break;
    case 'X': eraseMagCalibration(); break;
    case 'H':
    case '?':
      sendMessage(true).println("Commands: R=read, C=shot calibration, F=refit stored shots, D=dump shot calibration, E=erase shot calibration, A=rough accel calibration, M=rough mag calibration, X=erase mag calibration, H=help");
      break;
    default:
      sendMessage(true).println("Unknown command. Use R, C, F, D, E, A, M, X, or H.");
      break;
  }
}

void processInputCharacter(char input) {
  if (processMagnetometerCalibrationInput(input) ||
      processAccelerometerCalibrationInput(input) ||
      processHeebCalibrationInput(input)) {
    return;
  }
  if (input == '\r' || input == '\n' || input == ' ' || input == '\t') return;
  handleCommand(input);
}

void pollCommands() {
  while (Serial.available()) {
    processInputCharacter(static_cast<char>(Serial.read()));
  }
  while (SerialBT.available()) {
    processInputCharacter(static_cast<char>(SerialBT.read()));
  }
}

// ============================================================
// RM3100 SPI Functions (uses reg | 0x80 format)
// ============================================================
void rm3100_writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg & 0x7F);
  SPI.transfer(value);
  digitalWrite(RM3100_CS, HIGH);
}


void initRM3100() {
  uint8_t revid = rm3100_readRegister(RM3100_REVID_REG);
  sendMessage(true).print("RM3100 REVID: 0x"); sendMessage(true).println(revid, HEX);

  rm3100_writeRegister(RM3100_CMM_REG, 0x00);
  delay(10);

  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(RM3100_CCX1_REG & 0x7F);
  SPI.transfer(0x00); SPI.transfer(0xC8); // X
  SPI.transfer(0x00); SPI.transfer(0xC8); // Y
  SPI.transfer(0x00); SPI.transfer(0xC8); // Z
  digitalWrite(RM3100_CS, HIGH);
  delay(10);

  rm3100_writeRegister(RM3100_TMRC_REG, 0x96);
  delay(10);

  rm3100_writeRegister(RM3100_CMM_REG, 0x79);
  delay(10);

  sendMessage(true).println("RM3100 Initialized");
}


void printCalibrationToSerial() {
  Serial.println("--- Active accelerometer calibration ---");
  Serial.println("Offset:");
  for (int axis = 0; axis < 3; ++axis) {
    Serial.print(accelerometerCalibration.offset[axis], 6);
    Serial.println();
  }
  Serial.println("Scale matrix:");
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      if (column > 0) Serial.print(", ");
      Serial.print(row == column ? accelerometerCalibration.scale[row] : 0.0f, 6);
    }
    Serial.println();
  }

  Serial.println("--- Active magnetometer calibration ---");
  Serial.println("Offset:");
  for (int axis = 0; axis < 3; ++axis) {
    Serial.print(magOffset[axis], 6);
    Serial.println();
  }
  Serial.println("Soft-iron matrix:");
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      if (column > 0) Serial.print(", ");
      Serial.print(magMatrix[row][column], 8);
    }
    Serial.println();
  }

  Serial.println(heebCalibrationValid
    ? "--- Shot calibration in use (supersedes the above; D for details) ---"
    : "--- No shot calibration; using the rough calibrations above ---");
}

// ============================================================
// Setup and Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(CALIBRATION_LED_PIN, OUTPUT);
  digitalWrite(CALIBRATION_LED_PIN, LOW);
  SerialBT.begin("Yacksi");
  delay(1000);

  pinMode(ENA_PIN, OUTPUT);
  digitalWrite(ENA_PIN, HIGH);
  Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2);
  sendMessage(true).println("--- M01 Laser Module Initializing ---");
  turnLaserOff();
  delay(200);
  while (Serial1.available()) Serial1.read();

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
  loadHeebCalibration();
  printCalibrationToSerial();

  sendMessage(true).println("--- System Ready ---");
  handleCommand('H');
}

void takeDistanceReading() {
  if (!reading) return;
  const unsigned long now = millis();
  if (laserCycleState == 0) {
    if (now < nextLaserCycle) return;
    laserDeadline = now + LASER_SETTLE_MS;
    laserCycleState = 1;
    return;
  }

  if (laserCycleState == 1) {
    if (now < laserDeadline) return;
    while (Serial1.available()) Serial1.read();
    turnLaserOn();
    laserDeadline = now + 500;
    laserCycleState = 2;
    return;
  }

  if (laserCycleState == 2) {
    if (now < laserDeadline) return;
    while (Serial1.available()) Serial1.read();
    Serial1.write(measureCmd, sizeof(measureCmd));
    laserDeadline = now + 2500;
    laserCycleState = 3;
    return;
  }

  if (laserCycleState != 3) return;
  if (now < laserDeadline) return;

  if (Serial1.available() >= 13) {
    uint8_t response[13];
    for (int i = 0; i < 13; ++i) response[i] = Serial1.read();
    if (response[0] == 0xAA) {
      lastDistanceMm = static_cast<uint16_t>(bcd32(&response[6]));
    }
  }

  turnLaserOff();
  laserDeadline = millis() + SENSOR_SETTLE_MS;
  laserCycleState = 4;
  return;
}

void takeRotationReading() {
  if (!reading || laserCycleState != 4 || millis() < laserDeadline) return;
  if (!rm3100_dataReady()) return;

  int32_t acceleration[3];
  readAccelerometer(acceleration);
  const int32_t ax = acceleration[0];
  const int32_t ay = acceleration[1];
  const int32_t az = acceleration[2];
  int32_t magnetic[3];
  readMagnetometer(magnetic);
  const int32_t mx = magnetic[0];
  const int32_t my = magnetic[1];
  const int32_t mz = magnetic[2];

  const float rawAccel[3] = {static_cast<float>(ax), static_cast<float>(ay),
                             static_cast<float>(az)};
  const float rawMag[3] = {static_cast<float>(mx), static_cast<float>(my),
                           static_cast<float>(mz)};
  float g[3], m[3];
  calibratedVectors(rawAccel, rawMag, g, m);
  float bearing, inclination;
  laserBearingInclination(g, m, bearing, inclination);
  // Field and gravity magnitudes are 1.0 under the shot calibration; a large
  // deviation in MLEN suggests local magnetic interference.
  const float gLength = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
  const float mLength = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);

  sendMessage(true).print("DIST_MM="); sendMessage(true).print(lastDistanceMm);
  sendMessage(true).print(",AX="); sendMessage(true).print(ax);
  sendMessage(true).print(",AY="); sendMessage(true).print(ay);
  sendMessage(true).print(",AZ="); sendMessage(true).print(az);
  sendMessage(true).print(",MX="); sendMessage(true).print(mx);
  sendMessage(true).print(",MY="); sendMessage(true).print(my);
  sendMessage(true).print(",MZ="); sendMessage(true).print(mz);
  sendMessage(true).print(",BEARING="); sendMessage(true).print(bearing, 1);
  sendMessage(true).print(",INCL="); sendMessage(true).print(inclination, 1);
  sendMessage(true).print(",GLEN="); sendMessage(true).print(gLength, 3);
  sendMessage(true).print(",MLEN="); sendMessage(true).print(mLength, 3);
  sendMessage(true).print(",CAL="); sendMessage(true).println(heebCalibrationValid ? "SHOT" : "ROUGH");
  reading = false;
  laserCycleState = 0;
}

void loop() {
  pollCommands();
  serviceAccelerometerCalibration();
  serviceMagnetometerCalibration();
  serviceHeebCalibration();
  takeDistanceReading();
  takeRotationReading();
}