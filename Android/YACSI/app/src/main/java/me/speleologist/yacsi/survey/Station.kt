package me.speleologist.yacsi.survey

private val STATION_PATTERN = Regex("""^(.*?)(\d+)$""")

/** A station name split into its prefix and trailing number, e.g. "LB12" -> ("LB", 12). */
data class StationName(val prefix: String, val number: Int, val digits: Int)

fun parseStation(name: String): StationName? {
    val match = STATION_PATTERN.matchEntire(name.trim()) ?: return null
    val digitsText = match.groupValues[2]
    val number = digitsText.toIntOrNull() ?: return null
    // Keep zero padding (e.g. LB01 -> LB02) only when the user typed it.
    val digits = if (digitsText.length > 1 && digitsText.startsWith("0")) digitsText.length else 0
    return StationName(match.groupValues[1], number, digits)
}

/** Station names are written to CSV unquoted, so drop separators and whitespace. */
fun sanitizeStation(text: String): String =
    text.filterNot { it == ',' || it == '"' || it.isWhitespace() }

/**
 * The station after the highest-numbered known station sharing [from]'s prefix,
 * so going back to an earlier station to survey a branch continues the numbering
 * instead of reusing numbers. Null when [from] has no trailing number.
 */
fun nextStation(from: String, knownStations: Iterable<String>): String? {
    val start = parseStation(from) ?: return null
    val highest = knownStations
        .mapNotNull { parseStation(it) }
        .filter { it.prefix.equals(start.prefix, ignoreCase = true) }
        .maxOfOrNull { it.number }
    val next = maxOf(start.number, highest ?: start.number) + 1
    return start.prefix + next.toString().padStart(start.digits, '0')
}
