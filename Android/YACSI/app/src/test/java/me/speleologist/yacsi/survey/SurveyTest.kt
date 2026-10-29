package me.speleologist.yacsi.survey

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SurveyTest {
    @Test
    fun nextStation_continuesFromFrom() {
        assertEquals("LB2", nextStation("LB1", emptyList()))
    }

    @Test
    fun nextStation_usesHighestStationWhenGoingBack() {
        val stations = listOf("LB1", "LB2", "LB3", "LB9")
        assertEquals("LB10", nextStation("LB3", stations))
    }

    @Test
    fun nextStation_tracksEachPrefixSeparately() {
        val stations = listOf("LB1", "LB20", "C1", "C2")
        assertEquals("C3", nextStation("C1", stations))
        assertEquals("lb21", nextStation("lb5", stations))
    }

    @Test
    fun nextStation_keepsZeroPadding() {
        assertEquals("LB010", nextStation("LB009", emptyList()))
    }

    @Test
    fun nextStation_nullWithoutNumber() {
        assertNull(nextStation("ENTRANCE", listOf("LB1")))
    }

    @Test
    fun sanitizeStation_removesCsvSeparators() {
        assertEquals("LB1", sanitizeStation(" L,B\"1 "))
    }

    @Test
    fun csv_roundTrips() {
        val legs = listOf(
            SurveyLeg("LB1", "LB2", 12.345, 123.4, -5.2,
                mapOf(LrudSide.LEFT to 1.2, LrudSide.DOWN to 0.4), "2026-10-25T14:03:12"),
            SurveyLeg("LB2", "LB3", 3.0, 0.0, 45.0)
        )
        val text = SurveyCsv.format(legs)
        assertEquals(
            SurveyCsv.HEADER + "\n" +
                "LB1,LB2,12.345,123.4,-5.2,1.20,,,0.40,2026-10-25T14:03:12\n" +
                "LB2,LB3,3.000,0.0,45.0,,,,,\n",
            text
        )
        assertEquals(SurveyData(legs), SurveyCsv.parse(text))
    }

    @Test
    fun csv_roundTripsStationLrud() {
        val data = SurveyData(
            legs = listOf(SurveyLeg("LB1", "LB2", 3.0, 0.0, 45.0)),
            stationLrud = listOf(
                StationLrud("LB2", mapOf(LrudSide.LEFT to 0.8, LrudSide.UP to 2.0), "2026-10-27T10:00:00")
            )
        )
        val text = SurveyCsv.format(data)
        assertEquals(
            SurveyCsv.HEADER + "\n" +
                "LB1,LB2,3.000,0.0,45.0,,,,,\n" +
                "LB2,,,,,0.80,,2.00,,2026-10-27T10:00:00\n",
            text
        )
        assertEquals(data, SurveyCsv.parse(text))
    }

    @Test
    fun csv_readsFilesWithoutStationRows() {
        val text = SurveyCsv.HEADER + "\nLB1,LB2,3.000,0.0,45.0,,,,,\n"
        assertEquals(emptyList<StationLrud>(), SurveyCsv.parse(text).stationLrud)
        assertEquals(1, SurveyCsv.parse(text).legs.size)
    }
}
