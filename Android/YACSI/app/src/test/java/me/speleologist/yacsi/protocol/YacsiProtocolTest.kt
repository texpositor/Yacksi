package me.speleologist.yacsi.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class YacsiProtocolTest {
    @Test
    fun parseBatteryLine_low() {
        val message = parseBatteryLine("BATTERY_MV=6712,STATUS=LOW")!!
        assertEquals(6712, message.millivolts)
        assertTrue(message.isLow)
    }

    @Test
    fun parseBatteryLine_okAndNoBatteryAreNotLow() {
        assertFalse(parseBatteryLine("BATTERY_MV=8120,STATUS=OK")!!.isLow)
        assertFalse(parseBatteryLine("BATTERY_MV=140,STATUS=NONE")!!.isLow)
    }

    @Test
    fun parseBatteryLine_ignoresOtherLines() {
        assertNull(parseBatteryLine("DIST_OFFSET_MM=12,STATUS=CURRENT"))
        assertNull(parseBatteryLine("BATTERY_MV=abc,STATUS=LOW"))
    }

    @Test
    fun parseAccelPositionLine_prompt() {
        val message = parseAccelPositionLine("Position 3/6: Y axis pointing up")!!
        assertEquals(3, message.position)
        assertEquals(6, message.total)
        assertEquals("Y axis pointing up", message.orientation)
    }

    @Test
    fun parseAccelPositionLine_ignoresOtherLines() {
        assertNull(parseAccelPositionLine("Sampling position..."))
        assertNull(parseAccelPositionLine("Axis X offset: 12.0, scale: 1.000000"))
    }

    @Test
    fun parseYacsiLine_ignoresBatteryLine() {
        assertNull(parseYacsiLine("BATTERY_MV=6712,STATUS=LOW"))
    }
}
