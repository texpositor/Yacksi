package me.speleologist.yacsi.protocol

data class YacsiReading(
    val distMm: Float = 0f, // laser distance with the device's distance offset applied
    val laserMm: Float? = null, // uncorrected laser distance
    val ax: Int = 0,
    val ay: Int = 0,
    val az: Int = 0,
    val mx: Int = 0,
    val my: Int = 0,
    val mz: Int = 0,
    val bearing: Float? = null, // null if nan
    val incl: Float = 0f,
    val calibration: String? = null, // "SHOT" or "ROUGH"
    val mLen: Float? = null, // calibrated field magnitude; 1.0 under shot calibration
    val rawLine: String = ""
)

/**
 * Machine-readable calibration line from the device:
 * `CAL,STATE=<state>,KEY=value,...,MSG=<free text to end of line>`
 */
data class CalMessage(
    val state: String,
    val fields: Map<String, String>,
    val message: String?
) {
    fun int(key: String): Int? = fields[key]?.toIntOrNull()
    fun float(key: String): Float? = fields[key]?.toFloatOrNull()
}

fun parseCalLine(line: String): CalMessage? {
    if (!line.startsWith("CAL,")) return null
    val body = line.removePrefix("CAL,")
    val msgIndex = body.indexOf("MSG=")
    val fieldPart = if (msgIndex >= 0) body.substring(0, msgIndex) else body
    val message = if (msgIndex >= 0) body.substring(msgIndex + 4).trim() else null
    val fields = fieldPart.split(",").mapNotNull { part ->
        val kv = part.split("=", limit = 2)
        if (kv.size == 2) kv[0].trim() to kv[1].trim() else null
    }.toMap()
    val state = fields["STATE"] ?: return null
    return CalMessage(state, fields, message)
}

/**
 * Distance offset status line from the device:
 * `DIST_OFFSET_MM=<mm>,STATUS=CURRENT|SAVED|INVALID|FAILED`
 * where DIST_OFFSET_MM is always the offset now in use.
 */
data class DistanceOffsetMessage(val offsetMm: Int, val status: String)

fun parseDistanceOffsetLine(line: String): DistanceOffsetMessage? {
    if (!line.startsWith("DIST_OFFSET_MM=")) return null
    val fields = parseFields(line)
    val offset = fields["DIST_OFFSET_MM"]?.toIntOrNull() ?: return null
    return DistanceOffsetMessage(offset, fields["STATUS"] ?: "CURRENT")
}

/**
 * Battery status line the device sends before every reading:
 * `BATTERY_MV=<mV>,STATUS=OK|LOW|NONE` where NONE means no battery is
 * connected (the device is running from USB).
 */
data class BatteryMessage(val millivolts: Int, val status: String) {
    val isLow: Boolean get() = status == "LOW"
}

fun parseBatteryLine(line: String): BatteryMessage? {
    if (!line.startsWith("BATTERY_MV=")) return null
    val fields = parseFields(line)
    val millivolts = fields["BATTERY_MV"]?.toIntOrNull() ?: return null
    return BatteryMessage(millivolts, fields["STATUS"] ?: "OK")
}

/**
 * Rough accelerometer calibration prompt from the device:
 * `Position <n>/<total>: <orientation>`, e.g. `Position 1/6: X axis pointing up`
 */
data class AccelPositionMessage(val position: Int, val total: Int, val orientation: String)

private val accelPositionPattern = Regex("""Position (\d+)/(\d+): (.+)""")

fun parseAccelPositionLine(line: String): AccelPositionMessage? {
    val match = accelPositionPattern.matchEntire(line.trim()) ?: return null
    val (position, total, orientation) = match.destructured
    return AccelPositionMessage(position.toInt(), total.toInt(), orientation.trim())
}

private fun parseFields(line: String): Map<String, String> =
    line.split(",").mapNotNull { part ->
        val kv = part.split("=", limit = 2)
        if (kv.size == 2) kv[0].trim() to kv[1].trim() else null
    }.toMap()

fun parseYacsiLine(line: String): YacsiReading? {
    if (!line.contains("DIST_MM=")) return null
    try {
        var distMm = 0f
        var laserMm: Float? = null
        var ax = 0
        var ay = 0
        var az = 0
        var mx = 0
        var my = 0
        var mz = 0
        var bearing: Float? = null
        var incl = 0f
        var calibration: String? = null
        var mLen: Float? = null

        val parts = line.split(",")
        for (part in parts) {
            val kv = part.split("=")
            if (kv.size == 2) {
                val key = kv[0].trim()
                val value = kv[1].trim()
                when (key) {
                    "DIST_MM" -> distMm = value.toFloatOrNull() ?: 0f
                    "LASER_MM" -> laserMm = value.toFloatOrNull()
                    "AX" -> ax = value.toIntOrNull() ?: 0
                    "AY" -> ay = value.toIntOrNull() ?: 0
                    "AZ" -> az = value.toIntOrNull() ?: 0
                    "MX" -> mx = value.toIntOrNull() ?: 0
                    "MY" -> my = value.toIntOrNull() ?: 0
                    "MZ" -> mz = value.toIntOrNull() ?: 0
                    "BEARING" -> {
                        bearing = if (value.equals("nan", ignoreCase = true)) {
                            null
                        } else {
                            value.toFloatOrNull()
                        }
                    }
                    "INCL" -> incl = value.toFloatOrNull() ?: 0f
                    "CAL" -> calibration = value
                    "MLEN" -> mLen = value.toFloatOrNull()
                }
            }
        }
        return YacsiReading(
            distMm = distMm,
            laserMm = laserMm,
            ax = ax,
            ay = ay,
            az = az,
            mx = mx,
            my = my,
            mz = mz,
            bearing = bearing,
            incl = incl,
            calibration = calibration,
            mLen = mLen,
            rawLine = line
        )
    } catch (_: Exception) {
        return null
    }
}

// --- SACSI side scan ---

/** How far a side (VL53L4CD) distance can be trusted, from its range status. */
enum class SideQuality {
    GOOD,      // status 0
    DOUBTFUL,  // status 1-2 (sigma or signal warning) with a distance
    INVALID,   // status 3 or above, or a zero distance: logged but not shown
    NONE       // no side reading in this sample
}

/**
 * One side-scan sample from the device:
 * `SCAN,N=12,T=3010,Z_MM=2343,Z_AGE_MS=120,LASER_ERR=0,Y_MM=845,Y_STATUS=0,
 *  HEADING=123.4,YINCL=-1.2,TILT=2.8,AX=..,AY=..,AZ=..,MX=..,MY=..,MZ=..,CAL=SHOT`
 * Z is the latest laser distance (down), Y the side distance; blank values
 * mean no reading and `nan` an undefined angle.
 */
data class ScanSample(
    val n: Int,
    val tMs: Long,
    val zMm: Int?,
    val zAgeMs: Int?,
    val laserError: Int,
    val yMm: Int?,
    val yStatus: Int?,
    val heading: Float?,
    val yIncl: Float?,
    val tilt: Float?,
    val ax: Int?,
    val ay: Int?,
    val az: Int?,
    val mx: Int?,
    val my: Int?,
    val mz: Int?,
    val calibration: String?,
    val rawLine: String
) {
    val sideQuality: SideQuality
        get() = when {
            yMm == null || yStatus == null -> SideQuality.NONE
            yMm <= 0 || yStatus >= 3 -> SideQuality.INVALID
            yStatus == 0 -> SideQuality.GOOD
            else -> SideQuality.DOUBTFUL
        }
}

fun parseScanLine(line: String): ScanSample? {
    if (!line.startsWith("SCAN,")) return null
    val fields = parseFields(line)
    val n = fields["N"]?.toIntOrNull() ?: return null
    fun int(key: String) = fields[key]?.toIntOrNull()
    fun angle(key: String) = fields[key]?.toFloatOrNull()?.takeIf { !it.isNaN() }
    return ScanSample(
        n = n,
        tMs = fields["T"]?.toLongOrNull() ?: 0L,
        zMm = int("Z_MM"),
        zAgeMs = int("Z_AGE_MS"),
        laserError = int("LASER_ERR") ?: 0,
        yMm = int("Y_MM"),
        yStatus = int("Y_STATUS"),
        heading = angle("HEADING"),
        yIncl = angle("YINCL"),
        tilt = angle("TILT"),
        ax = int("AX"),
        ay = int("AY"),
        az = int("AZ"),
        mx = int("MX"),
        my = int("MY"),
        mz = int("MZ"),
        calibration = fields["CAL"],
        rawLine = line
    )
}

/**
 * Scan start/stop announcements:
 * `SCAN,STATE=STARTED` and `SCAN,STATE=STOPPED,SAMPLES=<n>,TOF_ERRORS=<n>,LASER_RESTARTS=<n>`.
 */
data class ScanStateMessage(
    val state: String,
    val samples: Int?,
    val tofErrors: Int?,
    val laserRestarts: Int?
) {
    val isStarted: Boolean get() = state == "STARTED"
    val isStopped: Boolean get() = state == "STOPPED"
}

fun parseScanStateLine(line: String): ScanStateMessage? {
    if (!line.startsWith("SCAN,")) return null
    val fields = parseFields(line)
    val state = fields["STATE"] ?: return null
    return ScanStateMessage(
        state = state,
        samples = fields["SAMPLES"]?.toIntOrNull(),
        tofErrors = fields["TOF_ERRORS"]?.toIntOrNull(),
        laserRestarts = fields["LASER_RESTARTS"]?.toIntOrNull()
    )
}

/** Firmware identity: `FIRMWARE=SACSI,VERSION=0.1.0,BUILD=2026-11-18 21:14:34,GIT=50151fa`. */
data class FirmwareInfo(val name: String, val version: String, val build: String, val git: String) {
    val summary: String get() = "$name $version ($build, $git)"
}

fun parseFirmwareLine(line: String): FirmwareInfo? {
    if (!line.startsWith("FIRMWARE=")) return null
    val fields = parseFields(line)
    return FirmwareInfo(
        name = fields["FIRMWARE"] ?: return null,
        version = fields["VERSION"] ?: "",
        build = fields["BUILD"] ?: "",
        git = fields["GIT"] ?: ""
    )
}
