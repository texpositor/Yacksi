#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <stdarg.h>

// Rough health check for the ADXL355 and RM3100 (e.g. after an electrical
// fault). Runs every check at boot and prints TEST_PASS / TEST_WARN / TEST_FAIL per check.
// Keep the device still on a table while the automatic checks run.
//
// Serial commands:
//   r  rerun the automatic checks
//   t  rotation test (turn the device through every orientation for 20 s)

// --- SPI Pins (same as main firmware) ---
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23
#define ADXL355_CS 5
#define RM3100_CS 15

// --- ADXL355 Register Definitions ---
#define ADXL355_DEVID_AD   0x00
#define ADXL355_DEVID_MST  0x01
#define ADXL355_PARTID     0x02
#define ADXL355_STATUS     0x04
#define ADXL355_TEMP2      0x06
#define ADXL355_XDATA3     0x08
#define ADXL355_OFFSET_X_H 0x1E
#define ADXL355_RANGE      0x2C
#define ADXL355_POWER_CTL  0x2D
#define ADXL355_SELF_TEST  0x2E
#define ADXL355_RESET      0x2F

#define ADXL355_RANGE_2G 0x01
#define ADXL355_RANGE_8G 0x03
#define ADXL355_LSB_PER_G_2G 256000.0f
#define ADXL355_LSB_PER_G_8G 64000.0f

// --- RM3100 Register Definitions ---
#define RM3100_POLL_REG   0x00
#define RM3100_CMM_REG    0x01
#define RM3100_CCX1_REG   0x04
#define RM3100_TMRC_REG   0x0B
#define RM3100_MX2_REG    0x24
#define RM3100_BIST_REG   0x33
#define RM3100_STATUS_REG 0x34
#define RM3100_REVID_REG  0x36

#define RM3100_LSB_PER_UT 75.0f  // gain at cycle count 200

static const SPISettings spiSettings(1000000, MSBFIRST, SPI_MODE0);

enum Result { TEST_PASS, TEST_WARN, TEST_FAIL };
static const char *resultNames[] = {"PASS", "WARN", "FAIL"};
int resultCounts[3];

void report(Result r, const char *name, const char *fmt, ...) {
  char detail[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);
  resultCounts[r]++;
  Serial.printf("  [%s] %-22s %s\n", resultNames[r], name, detail);
}

// --- Raw SPI ---

void spiRead(uint8_t cs, uint8_t cmd, uint8_t *buf, size_t len) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(cs, LOW);
  SPI.transfer(cmd);
  for (size_t i = 0; i < len; i++) buf[i] = SPI.transfer(0x00);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
}

void spiWrite(uint8_t cs, uint8_t cmd, const uint8_t *buf, size_t len) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(cs, LOW);
  SPI.transfer(cmd);
  for (size_t i = 0; i < len; i++) SPI.transfer(buf[i]);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
}

// ADXL355 uses (reg << 1) | RW, RM3100 uses reg | 0x80 for reads
uint8_t adxlRead(uint8_t reg) {
  uint8_t v;
  spiRead(ADXL355_CS, (reg << 1) | 0x01, &v, 1);
  return v;
}

void adxlWrite(uint8_t reg, uint8_t v) {
  spiWrite(ADXL355_CS, reg << 1, &v, 1);
}

uint8_t rmRead(uint8_t reg) {
  uint8_t v;
  spiRead(RM3100_CS, reg | 0x80, &v, 1);
  return v;
}

void rmWrite(uint8_t reg, uint8_t v) {
  spiWrite(RM3100_CS, reg & 0x7F, &v, 1);
}

bool noResponse(uint8_t a, uint8_t b) {
  return (a == 0x00 && b == 0x00) || (a == 0xFF && b == 0xFF);
}

// --- ADXL355 ---

bool adxlWaitReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!(adxlRead(ADXL355_STATUS) & 0x01)) {
    if (millis() - start > timeoutMs) return false;
  }
  return true;
}

void adxlReadRaw(int32_t out[3]) {
  uint8_t b[9];
  spiRead(ADXL355_CS, (ADXL355_XDATA3 << 1) | 0x01, b, 9);
  for (int i = 0; i < 3; i++) {
    int32_t v = ((int32_t)b[i * 3] << 12) | ((int32_t)b[i * 3 + 1] << 4) | (b[i * 3 + 2] >> 4);
    if (v & 0x80000) v |= 0xFFF00000;
    out[i] = v;
  }
}

// Mean and standard deviation (in g) over n samples. False if DRDY never asserts.
bool adxlSample(int n, float lsbPerG, float mean[3], float sd[3]) {
  double sum[3] = {0}, sum2[3] = {0};
  for (int k = 0; k < n; k++) {
    if (!adxlWaitReady(50)) return false;
    int32_t raw[3];
    adxlReadRaw(raw);
    for (int i = 0; i < 3; i++) {
      double g = raw[i] / lsbPerG;
      sum[i] += g;
      sum2[i] += g * g;
    }
  }
  for (int i = 0; i < 3; i++) {
    mean[i] = sum[i] / n;
    sd[i] = sqrt(fmax(0.0, sum2[i] / n - (sum[i] / n) * (sum[i] / n)));
  }
  return true;
}

// Range can only be changed in standby
void adxlSetRange(uint8_t range) {
  adxlWrite(ADXL355_POWER_CTL, 0x01);
  adxlWrite(ADXL355_RANGE, (adxlRead(ADXL355_RANGE) & 0xFC) | range);
  adxlWrite(ADXL355_POWER_CTL, 0x00);  // measurement mode, temperature on
  delay(50);
}

float magnitude(const float v[3]) {
  return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void testADXL355() {
  Serial.println("\n--- ADXL355 ---");
  adxlWrite(ADXL355_RESET, 0x52);
  delay(20);

  uint8_t ad = adxlRead(ADXL355_DEVID_AD);
  uint8_t mst = adxlRead(ADXL355_DEVID_MST);
  uint8_t part = adxlRead(ADXL355_PARTID);
  bool idOk = ad == 0xAD && mst == 0x1D && part == 0xED;
  report(idOk ? TEST_PASS : TEST_FAIL, "ID registers", "0x%02X 0x%02X 0x%02X (expect 0xAD 0x1D 0xED)", ad, mst, part);
  if (!idOk) {
    if (noResponse(ad, part)) {
      Serial.println("  No response on MISO: check 3V3/GND at the module, CS (GPIO5), SCK, MISO.");
    }
    Serial.println("  Skipping remaining ADXL355 checks.");
    return;
  }

  // Register write/readback; the device is in standby right after reset
  bool rwOk = true;
  const uint8_t patterns[] = {0x55, 0xAA, 0x00};
  for (uint8_t p : patterns) {
    adxlWrite(ADXL355_OFFSET_X_H, p);
    if (adxlRead(ADXL355_OFFSET_X_H) != p) rwOk = false;
  }
  report(rwOk ? TEST_PASS : TEST_FAIL, "Register write/read", rwOk ? "0x55/0xAA/0x00 read back" : "readback mismatch (MOSI or chip fault)");

  adxlSetRange(ADXL355_RANGE_2G);

  uint8_t t[2];
  spiRead(ADXL355_CS, (ADXL355_TEMP2 << 1) | 0x01, t, 2);
  int rawTemp = ((t[0] & 0x0F) << 8) | t[1];
  float tempC = 25.0f + (rawTemp - 1885) / -9.05f;
  report(tempC > 0 && tempC < 60 ? TEST_PASS : TEST_WARN, "Temperature", "%.1f C (raw %d)", tempC, rawTemp);

  float mean[3], sd[3];
  if (!adxlSample(512, ADXL355_LSB_PER_G_2G, mean, sd)) {
    report(TEST_FAIL, "Data ready", "DRDY never asserted, no measurements");
    return;
  }
  float g = magnitude(mean);
  Result gr = fabsf(g - 1.0f) < 0.1f ? TEST_PASS : fabsf(g - 1.0f) < 0.2f ? TEST_WARN : TEST_FAIL;
  report(gr, "Gravity magnitude", "%.4f g  (x %+.4f  y %+.4f  z %+.4f)", g, mean[0], mean[1], mean[2]);

  float noise = fmaxf(sd[0], fmaxf(sd[1], sd[2])) * 1000.0f;
  Result nr = noise < 3.0f ? TEST_PASS : noise < 10.0f ? TEST_WARN : TEST_FAIL;
  report(nr, "Noise (worst axis)", "%.2f mg rms  (x %.2f  y %.2f  z %.2f)", noise, sd[0] * 1000, sd[1] * 1000, sd[2] * 1000);

  // Self-test: an electrostatic force deflects the proof mass. Typical
  // response is 0.3 g on X/Y and 1.5 g on Z, so use the 8 g range to avoid
  // clipping with gravity on top.
  adxlSetRange(ADXL355_RANGE_8G);
  float base[3], st[3];
  adxlWrite(ADXL355_SELF_TEST, 0x01);
  delay(20);
  bool ok = adxlSample(256, ADXL355_LSB_PER_G_8G, base, sd);
  adxlWrite(ADXL355_SELF_TEST, 0x03);
  delay(20);
  ok = ok && adxlSample(256, ADXL355_LSB_PER_G_8G, st, sd);
  adxlWrite(ADXL355_SELF_TEST, 0x00);
  adxlSetRange(ADXL355_RANGE_2G);
  if (!ok) {
    report(TEST_FAIL, "Self-test", "DRDY stopped during self-test");
    return;
  }
  float d[3];
  for (int i = 0; i < 3; i++) d[i] = fabsf(st[i] - base[i]);
  bool inBand = d[0] > 0.1f && d[0] < 0.6f && d[1] > 0.1f && d[1] < 0.6f && d[2] > 0.5f && d[2] < 2.5f;
  bool dead = d[0] < 0.03f || d[1] < 0.03f || d[2] < 0.03f;
  report(inBand ? TEST_PASS : dead ? TEST_FAIL : TEST_WARN, "Self-test response",
         "x %.3f  y %.3f  z %.3f g (typ 0.3 0.3 1.5)", d[0], d[1], d[2]);
}

// --- RM3100 ---

bool rmWaitReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!(rmRead(RM3100_STATUS_REG) & 0x80)) {
    if (millis() - start > timeoutMs) return false;
  }
  return true;
}

void rmReadRaw(int32_t out[3]) {
  uint8_t b[9];
  spiRead(RM3100_CS, RM3100_MX2_REG | 0x80, b, 9);
  for (int i = 0; i < 3; i++) {
    int32_t v = ((int32_t)b[i * 3] << 16) | ((int32_t)b[i * 3 + 1] << 8) | b[i * 3 + 2];
    if (v & 0x800000) v |= 0xFF000000;
    out[i] = v;
  }
}

// Single measurement on all axes, in uT
bool rmMeasure(float out[3]) {
  rmWrite(RM3100_POLL_REG, 0x70);
  if (!rmWaitReady(100)) return false;
  int32_t raw[3];
  rmReadRaw(raw);
  for (int i = 0; i < 3; i++) out[i] = raw[i] / RM3100_LSB_PER_UT;
  return true;
}

void rmSetCycleCounts(const uint8_t cc[6]) {
  spiWrite(RM3100_CS, RM3100_CCX1_REG & 0x7F, cc, 6);
}

void testRM3100() {
  Serial.println("\n--- RM3100 ---");
  rmWrite(RM3100_CMM_REG, 0x00);  // make sure continuous mode is off
  rmWrite(RM3100_BIST_REG, 0x00);
  delay(10);

  uint8_t rev = rmRead(RM3100_REVID_REG);
  uint8_t tmrc = rmRead(RM3100_TMRC_REG);
  report(rev == 0x22 ? TEST_PASS : TEST_FAIL, "REVID", "0x%02X (expect 0x22)", rev);
  if (rev != 0x22) {
    if (noResponse(rev, tmrc)) {
      Serial.println("  No response on MISO: check 3V3/GND at the module, CS (GPIO15), SCK, MISO.");
    }
    Serial.println("  Skipping remaining RM3100 checks.");
    return;
  }

  // Cycle count registers write/readback, then restore 200 on all axes
  const uint8_t pattern[6] = {0x01, 0x2C, 0x00, 0x64, 0x00, 0x32};
  const uint8_t cc200[6] = {0x00, 0xC8, 0x00, 0xC8, 0x00, 0xC8};
  uint8_t back[6];
  rmSetCycleCounts(pattern);
  spiRead(RM3100_CS, RM3100_CCX1_REG | 0x80, back, 6);
  bool rwOk = memcmp(pattern, back, 6) == 0;
  rmSetCycleCounts(cc200);
  spiRead(RM3100_CS, RM3100_CCX1_REG | 0x80, back, 6);
  rwOk = rwOk && memcmp(cc200, back, 6) == 0;
  rmWrite(RM3100_TMRC_REG, 0x96);
  rwOk = rwOk && rmRead(RM3100_TMRC_REG) == 0x96;
  report(rwOk ? TEST_PASS : TEST_FAIL, "Register write/read", rwOk ? "cycle counts and TMRC read back" : "readback mismatch (MOSI or chip fault)");

  // Built-in self test of the three LR oscillators
  rmWrite(RM3100_BIST_REG, 0x8F);
  rmWrite(RM3100_POLL_REG, 0x70);
  rmWaitReady(200);
  delay(10);
  uint8_t bist = rmRead(RM3100_BIST_REG);
  rmWrite(RM3100_BIST_REG, 0x00);
  int32_t discard[3];
  rmReadRaw(discard);  // clears DRDY left over from the self-test poll
  bool bx = bist & 0x10, by = bist & 0x20, bz = bist & 0x40;
  report(bx && by && bz ? TEST_PASS : TEST_FAIL, "Built-in self test", "X %s  Y %s  Z %s (BIST 0x%02X)",
         bx ? "ok" : "BAD", by ? "ok" : "BAD", bz ? "ok" : "BAD", bist);

  const int n = 40;
  double sum[3] = {0}, sum2[3] = {0};
  for (int k = 0; k < n; k++) {
    float m[3];
    if (!rmMeasure(m)) {
      report(TEST_FAIL, "Data ready", "measurement never completed (DRDY timeout)");
      return;
    }
    for (int i = 0; i < 3; i++) {
      sum[i] += m[i];
      sum2[i] += (double)m[i] * m[i];
    }
  }
  float mean[3], sd[3];
  for (int i = 0; i < 3; i++) {
    mean[i] = sum[i] / n;
    sd[i] = sqrt(fmax(0.0, sum2[i] / n - (sum[i] / n) * (sum[i] / n)));
  }

  float b = magnitude(mean);
  Result br = b > 20 && b < 80 ? TEST_PASS : b > 5 && b < 200 ? TEST_WARN : TEST_FAIL;
  report(br, "Field magnitude", "%.1f uT  (x %+.1f  y %+.1f  z %+.1f), Earth ~25-65", b, mean[0], mean[1], mean[2]);

  float noise = fmaxf(sd[0], fmaxf(sd[1], sd[2]));
  bool stuck = sd[0] == 0 || sd[1] == 0 || sd[2] == 0;
  Result nr = stuck ? TEST_WARN : noise < 0.2f ? TEST_PASS : noise < 1.0f ? TEST_WARN : TEST_FAIL;
  report(nr, "Noise (worst axis)", "%.3f uT rms  (x %.3f  y %.3f  z %.3f)%s", noise, sd[0], sd[1], sd[2],
         stuck ? " - an axis never changed" : "");
}

// --- Rotation test ---

Result spanResult(float span, float pass, float warn) {
  return span >= pass ? TEST_PASS : span >= warn ? TEST_WARN : TEST_FAIL;
}

void rotationTest() {
  Serial.println("\n--- Rotation test ---");
  Serial.println("Slowly turn the device through every orientation (each face up and down) for 20 s.");
  for (int i = 3; i > 0; i--) {
    Serial.printf("  starting in %d...\n", i);
    delay(1000);
  }
  memset(resultCounts, 0, sizeof(resultCounts));

  float aMin[3], aMax[3], mMin[3], mMax[3];
  for (int i = 0; i < 3; i++) {
    aMin[i] = mMin[i] = 1e9;
    aMax[i] = mMax[i] = -1e9;
  }
  float gMin = 1e9, gMax = 0, bMin = 1e9, bMax = 0;
  bool adxlOk = true, rmOk = true;

  uint32_t end = millis() + 20000;
  uint32_t lastPrint = 0;
  while (millis() < end) {
    float a[3], sd[3], m[3];
    rmWrite(RM3100_POLL_REG, 0x70);
    adxlOk = adxlOk && adxlSample(16, ADXL355_LSB_PER_G_2G, a, sd);
    rmOk = rmOk && rmWaitReady(100);
    if (!adxlOk || !rmOk) break;
    int32_t raw[3];
    rmReadRaw(raw);
    for (int i = 0; i < 3; i++) {
      m[i] = raw[i] / RM3100_LSB_PER_UT;
      aMin[i] = fminf(aMin[i], a[i]);
      aMax[i] = fmaxf(aMax[i], a[i]);
      mMin[i] = fminf(mMin[i], m[i]);
      mMax[i] = fmaxf(mMax[i], m[i]);
    }
    float g = magnitude(a), b = magnitude(m);
    gMin = fminf(gMin, g);
    gMax = fmaxf(gMax, g);
    bMin = fminf(bMin, b);
    bMax = fmaxf(bMax, b);

    if (millis() - lastPrint > 500) {
      lastPrint = millis();
      Serial.printf("  g %+.2f %+.2f %+.2f |%.2f|   uT %+6.1f %+6.1f %+6.1f |%.1f|\n",
                    a[0], a[1], a[2], g, m[0], m[1], m[2], b);
    }
  }

  if (!adxlOk) report(TEST_FAIL, "ADXL355 data ready", "DRDY stopped during rotation test");
  if (!rmOk) report(TEST_FAIL, "RM3100 data ready", "DRDY stopped during rotation test");
  if (!adxlOk || !rmOk) return;

  Serial.println();
  const char axes[] = {'X', 'Y', 'Z'};
  char name[32];
  for (int i = 0; i < 3; i++) {
    snprintf(name, sizeof(name), "Accel %c span", axes[i]);
    report(spanResult(aMax[i] - aMin[i], 1.6f, 0.8f), name, "%+.2f .. %+.2f g (expect about -1 .. +1)", aMin[i], aMax[i]);
  }
  for (int i = 0; i < 3; i++) {
    snprintf(name, sizeof(name), "Mag %c span", axes[i]);
    report(spanResult(mMax[i] - mMin[i], 40.0f, 15.0f), name, "%+.1f .. %+.1f uT (expect about +/- field)", mMin[i], mMax[i]);
  }
  Serial.printf("  [INFO] |g| ranged %.3f .. %.3f (hand motion adds a little)\n", gMin, gMax);
  Serial.printf("  [INFO] |B| ranged %.1f .. %.1f uT (uncalibrated, nearby iron widens this)\n", bMin, bMax);
  Serial.printf("\nRotation summary: %d pass, %d warn, %d fail\n", resultCounts[TEST_PASS], resultCounts[TEST_WARN], resultCounts[TEST_FAIL]);
}

// --- Main ---

void runChecks() {
  memset(resultCounts, 0, sizeof(resultCounts));
  Serial.println("\n===== Sensor health check (keep the device still) =====");
  testADXL355();
  testRM3100();
  Serial.printf("\nSummary: %d pass, %d warn, %d fail\n", resultCounts[TEST_PASS], resultCounts[TEST_WARN], resultCounts[TEST_FAIL]);
}

void printMenu() {
  Serial.println("\nCommands: r = rerun checks, t = rotation test (20 s)");
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(ADXL355_CS, OUTPUT);
  pinMode(RM3100_CS, OUTPUT);
  digitalWrite(ADXL355_CS, HIGH);
  digitalWrite(RM3100_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  delay(50);

  runChecks();
  printMenu();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'r' || c == 'R') {
    runChecks();
    printMenu();
  } else if (c == 't' || c == 'T') {
    rotationTest();
    printMenu();
  }
}
