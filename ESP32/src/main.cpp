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

// Fixed correction added to every laser distance, e.g. to correct the module
// against a steel tape or to move the datum to the rear of the enclosure.
int32_t distanceOffsetMm = 0;
const int32_t MAX_DISTANCE_OFFSET_MM = 2000;
bool distanceOffsetEntryActive = false;
char distanceOffsetEntry[8];
uint8_t distanceOffsetEntryLength = 0;

// Push button: the first press turns the laser on for aiming and the second
// takes the reading. The laser turns off again if no reading is taken in time.
const unsigned long BUTTON_DEBOUNCE_MS = 30;
const unsigned long AIM_TIMEOUT_MS = 60000;
bool buttonAiming = false;
unsigned long aimStartTime = 0;
int buttonStableLevel = HIGH;
int buttonLastLevel = HIGH;
unsigned long buttonLevelChangeTime = 0;

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

void processInputCharacter(char input);

bool isAnyCalibrationActive() {
  return isMagnetometerCalibrationActive() || isAccelerometerCalibrationActive() ||
         isHeebCalibrationActive();
}

void startReading() {
  if (isAnyCalibrationActive()) {
    sendMessage(true).println("Finish calibration before taking a reading.");
    return;
  }
  if (reading) {
    sendMessage(true).println("A reading is already in progress.");
    return;
  }
  reading = true;
  buttonAiming = false;
  if (laserState == 1) {
    // Already on for aiming, so measure straight away.
    laserCycleState = 2;
    laserDeadline = millis();
  } else {
    laserCycleState = 0;
    nextLaserCycle = millis();
  }
}

void handleButtonPress() {
  // During calibration the button stands in for the key that records a point.
  if (isHeebCalibrationActive()) {
    processHeebCalibrationInput('T');
    return;
  }
  if (isAccelerometerCalibrationActive() || isMagnetometerCalibrationActive()) {
    processInputCharacter('\r');
    return;
  }
  if (reading) return;
  if (!buttonAiming || laserState == 0) {
    turnLaserOn();
    buttonAiming = true;
    aimStartTime = millis();
    return;
  }
  playSound(Sound::Beep);
  startReading();
}

void pollButton() {
  const int level = digitalRead(BUTTON_PIN);
  const unsigned long now = millis();
  if (level != buttonLastLevel) {
    buttonLastLevel = level;
    buttonLevelChangeTime = now;
  }
  if (level != buttonStableLevel && now - buttonLevelChangeTime >= BUTTON_DEBOUNCE_MS) {
    buttonStableLevel = level;
    if (level == LOW) handleButtonPress();
  }
  if (buttonAiming && now - aimStartTime >= AIM_TIMEOUT_MS) {
    buttonAiming = false;
    if (!reading && !isAnyCalibrationActive()) turnLaserOff();
  }
}

void loadDistanceOffset() {
  distanceOffsetMm = 0;
  if (!calibrationPreferences.begin("laser", true)) return;
  distanceOffsetMm = calibrationPreferences.getInt("offset_mm", 0);
  calibrationPreferences.end();
}

bool saveDistanceOffset(int32_t offsetMm) {
  if (!calibrationPreferences.begin("laser", false)) return false;
  const bool saved = calibrationPreferences.putInt("offset_mm", offsetMm) > 0;
  calibrationPreferences.end();
  return saved;
}

// Machine-readable status line for the Android app; STATUS is CURRENT, SAVED,
// INVALID or FAILED and DIST_OFFSET_MM is always the offset now in use.
void reportDistanceOffset(const char* status) {
  sendMessage(true).printf("DIST_OFFSET_MM=%ld,STATUS=%s\n",
                           static_cast<long>(distanceOffsetMm), status);
}

// "O" reports the offset; "O<mm>" (e.g. O125 or O-12) sets and saves it.
// Both are terminated by CR or LF.
void finishDistanceOffsetEntry() {
  distanceOffsetEntryActive = false;
  distanceOffsetEntry[distanceOffsetEntryLength] = '\0';
  if (distanceOffsetEntryLength == 0) {
    reportDistanceOffset("CURRENT");
    return;
  }

  char* end = nullptr;
  const long value = strtol(distanceOffsetEntry, &end, 10);
  if (end == distanceOffsetEntry || *end != '\0' ||
      value < -MAX_DISTANCE_OFFSET_MM || value > MAX_DISTANCE_OFFSET_MM) {
    sendMessage(true).printf("Invalid distance offset; use O<mm> between %ld and %ld.\n",
                             static_cast<long>(-MAX_DISTANCE_OFFSET_MM),
                             static_cast<long>(MAX_DISTANCE_OFFSET_MM));
    reportDistanceOffset("INVALID");
    return;
  }
  if (!saveDistanceOffset(value)) {
    sendMessage(true).println("Could not save distance offset; offset unchanged.");
    reportDistanceOffset("FAILED");
    return;
  }
  distanceOffsetMm = value;
  reportDistanceOffset("SAVED");
}

bool processDistanceOffsetInput(char input) {
  if (!distanceOffsetEntryActive) return false;
  if (input == '\r' || input == '\n') {
    finishDistanceOffsetEntry();
  } else if (input != ' ' && input != '\t' &&
             distanceOffsetEntryLength < sizeof(distanceOffsetEntry) - 1) {
    distanceOffsetEntry[distanceOffsetEntryLength++] = input;
  } else if (input != ' ' && input != '\t') {
    // Too long to be a valid offset; keep the entry so it is rejected at the end.
    distanceOffsetEntry[sizeof(distanceOffsetEntry) - 2] = 'x';
  }
  return true;
}

void startDistanceOffsetEntry() {
  distanceOffsetEntryActive = true;
  distanceOffsetEntryLength = 0;
}

void handleCommand(char command) {
  switch (toupper(static_cast<unsigned char>(command))) {
    case 'R': startReading(); break;
    case 'O': startDistanceOffsetEntry(); break;
    case 'M': startMagnetometerCalibration(); break;
    case 'C': startHeebCalibration(); break;
    case 'F': refitStoredHeebShots(); break;
    case 'D': dumpHeebCalibration(); break;
    case 'E': eraseHeebCalibration(); break;
    case 'A': startAccelerometerCalibration(); break;
    case 'X': eraseMagCalibration(); break;
    case 'H':
    case '?':
      sendMessage(true).println("Commands: R=read, O=show distance offset, O<mm>=set distance offset, C=shot calibration, F=refit stored shots, D=dump shot calibration, E=erase shot calibration, A=rough accel calibration, M=rough mag calibration, X=erase mag calibration, H=help");
      break;
    default:
      sendMessage(true).println("Unknown command. Use R, O, C, F, D, E, A, M, X, or H.");
      break;
  }
}

void processInputCharacter(char input) {
  if (processDistanceOffsetInput(input) ||
      processMagnetometerCalibrationInput(input) ||
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
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  initBuzzer();
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
  loadDistanceOffset();
  printCalibrationToSerial();
  reportDistanceOffset("CURRENT");

  sendMessage(true).println("--- System Ready ---");
  playSound(Sound::Beep);
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
    // Stays 0 (reported as a failed shot) unless a valid response arrives.
    lastDistanceMm = 0;
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
  const bool orientationValid = laserBearingInclination(g, m, bearing, inclination);
  // Field and gravity magnitudes are 1.0 under the shot calibration; a large
  // deviation in MLEN suggests local magnetic interference.
  const float gLength = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
  const float mLength = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);

  // A zero reading means the laser returned no measurement, so it is left
  // uncorrected rather than reported as just the offset.
  const int32_t distanceMm = lastDistanceMm > 0
    ? static_cast<int32_t>(lastDistanceMm) + distanceOffsetMm : 0;

  sendMessage(true).print("DIST_MM="); sendMessage(true).print(distanceMm);
  sendMessage(true).print(",LASER_MM="); sendMessage(true).print(lastDistanceMm);
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
  playSound(distanceMm > 0 && orientationValid ? Sound::Success : Sound::Error);
  reading = false;
  laserCycleState = 0;
}

void loop() {
  pollCommands();
  pollButton();
  serviceAccelerometerCalibration();
  serviceMagnetometerCalibration();
  serviceHeebCalibration();
  takeDistanceReading();
  takeRotationReading();
}