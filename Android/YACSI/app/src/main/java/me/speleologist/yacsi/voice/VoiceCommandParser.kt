package me.speleologist.yacsi.voice

/** A command recognised from speech. */
sealed interface VoiceAction {
    data object Go : VoiceAction
    data object Stop : VoiceAction
}

/**
 * Every word the recogniser may return; anything else comes back as [unk].
 * "sack" and "see" let "sack see go" be said as one phrase, which carries
 * better than a lone "go"; only the command word matters.
 */
val VOICE_GRAMMAR_WORDS: List<String> = listOf("sack", "see", "go", "stop")

/** Finds the first command word in [text] (Vosk's lower-case transcript). */
fun parseVoiceCommand(text: String): VoiceAction? {
    for (word in text.lowercase().split(Regex("\\s+"))) {
        when (word) {
            "go" -> return VoiceAction.Go
            "stop" -> return VoiceAction.Stop
        }
    }
    return null
}
