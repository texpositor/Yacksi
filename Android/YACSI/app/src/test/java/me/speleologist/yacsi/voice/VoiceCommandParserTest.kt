package me.speleologist.yacsi.voice

import me.speleologist.yacsi.survey.LrudSide
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class VoiceCommandParserTest {
    private fun number(text: String) = parseSpokenNumber(text.split(" "))

    @Test
    fun parseSpokenNumber_compoundNumbers() {
        assertEquals(1234, number("one thousand two hundred and thirty four"))
        assertEquals(1234, number("one thousand two hundred thirty four"))
        assertEquals(765, number("seven hundred and sixty five"))
        assertEquals(96, number("ninety six"))
        assertEquals(2005, number("two thousand and five"))
        assertEquals(100, number("hundred"))
        assertEquals(15, number("fifteen"))
    }

    @Test
    fun parseSpokenNumber_zero() {
        assertEquals(0, number("zero"))
        assertEquals(0, number("nought"))
    }

    @Test
    fun parseSpokenNumber_digitByDigit() {
        assertEquals(765, number("seven six five"))
        assertEquals(1234, number("one two three four"))
        assertEquals(5, number("zero five"))
    }

    @Test
    fun parseSpokenNumber_nullWhenNoNumber() {
        assertNull(parseSpokenNumber(emptyList()))
        assertNull(parseSpokenNumber(listOf("and")))
    }

    @Test
    fun parseVoiceCommand_lrud() {
        assertEquals(VoiceAction.SetLrud(LrudSide.LEFT, 1234),
            parseVoiceCommand("yak see left one thousand two hundred and thirty four"))
        assertEquals(VoiceAction.SetLrud(LrudSide.RIGHT, 96), parseVoiceCommand("yak see right ninety six"))
        assertEquals(VoiceAction.SetLrud(LrudSide.UP, 0), parseVoiceCommand("yak see up zero"))
        assertEquals(VoiceAction.SetLrud(LrudSide.DOWN, 765), parseVoiceCommand("[unk] see down seven six five"))
    }

    @Test
    fun parseVoiceCommand_sideWithoutNumber() {
        assertEquals(VoiceAction.MissingNumber(LrudSide.LEFT), parseVoiceCommand("yak see left"))
        assertEquals(VoiceAction.MissingNumber(LrudSide.UP), parseVoiceCommand("yak see up [unk]"))
    }

    @Test
    fun parseVoiceCommand_legAndSection() {
        assertEquals(VoiceAction.SetTargetLeg, parseVoiceCommand("yak see leg"))
        assertEquals(VoiceAction.SetTargetSection, parseVoiceCommand("yak see section"))
        assertNull(parseVoiceCommand("yak see"))
        assertNull(parseVoiceCommand("[unk]"))
    }
}
