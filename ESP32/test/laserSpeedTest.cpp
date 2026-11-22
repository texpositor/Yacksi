#include <SPI.h>
#include <math.h>

#include "Calibration.h"

// SACSI phase 0: how fast can the M01 laser measure, and does the laser
// disturb the magnetometer? Needs the laser module and RM3100 connected.
// Point the laser at something 1-5 m away (a wall or the floor) and keep the
// instrument still while the magnetometer test runs.
//
// Serial commands:
//   p          probe wiring: raw laser replies and SPI sensor IDs
//   1 / 2 / 3  back-to-back single measurements (reg 0x0020), laser left on,
//              payload 0x00 / 0x01 / 0x02 (only 0x00 has worked so far)
//   4 / 5 / 6  continuous measurement (reg 0x0021) for 10 s, payload
//              0x00 / 0x01 / 0x02, then tries each way of stopping it in turn
//   c          start continuous measurement and leave it running
//   s1 .. s6   send one stop method (list with s alone):
//              1 0x58 stop byte, 2 laser off, 3 single measurement,
//              4 continuous again, 5 ENA low for 1 s, 6 laser on
//   q          send laser off until the laser is silent
//   S          try every stop method in turn on a fresh stream each time
// Whenever no test is running, every frame from the laser is printed with the
// time since the last command, so after c and sN you can see (and watch the
// laser dot) whether readings stop.
//   m          magnetometer with laser off / pointer on / single / continuous
//   a          run 1, 4, 5, 6 and m in turn
//   l          laser pointer on, o  laser off
//   r          print raw bytes from the laser for 5 s (for unknown replies)
//
// Frames are AA, addr, reg(2), count(2), payload, checksum, where the checksum
// is the sum of every byte after AA. Most replies carry count * 2 payload
// bytes (laser on/off: count 1, 9 bytes), but a measurement reply (count 4)
// is 13 bytes: distance as BCD in bytes 6-9 and signal quality in bytes
// 10-11, e.g.
//   AA 00 00 20 00 04 00 00 23 42 00 09 92  -> 2342 mm, quality 9
// Errors come back on reg 0x0000 with the error code as the payload.
// Registers from the M01 manual: 0x0020 single measurement, 0x0021 continuous
// measurement (up to 20 Hz), 0x0022 read result, 0x01BE laser on/off.
//
// Findings (module m01-v02, wall at 2.34 m, Nov 2026):
// - Single (0x0020, payload 0x00): 0.46-1.9 s per shot depending on signal
//   quality. Payloads 0x01/0x02 get no reply.
// - Continuous (0x0021): acknowledged with an echo of the command, then a
//   reading every ~350 ms (2.7 Hz). The payload makes no difference.
// - Stop continuous with the laser off command: acknowledged, stream ends.
//   0x58, a single-measurement command and repeating 0x0021 do not stop it.
//   ENA low for 1 s halts it, but the stream seems to resume at the next
//   laser on, so ENA pauses rather than cancels continuous mode.
// - Magnetometer shift from the laser: at most 0.2 uT in any mode.
//
// Second M01 module (fitted after the first failed, Nov 2026):
// - Continuous: a reading every 0.6-0.9 s on a nearby target, and slow to
//   acknowledge commands while measuring (up to ~2.7 s).
// - Laser off is acknowledged but does NOT end continuous mode. Sending the
//   continuous command again toggles continuous mode off (s4). 0x58 and a
//   single-measurement command do not stop it, and ENA appears to have no
//   effect on this module.

static const uint8_t laserOnCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x01, 0xC1};
static const uint8_t laserOffCmd[] = {0xAA, 0x00, 0x01, 0xBE, 0x00, 0x01, 0x00, 0x00, 0xC0};
static const uint8_t stopContinuousByte = 0x58;
static const size_t MAX_FRAME = 32;
static const int SINGLE_SHOTS = 20;
static const uint32_t SINGLE_TIMEOUT_MS = 4000;
static const uint32_t CONTINUOUS_RUN_MS = 10000;
static const float RM3100_LSB_PER_UT = 75.0f;  // cycle count 200

static void sendCommand(uint16_t reg, uint8_t payload) {
  uint8_t cmd[] = {0xAA, 0x00, uint8_t(reg >> 8), uint8_t(reg), 0x00, 0x01, 0x00, payload, 0x00};
  uint8_t sum = 0;
  for (size_t i = 1; i < sizeof(cmd) - 1; ++i) sum += cmd[i];
  cmd[sizeof(cmd) - 1] = sum;
  Serial1.write(cmd, sizeof(cmd));
}

static void sendMeasure(uint8_t mode) { sendCommand(0x0020, mode); }
static void sendContinuous(uint8_t mode) { sendCommand(0x0021, mode); }

static bool isMeasurementReg(const uint8_t* frame) {
  return frame[2] == 0x00 && frame[3] >= 0x20 && frame[3] <= 0x22;
}

static const char* errorName(uint16_t code) {
  static const char* names[] = {"no error", "hardware", "no output data", "signal weak",
                                "signal strong", "temperature high", "temperature low",
                                "supply low", "out of range", "read error", "write error",
                                "address error"};
  return code < sizeof(names) / sizeof(names[0]) ? names[code] : "unknown";
}

static void flushLaser() {
  while (Serial1.available()) Serial1.read();
}

static void laserOn() {
  Serial1.write(laserOnCmd, sizeof(laserOnCmd));
}

static void laserOff() {
  Serial1.write(laserOffCmd, sizeof(laserOffCmd));
}

static void printHex(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) Serial.printf("%02X ", data[i]);
}

static uint32_t bcd32(const uint8_t* b) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = v * 100 + ((b[i] >> 4) & 0x0F) * 10 + (b[i] & 0x0F);
  return v;
}

// Reads one frame, resynchronising on 0xAA. Returns its length, or 0 on timeout.
static size_t readFrame(uint8_t* frame, uint32_t timeoutMs) {
  const uint32_t start = millis();
  size_t length = 0;
  size_t expected = 7;  // header plus checksum until the count is known
  while (millis() - start < timeoutMs) {
    if (!Serial1.available()) {
      delay(1);
      continue;
    }
    const uint8_t byte = Serial1.read();
    if (length == 0 && byte != 0xAA) continue;
    frame[length++] = byte;
    if (length == 6) {
      const size_t count = (size_t(frame[4]) << 8) | frame[5];
      expected = isMeasurementReg(frame) && count == 4 ? 13 : 7 + 2 * count;
      if (expected > MAX_FRAME) {  // not a real header; look for the next 0xAA
        length = 0;
        expected = 7;
      }
    }
    if (length == expected) return length;
  }
  return 0;
}

static bool checksumOk(const uint8_t* frame, size_t length) {
  uint8_t sum = 0;
  for (size_t i = 1; i < length - 1; ++i) sum += frame[i];
  return sum == frame[length - 1];
}

static bool isMeasurement(const uint8_t* frame, size_t length) {
  return length == 13 && isMeasurementReg(frame);
}

static void printFrame(const uint8_t* frame, size_t length) {
  printHex(frame, length);
  if (!checksumOk(frame, length)) Serial.print(" BAD CHECKSUM");
  if (isMeasurement(frame, length)) {
    const uint16_t quality = (uint16_t(frame[10]) << 8) | frame[11];
    Serial.printf(" -> %lu mm, quality %u", (unsigned long)bcd32(&frame[6]), quality);
  } else if (frame[2] == 0x00 && frame[3] == 0x00 && length == 9) {
    const uint16_t code = (uint16_t(frame[6]) << 8) | frame[7];
    Serial.printf(" -> ERROR 0x%04X (%s)", code, errorName(code));
  } else {
    Serial.printf(" -> addr 0x%02X reg 0x%02X%02X (not a measurement)", frame[1], frame[2], frame[3]);
  }
}

struct Stats {
  uint32_t count = 0, minMs = UINT32_MAX, maxMs = 0, totalMs = 0;
  void add(uint32_t ms) {
    ++count;
    totalMs += ms;
    if (ms < minMs) minMs = ms;
    if (ms > maxMs) maxMs = ms;
  }
  void print(const char* label) const {
    if (!count) {
      Serial.printf("%s: no measurements\n", label);
      return;
    }
    Serial.printf("%s: %lu measurements, min %lu ms, avg %lu ms, max %lu ms\n", label,
                  (unsigned long)count, (unsigned long)minMs, (unsigned long)(totalMs / count),
                  (unsigned long)maxMs);
  }
};

static void singleShotTest(uint8_t mode) {
  Serial.printf("\n=== Back-to-back single measurements, mode 0x%02X ===\n", mode);
  laserOn();
  delay(500);
  flushLaser();
  Stats stats;
  int failures = 0;
  for (int shot = 1; shot <= SINGLE_SHOTS; ++shot) {
    flushLaser();
    const uint32_t start = millis();
    sendMeasure(mode);
    uint8_t frame[MAX_FRAME];
    size_t length;
    // Skip any acknowledgement frames until the measurement arrives.
    auto remaining = [&]() -> uint32_t {
      const uint32_t used = millis() - start;
      return used < SINGLE_TIMEOUT_MS ? SINGLE_TIMEOUT_MS - used : 0;
    };
    while ((length = readFrame(frame, remaining())) && !isMeasurement(frame, length)) {
      Serial.print("  other: ");
      printFrame(frame, length);
      Serial.println();
    }
    const uint32_t elapsed = millis() - start;
    if (!length) {
      ++failures;
      Serial.printf("  shot %2d: no reply after %lu ms\n", shot, (unsigned long)elapsed);
      continue;
    }
    stats.add(elapsed);
    Serial.printf("  shot %2d: %4lu ms  ", shot, (unsigned long)elapsed);
    printFrame(frame, length);
    Serial.println();
  }
  laserOff();
  char label[48];
  snprintf(label, sizeof(label), "RESULT single mode 0x%02X", mode);
  stats.print(label);
  Serial.printf("RESULT single mode 0x%02X: %d of %d timed out\n", mode, failures, SINGLE_SHOTS);
}

// Counts bytes arriving in the window; none means the stream has stopped.
static int bytesWithin(uint32_t ms) {
  const uint32_t start = millis();
  int count = 0;
  while (millis() - start < ms) {
    while (Serial1.available()) {
      Serial1.read();
      ++count;
    }
    delay(2);
  }
  return count;
}

// The manual does not say how to stop continuous mode, so try the likely ways
// in turn, gentlest first, and report the first one that works. Each action
// gets 300 ms for any acknowledgement, then a quiet window long enough to
// catch a slow measurement stream.
static bool quietAfterAction() {
  delay(300);
  flushLaser();
  return bytesWithin(2500) == 0;
}

static const char* stopContinuous() {
  Serial1.write(stopContinuousByte);
  if (quietAfterAction()) return "0x58 stop byte";
  sendMeasure(0x00);  // a single measurement may end continuous mode
  delay(2000);
  if (quietAfterAction()) return "single-measurement command";
  laserOff();
  if (quietAfterAction()) return "laser off command";
  digitalWrite(ENA_PIN, LOW);  // last resort: power-cycle the module
  delay(200);
  digitalWrite(ENA_PIN, HIGH);
  if (quietAfterAction()) return "ENA power cycle";
  return "nothing (still streaming)";
}

// Prints every frame for the window with its arrival time. Returns how many
// measurement frames arrived after the first 500 ms (earlier ones may already
// have been in flight when a stop was sent).
// With stopAfter > 0, returns as soon as that many measurements have arrived.
static int dumpFrames(uint32_t windowMs, int stopAfter = 0) {
  const uint32_t start = millis();
  int late = 0;
  int measurements = 0;
  for (uint32_t used; (used = millis() - start) < windowMs;) {
    uint8_t frame[MAX_FRAME];
    const size_t length = readFrame(frame, windowMs - used);
    if (!length) break;
    const uint32_t at = millis() - start;
    if (isMeasurement(frame, length)) {
      ++measurements;
      if (at > 500) ++late;
    }
    if (stopAfter > 0 && measurements >= stopAfter) {
      Serial.printf("    %5lu ms  ", (unsigned long)at);
      printFrame(frame, length);
      Serial.println();
      return measurements;
    }
    Serial.printf("    %5lu ms  ", (unsigned long)at);
    printFrame(frame, length);
    Serial.println();
  }
  return late;
}

static void enaPowerCycle() {
  digitalWrite(ENA_PIN, LOW);
  delay(1000);
  digitalWrite(ENA_PIN, HIGH);
  delay(500);
}

struct StopMethod {
  const char* name;
  void (*apply)();
};

static const StopMethod stopMethods[] = {
  {"0x58 stop byte", [] { Serial1.write(stopContinuousByte); }},
  {"laser off command", [] { laserOff(); }},
  {"single-measurement command", [] { sendMeasure(0x00); }},
  {"continuous command again", [] { sendContinuous(0x00); }},
  {"ENA low for 1 s", [] { enaPowerCycle(); }},
  {"laser on command", [] { laserOn(); }},
};
static const int STOP_METHOD_COUNT = sizeof(stopMethods) / sizeof(stopMethods[0]);

// Counts measurement frames in the window, ignoring other frames such as the
// acknowledgement of the command just sent.
static int measurementsWithin(uint32_t windowMs) {
  const uint32_t start = millis();
  int count = 0;
  for (uint32_t used; (used = millis() - start) < windowMs;) {
    uint8_t frame[MAX_FRAME];
    const size_t length = readFrame(frame, windowMs - used);
    if (!length) break;
    if (isMeasurement(frame, length)) ++count;
  }
  return count;
}

// Sends laser off until no readings arrive for 2.5 s, and reports how many
// it took. This is also the main experiment: whether, and how reliably, laser
// off ends a running continuous stream on this module.
static void quiesce() {
  const int MAX_TRIES = 8;
  for (int attempt = 1; attempt <= MAX_TRIES; ++attempt) {
    laserOff();
    delay(300);  // readings already in flight
    flushLaser();
    if (measurementsWithin(2500) == 0) {
      Serial.printf("RESULT quiesce: silent after %d laser off command(s)\n", attempt);
      return;
    }
  }
  Serial.printf("RESULT quiesce: STILL STREAMING after %d laser off commands\n", MAX_TRIES);
}

// For each method: make sure the module is silent, start a fresh continuous
// stream, wait until it is really producing readings, apply the method, and
// watch for 6 s whether measurements keep coming.
static void stopTest() {
  Serial.println();
  Serial.println("=== Ways to stop continuous measurement ===");
  for (const StopMethod& method : stopMethods) {
    Serial.printf("--- %s ---\n", method.name);
    quiesce();
    laserOn();
    delay(500);
    flushLaser();
    sendContinuous(0x00);
    // Only a stream that is really producing readings shows whether a stop works.
    Serial.println("  started (waiting for 2 readings, up to 8 s):");
    const int before = dumpFrames(8000, 2);
    Serial.println("  after stop:");
    method.apply();
    const int late = dumpFrames(6000);
    Serial.printf("RESULT stop by %s: %s\n", method.name,
                  before < 2 ? "INCONCLUSIVE (no readings before the stop; aim at a wall 1-3 m away)"
                  : late    ? "STILL STREAMING"
                            : "stopped");
  }
  Serial.println("--- final clean-up ---");
  quiesce();
}

static void continuousTest(uint8_t mode) {
  Serial.printf("\n=== Continuous measurement for %lu s, mode 0x%02X ===\n",
                (unsigned long)(CONTINUOUS_RUN_MS / 1000), mode);
  laserOn();
  delay(500);
  flushLaser();
  const uint32_t start = millis();
  sendContinuous(mode);
  Stats intervals;
  uint32_t last = start;
  uint32_t measurements = 0;
  for (uint32_t used; (used = millis() - start) < CONTINUOUS_RUN_MS;) {
    uint8_t frame[MAX_FRAME];
    const size_t length = readFrame(frame, CONTINUOUS_RUN_MS - used);
    if (!length) break;
    const uint32_t now = millis();
    if (isMeasurement(frame, length)) {
      ++measurements;
      if (measurements > 1) intervals.add(now - last);  // the first includes start-up
      Serial.printf("  +%4lu ms  ", (unsigned long)(now - last));
      last = now;
    } else {
      Serial.print("  other     ");
    }
    printFrame(frame, length);
    Serial.println();
  }
  const char* stoppedBy = stopContinuous();
  laserOff();
  delay(200);
  flushLaser();
  char label[56];
  snprintf(label, sizeof(label), "RESULT continuous mode 0x%02X interval", mode);
  intervals.print(label);
  if (!measurements) {
    Serial.printf("RESULT continuous mode 0x%02X: no measurements (mode probably unsupported)\n", mode);
  }
  Serial.printf("RESULT continuous mode 0x%02X: stopped by %s\n", mode, stoppedBy);
}

// --- RM3100, set up as in the main firmware ---
static uint8_t rmRead(uint8_t reg) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg | 0x80);
  const uint8_t value = SPI.transfer(0x00);
  digitalWrite(RM3100_CS, HIGH);
  return value;
}

static void rmWrite(uint8_t reg, uint8_t value) {
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(reg & 0x7F);
  SPI.transfer(value);
  digitalWrite(RM3100_CS, HIGH);
}

// The RM3100 can take a moment to answer after power-up, so wait for its ID.
static void initMag() {
  uint8_t revid = 0;
  for (int attempt = 0; attempt < 20 && (revid = rmRead(RM3100_REVID_REG)) != 0x22; ++attempt) {
    delay(100);
  }
  Serial.printf("RM3100 REVID 0x%02X (expect 0x22)\n", revid);
  rmWrite(RM3100_CMM_REG, 0x00);
  delay(10);
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(RM3100_CCX1_REG & 0x7F);
  for (int axis = 0; axis < 3; ++axis) {
    SPI.transfer(0x00);
    SPI.transfer(0xC8);
  }
  digitalWrite(RM3100_CS, HIGH);
  delay(10);
  rmWrite(RM3100_TMRC_REG, 0x96);
  rmWrite(RM3100_CMM_REG, 0x79);
  delay(10);
}

static bool readMag(float out[3]) {
  const uint32_t start = millis();
  while (!(rmRead(RM3100_STATUS_REG) & 0x80)) {
    if (millis() - start > 200) return false;
  }
  digitalWrite(RM3100_CS, LOW);
  SPI.transfer(RM3100_MX2_REG | 0x80);
  for (int axis = 0; axis < 3; ++axis) {
    int32_t value = int32_t(SPI.transfer(0)) << 16;
    value |= int32_t(SPI.transfer(0)) << 8;
    value |= SPI.transfer(0);
    if (value & 0x800000) value |= 0xFF000000;
    out[axis] = value;
  }
  digitalWrite(RM3100_CS, HIGH);
  return true;
}

enum class LaserState { Off, Pointer, Single, Continuous };
static const char* laserStateNames[] = {"laser off", "pointer on", "single", "continuous"};

// Averages the field for 3 s. In Single, keeps a single measurement going at
// all times so the laser draws its measuring current throughout.
static void sampleMag(LaserState state, float mean[3], float sd[3]) {
  double sum[3] = {0, 0, 0}, sumSq[3] = {0, 0, 0};
  int samples = 0;
  uint32_t lastShot = 0;
  const uint32_t start = millis();
  while (millis() - start < 3000) {
    if (state == LaserState::Continuous) flushLaser();
    if (state == LaserState::Single &&
        (Serial1.available() >= 13 || millis() - lastShot > 1500)) {
      flushLaser();
      sendMeasure(0x00);
      lastShot = millis();
    }
    float field[3];
    if (!readMag(field)) continue;
    for (int axis = 0; axis < 3; ++axis) {
      sum[axis] += field[axis];
      sumSq[axis] += double(field[axis]) * field[axis];
    }
    ++samples;
  }
  for (int axis = 0; axis < 3; ++axis) {
    mean[axis] = samples ? sum[axis] / samples : NAN;
    sd[axis] = samples > 1 ? sqrt(fmax(0.0, sumSq[axis] / samples - double(mean[axis]) * mean[axis])) : NAN;
  }
  Serial.printf("  %-10s %3d samples  X %9.0f  Y %9.0f  Z %9.0f counts  (noise sd %.0f %.0f %.0f)\n",
                laserStateNames[int(state)], samples, mean[0], mean[1], mean[2], sd[0], sd[1], sd[2]);
}

static void setLaser(LaserState state) {
  if (state == LaserState::Off) {
    laserOff();
  } else {
    laserOn();
  }
  delay(500);
  if (state == LaserState::Continuous) sendContinuous(0x00);
  flushLaser();
}

static void magTest() {
  Serial.println("\n=== Magnetometer vs laser (keep the instrument still) ===");
  initMag();
  float maxShift[3] = {0, 0, 0};
  for (int round = 1; round <= 3; ++round) {
    Serial.printf(" Round %d\n", round);
    float offMean[3], mean[3], sd[3];
    setLaser(LaserState::Off);
    sampleMag(LaserState::Off, offMean, sd);
    for (int s = 1; s <= 3; ++s) {
      const LaserState state = LaserState(s);
      setLaser(state);
      sampleMag(state, mean, sd);
      float shift = 0;
      for (int axis = 0; axis < 3; ++axis) {
        const float d = mean[axis] - offMean[axis];
        shift += d * d;
      }
      shift = sqrtf(shift) / RM3100_LSB_PER_UT;
      if (shift > maxShift[s - 1]) maxShift[s - 1] = shift;
      Serial.printf("    shift vs off: %.2f uT\n", shift);
      if (state == LaserState::Continuous) stopContinuous();
    }
  }
  setLaser(LaserState::Off);
  Serial.printf("RESULT mag shift, pointer on: max %.2f uT; single: max %.2f uT; continuous: max %.2f uT "
                "(Earth's field is ~50 uT; 1 uT across the field is roughly 1 deg of heading)\n",
                maxShift[0], maxShift[1], maxShift[2]);
}

static void rawDump() {
  Serial.println("\n=== Raw bytes from laser for 5 s ===");
  const uint32_t start = millis();
  while (millis() - start < 5000) {
    while (Serial1.available()) Serial.printf("%02X ", Serial1.read());
    delay(5);
  }
  Serial.println();
}

// Prints every byte that arrives within the window, without any frame parsing.
static void dumpFor(uint32_t ms) {
  const uint32_t start = millis();
  int count = 0;
  while (millis() - start < ms) {
    while (Serial1.available()) {
      Serial.printf("%02X ", Serial1.read());
      ++count;
    }
    delay(2);
  }
  Serial.printf(" [%d bytes]\n", count);
}

// Wiring check with no parsing: shows whether anything comes back on GPIO RXD2
// and whether the SPI sensors answer at all.
static void probe() {
  Serial.println("\n=== Probe ===");
  digitalWrite(ADXL355_CS, LOW);
  SPI.transfer((ADXL355_DEVID_AD << 1) | 0x01);
  const uint8_t adxlId = SPI.transfer(0x00);
  digitalWrite(ADXL355_CS, HIGH);
  Serial.printf("ADXL355 DEVID 0x%02X (expect 0xAD), RM3100 REVID 0x%02X (expect 0x22)\n",
                adxlId, rmRead(RM3100_REVID_REG));
  Serial.printf("RX pin GPIO%d idles %s (should be HIGH)\n", RXD2,
                digitalRead(RXD2) ? "HIGH" : "LOW");
  flushLaser();
  Serial.print("Laser on, reply:  ");
  laserOn();
  dumpFor(1000);
  Serial.print("Measure, reply:   ");
  sendMeasure(0x00);
  dumpFor(4000);
  Serial.print("Laser off, reply: ");
  laserOff();
  dumpFor(1000);
}

static void printHelp() {
  Serial.println("Commands: p probe wiring, 1/2/3 single 0x00-0x02, 4/5/6 continuous 0x00-0x02, "
                 "c start continuous, s1-s6 one stop method, q laser off until silent, S all stop methods, "
                 "m magnetometer, a all, l laser on, o laser off, r raw bytes");
}

static uint32_t lastCommandMs = 0;

// Prints laser frames as they arrive, timed from the last command.
static void monitorLaser() {
  while (Serial1.available()) {
    uint8_t frame[MAX_FRAME];
    const size_t length = readFrame(frame, 50);  // a frame takes ~14 ms at 9600 baud
    if (!length) return;
    Serial.printf("  +%5lu ms  ", (unsigned long)(millis() - lastCommandMs));
    printFrame(frame, length);
    Serial.println();
  }
}

static void startContinuous() {
  laserOn();
  delay(500);  // as in the tests that worked; the laser on reply prints below
  monitorLaser();
  sendContinuous(0x00);
  lastCommandMs = millis();
  Serial.println("Sent continuous measurement; readings print as they arrive.");
}

// "s" followed by a digit sends that stop method; "s" alone lists them.
static void stopMethodCommand() {
  const uint32_t start = millis();
  int digit = -1;
  while (millis() - start < 2000) {
    if (!Serial.available()) continue;
    const char c = Serial.read();
    if (c >= '1' && c <= '9') digit = c - '0';
    break;
  }
  if (digit < 1 || digit > STOP_METHOD_COUNT) {
    Serial.println("Stop methods (type s and the number):");
    for (int i = 0; i < STOP_METHOD_COUNT; ++i) Serial.printf("  s%d  %s\n", i + 1, stopMethods[i].name);
    return;
  }
  const StopMethod& method = stopMethods[digit - 1];
  method.apply();
  lastCommandMs = millis();
  Serial.printf("Sent stop method %d: %s. Watch for readings below and for the laser dot.\n", digit, method.name);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nSACSI laser speed test");

  pinMode(ENA_PIN, OUTPUT);
  digitalWrite(ENA_PIN, HIGH);
  Serial1.begin(9600, SERIAL_8N1, RXD2, TXD2);
  delay(200);
  laserOff();
  delay(200);
  flushLaser();

  pinMode(ADXL355_CS, OUTPUT);
  pinMode(RM3100_CS, OUTPUT);
  digitalWrite(ADXL355_CS, HIGH);
  digitalWrite(RM3100_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  initMag();

  printHelp();
}

void loop() {
  monitorLaser();
  if (!Serial.available()) return;
  switch (Serial.read()) {
    case 'p': probe(); break;
    case '1': singleShotTest(0x00); break;
    case '2': singleShotTest(0x01); break;
    case '3': singleShotTest(0x02); break;
    case '4': continuousTest(0x00); break;
    case '5': continuousTest(0x01); break;
    case '6': continuousTest(0x02); break;
    case 'c': startContinuous(); break;
    case 's': stopMethodCommand(); break;
    case 'S': stopTest(); break;
    case 'q': quiesce(); lastCommandMs = millis(); break;
    case 'm': magTest(); break;
    case 'a':
      singleShotTest(0x00);
      for (uint8_t mode = 0; mode <= 2; ++mode) continuousTest(mode);
      magTest();
      Serial.println("\n=== All tests done; copy the RESULT lines (or everything) back ===");
      break;
    case 'l': laserOn(); lastCommandMs = millis(); Serial.println("Laser on"); break;
    case 'o': laserOff(); lastCommandMs = millis(); Serial.println("Laser off"); break;
    case 'r': rawDump(); break;
    case '\r': case '\n': case ' ': break;
    default: printHelp(); break;
  }
}
