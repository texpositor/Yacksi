#include <Wire.h>
#include <vl53l4cd_class.h>

#include "Calibration.h"

// Bring-up test for the VL53L4CD side-scan sensor with nothing else attached.
// Scans the I2C bus at boot, then prints one distance reading per second.
//
// Wiring: VIN -> 3V3, GND -> GND, SDA -> I2C_SDA, SCL -> I2C_SCL,
//         XSHUT -> VL53L4CD_XSHUT (GPIO1 not needed for polling).
//
// Serial commands:
//   s  rescan the I2C bus

// Exposes the library's protected register access so the start-up sequence
// can be run one step at a time with each status reported.
class DebugTof : public VL53L4CD {
public:
  using VL53L4CD::VL53L4CD;
  uint8_t rdByte(uint16_t reg, uint8_t* v) { return VL53L4CD_RdByte(dev, reg, v); }
  uint8_t rdWord(uint16_t reg, uint16_t* v) { return VL53L4CD_RdWord(dev, reg, v); }
  uint8_t wrByte(uint16_t reg, uint8_t v) { return VL53L4CD_WrByte(dev, reg, v); }
  uint8_t wrWord(uint16_t reg, uint16_t v) { return VL53L4CD_WrWord(dev, reg, v); }
};

static DebugTof tof(&Wire, VL53L4CD_XSHUT);

// Copy of VL53L4CD_DEFAULT_CONFIGURATION (registers 0x2D-0x87) from the
// library, which keeps it private.
static const uint8_t kDefaultConfig[] = {
  0x12,0x00,0x00,0x11,0x02,0x00,0x02,0x08,0x00,0x08,0x10,0x01,0x01,0x00,0x00,0x00,
  0x00,0xff,0x00,0x0F,0x00,0x00,0x00,0x00,0x00,0x20,0x0b,0x00,0x00,0x02,0x14,0x21,
  0x00,0x00,0x05,0x00,0x00,0x00,0x00,0xc8,0x00,0x00,0x38,0xff,0x01,0x00,0x08,0x00,
  0x00,0x01,0xcc,0x07,0x01,0xf1,0x05,0x00,0xa0,0x00,0x80,0x08,0x38,0x00,0x00,0x00,
  0x00,0x0f,0x89,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x07,0x05,0x06,0x06,0x00,
  0x00,0x02,0xc7,0xff,0x9B,0x00,0x00,0x00,0x01,0x00,0x00,
};
static_assert(sizeof(kDefaultConfig) == 0x87 - 0x2D + 1, "config table size");

// The library's InitSensor + SensorInit, step by step, reporting each status.
// Library read/write status: 0 ok, non-zero failed (for writes it is the
// endTransmission code: 2 address NACK, 3 data NACK, 4 other, 5 timeout).
static void stepInit() {
  Serial.println("--- Step-by-step init ---");
  tof.VL53L4CD_Off();
  tof.VL53L4CD_On();

  uint16_t id = 0;
  uint8_t st = tof.VL53L4CD_GetSensorId(&id);
  Serial.printf("GetSensorId: status %u id 0x%04X (expect 0 0xEBAA)\n", st, id);

  uint8_t boot = 0;
  unsigned long t0 = millis();
  int tries = 0;
  do {
    st = tof.rdByte(0x00E5, &boot);
    if (boot == 3) break;
    delay(1);
  } while (++tries < 1000);
  Serial.printf("Boot wait: status %u value %u after %lu ms (expect 0 3)\n",
                st, boot, millis() - t0);

  int failures = 0;
  for (uint16_t reg = 0x2D; reg <= 0x87; ++reg) {
    st = tof.wrByte(reg, kDefaultConfig[reg - 0x2D]);
    if (st && failures++ < 8) Serial.printf("  config write 0x%02X failed: %u\n", reg, st);
  }
  Serial.printf("Config writes: %d failed of %d\n", failures, 0x87 - 0x2D + 1);

  // Read some back to see whether the writes landed.
  for (uint16_t reg : {0x2D, 0x30, 0x46, 0x6F}) {
    uint8_t v = 0;
    st = tof.rdByte(reg, &v);
    Serial.printf("  readback 0x%02X: status %u value 0x%02X (wrote 0x%02X)\n",
                  reg, st, v, kDefaultConfig[reg - 0x2D]);
  }

  st = tof.wrByte(0x0087, 0x40);  // start VHV (one-off ranging)
  Serial.printf("Start VHV: status %u\n", st);
  uint8_t ready = 0;
  t0 = millis();
  tries = 0;
  do {
    st = tof.VL53L4CD_CheckForDataReady(&ready);
    if (ready) break;
    delay(1);
  } while (++tries < 1000);
  Serial.printf("VHV data ready: status %u ready %u after %lu ms (expect 0 1)\n",
                st, ready, millis() - t0);

  Serial.printf("ClearInterrupt: %u\n", tof.VL53L4CD_ClearInterrupt());
  Serial.printf("StopRanging: %u\n", tof.VL53L4CD_StopRanging());
  Serial.printf("VHV bound write: %u\n", tof.wrByte(0x0008, 0x09));
  Serial.printf("0x0B write: %u\n", tof.wrByte(0x000B, 0));
  Serial.printf("0x0024 write: %u\n", tof.wrWord(0x0024, 0x500));

  uint16_t osc = 0;
  st = tof.rdWord(0x0006, &osc);
  Serial.printf("Oscillator 0x0006: status %u value %u (must be non-zero)\n", st, osc);
  Serial.printf("SetRangeTiming(50,0): %u (expect 0)\n", tof.VL53L4CD_SetRangeTiming(50, 0));
  Serial.println("--- End step-by-step init ---");
}

static const char* rangeStatusName(uint8_t status) {
  switch (status) {
    case 0: return "valid";
    case 1: return "sigma above threshold";
    case 2: return "signal below threshold";
    case 3: return "below minimum range";
    case 4: return "phase out of limits";
    case 5: return "hardware fail";
    case 6: return "no wraparound check";
    case 7: return "wrapped target";
    case 8: return "processing fail";
    case 9: return "crosstalk fail";
    case 10: return "interrupt error";
    case 11: return "merged target";
    case 12: return "signal too low";
    default: return "other error";
  }
}

static void scanI2C() {
  Serial.println("I2C scan:");
  int found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X%s\n", address,
                    address == 0x29 ? " (VL53L4CD default)" : "");
      ++found;
    }
  }
  if (!found) Serial.println("  nothing found - check power, SDA/SCL and XSHUT");
}

// Reads one 16-bit-addressed register with plain Wire calls and reports each
// step, so a failure shows whether the address, the register write or the
// read itself goes wrong. endTransmission: 0 ok, 2 address NACK, 3 data NACK,
// 4 other error, 5 timeout.
static void rawRead(uint16_t reg, uint8_t expect) {
  Wire.beginTransmission(0x29);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  const uint8_t tx = Wire.endTransmission(false);
  const uint8_t got = Wire.requestFrom((uint8_t)0x29, (uint8_t)1);
  const int value = got ? Wire.read() : -1;
  Serial.printf("  reg 0x%04X: write=%u read %u byte(s) value=0x%02X (expect 0x%02X)%s\n",
                reg, tx, got, value & 0xFF, expect,
                (got && value == expect) ? " OK" : " FAIL");
}

static void scanQuiet(const char* label) {
  int found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  %s: device at 0x%02X\n", label, address);
      ++found;
    }
  }
  if (!found) Serial.printf("  %s: nothing found\n", label);
}

static void diagnose() {
  Serial.println("--- Diagnostics ---");
  Wire.end();  // release the pins so they can be read directly

  // Idle levels with no ESP32 pull-ups: both should read 1 (the breakout's
  // pull-ups). SDA 0 means something is holding the bus.
  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);
  delay(2);
  Serial.printf("Idle levels (external pull-ups only): SDA=%d SCL=%d (expect 1 1)\n",
                digitalRead(I2C_SDA), digitalRead(I2C_SCL));

  // If SDA is held low, clock SCL nine times to release a slave stuck mid-byte.
  if (digitalRead(I2C_SDA) == LOW) {
    Serial.println("SDA held low - clocking SCL to free the bus");
    pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; ++i) {
      digitalWrite(I2C_SCL, LOW);
      delayMicroseconds(10);
      digitalWrite(I2C_SCL, HIGH);
      delayMicroseconds(10);
    }
    pinMode(I2C_SCL, INPUT);
    Serial.printf("  SDA now %d\n", digitalRead(I2C_SDA));
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // With XSHUT low the sensor is shut down and must not answer. If it still
  // answers, the XSHUT wire isn't reaching the sensor, so the library can't
  // reset it.
  pinMode(VL53L4CD_XSHUT, OUTPUT);
  digitalWrite(VL53L4CD_XSHUT, LOW);
  delay(10);
  scanQuiet("XSHUT low (expect nothing)");
  digitalWrite(VL53L4CD_XSHUT, HIGH);
  delay(10);
  scanQuiet("XSHUT high (expect 0x29)");

  Serial.println("Raw reads at 100 kHz:");
  rawRead(0x010F, 0xEB);  // model ID
  rawRead(0x0110, 0xAA);  // module type
  rawRead(0x00E5, 0x03);  // firmware system status: 3 = booted

  Wire.setClock(400000);
  Serial.println("Raw reads at 400 kHz:");
  rawRead(0x010F, 0xEB);
  rawRead(0x0110, 0xAA);
  Wire.setClock(100000);
  Serial.println("--- End diagnostics ---");
}

// Reads the model ID over and over and prints the error count each second, so
// wires and joints can be flexed while watching. Any key stops it.
static void errorRateTest() {
  Serial.println("--- I2C error rate (any key to stop) ---");
  while (Serial.available()) Serial.read();
  unsigned long totalReads = 0, totalErrors = 0;
  while (!Serial.available()) {
    unsigned long reads = 0, nack = 0, timeout = 0, other = 0, bad = 0;
    const unsigned long start = millis();
    while (millis() - start < 1000) {
      Wire.beginTransmission(0x29);
      Wire.write(0x01);
      Wire.write(0x0F);
      const uint8_t tx = Wire.endTransmission(false);
      const uint8_t got = Wire.requestFrom((uint8_t)0x29, (uint8_t)1);
      const int value = got ? Wire.read() : -1;
      ++reads;
      if (tx == 2 || tx == 3) ++nack;
      else if (tx == 5) ++timeout;
      else if (tx != 0 || got != 1) ++other;
      else if (value != 0xEB) ++bad;
    }
    const unsigned long errors = nack + timeout + other + bad;
    totalReads += reads;
    totalErrors += errors;
    Serial.printf("%4lu reads  %3lu errors (nack %lu, timeout %lu, other %lu, wrong value %lu)  "
                  "total %lu/%lu\n",
                  reads, errors, nack, timeout, other, bad, totalErrors, totalReads);
  }
  while (Serial.available()) Serial.read();
  Serial.println("--- End error rate ---");
}

// Timestamped progress marker, flushed so the last one printed shows where a
// hang happened.
static void step(const char* what) {
  Serial.printf("[%6lu ms] %s\n", millis(), what);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("VL53L4CD test");

  diagnose();

  step("tof.begin (XSHUT low)");
  tof.begin();         // XSHUT output, held low (sensor off)
  step("XSHUT high");
  tof.VL53L4CD_On();   // XSHUT high so the sensor answers the scan
  step("I2C scan");
  scanI2C();

  // Cycles XSHUT, checks the sensor ID (0xEBAA) and loads the default config.
  step("InitSensor");
  VL53L4CD_ERROR status = tof.InitSensor();
  if (status != VL53L4CD_ERROR_NONE) {
    Serial.printf("InitSensor failed (error %u).\n", status);
    stepInit();
    Serial.println("Send d to rerun diagnostics, i to rerun the step-by-step init, "
                   "e for the I2C error rate.");
    uint32_t lastAlive = millis();
    while (true) {
      if (Serial.available()) {
        const char c = Serial.read();
        if (c == 'd') diagnose();
        if (c == 'i') stepInit();
        if (c == 'e') errorRateTest();
      }
      if (millis() - lastAlive >= 5000) {
        lastAlive = millis();
        step("halted, still alive");
      }
      delay(10);
    }
  }

  step("GetSensorId");
  uint16_t sensorId = 0;
  tof.VL53L4CD_GetSensorId(&sensorId);
  Serial.printf("Sensor ID 0x%04X (expect 0xEBAA)\n", sensorId);

  // 100 ms timing budget, continuous ranging; loop() reads the latest result.
  step("SetRangeTiming");
  tof.VL53L4CD_SetRangeTiming(100, 0);
  step("StartRanging");
  tof.VL53L4CD_StartRanging();
  Serial.println("Ranging. Commands: s (I2C scan), e (I2C error rate)");
}

void loop() {
  if (Serial.available()) {
    const char c = Serial.read();
    if (c == 's') scanI2C();
    if (c == 'e') errorRateTest();
  }

  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  uint8_t ready = 0;
  tof.VL53L4CD_CheckForDataReady(&ready);
  if (!ready) {
    Serial.println("no new data");
    return;
  }
  tof.VL53L4CD_ClearInterrupt();

  VL53L4CD_Result_t result;
  tof.VL53L4CD_GetResult(&result);
  Serial.printf("distance %4u mm  status %u (%s)  signal %u kcps/spad  ambient %u kcps  sigma %u mm\n",
                result.distance_mm, result.range_status, rangeStatusName(result.range_status),
                result.signal_per_spad_kcps, result.ambient_rate_kcps, result.sigma_mm);
}
