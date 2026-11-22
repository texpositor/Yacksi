package me.speleologist.yacsi.scan

import me.speleologist.yacsi.protocol.FirmwareInfo
import me.speleologist.yacsi.protocol.parseScanLine
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDateTime

class RunCsvTest {
    private val start = LocalDateTime.of(2026, 11, 20, 14, 3, 22, 123_000_000)

    @Test
    fun fileName_fromStartTime() {
        assertEquals("SACSI_2026-11-20_14-03-22.csv", RunCsv.fileName(start))
    }

    @Test
    fun header_commentsThenColumns() {
        val lines = RunCsv.header(start, FirmwareInfo("SACSI", "0.1.0", "2026-11-18 21:14:34", "50151fa"))
            .trimEnd('\n').lines()
        assertTrue(lines.dropLast(1).all { it.startsWith("#") })
        assertTrue(lines.any { it == "# firmware,SACSI 0.1.0 (2026-11-18 21:14:34, 50151fa)" })
        assertEquals(RunCsv.COLUMNS.joinToString(","), lines.last())
    }

    @Test
    fun row_fullSample() {
        val sample = parseScanLine(
            "SCAN,N=8,T=2953,Z_MM=839,Z_AGE_MS=31,LASER_ERR=0,Y_MM=744,Y_STATUS=0,HEADING=36.7," +
                "YINCL=-1.9,TILT=1.9,AX=-253308,AY=-7882,AZ=-3615,MX=2586,MY=1178,MZ=-1102,CAL=SHOT"
        )!!
        assertEquals(
            "2026-11-20T14:03:22.123,8,2953,839,31,0,744,0,GOOD,36.7,-1.9,1.9," +
                "-253308,-7882,-3615,2586,1178,-1102,SHOT\n",
            RunCsv.row(sample, start)
        )
    }

    @Test
    fun row_blanksStayEmptyAndColumnsLineUp() {
        val sample = parseScanLine(
            "SCAN,N=1,T=302,Z_MM=,Z_AGE_MS=,LASER_ERR=0,Y_MM=,Y_STATUS=,HEADING=nan,YINCL=1.5,TILT=90.5," +
                "AX=3771,AY=2216,AZ=254718,MX=-91,MY=1231,MZ=-3761,CAL=ROUGH"
        )!!
        val row = RunCsv.row(sample, start)
        assertEquals(RunCsv.COLUMNS.size, row.trimEnd('\n').split(",").size)
        assertTrue(row.startsWith("2026-11-20T14:03:22.123,1,302,,,0,,,NONE,,1.5,90.5,"))
    }
}
