package me.speleologist.yacsi.viewmodel

import android.app.Application
import android.bluetooth.BluetoothDevice
import android.media.AudioManager
import android.media.ToneGenerator
import android.net.Uri
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
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
import me.speleologist.yacsi.protocol.BatteryMessage
import me.speleologist.yacsi.protocol.CalMessage
import me.speleologist.yacsi.protocol.DistanceOffsetMessage
import me.speleologist.yacsi.protocol.FirmwareInfo
import me.speleologist.yacsi.protocol.ScanSample
import me.speleologist.yacsi.protocol.ScanStateMessage
import me.speleologist.yacsi.protocol.parseAccelPositionLine
import me.speleologist.yacsi.protocol.parseBatteryLine
import me.speleologist.yacsi.protocol.parseCalLine
import me.speleologist.yacsi.protocol.parseDistanceOffsetLine
import me.speleologist.yacsi.protocol.parseFirmwareLine
import me.speleologist.yacsi.protocol.parseScanLine
import me.speleologist.yacsi.protocol.parseScanStateLine
import me.speleologist.yacsi.scan.RunCsv
import me.speleologist.yacsi.scan.RunStorage
import me.speleologist.yacsi.scan.RunWriter
import me.speleologist.yacsi.voice.VoiceAction
import me.speleologist.yacsi.voice.VoiceCommandManager
import me.speleologist.yacsi.voice.VoiceState
import java.time.LocalDateTime
import java.util.Locale

/** Bluetooth name of the SACSI instrument, connected to automatically when paired. */
const val DEFAULT_DEVICE_NAME = "Sacsi"

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

/** Progress through the rough accelerometer calibration's six positions. */
data class AccelCalibrationState(
    val position: Int = 1, // 1-based
    val positions: Int = 6,
    val orientation: String = "", // as the device words it, e.g. "X axis pointing up"
    val isSampling: Boolean = false
)

/** The current (or last) side-scan run, from Go to Stop. */
data class ScanState(
    val isRunning: Boolean = false, // between the device's STARTED and STOPPED
    val folderName: String? = null, // null until a folder for run files is chosen
    val fileName: String? = null, // file of the current or last run; null if not being saved
    val samples: List<ScanSample> = emptyList(), // oldest first, the most recent MAX_SHOWN_SAMPLES
    val sampleCount: Int = 0,
    val missedSamples: Int = 0, // gaps in the device's sample numbers, e.g. Bluetooth dropouts
    val notice: String? = null,
    val noticeIsError: Boolean = false
) {
    val latest: ScanSample? get() = samples.lastOrNull()
}

data class YacsiUiState(
    val connectionState: ConnectionState = ConnectionState.DISCONNECTED,
    val connectedDeviceName: String? = null,
    val isAutoReconnectEnabled: Boolean = false,
    val appMode: AppMode = AppMode.NORMAL,
    val rawLogs: List<String> = emptyList(),
    val warningMessage: String? = null,
    val infoMessage: String? = null,
    val shotCalibration: ShotCalibrationState = ShotCalibrationState(),
    val accelCalibration: AccelCalibrationState = AccelCalibrationState(),
    val distanceOffsetMm: Int? = null, // null until the device has reported it
    val isBatteryLow: Boolean = false, // as of the device's last battery report
    val firmware: FirmwareInfo? = null, // null until the device has reported it
    val scan: ScanState = ScanState(),
    val voiceState: VoiceState = VoiceState()
)

class YacsiViewModel(application: Application) : AndroidViewModel(application) {
    private val bluetoothService = BluetoothService()
    private val runStorage = RunStorage(application)
    private val voiceCommandManager = VoiceCommandManager(application)
    private var runFolder: Uri? = runStorage.folderUri()

    // Run file access happens only on this dispatcher, so writes land in order
    // and runWriter needs no locking.
    private val runDispatcher = Dispatchers.IO.limitedParallelism(1)
    private var runWriter: RunWriter? = null
    private var lastSampleNumber: Int? = null

    // Low battery alert; the alarm stream so it is heard even with media muted
    private var batteryToneGenerator: ToneGenerator? = try {
        ToneGenerator(AudioManager.STREAM_ALARM, ToneGenerator.MAX_VOLUME)
    } catch (e: RuntimeException) {
        Log.e("YacsiViewModel", "Failed to create tone generator", e)
        null
    }

    private val _uiState = MutableStateFlow(YacsiUiState())
    val uiState: StateFlow<YacsiUiState> = _uiState.asStateFlow()

    // Short-lived status messages shown as toasts; buffered until the UI collects them
    private val _toasts = Channel<String>(Channel.BUFFERED)
    val toasts: Flow<String> = _toasts.receiveAsFlow()

    init {
        runFolder?.let { folder ->
            viewModelScope.launch {
                val name = withContext(Dispatchers.IO) { runCatching { runStorage.folderName(folder) }.getOrNull() }
                updateScan { it.copy(folderName = name) }
            }
        }

        voiceCommandManager.onCommandRecognized = { action ->
            viewModelScope.launch {
                when (action) {
                    VoiceAction.Go -> {
                        toast("Voice: go")
                        go()
                    }
                    VoiceAction.Stop -> {
                        toast("Voice: stop")
                        stop()
                    }
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
                    val name = _uiState.value.connectedDeviceName ?: DEFAULT_DEVICE_NAME
                    // Only announce real transitions; auto-reconnect cycles through CONNECTING quietly
                    if (state == ConnectionState.CONNECTED && previous != ConnectionState.CONNECTED) {
                        toast("Connected to $name")
                        // Ask which firmware is running so each run file can record it
                        bluetoothService.sendCommand("V")
                    } else if (state == ConnectionState.DISCONNECTED && previous == ConnectionState.CONNECTED) {
                        toast(if (bluetoothService.isAutoReconnectEnabled.value) "Lost $name, reconnecting…" else "Disconnected")
                    }
                    previous = state
                    if (state == ConnectionState.DISCONNECTED && !bluetoothService.isAutoReconnectEnabled.value) {
                        _uiState.update { it.copy(appMode = AppMode.NORMAL) }
                        // Deliberately disconnected: no more samples will come for this run.
                        // (While reconnecting the run stays open; sample numbers show the gap.)
                        if (_uiState.value.scan.isRunning) endRun("Disconnected; run file closed.")
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
                it.name?.equals(DEFAULT_DEVICE_NAME, ignoreCase = true) == true
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

    // --- Side scan ---
    // Go and Stop ask the device to start or stop; the run itself follows the
    // device's SCAN,STATE=STARTED / STOPPED lines, so runs started from the
    // device's button are recorded the same way.

    val hasRunFolder: Boolean get() = runFolder != null

    /** Remembers the folder for run files; [thenGo] starts a scan once it is set. */
    fun setRunFolder(treeUri: Uri, thenGo: Boolean = false) {
        viewModelScope.launch {
            try {
                val name = withContext(Dispatchers.IO) {
                    runStorage.rememberFolder(treeUri)
                    runStorage.folderName(treeUri)
                }
                runFolder = treeUri
                updateScan { it.copy(folderName = name, notice = "Runs will be saved in $name.", noticeIsError = false) }
                if (thenGo) go()
            } catch (e: Exception) {
                scanNotice("Could not use that folder: ${e.message}", isError = true)
            }
        }
    }

    fun go() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) {
            scanNotice("Not connected to the instrument.", isError = true)
            return
        }
        if (state.appMode != AppMode.NORMAL) {
            _uiState.update { it.copy(warningMessage = "Finish the calibration before starting a scan.") }
            return
        }
        if (state.scan.isRunning) return
        sendCommand("G", "Go")
    }

    fun stop() {
        if (sendCommand("S", "Stop")) return
        // Not connected, so the device can't be told; stop recording here at least.
        if (_uiState.value.scan.isRunning) endRun("Stopped while disconnected; the instrument may still be scanning.")
    }

    private fun handleScanState(message: ScanStateMessage) {
        when {
            message.isStarted -> startRun()
            message.isStopped -> {
                val problems = listOfNotNull(
                    message.tofErrors?.takeIf { it > 0 }?.let { "$it side-sensor errors" },
                    message.laserRestarts?.takeIf { it > 0 }?.let { "$it laser restarts" }
                )
                endRun(if (problems.isEmpty()) null else "Instrument reported ${problems.joinToString(", ")}.")
            }
        }
    }

    private fun startRun() {
        if (_uiState.value.scan.isRunning) endRun(null) // a STOPPED was missed, e.g. in a dropout
        val start = LocalDateTime.now()
        val fileName = RunCsv.fileName(start)
        val folder = runFolder
        lastSampleNumber = null
        updateScan {
            it.copy(
                isRunning = true,
                fileName = if (folder != null) fileName else null,
                samples = emptyList(),
                sampleCount = 0,
                missedSamples = 0,
                notice = if (folder == null) "No folder chosen for run files; this run is not being saved." else null,
                noticeIsError = folder == null
            )
        }
        if (folder == null) return
        val header = RunCsv.header(start, _uiState.value.firmware)
        viewModelScope.launch(runDispatcher) {
            try {
                runWriter = runStorage.createRun(folder, fileName).also { it.writeHeader(header) }
            } catch (e: Exception) {
                runWriter = null
                updateScan { it.copy(fileName = null) }
                scanNotice("Could not create the run file: ${e.message}", isError = true)
            }
        }
    }

    private fun endRun(extraNotice: String?) {
        val scan = _uiState.value.scan
        updateScan { it.copy(isRunning = false) }
        viewModelScope.launch(runDispatcher) {
            val writer = runWriter ?: run {
                extraNotice?.let { scanNotice(it, isError = true) }
                return@launch
            }
            runWriter = null
            val saved = try {
                writer.close()
                "Saved ${writer.rows} samples to ${writer.fileName}."
            } catch (e: Exception) {
                "Could not finish ${writer.fileName}: ${e.message}"
            }
            val missed = if (scan.missedSamples > 0) " ${scan.missedSamples} samples missed." else ""
            scanNotice(listOfNotNull(saved + missed, extraNotice).joinToString(" "), isError = extraNotice != null)
        }
    }

    private fun recordSample(sample: ScanSample) {
        // Samples with no run open: the app connected while the instrument was
        // already scanning, so start a run file from here.
        if (!_uiState.value.scan.isRunning) startRun()
        val previous = lastSampleNumber
        val missed = if (previous != null && sample.n > previous + 1) sample.n - previous - 1 else 0
        lastSampleNumber = sample.n
        updateScan {
            it.copy(
                samples = (it.samples + sample).takeLast(MAX_SHOWN_SAMPLES),
                sampleCount = it.sampleCount + 1,
                missedSamples = it.missedSamples + missed
            )
        }
        val row = RunCsv.row(sample, LocalDateTime.now())
        viewModelScope.launch(runDispatcher) {
            val writer = runWriter ?: return@launch
            try {
                writer.writeRow(row)
            } catch (e: Exception) {
                runWriter = null
                runCatching { writer.close() }
                updateScan { it.copy(fileName = null) }
                scanNotice("Writing ${writer.fileName} failed (${e.message}); the rest of this run is not saved.", isError = true)
            }
        }
    }

    private fun scanNotice(message: String, isError: Boolean) {
        updateScan { it.copy(notice = message, noticeIsError = isError) }
    }

    private fun updateScan(transform: (ScanState) -> ScanState) {
        _uiState.update { it.copy(scan = transform(it.scan)) }
    }

    // --- Rough calibrations ---

    fun startAccelCalibration() {
        if (!sendCommand("A", "Rough Accelerometer Calibration")) return
        _uiState.update {
            it.copy(appMode = AppMode.ACCEL_CALIBRATION, accelCalibration = AccelCalibrationState())
        }
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
        if (_uiState.value.accelCalibration.isSampling) return
        bluetoothService.sendCommand("\r")
        appendLog(">>> Sent Enter (\\r)")
    }

    /** Stops the rough accelerometer or magnetometer calibration without saving. */
    fun cancelRoughCalibration() {
        sendCommand("q", "Cancel Calibration")
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
        // Samples arrive four times a second; keep them out of the log, which
        // is for everything else the device says.
        val sample = parseScanLine(line)
        if (sample != null) {
            recordSample(sample)
            return
        }
        appendLog(line)

        val scanState = parseScanStateLine(line)
        if (scanState != null) {
            handleScanState(scanState)
            return
        }

        val firmware = parseFirmwareLine(line)
        if (firmware != null) {
            _uiState.update { it.copy(firmware = firmware) }
            return
        }

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

        val battery = parseBatteryLine(line)
        if (battery != null) {
            handleBatteryMessage(battery)
            return
        }

        val accelPosition = parseAccelPositionLine(line)
        if (accelPosition != null) {
            _uiState.update {
                it.copy(
                    appMode = AppMode.ACCEL_CALIBRATION,
                    accelCalibration = AccelCalibrationState(
                        position = accelPosition.position,
                        positions = accelPosition.total,
                        orientation = accelPosition.orientation
                    )
                )
            }
            return
        }

        // The rough accelerometer/magnetometer calibrations only report in free text.
        val mode = _uiState.value.appMode
        if (mode == AppMode.ACCEL_CALIBRATION || mode == AppMode.MAG_CALIBRATION) {
            val upper = line.uppercase()
            if (mode == AppMode.ACCEL_CALIBRATION && upper.startsWith("SAMPLING POSITION")) {
                _uiState.update { it.copy(accelCalibration = it.accelCalibration.copy(isSampling = true)) }
            } else if (upper.contains("CALIBRATION COMPLETE") || upper.contains("OFFSET") ||
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

    /** The device reports the battery at Go and every 30 s during a scan. */
    private fun handleBatteryMessage(message: BatteryMessage) {
        val wasLow = _uiState.value.isBatteryLow
        _uiState.update { it.copy(isBatteryLow = message.isLow) }
        // Alert when it first goes low, not on every report during a long run
        if (!message.isLow || wasLow) return
        batteryToneGenerator?.startTone(ToneGenerator.TONE_CDMA_ALERT_CALL_GUARD, 1000)
        val volts = "%.2f V".format(Locale.US, message.millivolts / 1000.0)
        _uiState.update {
            it.copy(warningMessage = "Battery low ($volts). Recharge the batteries soon.")
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
        batteryToneGenerator?.release()
        batteryToneGenerator = null
        // Close any open run file; everything written so far is already flushed.
        runWriter?.let { runCatching { it.close() } }
        super.onCleared()
        bluetoothService.disconnect()
    }

    private companion object {
        const val MAX_SHOWN_SAMPLES = 2000
    }
}
