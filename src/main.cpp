#include <Arduino.h>
#include <SPI.h>

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

static inline uint32_t bcd32(const uint8_t* b){
  uint32_t v=0; for(int i=0;i<4;i++){ v=v*100 + ((b[i]>>4)&0x0F)*10 + (b[i]&0x0F); } return v;
}

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
  delay(200);
  Serial1.write(laserOnCmd, sizeof(laserOnCmd));
  delay(1500);
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
  
  Serial.println("--- System Ready ---");
}

void loop() {
  // --- M01 Laser Measurement ---
  while (Serial1.available()) Serial1.read();
  Serial1.write(laserOnCmd, sizeof(laserOnCmd));
  delay(500);
  while (Serial1.available()) Serial1.read();    
  Serial1.write(measureCmd, sizeof(measureCmd));
  delay(2500);

  if (Serial1.available() >= 13) {
    uint8_t response[13];
    for (int i = 0; i < 13; i++) response[i] = Serial1.read();
    if (response[0] == 0xAA) {
      uint16_t distanceMm = bcd32(&response[6]);
      Serial.print("Distance: "); Serial.print(distanceMm); Serial.println(" mm");
    }
  }

  // --- ADXL355 Read ---
  int32_t ax = readADXL355Axis(ADXL355_XDATA3);
  int32_t ay = readADXL355Axis(ADXL355_XDATA3 + 3);
  int32_t az = readADXL355Axis(ADXL355_XDATA3 + 6);
  
  Serial.print("ADXL355 Raw (X,Y,Z): ");
  Serial.print(ax); Serial.print(", ");
  Serial.print(ay); Serial.print(", ");
  Serial.println(az);

  // --- RM3100 Read ---
  int32_t mx = readRM3100Axis(RM3100_MX2_REG);
  int32_t my = readRM3100Axis(RM3100_MX2_REG + 3);
  int32_t mz = readRM3100Axis(RM3100_MX2_REG + 6);

  Serial.print("RM3100 Raw (X,Y,Z): ");
  Serial.print(mx); Serial.print(", ");
  Serial.print(my); Serial.print(", ");
  Serial.println(mz);

  Serial.println("---");
  delay(1000);
}