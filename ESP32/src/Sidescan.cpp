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
//   SCAN,STATE=STOPPED,SAMPLES=<n>,TOF_ERRORS=<n>,LASER_RESTARTS=<n>
// Z_MM includes the distance offset. LASER_ERR is the M01 error code from the
// most recent laser frame (0 when that frame was a measurement). HEADING and
// YINCL are the side beam's direction, TILT is the laser's angle from
// straight down.

namespace {

// Laser frames: AA, addr, reg(2), count(2), payload, checksum (sum of the
// bytes after AA). Measurement replies (reg 0x0020-0x0022, count 4) are 13
// bytes with the distance as BCD in bytes 6-9; other replies carry count * 2
// payload bytes. Errors come back on reg 0x0000. The laser off command is the
// only reliable way to end continuous mode (see test/laserSpeedTest.cpp).
const uint8_t continuousCmd[] = {0xAA, 0x00, 0x00, 0x21, 0x00, 0x01, 0x00, 0x00, 0x22};
const size_t MAX_FRAME = 32;
// Laser on, then the continuous command after this delay (as in the bench test).
const unsigned long LASER_START_DELAY_MS = 500;
// Samples wait this long for the first laser reading, then start without one.
const unsigned long FIRST_LASER_TIMEOUT_MS = 2500;
const unsigned long STOP_CHECK_MS = 1500;
const unsigned long STOP_GRACE_MS = 500;
const uint8_t MAX_STOP_ATTEMPTS = 3;
// Emit a sample without a side reading if the VL53L4CD misses its slot.
const unsigned long TOF_MISSING_MS = SCAN_INTERVAL_MS + 150;
// Every poll is an I2C transaction. Polling starts TOF_EARLY_POLL_MS before
// the next result is due and repeats every TOF_POLL_MS until it arrives.
const unsigned long TOF_POLL_MS = 10;
const unsigned long TOF_EARLY_POLL_MS = 40;
const unsigned long SECOND_LASER_OFF_MS = 400;
// Short I2C timeout so a bus error costs one sample, not a second of samples.
const uint16_t I2C_TIMEOUT_MS = 20;

enum class Phase : uint8_t { Idle, Starting, WaitingForLaser, Running, Stopping };

VL53L4CD tof(&Wire, VL53L4CD_XSHUT);
bool tofAvailable = false;

Phase phase = Phase::Idle;
unsigned long phaseStart = 0;
unsigned long scanStart = 0;
uint32_t sampleCount = 0;
uint32_t tofErrors = 0;
uint32_t laserRestarts = 0;
unsigned long lastSampleMs = 0;
unsigned long nextTofPollMs = 0;
unsigned long stopStartMs = 0;
bool secondLaserOffSent = false;
unsigned long lastBatteryMs = 0;
unsigned long lastLaserFrameMs = 0;
unsigned long lastStopCommandMs = 0;
uint8_t stopAttempts = 0;

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
      lastLaserFrameMs = now;
      measured = true;
    } else if (frameLength == 9 && frame[2] == 0x00 && frame[3] == 0x00) {
      laserError = (uint16_t(frame[6]) << 8) | frame[7];
      lastLaserFrameMs = now;
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
  pollLaser();

  if (now - lastLaserFrameMs >= SCAN_LASER_SILENCE_MS) {
    Serial1.write(continuousCmd, sizeof(continuousCmd));
    lastLaserFrameMs = now;  // throttle the retries
    ++laserRestarts;
  }

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

void serviceStopping() {
  const unsigned long now = millis();
  // The first laser off can end continuous mode yet leave the laser lit, so
  // always send a second one once the stream has had time to stop.
  if (!secondLaserOffSent && now - stopStartMs >= SECOND_LASER_OFF_MS) {
    turnLaserOff();
    secondLaserOffSent = true;
  }
  // A measurement well after the laser off command means the module ignored it.
  if (pollLaser() && now - lastStopCommandMs > STOP_GRACE_MS &&
      stopAttempts < MAX_STOP_ATTEMPTS) {
    turnLaserOff();
    lastStopCommandMs = now;
    ++stopAttempts;
  }
  if (now - lastStopCommandMs >= STOP_CHECK_MS) {
    while (Serial1.available()) Serial1.read();
    resetFrame();
    phase = Phase::Idle;
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
  laserRestarts = 0;
  lastSampleMs = now;
  lastBatteryMs = now;
  lastLaserFrameMs = now;
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
  turnLaserOff();
  if (tofAvailable) tof.VL53L4CD_StopRanging();
  phase = Phase::Stopping;
  lastStopCommandMs = millis();
  stopStartMs = lastStopCommandMs;
  secondLaserOffSent = false;
  stopAttempts = 0;
  sendMessage(true).printf("SCAN,STATE=STOPPED,SAMPLES=%lu,TOF_ERRORS=%lu,LASER_RESTARTS=%lu\n",
                           static_cast<unsigned long>(sampleCount),
                           static_cast<unsigned long>(tofErrors),
                           static_cast<unsigned long>(laserRestarts));
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
        lastLaserFrameMs = millis();
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
