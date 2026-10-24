package me.speleologist.yacsi.protocol

data class YacsiReading(
    val distMm: Float = 0f,
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

fun parseYacsiLine(line: String): YacsiReading? {
    if (!line.contains("DIST_MM=")) return null
    try {
        var distMm = 0f
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
