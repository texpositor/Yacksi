package me.speleologist.yacsi.voice

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class VoiceCommandParserTest {
    @Test
    fun parseVoiceCommand_goAndStop() {
        assertEquals(VoiceAction.Go, parseVoiceCommand("go"))
        assertEquals(VoiceAction.Go, parseVoiceCommand("sack see go"))
        assertEquals(VoiceAction.Stop, parseVoiceCommand("stop"))
        assertEquals(VoiceAction.Stop, parseVoiceCommand("[unk] see stop"))
    }

    @Test
    fun parseVoiceCommand_firstCommandWins() {
        assertEquals(VoiceAction.Stop, parseVoiceCommand("stop go"))
    }

    @Test
    fun parseVoiceCommand_nothingRecognised() {
        assertNull(parseVoiceCommand("sack see"))
        assertNull(parseVoiceCommand("[unk]"))
        assertNull(parseVoiceCommand(""))
    }
}
