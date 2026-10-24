// Heeb (DistoX-style) calibration of the accelerometer and magnetometer.
//
// The user takes HEEB_GROUPS groups of HEEB_ROLLS shots. All shots in a group
// are aimed at the same target, with the instrument rolled ~90 deg about the
// laser between shots. The fit finds a full linear calibration (offset + 3x3
// matrix) for each sensor such that:
//   - the shots in each group give the same laser direction, and
//   - the angle between gravity and the magnetic field (the dip) is constant.
// Because the roll constraint is about the laser axis, this also aligns both
// sensors to the laser, which is what makes readings independent of roll.
//
// Reference: B. Heeb, "An All-In-One Electronic Cave Surveying Device",
// CREG Journal 2009, and the DistoX calibration manual.
//
// Lines starting "CAL," are machine readable for the Android app:
//   CAL,STATE=<state>,KEY=value,...,MSG=<free text to end of line>

#include "Calibration.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

HeebCalibration heebCalibration = {};
bool heebCalibrationValid = false;

namespace {

struct HeebShots {
  uint32_t magic;
  uint16_t count;
  uint8_t group[HEEB_MAX_SHOTS];
  float g[HEEB_MAX_SHOTS][3];
  float m[HEEB_MAX_SHOTS][3];
};

struct FitResult {
  HeebCalibration calibration;
  int iterations;
  int maxShot;
  int groupCount;
  float groupError[HEEB_GROUPS];
};

enum class Phase { Idle, Aim, Settle, Sampling, Review };

Phase phase = Phase::Idle;
HeebShots shots = {};
FitResult pendingResult = {};
unsigned long settleUntil = 0;
int sampleCount = 0;
int32_t sampleAccel[HEEB_SAMPLES_PER_SHOT][3];
int32_t sampleMag[HEEB_SAMPLES_PER_SHOT][3];

// Fit working storage; static so it does not use the loop task's stack.
double gp[HEEB_MAX_SHOTS][3];
double mp[HEEB_MAX_SHOTS][3];
double gx[HEEB_MAX_SHOTS][3];
double mx[HEEB_MAX_SHOTS][3];
double gl[HEEB_MAX_SHOTS][3];
double ml[HEEB_MAX_SHOTS][3];
double rollCos[HEEB_MAX_SHOTS];
double rollSin[HEEB_MAX_SHOTS];

const double RAD_TO_DEG_D = 180.0 / PI;

// ------------------------------------------------------------------
// Vector helpers
// ------------------------------------------------------------------
double dot3(const double a[3], const double b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const double a[3], const double b[3], double out[3]) {
  const double x = a[1] * b[2] - a[2] * b[1];
  const double y = a[2] * b[0] - a[0] * b[2];
  const double z = a[0] * b[1] - a[1] * b[0];
  out[0] = x; out[1] = y; out[2] = z;
}

bool normalize3(double v[3]) {
  const double n = sqrt(dot3(v, v));
  if (!(n > 1.0e-12) || !isfinite(n)) return false;
  for (int k = 0; k < 3; ++k) v[k] /= n;
  return true;
}

void matVec3(const double A[3][3], const double v[3], double out[3]) {
  for (int r = 0; r < 3; ++r) {
    out[r] = A[r][0] * v[0] + A[r][1] * v[1] + A[r][2] * v[2];
  }
}

double det3(const double M[3][3]) {
  return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
       - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
       + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
}

bool invert3(const double M[3][3], double inv[3][3]) {
  const double d = det3(M);
  if (!isfinite(d) || fabs(d) < 1.0e-18) return false;
  inv[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / d;
  inv[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / d;
  inv[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / d;
  inv[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / d;
  inv[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / d;
  inv[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / d;
  inv[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / d;
  inv[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / d;
  inv[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / d;
  return true;
}

// Rotates v about the X (laser) axis by the angle with cosine c and sine s.
void turnX(const double v[3], double c, double s, double out[3]) {
  const double y = c * v[1] - s * v[2];
  const double z = s * v[1] + c * v[2];
  out[0] = v[0]; out[1] = y; out[2] = z;
}

// Laser direction in world coordinates (north, east, up) from calibrated
// gravity-reaction (up) and magnetic field vectors in the laser frame.
bool laserDirection(const double g[3], const double m[3], double d[3]) {
  double up[3] = {g[0], g[1], g[2]};
  if (!normalize3(up)) return false;
  double east[3];
  cross3(m, up, east);          // east = down x field
  if (!normalize3(east)) return false;
  double north[3];
  cross3(up, east, north);      // north = east x down
  d[0] = north[0];
  d[1] = east[0];
  d[2] = up[0];
  return true;
}

// Closest pair of unit vectors to (gr, mr), in their plane, that are separated
// by the dip angle (cos c, sin s).
void optVectors(const double gr[3], const double mr[3], double c, double s,
                double gOut[3], double mOut[3]) {
  double g[3] = {gr[0], gr[1], gr[2]};
  double m[3] = {mr[0], mr[1], mr[2]};
  normalize3(g);
  normalize3(m);
  double normal[3];
  cross3(g, m, normal);
  if (!normalize3(normal)) {
    memcpy(gOut, g, sizeof(g));
    memcpy(mOut, m, sizeof(m));
    return;
  }
  // m rotated back towards g by the dip angle, averaged with g.
  double mBack[3];
  cross3(m, normal, mBack);
  for (int k = 0; k < 3; ++k) gOut[k] = m[k] * c + mBack[k] * s + g[k];
  normalize3(gOut);
  double gPerp[3];
  cross3(normal, gOut, gPerp);
  for (int k = 0; k < 3; ++k) mOut[k] = gOut[k] * c + gPerp[k] * s;
}

void applyLinear(const double A[3][3], const double b[3], const double v[3],
                 double out[3]) {
  matVec3(A, v, out);
  for (int k = 0; k < 3; ++k) out[k] += b[k];
}

void estimateDip(int count, const double aG[3][3], const double bG[3],
                 const double aM[3][3], const double bM[3],
                 double& c, double& s) {
  double sumCos = 0.0, sumSin = 0.0;
  for (int i = 0; i < count; ++i) {
    double g[3], m[3], n[3];
    applyLinear(aG, bG, gp[i], g);
    applyLinear(aM, bM, mp[i], m);
    if (!normalize3(g) || !normalize3(m)) continue;
    cross3(g, m, n);
    sumCos += dot3(g, m);
    sumSin += sqrt(dot3(n, n));
  }
  const double d = sqrt(sumCos * sumCos + sumSin * sumSin);
  c = d > 0.0 ? sumCos / d : 0.0;
  s = d > 0.0 ? sumSin / d : 1.0;
}

// Least-squares A, b minimising sum |A * x_i + b - target_i|^2, using the
// precomputed inverse covariance of x.
void solveLinear(int count, const double x[][3], const double target[][3],
                 const double meanX[3], const double invCov[3][3],
                 double A[3][3], double b[3]) {
  double meanT[3] = {0.0, 0.0, 0.0};
  for (int i = 0; i < count; ++i)
    for (int k = 0; k < 3; ++k) meanT[k] += target[i][k] / count;
  double cross[3][3] = {};
  for (int i = 0; i < count; ++i)
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        cross[r][c] += (target[i][r] - meanT[r]) * (x[i][c] - meanX[c]);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      A[r][c] = cross[r][0] * invCov[0][c] + cross[r][1] * invCov[1][c] +
                cross[r][2] * invCov[2][c];
  double Am[3];
  matVec3(A, meanX, Am);
  for (int k = 0; k < 3; ++k) b[k] = meanT[k] - Am[k];
}

bool meanAndInverseCovariance(int count, const double x[][3], double mean[3],
                              double invCov[3][3]) {
  for (int k = 0; k < 3; ++k) mean[k] = 0.0;
  for (int i = 0; i < count; ++i)
    for (int k = 0; k < 3; ++k) mean[k] += x[i][k] / count;
  double cov[3][3] = {};
  for (int i = 0; i < count; ++i)
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        cov[r][c] += (x[i][r] - mean[r]) * (x[i][c] - mean[c]);
  return invert3(cov, invCov);
}

double maxAbsDifference(const double a[3][3], const double b[3][3]) {
  double d = 0.0;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) d = fmax(d, fabs(a[r][c] - b[r][c]));
  return d;
}

double maxAbsDifference(const double a[3], const double b[3]) {
  return fmax(fabs(a[0] - b[0]), fmax(fabs(a[1] - b[1]), fabs(a[2] - b[2])));
}

// Centre of the best-fit sphere through the points: solves
// |p|^2 = 2 p.c + k by linear least squares.
bool sphereCentre(int count, const float points[][3], double centre[3]) {
  double normal[4][5] = {};
  for (int i = 0; i < count; ++i) {
    const double row[4] = {2.0 * points[i][0], 2.0 * points[i][1], 2.0 * points[i][2], 1.0};
    const double rhs = static_cast<double>(points[i][0]) * points[i][0] +
                       static_cast<double>(points[i][1]) * points[i][1] +
                       static_cast<double>(points[i][2]) * points[i][2];
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) normal[r][c] += row[r] * row[c];
      normal[r][4] += row[r] * rhs;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r)
      if (fabs(normal[r][col]) > fabs(normal[pivot][col])) pivot = r;
    if (!(fabs(normal[pivot][col]) > 1.0e-12)) return false;
    for (int c = 0; c < 5; ++c) {
      const double t = normal[col][c]; normal[col][c] = normal[pivot][c]; normal[pivot][c] = t;
    }
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = normal[r][col] / normal[col][col];
      for (int c = col; c < 5; ++c) normal[r][c] -= f * normal[col][c];
    }
  }
  for (int k = 0; k < 3; ++k) centre[k] = normal[k][4] / normal[k][k];
  return isfinite(centre[0]) && isfinite(centre[1]) && isfinite(centre[2]);
}

// ------------------------------------------------------------------
// Fit
// ------------------------------------------------------------------
bool fitHeeb(const HeebShots& data, FitResult& result, const char*& failure) {
  const int n = data.count;
  if (n < HEEB_MIN_GROUPS * HEEB_ROLLS) {
    failure = "Not enough shots";
    return false;
  }

  // Start from the rough G/M calibrations, scaled to unit mean length, so the
  // iteration works on well-conditioned, nearly correct vectors.
  double A0G[3][3] = {}, b0G[3], A0M[3][3], b0M[3];
  for (int k = 0; k < 3; ++k) {
    A0G[k][k] = accelerometerCalibration.scale[k];
    b0G[k] = -accelerometerCalibration.scale[k] * accelerometerCalibration.offset[k];
  }
  for (int r = 0; r < 3; ++r) {
    b0M[r] = 0.0;
    for (int c = 0; c < 3; ++c) {
      A0M[r][c] = magMatrix[r][c];
      b0M[r] -= magMatrix[r][c] * magOffset[c];
    }
  }
  // Without a rough mag calibration the hard-iron offset can be comparable to
  // the field and the iteration may then converge to a mirrored solution, so
  // estimate the offset from the shots themselves.
  double centre[3];
  if (!magCalibrationValid && sphereCentre(n, data.m, centre)) {
    for (int r = 0; r < 3; ++r) {
      b0M[r] = -centre[r];
      for (int c = 0; c < 3; ++c) A0M[r][c] = r == c ? 1.0 : 0.0;
    }
  }
  double lengthG = 0.0, lengthM = 0.0;
  for (int i = 0; i < n; ++i) {
    const double rawG[3] = {data.g[i][0], data.g[i][1], data.g[i][2]};
    const double rawM[3] = {data.m[i][0], data.m[i][1], data.m[i][2]};
    applyLinear(A0G, b0G, rawG, gp[i]);
    applyLinear(A0M, b0M, rawM, mp[i]);
    lengthG += sqrt(dot3(gp[i], gp[i])) / n;
    lengthM += sqrt(dot3(mp[i], mp[i])) / n;
  }
  if (!(lengthG > 0.0) || !(lengthM > 0.0)) {
    failure = "Sensor readings are zero";
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    b0G[r] /= lengthG;
    b0M[r] /= lengthM;
    for (int c = 0; c < 3; ++c) {
      A0G[r][c] /= lengthG;
      A0M[r][c] /= lengthM;
    }
  }
  for (int i = 0; i < n; ++i)
    for (int k = 0; k < 3; ++k) {
      gp[i][k] /= lengthG;
      mp[i][k] /= lengthM;
    }

  double meanG[3], meanM[3], invG[3][3], invM[3][3];
  if (!meanAndInverseCovariance(n, gp, meanG, invG) ||
      !meanAndInverseCovariance(n, mp, meanM, invM)) {
    failure = "Shots do not cover enough orientations";
    return false;
  }

  double aG[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, bG[3] = {0, 0, 0};
  double aM[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, bM[3] = {0, 0, 0};
  double dipCos, dipSin;
  estimateDip(n, aG, bG, aM, bM, dipCos, dipSin);

  int iteration = 0;
  for (; iteration < HEEB_MAX_ITERATIONS; ++iteration) {
    for (int i = 0; i < n; ++i) {
      double gr[3], mr[3];
      applyLinear(aG, bG, gp[i], gr);
      applyLinear(aM, bM, mp[i], mr);
      optVectors(gr, mr, dipCos, dipSin, gx[i], mx[i]);
    }

    // Within each group, roll every shot onto the first, average, and roll
    // the average back to give each shot's target vectors.
    for (int first = 0; first < n;) {
      int end = first;
      while (end < n && data.group[end] == data.group[first]) ++end;
      double sumG[3] = {0, 0, 0}, sumM[3] = {0, 0, 0};
      for (int j = first; j < end; ++j) {
        const double* rg = gx[first];
        const double* rm = mx[first];
        double c = rg[1] * gx[j][1] + rg[2] * gx[j][2] +
                   rm[1] * mx[j][1] + rm[2] * mx[j][2];
        double s = rg[2] * gx[j][1] - rg[1] * gx[j][2] +
                   rm[2] * mx[j][1] - rm[1] * mx[j][2];
        const double d = sqrt(c * c + s * s);
        if (d > 1.0e-12) { c /= d; s /= d; } else { c = 1.0; s = 0.0; }
        rollCos[j] = c;
        rollSin[j] = s;
        double tg[3], tm[3];
        turnX(gx[j], c, s, tg);
        turnX(mx[j], c, s, tm);
        for (int k = 0; k < 3; ++k) { sumG[k] += tg[k]; sumM[k] += tm[k]; }
      }
      double avgG[3], avgM[3];
      optVectors(sumG, sumM, dipCos, dipSin, avgG, avgM);
      for (int j = first; j < end; ++j) {
        turnX(avgG, rollCos[j], -rollSin[j], gl[j]);
        turnX(avgM, rollCos[j], -rollSin[j], ml[j]);
      }
      first = end;
    }

    double newAG[3][3], newBG[3], newAM[3][3], newBM[3];
    solveLinear(n, gp, gl, meanG, invG, newAG, newBG);
    solveLinear(n, mp, ml, meanM, invM, newAM, newBM);
    const double change = fmax(
      fmax(maxAbsDifference(aG, newAG), maxAbsDifference(bG, newBG)),
      fmax(maxAbsDifference(aM, newAM), maxAbsDifference(bM, newBM)));
    memcpy(aG, newAG, sizeof(aG)); memcpy(bG, newBG, sizeof(bG));
    memcpy(aM, newAM, sizeof(aM)); memcpy(bM, newBM, sizeof(bM));
    estimateDip(n, aG, bG, aM, bM, dipCos, dipSin);

    if (!isfinite(change)) {
      failure = "Fit diverged";
      return false;
    }
    if (change < HEEB_CONVERGENCE) break;
    if ((iteration & 15) == 15) delay(1);
  }

  // A negative determinant means the fit flipped an axis; the result would be
  // a mirror image (e.g. bearings rotated 180 deg).
  if (det3(aG) <= 0.0 || det3(aM) <= 0.0) {
    failure = "Fit produced a mirrored axis; check sensor axis directions";
    return false;
  }

  // Compose with the pre-calibration so the result applies to raw counts.
  HeebCalibration& cal = result.calibration;
  cal.magic = HEEB_CAL_MAGIC;
  for (int r = 0; r < 3; ++r) {
    double offsetG = bG[r], offsetM = bM[r];
    for (int c = 0; c < 3; ++c) {
      double sumG = 0.0, sumM = 0.0;
      for (int k = 0; k < 3; ++k) {
        sumG += aG[r][k] * A0G[k][c];
        sumM += aM[r][k] * A0M[k][c];
      }
      cal.aG[r][c] = static_cast<float>(sumG);
      cal.aM[r][c] = static_cast<float>(sumM);
      offsetG += aG[r][c] * b0G[c];
      offsetM += aM[r][c] * b0M[c];
    }
    cal.bG[r] = static_cast<float>(offsetG);
    cal.bM[r] = static_cast<float>(offsetM);
    for (int c = 0; c < 3; ++c) {
      if (!isfinite(cal.aG[r][c]) || !isfinite(cal.aM[r][c])) {
        failure = "Fit produced invalid numbers";
        return false;
      }
    }
    if (!isfinite(cal.bG[r]) || !isfinite(cal.bM[r])) {
      failure = "Fit produced invalid numbers";
      return false;
    }
  }

  // Quality: angle of each shot's laser direction from its group's mean.
  double sumSquares = 0.0, maxError = 0.0;
  int maxShot = 0, groupIndex = 0;
  for (int first = 0; first < n && groupIndex < HEEB_GROUPS; ++groupIndex) {
    int end = first;
    while (end < n && data.group[end] == data.group[first]) ++end;
    double directions[HEEB_ROLLS * 2][3];
    double mean[3] = {0, 0, 0};
    const int groupSize = min(end - first, HEEB_ROLLS * 2);
    for (int j = 0; j < groupSize; ++j) {
      double g[3], m[3];
      for (int r = 0; r < 3; ++r) {
        g[r] = cal.bG[r]; m[r] = cal.bM[r];
        for (int c = 0; c < 3; ++c) {
          g[r] += static_cast<double>(cal.aG[r][c]) * data.g[first + j][c];
          m[r] += static_cast<double>(cal.aM[r][c]) * data.m[first + j][c];
        }
      }
      if (!laserDirection(g, m, directions[j])) {
        failure = "Could not compute a shot direction";
        return false;
      }
      for (int k = 0; k < 3; ++k) mean[k] += directions[j][k];
    }
    normalize3(mean);
    double groupSquares = 0.0;
    for (int j = 0; j < groupSize; ++j) {
      double c[3];
      cross3(directions[j], mean, c);
      const double error = atan2(sqrt(dot3(c, c)), dot3(directions[j], mean)) *
                           RAD_TO_DEG_D;
      groupSquares += error * error;
      sumSquares += error * error;
      if (error > maxError) { maxError = error; maxShot = first + j + 1; }
    }
    result.groupError[groupIndex] = static_cast<float>(sqrt(groupSquares / groupSize));
    first = end;
  }

  cal.rmsError = static_cast<float>(sqrt(sumSquares / n));
  cal.maxError = static_cast<float>(maxError);
  cal.dip = static_cast<float>(acos(dipCos) * RAD_TO_DEG_D - 90.0);
  result.iterations = iteration + 1;
  result.maxShot = maxShot;
  result.groupCount = groupIndex;
  return true;
}

// ------------------------------------------------------------------
// Storage
// ------------------------------------------------------------------
bool saveHeeb(const HeebCalibration& calibration, const HeebShots& data) {
  if (!calibrationPreferences.begin("heeb", false)) return false;
  const bool saved =
    calibrationPreferences.putBytes("shots", &data, sizeof(data)) == sizeof(data) &&
    calibrationPreferences.putBytes("cal", &calibration, sizeof(calibration)) ==
      sizeof(calibration);
  calibrationPreferences.end();
  return saved;
}

bool loadHeebShots(HeebShots& data) {
  if (!calibrationPreferences.begin("heeb", true)) return false;
  const bool loaded =
    calibrationPreferences.getBytesLength("shots") == sizeof(data) &&
    calibrationPreferences.getBytes("shots", &data, sizeof(data)) == sizeof(data);
  calibrationPreferences.end();
  return loaded && data.magic == HEEB_SHOTS_MAGIC && data.count <= HEEB_MAX_SHOTS;
}

// ------------------------------------------------------------------
// User interaction
// ------------------------------------------------------------------
const char* qualityLabel(float rms) {
  if (rms < 0.5f) return "GOOD";
  if (rms < 1.0f) return "OK";
  return "POOR";
}

// Targets 1-12 are level, 45 deg up and 45 deg down, each towards N, E, S, W;
// 13 and 14 are steeply up and down. The compass points only need to be
// roughly 90 deg apart, but naming them makes it easy to keep track.
void describeShot(int group, int roll, char* text, size_t size) {
  static const char* rolls[HEEB_ROLLS] = {
    "top face up",
    "rolled 90 deg clockwise",
    "upside down",
    "rolled 270 deg clockwise"
  };
  static const char* headings[4] = {"N", "E", "S", "W"};
  const char* heading;
  const char* elevation;
  if (group < 12) {
    static const char* elevations[3] = {"level", "45 deg up", "45 deg down"};
    heading = headings[group % 4];
    elevation = elevations[group / 4];
  } else {
    heading = group == 12 ? "N" : "S";
    elevation = group == 12 ? "steeply up (70 deg)" : "steeply down (70 deg)";
  }
  snprintf(text, size, "%s %s, %s - %s",
           roll == 0 ? "New target roughly" : "Continue pointing",
           heading, elevation, rolls[roll]);
}

void sendPrompt() {
  const int shot = shots.count;
  const int group = shot / HEEB_ROLLS;
  const int roll = shot % HEEB_ROLLS;
  char text[160];
  describeShot(group, roll, text, sizeof(text));
  sendMessage(true).printf(
    "CAL,STATE=AIM,SHOT=%d,TOTAL=%d,GROUP=%d,GROUPS=%d,ROLL=%d,ROLLS=%d,MSG=%s\n",
    shot + 1, HEEB_MAX_SHOTS, group + 1, HEEB_GROUPS, roll + 1, HEEB_ROLLS, text);
}

void endSession() {
  phase = Phase::Idle;
  turnLaserOff();
  digitalWrite(CALIBRATION_LED_PIN, LOW);
}

void runFitAndReview() {
  sendMessage(true).printf("CAL,STATE=FITTING,SHOTS=%d\n", shots.count);
  const char* failure = "Unknown error";
  if (!fitHeeb(shots, pendingResult, failure)) {
    sendMessage(true).printf("CAL,STATE=FAILED,MSG=Calibration fit failed: %s\n", failure);
    endSession();
    return;
  }
  const HeebCalibration& cal = pendingResult.calibration;
  sendMessage(true).printf(
    "CAL,STATE=RESULT,RMS=%.2f,MAX=%.2f,MAXSHOT=%d,DIP=%.1f,ITER=%d,SHOTS=%d,QUALITY=%s\n",
    cal.rmsError, cal.maxError, pendingResult.maxShot, cal.dip,
    pendingResult.iterations, shots.count, qualityLabel(cal.rmsError));
  for (int group = 0; group < pendingResult.groupCount; ++group) {
    sendMessage(true).printf("CAL,STATE=GROUPERR,GROUP=%d,ERR=%.2f\n",
                             group + 1, pendingResult.groupError[group]);
  }
  sendMessage(true).println(
    "CAL,STATE=REVIEW,MSG=Send Y to save and apply this calibration, or N to discard it.");
  phase = Phase::Review;
  turnLaserOff();
}

void beginShot() {
  turnLaserOff();
  settleUntil = millis() + HEEB_SETTLE_MS;
  sampleCount = 0;
  phase = Phase::Settle;
  sendMessage(true).printf("CAL,STATE=SAMPLING,SHOT=%d\n", shots.count + 1);
}

// Angle (deg) between the mean accelerometer direction of the first and second
// halves of the shot. Averaging each half suppresses sensor noise, so this
// responds to the instrument actually turning rather than to noise. Only the
// accelerometer is used: a stale or missing rough mag calibration would
// distort magnetometer angles.
float shotMovement() {
  const int half = HEEB_SAMPLES_PER_SHOT / 2;
  double first[3] = {0, 0, 0}, second[3] = {0, 0, 0};
  for (int i = 0; i < HEEB_SAMPLES_PER_SHOT; ++i) {
    double* sum = i < half ? first : second;
    for (int axis = 0; axis < 3; ++axis) sum[axis] += sampleAccel[i][axis];
  }
  float a[3], b[3], ca[3], cb[3];
  for (int axis = 0; axis < 3; ++axis) {
    a[axis] = static_cast<float>(first[axis] / half);
    b[axis] = static_cast<float>(second[axis] / (HEEB_SAMPLES_PER_SHOT - half));
  }
  roughCalibratedAccel(a, ca);
  roughCalibratedAccel(b, cb);
  const double u[3] = {ca[0], ca[1], ca[2]};
  const double v[3] = {cb[0], cb[1], cb[2]};
  double c[3];
  cross3(u, v, c);
  return static_cast<float>(atan2(sqrt(dot3(c, c)), dot3(u, v)) * RAD_TO_DEG_D);
}

void completeShot() {
  float meanA[3], meanM[3];
  for (int axis = 0; axis < 3; ++axis) {
    double sumA = 0.0, sumM = 0.0;
    for (int i = 0; i < HEEB_SAMPLES_PER_SHOT; ++i) {
      sumA += sampleAccel[i][axis];
      sumM += sampleMag[i][axis];
    }
    meanA[axis] = static_cast<float>(sumA / HEEB_SAMPLES_PER_SHOT);
    meanM[axis] = static_cast<float>(sumM / HEEB_SAMPLES_PER_SHOT);
  }

  const float movement = shotMovement();
  turnLaserOn();
  phase = Phase::Aim;
  if (movement > HEEB_MAX_SHOT_MOVEMENT_DEG) {
    sendMessage(true).printf(
      "CAL,STATE=REJECT,SHOT=%d,MOVED=%.2f,MSG=Instrument moved during the shot; take the shot again.\n",
      shots.count + 1, movement);
    sendPrompt();
    return;
  }

  const int index = shots.count;
  shots.group[index] = static_cast<uint8_t>(index / HEEB_ROLLS);
  memcpy(shots.g[index], meanA, sizeof(meanA));
  memcpy(shots.m[index], meanM, sizeof(meanM));
  ++shots.count;
  sendMessage(true).printf("CAL,STATE=SHOT,SHOT=%d,MOVED=%.2f\n", shots.count, movement);

  if (shots.count >= HEEB_MAX_SHOTS) {
    runFitAndReview();
  } else {
    sendPrompt();
  }
}

void acceptPendingCalibration() {
  if (!saveHeeb(pendingResult.calibration, shots)) {
    sendMessage(true).println("CAL,STATE=FAILED,MSG=Could not save calibration; previous calibration unchanged.");
    endSession();
    return;
  }
  heebCalibration = pendingResult.calibration;
  heebCalibrationValid = true;
  sendMessage(true).printf("CAL,STATE=SAVED,RMS=%.2f\n", heebCalibration.rmsError);
  endSession();
}

}  // namespace

// ------------------------------------------------------------------
// Public interface
// ------------------------------------------------------------------
bool isHeebCalibrationActive() {
  return phase != Phase::Idle;
}

void startHeebCalibration() {
  if (isMagnetometerCalibrationActive() || isAccelerometerCalibrationActive()) {
    sendMessage(true).println("Finish other calibration before starting shot calibration.");
    return;
  }
  reading = false;
  laserCycleState = 0;
  shots = {};
  shots.magic = HEEB_SHOTS_MAGIC;
  digitalWrite(CALIBRATION_LED_PIN, HIGH);
  turnLaserOn();
  phase = Phase::Aim;
  sendMessage(true).printf("CAL,STATE=START,GROUPS=%d,ROLLS=%d,MINGROUPS=%d\n",
                           HEEB_GROUPS, HEEB_ROLLS, HEEB_MIN_GROUPS);
  if (!magCalibrationValid) {
    sendMessage(true).println(
      "CAL,STATE=INFO,MSG=No magnetometer calibration; running M first gives the fit a better starting point.");
  }
  sendMessage(true).println(
    "CAL,STATE=INFO,MSG=Keys: T=take shot, U=undo last shot, F=finish and fit now, X=abort.");
  sendPrompt();
}

bool processHeebCalibrationInput(char input) {
  if (phase == Phase::Idle) return false;
  if (input == '\r' || input == '\n' || input == ' ' || input == '\t') return true;
  const char command = static_cast<char>(toupper(static_cast<unsigned char>(input)));

  if (command == 'X') {
    sendMessage(true).println("CAL,STATE=ABORTED,MSG=Shot calibration aborted; previous calibration unchanged.");
    endSession();
    return true;
  }

  switch (phase) {
    case Phase::Aim:
      if (command == 'T') {
        beginShot();
      } else if (command == 'U') {
        if (shots.count > 0) --shots.count;
        sendPrompt();
      } else if (command == 'F') {
        const int groups = shots.count / HEEB_ROLLS;
        if (groups < HEEB_MIN_GROUPS) {
          sendMessage(true).printf(
            "CAL,STATE=INFO,MSG=At least %d complete groups are needed to fit; %d done.\n",
            HEEB_MIN_GROUPS, groups);
        } else {
          shots.count = static_cast<uint16_t>(groups * HEEB_ROLLS);
          runFitAndReview();
        }
      }
      break;
    case Phase::Review:
      if (command == 'Y') {
        acceptPendingCalibration();
      } else if (command == 'N') {
        sendMessage(true).println("CAL,STATE=DISCARDED,MSG=New calibration discarded; previous calibration unchanged.");
        endSession();
      }
      break;
    default:
      break;  // Busy sampling; only X is accepted.
  }
  return true;
}

void serviceHeebCalibration() {
  if (phase == Phase::Settle) {
    if (static_cast<long>(millis() - settleUntil) < 0) return;
    phase = Phase::Sampling;
  }
  if (phase != Phase::Sampling) return;
  if (!rm3100_dataReady()) return;

  readMagnetometer(sampleMag[sampleCount]);
  readAccelerometer(sampleAccel[sampleCount]);
  if (++sampleCount >= HEEB_SAMPLES_PER_SHOT) completeShot();
}

bool loadHeebCalibration() {
  heebCalibrationValid = false;
  if (!calibrationPreferences.begin("heeb", true)) {
    sendMessage(true).println("No shot calibration storage; using rough G/M calibration.");
    return false;
  }
  HeebCalibration stored;
  const bool loaded =
    calibrationPreferences.getBytesLength("cal") == sizeof(stored) &&
    calibrationPreferences.getBytes("cal", &stored, sizeof(stored)) == sizeof(stored);
  calibrationPreferences.end();

  bool valid = loaded && stored.magic == HEEB_CAL_MAGIC;
  for (int r = 0; r < 3 && valid; ++r) {
    valid = isfinite(stored.bG[r]) && isfinite(stored.bM[r]);
    for (int c = 0; c < 3 && valid; ++c) {
      valid = isfinite(stored.aG[r][c]) && isfinite(stored.aM[r][c]);
    }
  }
  if (!valid) {
    sendMessage(true).println("No valid shot calibration; using rough G/M calibration.");
    return false;
  }
  heebCalibration = stored;
  heebCalibrationValid = true;
  sendMessage(true).printf("Loaded shot calibration (RMS error %.2f deg).\n",
                           heebCalibration.rmsError);
  return true;
}

void refitStoredHeebShots() {
  if (isMagnetometerCalibrationActive() || isAccelerometerCalibrationActive() ||
      isHeebCalibrationActive()) {
    sendMessage(true).println("Finish other calibration before refitting.");
    return;
  }
  if (!loadHeebShots(shots)) {
    sendMessage(true).println("CAL,STATE=FAILED,MSG=No stored calibration shots to refit.");
    return;
  }
  reading = false;
  laserCycleState = 0;
  digitalWrite(CALIBRATION_LED_PIN, HIGH);
  phase = Phase::Aim;  // So runFitAndReview's failure path ends the session cleanly.
  runFitAndReview();
}

void dumpHeebCalibration() {
  HeebShots stored;
  if (loadHeebShots(stored)) {
    sendMessage(true).printf("Stored calibration shots: %d\n", stored.count);
    for (int i = 0; i < stored.count; ++i) {
      sendMessage(true).printf(
        "CALSHOT,N=%d,GROUP=%d,GX=%.2f,GY=%.2f,GZ=%.2f,MX=%.2f,MY=%.2f,MZ=%.2f\n",
        i + 1, stored.group[i] + 1, stored.g[i][0], stored.g[i][1], stored.g[i][2],
        stored.m[i][0], stored.m[i][1], stored.m[i][2]);
    }
  } else {
    sendMessage(true).println("No stored calibration shots.");
  }
  if (!heebCalibrationValid) {
    sendMessage(true).println("No shot calibration in use.");
    return;
  }
  const HeebCalibration& cal = heebCalibration;
  sendMessage(true).printf("Shot calibration: RMS %.2f deg, max %.2f deg, dip %.1f deg\n",
                           cal.rmsError, cal.maxError, cal.dip);
  for (int r = 0; r < 3; ++r) {
    sendMessage(true).printf("aG[%d] = %.6e, %.6e, %.6e  bG = %.6f\n",
                             r, cal.aG[r][0], cal.aG[r][1], cal.aG[r][2], cal.bG[r]);
  }
  for (int r = 0; r < 3; ++r) {
    sendMessage(true).printf("aM[%d] = %.6e, %.6e, %.6e  bM = %.6f\n",
                             r, cal.aM[r][0], cal.aM[r][1], cal.aM[r][2], cal.bM[r]);
  }
}

void eraseHeebCalibration() {
  if (calibrationPreferences.begin("heeb", false)) {
    calibrationPreferences.remove("cal");
    calibrationPreferences.end();
  }
  heebCalibrationValid = false;
  sendMessage(true).println("Shot calibration erased (stored shots kept for refit); using rough G/M calibration.");
}

void calibratedVectors(const float rawAccel[3], const float rawMag[3],
                       float g[3], float m[3]) {
  if (!heebCalibrationValid) {
    roughCalibratedAccel(rawAccel, g);
    roughCalibratedMag(rawMag, m);
    return;
  }
  for (int r = 0; r < 3; ++r) {
    g[r] = heebCalibration.bG[r];
    m[r] = heebCalibration.bM[r];
    for (int c = 0; c < 3; ++c) {
      g[r] += heebCalibration.aG[r][c] * rawAccel[c];
      m[r] += heebCalibration.aM[r][c] * rawMag[c];
    }
  }
}

bool laserBearingInclination(const float g[3], const float m[3],
                             float& bearing, float& inclination) {
  inclination = atan2f(g[0], sqrtf(g[1] * g[1] + g[2] * g[2])) * (180.0f / PI);
  bearing = NAN;
  if (!heebCalibrationValid && !magCalibrationValid) return false;

  const double gd[3] = {g[0], g[1], g[2]};
  const double md[3] = {m[0], m[1], m[2]};
  double d[3];
  if (!laserDirection(gd, md, d)) return false;
  double heading = atan2(d[1], d[0]) * RAD_TO_DEG_D;
  if (heading < 0.0) heading += 360.0;
  if (heading >= 360.0) heading -= 360.0;
  bearing = static_cast<float>(heading);
  return true;
}
