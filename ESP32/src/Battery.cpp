#include "Calibration.h"

// Battery voltage from the internal ADC. analogReadMilliVolts() applies the
// chip's eFuse ADC calibration; averaging several samples smooths ADC noise.

namespace {

const uint8_t BATTERY_SAMPLES = 16;

}  // namespace

void initBatteryMonitor() {
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
}

uint32_t readBatteryMillivolts() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < BATTERY_SAMPLES; ++i) total += analogReadMilliVolts(BATTERY_PIN);
  const float pinMillivolts = static_cast<float>(total) / BATTERY_SAMPLES;
  return static_cast<uint32_t>(lroundf(pinMillivolts * BATTERY_DIVIDER_RATIO));
}

// Machine-readable status line for the Android app; STATUS is OK, LOW or NONE
// (no battery connected, e.g. running from USB).
bool reportBattery() {
  const uint32_t millivolts = readBatteryMillivolts();
  const char* status = millivolts < BATTERY_PRESENT_MV ? "NONE"
                     : millivolts < BATTERY_LOW_MV     ? "LOW"
                                                       : "OK";
  sendMessage(true).printf("BATTERY_MV=%lu,STATUS=%s\n",
                           static_cast<unsigned long>(millivolts), status);
  return millivolts >= BATTERY_PRESENT_MV && millivolts < BATTERY_LOW_MV;
}
