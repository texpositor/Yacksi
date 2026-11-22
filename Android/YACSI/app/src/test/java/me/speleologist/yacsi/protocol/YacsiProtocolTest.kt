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

    // Lines below are taken from real instrument output.

    @Test
    fun parseScanLine_fullSample() {
        val sample = parseScanLine(
            "SCAN,N=8,T=2953,Z_MM=839,Z_AGE_MS=31,LASER_ERR=0,Y_MM=744,Y_STATUS=0,HEADING=36.7," +
                "YINCL=-1.9,TILT=1.9,AX=-253308,AY=-7882,AZ=-3615,MX=2586,MY=1178,MZ=-1102,CAL=SHOT"
        )!!
        assertEquals(8, sample.n)
        assertEquals(2953L, sample.tMs)
        assertEquals(839, sample.zMm)
        assertEquals(31, sample.zAgeMs)
        assertEquals(0, sample.laserError)
        assertEquals(744, sample.yMm)
        assertEquals(36.7f, sample.heading!!, 0.001f)
        assertEquals(-1.9f, sample.yIncl!!, 0.001f)
        assertEquals(1.9f, sample.tilt!!, 0.001f)
        assertEquals(-253308, sample.ax)
        assertEquals(-1102, sample.mz)
        assertEquals("SHOT", sample.calibration)
        assertEquals(SideQuality.GOOD, sample.sideQuality)
    }

    @Test
    fun parseScanLine_blankValuesAndNan() {
        val sample = parseScanLine(
            "SCAN,N=1,T=302,Z_MM=,Z_AGE_MS=,LASER_ERR=0,Y_MM=,Y_STATUS=,HEADING=nan,YINCL=1.5,TILT=90.5," +
                "AX=3771,AY=2216,AZ=254718,MX=-91,MY=1231,MZ=-3761,CAL=ROUGH"
        )!!
        assertNull(sample.zMm)
        assertNull(sample.zAgeMs)
        assertNull(sample.yMm)
        assertNull(sample.heading)
        assertEquals(SideQuality.NONE, sample.sideQuality)
    }

    @Test
    fun scanSample_sideQualityFromStatus() {
        fun quality(y: String, status: String) =
            parseScanLine("SCAN,N=1,T=0,Y_MM=$y,Y_STATUS=$status")!!.sideQuality
        assertEquals(SideQuality.GOOD, quality("744", "0"))
        assertEquals(SideQuality.DOUBTFUL, quality("970", "2"))
        assertEquals(SideQuality.DOUBTFUL, quality("970", "1"))
        assertEquals(SideQuality.INVALID, quality("0", "2"))
        assertEquals(SideQuality.INVALID, quality("1845", "4"))
    }

    @Test
    fun parseScanLine_ignoresStateAndOtherLines() {
        assertNull(parseScanLine("SCAN,STATE=STARTED"))
        assertNull(parseScanLine("BATTERY_MV=7954,STATUS=OK"))
    }

    @Test
    fun parseScanStateLine_startedAndStopped() {
        assertTrue(parseScanStateLine("SCAN,STATE=STARTED")!!.isStarted)
        val stopped = parseScanStateLine("SCAN,STATE=STOPPED,SAMPLES=44,TOF_ERRORS=1,LASER_RESTARTS=4")!!
        assertTrue(stopped.isStopped)
        assertEquals(44, stopped.samples)
        assertEquals(1, stopped.tofErrors)
        assertEquals(4, stopped.laserRestarts)
        // Older firmware sent only the sample count
        assertNull(parseScanStateLine("SCAN,STATE=STOPPED,SAMPLES=45")!!.tofErrors)
    }

    @Test
    fun parseScanStateLine_ignoresSamples() {
        assertNull(parseScanStateLine("SCAN,N=1,T=302,Z_MM=840"))
    }

    @Test
    fun parseFirmwareLine_versionLine() {
        val info = parseFirmwareLine("FIRMWARE=SACSI,VERSION=0.1.0,BUILD=2026-11-18 21:14:34,GIT=50151fa-dirty")!!
        assertEquals("SACSI", info.name)
        assertEquals("0.1.0", info.version)
        assertEquals("2026-11-18 21:14:34", info.build)
        assertEquals("50151fa-dirty", info.git)
        assertNull(parseFirmwareLine("BATTERY_MV=7954,STATUS=OK"))
    }
}
