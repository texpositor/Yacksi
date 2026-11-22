package me.speleologist.yacsi.scan

import me.speleologist.yacsi.protocol.FirmwareInfo
import me.speleologist.yacsi.protocol.ScanSample
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import java.util.Locale

/**
 * CSV layout of a SACSI run file: a few `#` comment lines describing the run,
 * a header row, then one row per sample. Distances are in millimetres as the
 * device sends them; Z is down the shaft (laser), Y sideways (VL53L4CD).
 */
object RunCsv {
    private val fileNameFormat = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH-mm-ss")
    private val timeFormat = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS")

    val COLUMNS = listOf(
        "phone_time", "n", "t_ms", "z_mm", "z_age_ms", "laser_err", "y_mm", "y_status",
        "y_quality", "heading_deg", "y_incl_deg", "tilt_deg",
        "ax", "ay", "az", "mx", "my", "mz", "cal"
    )

    fun fileName(start: LocalDateTime): String = "SACSI_${start.format(fileNameFormat)}.csv"

    fun header(start: LocalDateTime, firmware: FirmwareInfo?): String = buildString {
        append("# SACSI side-scan run\n")
        append("# started,${start.format(timeFormat)}\n")
        append("# firmware,${firmware?.summary ?: "unknown"}\n")
        append("# z = laser distance down (latest reading, z_age_ms old); y = side distance\n")
        append("# y_quality: GOOD (status 0), DOUBTFUL (status 1-2), INVALID (status >= 3 or 0 mm), NONE\n")
        append(COLUMNS.joinToString(","))
        append('\n')
    }

    fun row(sample: ScanSample, phoneTime: LocalDateTime): String = listOf(
        phoneTime.format(timeFormat),
        sample.n,
        sample.tMs,
        sample.zMm,
        sample.zAgeMs,
        sample.laserError,
        sample.yMm,
        sample.yStatus,
        sample.sideQuality.name,
        sample.heading?.let(::angle),
        sample.yIncl?.let(::angle),
        sample.tilt?.let(::angle),
        sample.ax, sample.ay, sample.az,
        sample.mx, sample.my, sample.mz,
        sample.calibration
    ).joinToString(",", postfix = "\n") { it?.toString() ?: "" }

    private fun angle(value: Float) = "%.1f".format(Locale.US, value)
}
