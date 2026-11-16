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

static VL53L4CD tof(&Wire, VL53L4CD_XSHUT);

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

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("VL53L4CD test");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  tof.begin();         // XSHUT output, held low (sensor off)
  tof.VL53L4CD_On();   // XSHUT high so the sensor answers the scan
  scanI2C();

  // Cycles XSHUT, checks the sensor ID (0xEBAA) and loads the default config.
  VL53L4CD_ERROR status = tof.InitSensor();
  if (status != VL53L4CD_ERROR_NONE) {
    Serial.printf("InitSensor failed (error %u). Halting.\n", status);
    while (true) delay(1000);
  }

  uint16_t sensorId = 0;
  tof.VL53L4CD_GetSensorId(&sensorId);
  Serial.printf("Sensor ID 0x%04X (expect 0xEBAA)\n", sensorId);

  // 100 ms timing budget, continuous ranging; loop() reads the latest result.
  tof.VL53L4CD_SetRangeTiming(100, 0);
  tof.VL53L4CD_StartRanging();
  Serial.println("Ranging. Commands: s (I2C scan)");
}

void loop() {
  if (Serial.available() && Serial.read() == 's') scanI2C();

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
