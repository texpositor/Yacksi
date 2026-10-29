package me.speleologist.yacsi.voice

import me.speleologist.yacsi.survey.LrudSide

/** A command recognised from speech. */
sealed interface VoiceAction {
    data object SetTargetLeg : VoiceAction
    data object SetTargetSection : VoiceAction
    data class SetLrud(val side: LrudSide, val millimetres: Int) : VoiceAction
    /** A side was heard but no usable number followed it. */
    data class MissingNumber(val side: LrudSide) : VoiceAction
}

private val SMALL_NUMBERS = mapOf(
    "zero" to 0, "nought" to 0, "one" to 1, "two" to 2, "three" to 3, "four" to 4,
    "five" to 5, "six" to 6, "seven" to 7, "eight" to 8, "nine" to 9, "ten" to 10,
    "eleven" to 11, "twelve" to 12, "thirteen" to 13, "fourteen" to 14, "fifteen" to 15,
    "sixteen" to 16, "seventeen" to 17, "eighteen" to 18, "nineteen" to 19
)
private val TENS = mapOf(
    "twenty" to 20, "thirty" to 30, "forty" to 40, "fifty" to 50,
    "sixty" to 60, "seventy" to 70, "eighty" to 80, "ninety" to 90
)
private val NUMBER_WORDS = SMALL_NUMBERS.keys + TENS.keys + listOf("hundred", "thousand", "and")

private val SIDE_WORDS = mapOf(
    "left" to LrudSide.LEFT,
    "right" to LrudSide.RIGHT,
    "up" to LrudSide.UP,
    "down" to LrudSide.DOWN
)

/** Every word the recogniser may return; anything else comes back as [unk]. */
val VOICE_GRAMMAR_WORDS: List<String> =
    listOf("yak", "see", "leg", "section") + SIDE_WORDS.keys + NUMBER_WORDS

/**
 * Finds the first command word in [text] (Vosk's lower-case transcript). L, R, U and D
 * need a number after them, e.g. "yak see right seven hundred and sixty five".
 */
fun parseVoiceCommand(text: String): VoiceAction? {
    val words = text.lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() && it != "[unk]" }
    for ((index, word) in words.withIndex()) {
        when (word) {
            "leg" -> return VoiceAction.SetTargetLeg
            "section" -> return VoiceAction.SetTargetSection
        }
        val side = SIDE_WORDS[word] ?: continue
        val numberWords = words.drop(index + 1).takeWhile { it in NUMBER_WORDS }
        val millimetres = parseSpokenNumber(numberWords) ?: return VoiceAction.MissingNumber(side)
        return VoiceAction.SetLrud(side, millimetres)
    }
    return null
}

/**
 * Parses spoken English numbers: "one thousand two hundred and thirty four" -> 1234,
 * and digit by digit, "seven six five" -> 765. Null if the words don't form a number.
 */
fun parseSpokenNumber(words: List<String>): Int? {
    val tokens = words.filter { it != "and" }
    if (tokens.isEmpty()) return null

    if (tokens.size > 1 && tokens.all { (SMALL_NUMBERS[it] ?: 10) <= 9 }) {
        return tokens.joinToString("") { SMALL_NUMBERS.getValue(it).toString() }.toIntOrNull()
    }

    var total = 0
    var current = 0
    for (token in tokens) {
        when (token) {
            in SMALL_NUMBERS -> current += SMALL_NUMBERS.getValue(token)
            in TENS -> current += TENS.getValue(token)
            "hundred" -> current = (if (current == 0) 1 else current) * 100
            "thousand" -> {
                total += (if (current == 0) 1 else current) * 1000
                current = 0
            }
            else -> return null
        }
    }
    return total + current
}
