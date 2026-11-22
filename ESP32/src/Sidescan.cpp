#include <Wire.h>
#include <math.h>
#include <vl53l4cd_class.h>

#include "Calibration.h"

// SACSI side scan. Between Go and Stop:
// - the M01 laser runs in continuous mode (~3 readings/s) and the latest
//   distance is kept as Z (down the shaft);
// - the VL53L4CD measures sideways every SCAN_INTERVAL_MS on its own clock;
// - each VL53L4CD result is sent at once as a SCAN line with the orientation
//   read at that moment, so the heading matches the wall reading.
//
// Lines sent:
//   SCAN,STATE=STARTED
//   SCAN,N=<seq>,T=<ms since Go>,Z_MM=<mm|blank>,Z_AGE_MS=<ms|blank>,LASER_ERR=<code>,
//        Y_MM=<mm|blank>,Y_STATUS=<code|blank>,HEADING=<deg|nan>,YINCL=<deg>,TILT=<deg>,
//        AX=..,AY=..,AZ=..,MX=..,MY=..,MZ=..,CAL=SHOT|ROUGH
//   SCAN,STATE=STOPPED,SAMPLES=<n>,TOF_ERRORS=<n>
//   LASER_OFF=<how>,COMMANDS=<n>  once the laser has stopped, where <how> is
//        LASER_OFF (the laser off command did it), TOGGLED (a second
//        continuous command did it) or POWER_CYCLED (ENA, last resort)
// Z_MM includes the distance offset. LASER_ERR is the M01 error code from the
// most recent laser frame (0 when that frame was a measurement). HEADING and
// YINCL are the side beam's direction, TILT is the laser's angle from
// straight down.

namespace {

// Laser frames: AA, addr, reg(2), count(2), payload, checksum (sum of the
// bytes after AA). Measurement replies (reg 0x0020-0x0022, count 4) are 13
// bytes with the distance as BCD in bytes 6-9; other replies carry count * 2
// payload bytes. Errors come back on reg 0x0000.
//
// Ending continuous mode differs between M01 modules (test/laserSpeedTest.cpp):
// on the first, the laser off command ended it; on the second, laser off is
// acknowledged (AA 00 01 BE 00 01 00 00 C0) but ignored, and a second
// continuous command toggles continuous mode off. So Stop tries laser off,
// then the toggle if readings carry on. Because of the toggle, the continuous
// command must never be resent while a scan is running.
const uint8_t continuousCmd[] = {0xAA, 0x00, 0x00, 0x21, 0x00, 0x01, 0x00, 0x00, 0x22};
const size_t MAX_FRAME = 32;
// Laser on, then the continuous command after this delay (as in the bench test).
const unsigned long LASER_START_DELAY_MS = 500;
// Samples wait this long for the first laser reading, then start without one.
const unsigned long FIRST_LASER_TIMEOUT_MS = 2500;
// Stop: after each command, a reading that arrives once the command has been
// acknowledged (or STOP_ACK_TIMEOUT_MS after it, if no acknowledgement comes)
// means the laser is still measuring, so the other command is tried. The
// laser counts as stopped after STOP_QUIET_MS with no readings. After
// MAX_STOP_COMMANDS, ENA is pulled low as a last resort.
const unsigned long STOP_ACK_TIMEOUT_MS = 1500;
const unsigned long STOP_QUIET_MS = 2500;
const uint8_t MAX_STOP_COMMANDS = 6;
const unsigned long ENA_OFF_MS = 1000;
const unsigned long ENA_BOOT_MS = 500;
// Emit a sample without a side reading if the VL53L4CD misses its slot.
const unsigned long TOF_MISSING_MS = SCAN_INTERVAL_MS + 150;
// Every poll is an I2C transaction. Polling starts TOF_EARLY_POLL_MS before
// the next result is due and repeats every TOF_POLL_MS until it arrives.
const unsigned long TOF_POLL_MS = 10;
const unsigned long TOF_EARLY_POLL_MS = 40;
// Short I2C timeout so a bus error costs one sample, not a second of samples.
const uint16_t I2C_TIMEOUT_MS = 20;

enum class Phase : uint8_t { Idle, Starting, WaitingForLaser, Running, Stopping };
enum class StopStep : uint8_t { AfterLaserOff, AfterToggle, EnaLow, EnaBoot };

VL53L4CD tof(&Wire, VL53L4CD_XSHUT);
bool tofAvailable = false;

Phase phase = Phase::Idle;
unsigned long phaseStart = 0;
unsigned long scanStart = 0;
uint32_t sampleCount = 0;
uint32_t tofErrors = 0;
unsigned long lastSampleMs = 0;
unsigned long nextTofPollMs = 0;
unsigned long lastBatteryMs = 0;
StopStep stopStep = StopStep::AfterLaserOff;
unsigned long stopStepStart = 0;
unsigned long stopQuietSince = 0;
uint8_t stopCommands = 0;
bool stopStepAcked = false;
// Set by pollLaser when the matching acknowledgement arrives.
bool laserOffAcked = false;
bool continuousAcked = false;

bool haveLaserDistance = false;
uint32_t laserDistanceMm = 0;
unsigned long laserDistanceMs = 0;
uint16_t laserError = 0;

uint8_t frame[MAX_FRAME];
size_t frameLength = 0;
size_t frameExpected = 7;

uint32_t bcd32(const uint8_t* b) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = v * 100 + ((b[i] >> 4) & 0x0F) * 10 + (b[i] & 0x0F);
  return v;
}

bool isMeasurementReg(const uint8_t* f) {
  return f[2] == 0x00 && f[3] >= 0x20 && f[3] <= 0x22;
}

void resetFrame() {
  frameLength = 0;
  frameExpected = 7;
}

// Feeds one byte; returns true when a complete frame with a good checksum is in `frame`.
bool feedLaserByte(uint8_t byte) {
  if (frameLength == 0 && byte != 0xAA) return false;
  frame[frameLength++] = byte;
  if (frameLength == 6) {
    const size_t count = (size_t(frame[4]) << 8) | frame[5];
    frameExpected = isMeasurementReg(frame) && count == 4 ? 13 : 7 + 2 * count;
    if (frameExpected > MAX_FRAME) {
      resetFrame();
      return false;
    }
  }
  if (frameLength < frameExpected) return false;
  uint8_t sum = 0;
  for (size_t i = 1; i < frameLength - 1; ++i) sum += frame[i];
  const bool ok = sum == frame[frameLength - 1];
  if (!ok) resetFrame();
  return ok;
}

// Reads whatever the laser has sent. Returns true if a measurement arrived.
bool pollLaser() {
  bool measured = false;
  while (Serial1.available()) {
    if (!feedLaserByte(static_cast<uint8_t>(Serial1.read()))) continue;
    const unsigned long now = millis();
    if (frameLength == 13 && isMeasurementReg(frame)) {
      laserDistanceMm = bcd32(&frame[6]);
      laserDistanceMs = now;
      haveLaserDistance = true;
      laserError = 0;
      measured = true;
    } else if (frameLength == 9 && frame[2] == 0x00 && frame[3] == 0x00) {
      laserError = (uint16_t(frame[6]) << 8) | frame[7];
    } else if (frameLength == 9 && frame[2] == 0x01 && frame[3] == 0xBE &&
               frame[6] == 0x00 && frame[7] == 0x00) {
      laserOffAcked = true;  // AA 00 01 BE 00 01 00 00 C0
    } else if (frameLength == 9 && frame[2] == 0x00 && frame[3] == 0x21) {
      continuousAcked = true;  // AA 00 00 21 00 01 00 00 22
    }
    resetFrame();
  }
  return measured;
}

void printOptional(Print& out, const char* key, bool valid, long value) {
  out.print(key);
  if (valid) out.print(value);
}

void printFloat(Print& out, const char* key, float value, int decimals) {
  out.print(key);
  if (isnan(value)) {
    out.print("nan");
  } else {
    out.print(value, decimals);
  }
}

void sendSample(bool haveSide, const VL53L4CD_Result_t& side) {
  const unsigned long now = millis();
  int32_t accel[3], mag[3];
  readAccelerometer(accel);
  readMagnetometer(mag);
  const float rawAccel[3] = {float(accel[0]), float(accel[1]), float(accel[2])};
  const float rawMag[3] = {float(mag[0]), float(mag[1]), float(mag[2])};
  float g[3], m[3];
  calibratedVectors(rawAccel, rawMag, g, m);

  const float beam[3] = {SIDESCAN_DIR_X, SIDESCAN_DIR_Y, SIDESCAN_DIR_Z};
  float heading, sideInclination;
  beamBearingInclination(g, m, beam, heading, sideInclination);
  float laserBearing, laserInclination;
  laserBearingInclination(g, m, laserBearing, laserInclination);
  const float tilt = 90.0f + laserInclination;  // 0 when the laser points straight down

  MessageOutput out = sendMessage(true);
  out.printf("SCAN,N=%lu,T=%lu", static_cast<unsigned long>(++sampleCount),
             static_cast<unsigned long>(now - scanStart));
  printOptional(out, ",Z_MM=", haveLaserDistance,
                static_cast<long>(laserDistanceMm) + distanceOffsetMm);
  printOptional(out, ",Z_AGE_MS=", haveLaserDistance, static_cast<long>(now - laserDistanceMs));
  out.printf(",LASER_ERR=%u", laserError);
  printOptional(out, ",Y_MM=", haveSide, side.distance_mm);
  printOptional(out, ",Y_STATUS=", haveSide, side.range_status);
  printFloat(out, ",HEADING=", heading, 1);
  printFloat(out, ",YINCL=", sideInclination, 1);
  printFloat(out, ",TILT=", tilt, 1);
  out.printf(",AX=%ld,AY=%ld,AZ=%ld,MX=%ld,MY=%ld,MZ=%ld,CAL=%s\n",
             static_cast<long>(accel[0]), static_cast<long>(accel[1]), static_cast<long>(accel[2]),
             static_cast<long>(mag[0]), static_cast<long>(mag[1]), static_cast<long>(mag[2]),
             heebCalibrationValid ? "SHOT" : "ROUGH");
  lastSampleMs = now;
}

void serviceRunning() {
  const unsigned long now = millis();
  // A silent laser is left alone: resending the continuous command would
  // switch continuous mode off on modules that toggle. Z_AGE_MS shows it.
  pollLaser();

  if (now - lastBatteryMs >= SCAN_BATTERY_REPORT_MS) {
    lastBatteryMs = now;
    reportBattery();
  }

  if (tofAvailable && static_cast<long>(now - nextTofPollMs) >= 0) {
    nextTofPollMs = now + TOF_POLL_MS;
    uint8_t ready = 0;
    if (tof.VL53L4CD_CheckForDataReady(&ready) != VL53L4CD_ERROR_NONE) {
      ++tofErrors;
    } else if (ready) {
      VL53L4CD_Result_t result;
      const bool readOk = tof.VL53L4CD_GetResult(&result) == VL53L4CD_ERROR_NONE;
      if (tof.VL53L4CD_ClearInterrupt() != VL53L4CD_ERROR_NONE || !readOk) ++tofErrors;
      // The next result is due one interval from now, so leave the bus alone
      // until shortly before then: every I2C transaction risks an error, and
      // an I2C error stalls the loop for over a second.
      nextTofPollMs = now + SCAN_INTERVAL_MS - TOF_EARLY_POLL_MS;
      // A failed read is sent without a side distance rather than as a bogus one.
      sendSample(readOk, result);
      return;
    }
  }
  if (now - lastSampleMs >= TOF_MISSING_MS) {
    const VL53L4CD_Result_t none = {};
    sendSample(false, none);
  }
}

// Sends laser off (AfterLaserOff) or the continuous toggle (AfterToggle) and
// starts watching for readings.
void beginStopStep(StopStep step, unsigned long now) {
  stopStep = step;
  stopStepStart = now;
  stopQuietSince = now;
  stopStepAcked = false;
  laserOffAcked = false;
  continuousAcked = false;
  ++stopCommands;
  if (step == StopStep::AfterLaserOff) {
    turnLaserOff();
  } else {
    Serial1.write(continuousCmd, sizeof(continuousCmd));
  }
}

void beginPowerCycle(unsigned long now) {
  digitalWrite(ENA_PIN, LOW);
  stopStep = StopStep::EnaLow;
  stopStepStart = now;
}

void finishStop(const char* how) {
  while (Serial1.available()) Serial1.read();
  resetFrame();
  phase = Phase::Idle;
  sendMessage(true).printf("LASER_OFF=%s,COMMANDS=%u\n", how, stopCommands);
}

void serviceStopping() {
  const unsigned long now = millis();
  const bool measured = pollLaser();
  switch (stopStep) {
    case StopStep::AfterLaserOff:
    case StopStep::AfterToggle: {
      const bool acked = stopStep == StopStep::AfterLaserOff ? laserOffAcked : continuousAcked;
      if (acked && !stopStepAcked) {
        stopStepAcked = true;
        stopQuietSince = now;
      }
      if (measured) {
        stopQuietSince = now;
        // Readings before the acknowledgement were already on their way;
        // after it, the laser is still measuring, so try the other command.
        if (stopStepAcked || now - stopStepStart >= STOP_ACK_TIMEOUT_MS) {
          if (stopCommands >= MAX_STOP_COMMANDS) {
            beginPowerCycle(now);
          } else {
            beginStopStep(stopStep == StopStep::AfterLaserOff ? StopStep::AfterToggle
                                                              : StopStep::AfterLaserOff, now);
          }
          break;
        }
      }
      if (now - stopQuietSince >= STOP_QUIET_MS) {
        if (stopStep == StopStep::AfterToggle) {
          // Continuous mode has ended; make sure the laser itself is off too.
          turnLaserOff();
          finishStop("TOGGLED");
        } else {
          finishStop("LASER_OFF");
        }
      }
      break;
    }
    case StopStep::EnaLow:
      if (now - stopStepStart >= ENA_OFF_MS) {
        digitalWrite(ENA_PIN, HIGH);
        stopStep = StopStep::EnaBoot;
        stopStepStart = now;
      }
      break;
    case StopStep::EnaBoot:
      // The module restarts with the laser off; send laser off anyway in case
      // it resumes its previous mode (seen with the first M01).
      if (now - stopStepStart >= ENA_BOOT_MS) {
        turnLaserOff();
        finishStop("POWER_CYCLED");
      }
      break;
  }
}

}  // namespace

void initSidescan() {
  Wire.begin(I2C_SDA, I2C_SCL);
  // 100 kHz is ample for one reading every 250 ms and more tolerant of the
  // wiring and of noise from the laser than 400 kHz.
  Wire.setClock(100000);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  tof.begin();
  // Cycles XSHUT, checks the sensor ID and loads the default configuration.
  tofAvailable = tof.InitSensor() == VL53L4CD_ERROR_NONE &&
                 tof.VL53L4CD_SetRangeTiming(SCAN_TOF_BUDGET_MS, SCAN_INTERVAL_MS) ==
                     VL53L4CD_ERROR_NONE;
  sendMessage(true).println(tofAvailable
      ? "VL53L4CD Initialized"
      : "VL53L4CD not found; scans will have no side distances.");
}

bool isScanBusy() { return phase != Phase::Idle; }
bool isScanRunning() {
  return phase == Phase::Starting || phase == Phase::WaitingForLaser || phase == Phase::Running;
}

void startScan() {
  if (isScanBusy()) {
    sendMessage(true).println("A scan is already running.");
    return;
  }
  if (reading || isMagnetometerCalibrationActive() || isAccelerometerCalibrationActive() ||
      isHeebCalibrationActive()) {
    sendMessage(true).println("Finish the reading or calibration before starting a scan.");
    return;
  }
  reportBattery();
  while (Serial1.available()) Serial1.read();
  resetFrame();
  turnLaserOn();
  if (tofAvailable) tof.VL53L4CD_StartRanging();

  const unsigned long now = millis();
  phase = Phase::Starting;
  phaseStart = now;
  scanStart = now;
  sampleCount = 0;
  tofErrors = 0;
  lastSampleMs = now;
  lastBatteryMs = now;
  haveLaserDistance = false;
  laserError = 0;
  sendMessage(true).println("SCAN,STATE=STARTED");
  playSound(Sound::Beep);
}

void stopScan() {
  if (!isScanRunning()) {
    // Still useful as a manual "laser off" if the laser was left lit.
    turnLaserOff();
    sendMessage(true).println("No scan is running; laser off sent.");
    return;
  }
  if (tofAvailable) tof.VL53L4CD_StopRanging();
  phase = Phase::Stopping;
  stopCommands = 0;
  beginStopStep(StopStep::AfterLaserOff, millis());
  sendMessage(true).printf("SCAN,STATE=STOPPED,SAMPLES=%lu,TOF_ERRORS=%lu\n",
                           static_cast<unsigned long>(sampleCount),
                           static_cast<unsigned long>(tofErrors));
  playSound(Sound::Success);
}

void serviceScan() {
  switch (phase) {
    case Phase::Idle:
      break;
    case Phase::Starting:
      // The module needs a moment after laser on before it takes the next command.
      if (millis() - phaseStart >= LASER_START_DELAY_MS) {
        while (Serial1.available()) Serial1.read();
        resetFrame();
        Serial1.write(continuousCmd, sizeof(continuousCmd));
        phase = Phase::WaitingForLaser;
        phaseStart = millis();
      }
      break;
    case Phase::WaitingForLaser:
      // Samples start with the first laser reading so every one carries a Z.
      if (pollLaser() || millis() - phaseStart >= FIRST_LASER_TIMEOUT_MS) {
        if (tofAvailable) tof.VL53L4CD_ClearInterrupt();  // drop the stale side reading
        lastSampleMs = millis();
        nextTofPollMs = lastSampleMs;
        phase = Phase::Running;
      }
      break;
    case Phase::Running:
      serviceRunning();
      break;
    case Phase::Stopping:
      serviceStopping();
      break;
  }
}
