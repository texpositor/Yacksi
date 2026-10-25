package me.speleologist.yacsi.survey

import java.util.Locale

enum class LrudSide(val label: String) {
    LEFT("L"),
    RIGHT("R"),
    UP("U"),
    DOWN("D")
}

/** One survey leg; the LRUD distances are taken at the From station. */
data class SurveyLeg(
    val from: String,
    val to: String,
    val distanceM: Double,
    val bearing: Double,
    val inclination: Double,
    val lrud: Map<LrudSide, Double> = emptyMap(),
    val time: String = ""
)

fun List<SurveyLeg>.stations(): List<String> = flatMap { listOf(it.from, it.to) }

/**
 * Survey file format: one leg per line,
 * `From,To,Distance_m,Bearing_deg,Inclination_deg,Left_m,Right_m,Up_m,Down_m,Time`,
 * with blank LRUD columns where none was taken.
 */
object SurveyCsv {
    const val HEADER = "From,To,Distance_m,Bearing_deg,Inclination_deg,Left_m,Right_m,Up_m,Down_m,Time"

    fun format(legs: List<SurveyLeg>): String = buildString {
        append(HEADER).append('\n')
        legs.forEach { append(formatLeg(it)).append('\n') }
    }

    fun formatLeg(leg: SurveyLeg): String = (
        listOf(
            leg.from,
            leg.to,
            "%.3f".format(Locale.US, leg.distanceM),
            "%.1f".format(Locale.US, leg.bearing),
            "%.1f".format(Locale.US, leg.inclination)
        ) +
            LrudSide.entries.map { side -> leg.lrud[side]?.let { "%.2f".format(Locale.US, it) } ?: "" } +
            leg.time
        ).joinToString(",")

    fun parse(text: String): List<SurveyLeg> = text.lineSequence()
        .map { it.trim() }
        .filter { it.isNotEmpty() && !it.startsWith("From,") }
        .mapNotNull { parseLeg(it) }
        .toList()

    private fun parseLeg(line: String): SurveyLeg? {
        val fields = line.split(",").map { it.trim() }
        if (fields.size < 5) return null
        val lrud = LrudSide.entries.mapIndexedNotNull { index, side ->
            fields.getOrNull(5 + index)?.toDoubleOrNull()?.let { side to it }
        }.toMap()
        return SurveyLeg(
            from = fields[0],
            to = fields[1],
            distanceM = fields[2].toDoubleOrNull() ?: return null,
            bearing = fields[3].toDoubleOrNull() ?: return null,
            inclination = fields[4].toDoubleOrNull() ?: return null,
            lrud = lrud,
            time = fields.getOrNull(9) ?: ""
        )
    }
}
