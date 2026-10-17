
#include <Arduino.h>
#include <SPI.h>
#include <string.h>

// --- RM3100 SPI Pins (same as main firmware) ---
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23
#define RM3100_CS 15

// --- RM3100 Register Definitions ---
#define RM3100_REVID_REG    0x36
#define RM3100_CMM_REG      0x01
#define RM3100_STATUS_REG   0x34
#define RM3100_CCX1_REG     0x04
#define RM3100_MX2_REG      0x24
#define RM3100_TMRC_REG     0x0B

constexpr size_t MAX_SAMPLES = 5000;
constexpr unsigned long SAMPLE_INTERVAL_MS = 100;

struct MagnetometerSample {
  int32_t x;
  int32_t y;
  int32_t z;
};

MagnetometerSample sampleBuffer[MAX_SAMPLES];
size_t sampleCount = 0;
bool captureRunning = false;
unsigned long lastSampleTime = 0;

void rm3100_writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg & 0x7F);
  SPI.transfer(value);
  digitalWrite(RM3100_CS, HIGH);
}

uint8_t rm3100_readRegister(uint8_t reg) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg | 0x80);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(RM3100_CS, HIGH);
  return value;
}

void initRM3100() {
  uint8_t revid = rm3100_readRegister(RM3100_REVID_REG);
  Serial.print("RM3100 REVID: 0x"); Serial.println(revid, HEX);

  // Reset CMM before writing TMRC
  rm3100_writeRegister(RM3100_CMM_REG, 0x00);
  delay(10);

  // Set cycle count to 200 (0x00C8) for all three axes
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(RM3100_CCX1_REG & 0x7F);
  SPI.transfer(0x00); SPI.transfer(0xC8); // X
  SPI.transfer(0x00); SPI.transfer(0xC8); // Y
  SPI.transfer(0x00); SPI.transfer(0xC8); // Z
  digitalWrite(RM3100_CS, HIGH);
  delay(10);

  // Set update rate (~37 Hz)
  rm3100_writeRegister(RM3100_TMRC_REG, 0x96);
  delay(10);

  // Continuous measurement mode, all 3 axes
  rm3100_writeRegister(RM3100_CMM_REG, 0x79);
  delay(10);

  Serial.println("RM3100 Initialized");
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

void setup() {
  Serial.begin(115200);

  pinMode(RM3100_CS, OUTPUT);
  digitalWrite(RM3100_CS, HIGH);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  initRM3100();

  Serial.println("--- RM3100 capture ready ---");
  Serial.println("Commands: R=run, S=stop and dump, C=clear buffer");
}

void dumpSamples() {
  Serial.println("MX,MY,MZ");
  for (size_t i = 0; i < sampleCount; ++i) {
    Serial.print(sampleBuffer[i].x);
    Serial.print(",");
    Serial.print(sampleBuffer[i].y);
    Serial.print(",");
    Serial.println(sampleBuffer[i].z);
  }
  Serial.print("Samples: ");
  Serial.println(sampleCount);
}

void handleCommand(char command) {
  switch (command) {
    case 'R':
      if (sampleCount == MAX_SAMPLES) {
        Serial.println("Buffer full; send C to clear it before running again.");
        break;
      }
      captureRunning = true;
      lastSampleTime = millis();
      Serial.println("Capture running.");
      break;

    case 'S':
      captureRunning = false;
      Serial.println("Capture stopped; dumping buffer.");
      dumpSamples();
      break;

    case 'C':
      captureRunning = false;
      memset(sampleBuffer, 0, sizeof(sampleBuffer));
      sampleCount = 0;
      Serial.println("Buffer cleared.");
      break;
  }
}

void loop() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command >= 'a' && command <= 'z') {
      handleCommand(static_cast<char>(command - 'a' + 'A'));
    } else if (command != '\r' && command != '\n') {
      handleCommand(command);
    }
  }

  const unsigned long now = millis();
  if (captureRunning && now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    sampleBuffer[sampleCount].x = readRM3100Axis(RM3100_MX2_REG);
    sampleBuffer[sampleCount].y = readRM3100Axis(RM3100_MX2_REG + 3);
    sampleBuffer[sampleCount].z = readRM3100Axis(RM3100_MX2_REG + 6);
    ++sampleCount;

    if (sampleCount == MAX_SAMPLES) {
      captureRunning = false;
      Serial.println("Buffer full; capture stopped. Send S to dump or C to clear.");
    }
  }
}