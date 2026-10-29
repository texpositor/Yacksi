package me.speleologist.yacsi.survey

import java.util.Locale

enum class LrudSide(val label: String) {
    LEFT("L"),
    RIGHT("R"),
    UP("U"),
    DOWN("D")
}

/** One survey leg; the LRUD distances are taken at [from], the station the surveyor is At. */
data class SurveyLeg(
    val from: String,
    val to: String,
    val distanceM: Double,
    val bearing: Double,
    val inclination: Double,
    val lrud: Map<LrudSide, Double> = emptyMap(),
    val time: String = ""
)

/**
 * LRUD taken at a station with no leg from it carrying them yet: the station being
 * surveyed before its next leg is shot, or the end of a passage.
 */
data class StationLrud(
    val station: String,
    val lrud: Map<LrudSide, Double>,
    val time: String = ""
)

/** Everything in a survey file. */
data class SurveyData(
    val legs: List<SurveyLeg> = emptyList(),
    val stationLrud: List<StationLrud> = emptyList()
)

fun List<SurveyLeg>.stations(): List<String> = flatMap { listOf(it.from, it.to) }

/**
 * Survey file format: one leg per line,
 * `From,To,Distance_m,Bearing_deg,Inclination_deg,Left_m,Right_m,Up_m,Down_m,Time`,
 * with blank LRUD columns where none was taken. LRUD at a station with no leg from it
 * is a station row after the legs: the station in From, then blank To and shot columns,
 * `LB9,,,,,0.80,1.10,2.00,0.30,Time`.
 */
object SurveyCsv {
    const val HEADER = "From,To,Distance_m,Bearing_deg,Inclination_deg,Left_m,Right_m,Up_m,Down_m,Time"

    fun format(legs: List<SurveyLeg>, stationLrud: List<StationLrud> = emptyList()): String = buildString {
        append(HEADER).append('\n')
        legs.forEach { append(formatLeg(it)).append('\n') }
        stationLrud.forEach { append(formatStation(it)).append('\n') }
    }

    fun format(data: SurveyData): String = format(data.legs, data.stationLrud)

    fun formatLeg(leg: SurveyLeg): String = (
        listOf(
            leg.from,
            leg.to,
            "%.3f".format(Locale.US, leg.distanceM),
            "%.1f".format(Locale.US, leg.bearing),
            "%.1f".format(Locale.US, leg.inclination)
        ) +
            formatLrud(leg.lrud) +
            leg.time
        ).joinToString(",")

    private fun formatStation(station: StationLrud): String =
        (listOf(station.station, "", "", "", "") + formatLrud(station.lrud) + station.time).joinToString(",")

    private fun formatLrud(lrud: Map<LrudSide, Double>): List<String> =
        LrudSide.entries.map { side -> lrud[side]?.let { "%.2f".format(Locale.US, it) } ?: "" }

    fun parse(text: String): SurveyData {
        val rows = text.lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() && !it.startsWith("From,") }
            .map { line -> line.split(",").map { it.trim() } }
            .filter { it.size >= 5 }
            .toList()
        // Station rows have a station but no To or shot
        val (stationRows, legRows) = rows.partition { fields -> (1..4).all { fields[it].isEmpty() } }
        return SurveyData(
            legs = legRows.mapNotNull { parseLeg(it) },
            stationLrud = stationRows.map { fields ->
                StationLrud(fields[0], parseLrud(fields), fields.getOrNull(9) ?: "")
            }
        )
    }

    private fun parseLrud(fields: List<String>): Map<LrudSide, Double> =
        LrudSide.entries.mapIndexedNotNull { index, side ->
            fields.getOrNull(5 + index)?.toDoubleOrNull()?.let { side to it }
        }.toMap()

    private fun parseLeg(fields: List<String>): SurveyLeg? {
        val lrud = parseLrud(fields)
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
