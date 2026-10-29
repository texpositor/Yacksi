package me.speleologist.yacsi.viewmodel

import android.app.Application
import android.bluetooth.BluetoothDevice
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.speleologist.yacsi.bluetooth.BluetoothService
import me.speleologist.yacsi.bluetooth.ConnectionState
import me.speleologist.yacsi.protocol.CalMessage
import me.speleologist.yacsi.protocol.DistanceOffsetMessage
import me.speleologist.yacsi.protocol.YacsiReading
import me.speleologist.yacsi.protocol.parseCalLine
import me.speleologist.yacsi.protocol.parseDistanceOffsetLine
import me.speleologist.yacsi.protocol.parseYacsiLine
import me.speleologist.yacsi.survey.LrudSide
import me.speleologist.yacsi.survey.StationLrud
import me.speleologist.yacsi.survey.SurveyData
import me.speleologist.yacsi.survey.SurveyLeg
import me.speleologist.yacsi.survey.SurveyStorage
import me.speleologist.yacsi.survey.nextStation
import me.speleologist.yacsi.survey.sanitizeStation
import me.speleologist.yacsi.survey.stations
import me.speleologist.yacsi.voice.VoiceAction
import me.speleologist.yacsi.voice.VoiceCommandManager
import me.speleologist.yacsi.voice.VoiceState
import java.time.LocalDateTime
import java.time.temporal.ChronoUnit
import java.util.Locale

enum class AppMode {
    NORMAL,
    ACCEL_CALIBRATION,
    SHOT_CALIBRATION,
    MAG_CALIBRATION
}

enum class ShotPhase {
    AIMING,
    SAMPLING,
    FITTING,
    REVIEW
}

data class ShotCalibrationResult(
    val rmsError: Float,
    val maxError: Float,
    val maxShot: Int,
    val dip: Float,
    val quality: String,
    val shots: Int,
    val groupErrors: List<Float> = emptyList()
)

data class ShotCalibrationState(
    val phase: ShotPhase = ShotPhase.AIMING,
    val shot: Int = 1, // next shot to take, 1-based
    val totalShots: Int = 56,
    val group: Int = 1,
    val groups: Int = 14,
    val roll: Int = 1,
    val rolls: Int = 4,
    val minGroups: Int = 8,
    val instruction: String = "",
    val notice: String? = null,
    val noticeIsError: Boolean = false,
    val result: ShotCalibrationResult? = null
) {
    val shotsTaken: Int get() = shot - 1
    val completedGroups: Int get() = shotsTaken / rolls
    val canFinishEarly: Boolean get() = completedGroups >= minGroups
}

/** What the next reading from the device is recorded as. */
enum class ShotTarget(val label: String, val side: LrudSide?) {
    LEG("Leg", null),
    LEFT("L", LrudSide.LEFT),
    RIGHT("R", LrudSide.RIGHT),
    UP("U", LrudSide.UP),
    DOWN("D", LrudSide.DOWN)
}

data class SurveyState(
    val fileName: String? = null, // null when no survey is open
    val legs: List<SurveyLeg> = emptyList(),
    val atStation: String = "",
    val toStation: String = "",
    val lrud: Map<LrudSide, String> = emptyMap(), // the LRUD boxes, for the At station
    // LRUD at stations with no leg from them yet, kept in step with the boxes and
    // written to the file as station rows so nothing is lost before the next leg
    val stationLrud: Map<String, StationLrud> = emptyMap(),
    val target: ShotTarget = ShotTarget.LEG,
    val lrudBeforeLeg: Boolean = false, // start each station with L, R, U, D before the leg
    val notice: String? = null,
    val noticeIsError: Boolean = false
) {
    val isOpen: Boolean get() = fileName != null

    fun toData() = SurveyData(legs, stationLrud.values.toList())
}

data class YacsiUiState(
    val connectionState: ConnectionState = ConnectionState.DISCONNECTED,
    val connectedDeviceName: String? = null,
    val isAutoReconnectEnabled: Boolean = false,
    val appMode: AppMode = AppMode.NORMAL,
    val latestReading: YacsiReading? = null,
    val rawLogs: List<String> = emptyList(),
    val warningMessage: String? = null,
    val infoMessage: String? = null,
    val shotCalibration: ShotCalibrationState = ShotCalibrationState(),
    val distanceOffsetMm: Int? = null, // null until the device has reported it
    val survey: SurveyState = SurveyState(),
    val voiceState: VoiceState = VoiceState()
)

class YacsiViewModel(application: Application) : AndroidViewModel(application) {
    private val bluetoothService = BluetoothService()
    private val surveyStorage = SurveyStorage(application)
    private val voiceCommandManager = VoiceCommandManager(application)
    private var surveyUri: Uri? = null
    private var lrudSaveJob: Job? = null
    // Serialises survey file writes so they land in order.
    private val surveyDispatcher = Dispatchers.IO.limitedParallelism(1)

    private val _uiState = MutableStateFlow(
        YacsiUiState(survey = SurveyState(lrudBeforeLeg = surveyStorage.lrudBeforeLeg))
    )
    val uiState: StateFlow<YacsiUiState> = _uiState.asStateFlow()

    // Short-lived status messages shown as toasts; buffered until the UI collects them
    private val _toasts = Channel<String>(Channel.BUFFERED)
    val toasts: Flow<String> = _toasts.receiveAsFlow()

    // LRUD field the UI should focus next, after a value is entered by voice; dropped
    // when the survey tab isn't showing, so it doesn't pop up the keyboard later
    private val _lrudFocusRequests = MutableSharedFlow<LrudSide>(
        extraBufferCapacity = 1, onBufferOverflow = BufferOverflow.DROP_OLDEST
    )
    val lrudFocusRequests: Flow<LrudSide> = _lrudFocusRequests.asSharedFlow()

    init {
        surveyStorage.lastSurveyUri()?.let { openSurvey(it) }

        voiceCommandManager.onCommandRecognized = { action ->
            viewModelScope.launch {
                when (action) {
                    VoiceAction.SetTargetLeg -> {
                        setShotTarget(ShotTarget.LEG)
                        toast("Voice: shot set to Leg")
                    }
                    VoiceAction.SetTargetSection -> {
                        setShotTarget(ShotTarget.LEFT)
                        toast("Voice: shot set to L (section)")
                    }
                    is VoiceAction.SetLrud -> setLrudFromVoice(action.side, action.millimetres)
                    is VoiceAction.MissingNumber -> toast("Voice: heard ${action.side.label} but no number")
                }
            }
        }

        viewModelScope.launch {
            voiceCommandManager.state.collect { vState ->
                _uiState.update { it.copy(voiceState = vState) }
            }
        }

        viewModelScope.launch {
            // Skip the initial "Voice inactive" so nothing pops up at startup
            voiceCommandManager.state.map { it.statusMessage }.distinctUntilChanged().drop(1)
                .collect { toast(it) }
        }

        viewModelScope.launch {
            launch {
                var previous = bluetoothService.connectionState.value
                bluetoothService.connectionState.collect { state ->
                    _uiState.update { it.copy(connectionState = state) }
                    val name = _uiState.value.connectedDeviceName ?: "Yacksi"
                    // Only announce real transitions; auto-reconnect cycles through CONNECTING quietly
                    if (state == ConnectionState.CONNECTED && previous != ConnectionState.CONNECTED) {
                        toast("Connected to $name")
                    } else if (state == ConnectionState.DISCONNECTED && previous == ConnectionState.CONNECTED) {
                        toast(if (bluetoothService.isAutoReconnectEnabled.value) "Lost $name, reconnecting…" else "Disconnected")
                    }
                    previous = state
                    if (state == ConnectionState.DISCONNECTED && !_uiState.value.isAutoReconnectEnabled) {
                        _uiState.update { it.copy(appMode = AppMode.NORMAL) }
                    }
                }
            }
            launch {
                bluetoothService.connectedDeviceName.collect { name ->
                    _uiState.update { it.copy(connectedDeviceName = name) }
                }
            }
            launch {
                bluetoothService.isAutoReconnectEnabled.collect { enabled ->
                    _uiState.update { it.copy(isAutoReconnectEnabled = enabled) }
                }
            }
            launch {
                bluetoothService.receivedLines.collect { line ->
                    handleReceivedLine(line)
                }
            }
        }
    }

    @android.annotation.SuppressLint("MissingPermission")
    fun autoConnectDefaultDevice() {
        if (_uiState.value.connectionState == ConnectionState.DISCONNECTED && !_uiState.value.isAutoReconnectEnabled) {
            val defaultDevice = bluetoothService.getBondedDevices().firstOrNull {
                it.name?.equals("Yacksi", ignoreCase = true) == true
            }
            if (defaultDevice != null) {
                connect(defaultDevice)
            }
        }
    }

    fun getBondedDevices(): Set<BluetoothDevice> {
        return bluetoothService.getBondedDevices()
    }

    fun connect(device: BluetoothDevice) {
        bluetoothService.connect(device, autoReconnect = true)
    }

    fun disconnect() {
        bluetoothService.disconnect(stopAutoReconnect = true)
    }

    fun takeReading() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) return

        if (state.appMode != AppMode.NORMAL) {
            _uiState.update { it.copy(warningMessage = "Cannot take reading while in calibration mode.") }
            return
        }

        sendCommand("R", "Take Reading")
    }

    fun startAccelCalibration() {
        if (!sendCommand("A", "Rough Accelerometer Calibration")) return
        _uiState.update { it.copy(appMode = AppMode.ACCEL_CALIBRATION) }
    }

    fun startMagCalibration() {
        if (!sendCommand("M", "Rough Magnetometer Calibration")) return
        _uiState.update { it.copy(appMode = AppMode.MAG_CALIBRATION) }
    }

    fun eraseMagnetometerCalibration() {
        sendCommand("X", "Erase Magnetometer Calibration")
    }

    fun sendCalibrationContinue() {
        // Sends \r for accelerometer calibration step
        bluetoothService.sendCommand("\r")
        appendLog(">>> Sent Enter (\\r)")
    }

    // --- Shot (Heeb) calibration ---
    // The device drives the sequence and reports progress with CAL,... lines;
    // the app mirrors that state and sends single-key commands.

    fun startShotCalibration() {
        if (!sendCommand("C", "Shot Calibration")) return
        _uiState.update {
            it.copy(appMode = AppMode.SHOT_CALIBRATION, shotCalibration = ShotCalibrationState())
        }
    }

    fun takeCalibrationShot() {
        if (_uiState.value.shotCalibration.phase != ShotPhase.AIMING) return
        if (!sendCommand("T", "Take Shot")) return
        updateShot { it.copy(phase = ShotPhase.SAMPLING, notice = null) }
    }

    fun undoCalibrationShot() {
        sendCommand("U", "Undo Shot")
    }

    fun finishShotCalibrationEarly() {
        sendCommand("F", "Finish and Fit")
    }

    fun abortShotCalibration() {
        sendCommand("X", "Abort Shot Calibration")
    }

    fun acceptShotCalibration() {
        sendCommand("Y", "Save Shot Calibration")
    }

    fun discardShotCalibration() {
        sendCommand("N", "Discard Shot Calibration")
    }

    fun refitStoredShots() {
        sendCommand("F", "Refit Stored Shots")
    }

    fun dumpShotCalibration() {
        sendCommand("D", "Dump Shot Calibration")
    }

    fun eraseShotCalibration() {
        sendCommand("E", "Erase Shot Calibration")
    }

    // --- Distance offset ---
    // A fixed correction the device adds to every laser distance; it is stored
    // on the device and reported back on DIST_OFFSET_MM=... lines.

    fun requestDistanceOffset() {
        sendCommand("O", "Show Distance Offset")
    }

    fun setDistanceOffset(offsetMm: Int) {
        sendCommand("O$offsetMm", "Set Distance Offset")
    }

    // --- Survey recording ---
    // Readings arrive whether the device button or the app triggered them; each
    // is recorded as the current shot target (a leg or one of L/R/U/D).

    fun createSurvey(uri: Uri) {
        viewModelScope.launch {
            try {
                val name = withContext(surveyDispatcher) {
                    surveyStorage.save(uri, SurveyData())
                    surveyStorage.remember(uri)
                    surveyStorage.displayName(uri)
                }
                surveyUri = uri
                updateSurvey {
                    SurveyState(
                        fileName = name,
                        lrudBeforeLeg = it.lrudBeforeLeg,
                        target = initialTarget(it.lrudBeforeLeg),
                        notice = "New survey started. Enter the At station."
                    )
                }
            } catch (e: Exception) {
                surveyNotice("Could not create survey file: ${e.message}", isError = true)
            }
        }
    }

    fun openSurvey(uri: Uri) {
        viewModelScope.launch {
            try {
                val (name, data) = withContext(surveyDispatcher) {
                    surveyStorage.remember(uri)
                    surveyStorage.displayName(uri) to surveyStorage.load(uri)
                }
                surveyUri = uri
                val legs = data.legs
                val stationLrud = data.stationLrud.associateBy { it.station }
                // Continue from the end of the last leg.
                val at = legs.lastOrNull()?.to ?: ""
                updateSurvey {
                    SurveyState(
                        fileName = name,
                        legs = legs,
                        atStation = at,
                        toStation = nextStation(at, legs.stations()) ?: "",
                        lrud = stationLrud[at]?.lrud.toLrudText(),
                        stationLrud = stationLrud,
                        lrudBeforeLeg = it.lrudBeforeLeg,
                        target = initialTarget(it.lrudBeforeLeg),
                        notice = "${legs.size} legs loaded."
                    )
                }
            } catch (e: Exception) {
                surveyNotice("Could not open survey file: ${e.message}", isError = true)
            }
        }
    }

    fun setAtStation(text: String) {
        val at = sanitizeStation(text)
        updateSurvey {
            val known = it.legs.stations()
            val stored = it.stationLrud[at]
            val moved = if (stored == null && it.atStation !in known) {
                // A new or mistyped name: the LRUD entered so far moves with it
                it.copy(atStation = at, stationLrud = it.stationLrud - it.atStation).withLrud(it.lrud)
            } else {
                // A real station keeps its LRUD; show what is stored for this one
                it.copy(atStation = at, lrud = stored?.lrud.toLrudText())
            }
            moved.copy(toStation = nextStation(at, known) ?: it.toStation)
        }
        scheduleLrudSave()
    }

    fun setToStation(text: String) {
        updateSurvey { it.copy(toStation = sanitizeStation(text)) }
    }

    fun setLrud(side: LrudSide, text: String) {
        val value = text.filter { it.isDigit() || it == '.' }
        updateSurvey { it.withLrud(it.lrud + (side to value)) }
        scheduleLrudSave()
    }

    fun setShotTarget(target: ShotTarget) {
        updateSurvey { it.copy(target = target) }
    }

    fun setLrudBeforeLeg(enabled: Boolean) {
        surveyStorage.lrudBeforeLeg = enabled
        updateSurvey { it.copy(lrudBeforeLeg = enabled) }
    }

    /** Removes the last leg and puts its stations and LRUD back for re-shooting. */
    fun undoLastLeg() {
        val survey = _uiState.value.survey
        val leg = survey.legs.lastOrNull() ?: return
        val legs = survey.legs.dropLast(1)
        updateSurvey {
            it.copy(
                legs = legs,
                atStation = leg.from,
                toStation = leg.to,
                target = ShotTarget.LEG,
                notice = "Removed leg ${leg.from} → ${leg.to}.",
                noticeIsError = false
            ).withLrud(leg.lrud.toLrudText()) // back to a station row until re-shot
        }
        saveSurvey()
    }

    private fun recordSurveyShot(reading: YacsiReading) {
        val survey = _uiState.value.survey
        if (!survey.isOpen) return
        if (reading.distMm <= 0f) {
            surveyNotice("Laser shot failed; nothing recorded.", isError = true)
            return
        }
        val distanceM = reading.distMm / 1000.0

        val side = survey.target.side
        if (side != null) {
            updateSurvey {
                it.withLrud(it.lrud + (side to formatMetres(distanceM))).copy(
                    target = nextTarget(it.target),
                    notice = "${side.label} ${formatMetres(distanceM)} m at ${it.atStation.ifBlank { "the At station" }}.",
                    noticeIsError = false
                )
            }
            saveSurvey()
            return
        }

        val bearing = reading.bearing
        if (bearing == null) {
            surveyNotice("No bearing (check the calibration); leg not recorded.", isError = true)
            return
        }
        if (survey.atStation.isBlank() || survey.toStation.isBlank()) {
            surveyNotice("Enter the At and To stations; leg not recorded.", isError = true)
            return
        }

        val leg = SurveyLeg(
            from = survey.atStation,
            to = survey.toStation,
            distanceM = distanceM,
            bearing = bearing.toDouble(),
            inclination = reading.incl.toDouble(),
            lrud = survey.lrud.parseLrud(),
            time = now()
        )
        val legs = survey.legs + leg
        updateSurvey {
            // The leg now carries the LRUD for its From station
            val stationLrud = it.stationLrud - leg.from
            it.copy(
                legs = legs,
                atStation = leg.to,
                toStation = nextStation(leg.to, legs.stations()) ?: "",
                lrud = stationLrud[leg.to]?.lrud.toLrudText(),
                stationLrud = stationLrud,
                target = initialTarget(it.lrudBeforeLeg),
                notice = "Recorded ${leg.from} → ${leg.to}.",
                noticeIsError = false
            )
        }
        saveSurvey()
    }

    /** Writes the survey as it is now; saves run in order on [surveyDispatcher]. */
    private fun saveSurvey() {
        val uri = surveyUri ?: return
        lrudSaveJob?.cancel()
        val data = _uiState.value.survey.toData()
        viewModelScope.launch {
            try {
                withContext(surveyDispatcher) { surveyStorage.save(uri, data) }
            } catch (e: Exception) {
                surveyNotice("Could not write survey file: ${e.message}", isError = true)
            }
        }
    }

    /** Saves typed LRUD and station changes once typing pauses, not on every key. */
    private fun scheduleLrudSave() {
        lrudSaveJob?.cancel()
        lrudSaveJob = viewModelScope.launch {
            delay(1000)
            saveSurvey()
        }
    }

    /** Sets the LRUD boxes and keeps the stored LRUD for the At station in step with them. */
    private fun SurveyState.withLrud(lrud: Map<LrudSide, String>): SurveyState {
        val values = lrud.parseLrud()
        val stations = when {
            atStation.isBlank() -> stationLrud
            values.isEmpty() -> stationLrud - atStation
            else -> stationLrud + (atStation to StationLrud(atStation, values, now()))
        }
        return copy(lrud = lrud, stationLrud = stations)
    }

    private fun Map<LrudSide, String>.parseLrud(): Map<LrudSide, Double> =
        mapNotNull { (side, text) -> text.toDoubleOrNull()?.let { side to it } }.toMap()

    private fun Map<LrudSide, Double>?.toLrudText(): Map<LrudSide, String> =
        this?.mapValues { (_, metres) -> formatMetres(metres) } ?: emptyMap()

    private fun now() = LocalDateTime.now().truncatedTo(ChronoUnit.SECONDS).toString()

    private fun initialTarget(lrudBeforeLeg: Boolean) =
        if (lrudBeforeLeg) ShotTarget.LEFT else ShotTarget.LEG

    /** L -> R -> U -> D -> leg, the order LRUD shots are taken in. */
    private fun nextTarget(target: ShotTarget) =
        if (target == ShotTarget.DOWN) ShotTarget.LEG else ShotTarget.entries[target.ordinal + 1]

    /** Voice-entered LRUD; the value is spoken in millimetres. */
    private fun setLrudFromVoice(side: LrudSide, millimetres: Int) {
        if (!_uiState.value.survey.isOpen) {
            toast("Voice: ${side.label} ignored, no survey open")
            return
        }
        updateSurvey {
            it.withLrud(it.lrud + (side to formatMetres(millimetres / 1000.0))).copy(
                // Keep shot target in step, as if this side had been shot with the laser
                target = if (it.target.side == side) nextTarget(it.target) else it.target
            )
        }
        saveSurvey()
        toast("Voice: ${side.label} $millimetres mm")
        // D wraps round to L
        _lrudFocusRequests.tryEmit(LrudSide.entries[(side.ordinal + 1) % LrudSide.entries.size])
    }

    private fun formatMetres(metres: Double) = "%.2f".format(Locale.US, metres)

    private fun surveyNotice(message: String, isError: Boolean) {
        updateSurvey { it.copy(notice = message, noticeIsError = isError) }
    }

    private fun updateSurvey(transform: (SurveyState) -> SurveyState) {
        _uiState.update { it.copy(survey = transform(it.survey)) }
    }

    fun sendHelp() {
        bluetoothService.sendCommand("?")
    }

    fun clearWarning() {
        _uiState.update { it.copy(warningMessage = null) }
    }

    fun clearInfo() {
        _uiState.update { it.copy(infoMessage = null) }
    }

    /** Loads the voice model on first use, then toggles listening on and off. */
    fun toggleVoiceControl() {
        val voice = _uiState.value.voiceState
        when {
            voice.isListening -> voiceCommandManager.stopListening()
            voice.isInitialized -> voiceCommandManager.startListening()
            else -> voiceCommandManager.initialize()
        }
    }

    private fun toast(message: String) {
        _toasts.trySend(message)
    }

    private fun sendCommand(command: String, description: String): Boolean {
        if (_uiState.value.connectionState != ConnectionState.CONNECTED) return false
        bluetoothService.sendCommand(command)
        appendLog(">>> Sent $description ($command)")
        return true
    }

    private fun updateShot(transform: (ShotCalibrationState) -> ShotCalibrationState) {
        _uiState.update { it.copy(shotCalibration = transform(it.shotCalibration)) }
    }

    private fun handleReceivedLine(line: String) {
        appendLog(line)

        val cal = parseCalLine(line)
        if (cal != null) {
            handleCalMessage(cal)
            return
        }

        val offset = parseDistanceOffsetLine(line)
        if (offset != null) {
            handleDistanceOffsetMessage(offset)
            return
        }

        // Check if line is a sensor reading
        val reading = parseYacsiLine(line)
        if (reading != null) {
            _uiState.update { it.copy(latestReading = reading) }
            recordSurveyShot(reading)
        }

        // The rough accelerometer/magnetometer calibrations only report in free text.
        val mode = _uiState.value.appMode
        if (mode == AppMode.ACCEL_CALIBRATION || mode == AppMode.MAG_CALIBRATION) {
            val upper = line.uppercase()
            if (upper.contains("CALIBRATION COMPLETE") || upper.contains("OFFSET") ||
                upper.contains("SAVED") || upper.contains("FAILED") || upper.contains("STOPPED")
            ) {
                _uiState.update { it.copy(appMode = AppMode.NORMAL) }
            }
        }
    }

    private fun handleCalMessage(cal: CalMessage) {
        when (cal.state) {
            "START" -> _uiState.update {
                it.copy(
                    appMode = AppMode.SHOT_CALIBRATION,
                    shotCalibration = ShotCalibrationState(
                        groups = cal.int("GROUPS") ?: 14,
                        rolls = cal.int("ROLLS") ?: 4,
                        minGroups = cal.int("MINGROUPS") ?: 8
                    )
                )
            }
            "INFO" -> updateShot { it.copy(notice = cal.message, noticeIsError = false) }
            "AIM" -> {
                ensureShotMode()
                updateShot {
                    it.copy(
                        phase = ShotPhase.AIMING,
                        shot = cal.int("SHOT") ?: it.shot,
                        totalShots = cal.int("TOTAL") ?: it.totalShots,
                        group = cal.int("GROUP") ?: it.group,
                        groups = cal.int("GROUPS") ?: it.groups,
                        roll = cal.int("ROLL") ?: it.roll,
                        rolls = cal.int("ROLLS") ?: it.rolls,
                        instruction = cal.message ?: ""
                    )
                }
            }
            "SAMPLING" -> updateShot { it.copy(phase = ShotPhase.SAMPLING, notice = null) }
            "SHOT" -> updateShot {
                it.copy(notice = "Shot ${cal.int("SHOT") ?: ""} recorded.", noticeIsError = false)
            }
            "REJECT" -> updateShot { it.copy(notice = cal.message, noticeIsError = true) }
            "FITTING" -> {
                ensureShotMode()
                updateShot { it.copy(phase = ShotPhase.FITTING, notice = null, result = null) }
            }
            "RESULT" -> updateShot {
                it.copy(
                    result = ShotCalibrationResult(
                        rmsError = cal.float("RMS") ?: Float.NaN,
                        maxError = cal.float("MAX") ?: Float.NaN,
                        maxShot = cal.int("MAXSHOT") ?: 0,
                        dip = cal.float("DIP") ?: Float.NaN,
                        quality = cal.fields["QUALITY"] ?: "",
                        shots = cal.int("SHOTS") ?: 0
                    )
                )
            }
            "GROUPERR" -> updateShot { state ->
                val result = state.result ?: return@updateShot state
                val error = cal.float("ERR") ?: return@updateShot state
                state.copy(result = result.copy(groupErrors = result.groupErrors + error))
            }
            "REVIEW" -> {
                ensureShotMode()
                updateShot { it.copy(phase = ShotPhase.REVIEW) }
            }
            "SAVED" -> endShotCalibration(
                info = "Shot calibration saved and applied" +
                    (cal.float("RMS")?.let { " (RMS error %.2f°).".format(it) } ?: ".")
            )
            "DISCARDED", "ABORTED" -> endShotCalibration(info = cal.message)
            "FAILED" -> endShotCalibration(warning = cal.message ?: "Shot calibration failed.")
        }
    }

    private fun handleDistanceOffsetMessage(message: DistanceOffsetMessage) {
        val offsetText = "%+d mm".format(message.offsetMm)
        _uiState.update {
            it.copy(
                distanceOffsetMm = message.offsetMm,
                infoMessage = if (message.status == "SAVED") {
                    "Distance offset set to $offsetText."
                } else {
                    it.infoMessage
                },
                warningMessage = when (message.status) {
                    "INVALID" -> "The device rejected the distance offset; it is still $offsetText."
                    "FAILED" -> "The device could not save the distance offset; it is still $offsetText."
                    else -> it.warningMessage
                }
            )
        }
    }

    private fun ensureShotMode() {
        if (_uiState.value.appMode != AppMode.SHOT_CALIBRATION) {
            _uiState.update { it.copy(appMode = AppMode.SHOT_CALIBRATION) }
        }
    }

    private fun endShotCalibration(info: String? = null, warning: String? = null) {
        _uiState.update {
            it.copy(
                appMode = if (it.appMode == AppMode.SHOT_CALIBRATION) AppMode.NORMAL else it.appMode,
                infoMessage = info ?: it.infoMessage,
                warningMessage = warning ?: it.warningMessage
            )
        }
    }

    private fun appendLog(line: String) {
        _uiState.update { state ->
            val newLogs = state.rawLogs + line
            // Keep last 500 lines to avoid memory overflow
            val trimmedLogs = if (newLogs.size > 500) newLogs.takeLast(500) else newLogs
            state.copy(rawLogs = trimmedLogs)
        }
    }

    override fun onCleared() {
        voiceCommandManager.destroy()
        super.onCleared()
        bluetoothService.disconnect()
    }
}
