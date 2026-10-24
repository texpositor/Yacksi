package me.speleologist.yacsi.viewmodel

import android.bluetooth.BluetoothDevice
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import me.speleologist.yacsi.bluetooth.BluetoothService
import me.speleologist.yacsi.bluetooth.ConnectionState
import me.speleologist.yacsi.protocol.CalMessage
import me.speleologist.yacsi.protocol.YacsiReading
import me.speleologist.yacsi.protocol.parseCalLine
import me.speleologist.yacsi.protocol.parseYacsiLine

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

data class YacsiUiState(
    val connectionState: ConnectionState = ConnectionState.DISCONNECTED,
    val connectedDeviceName: String? = null,
    val isAutoReconnectEnabled: Boolean = false,
    val appMode: AppMode = AppMode.NORMAL,
    val latestReading: YacsiReading? = null,
    val rawLogs: List<String> = emptyList(),
    val warningMessage: String? = null,
    val infoMessage: String? = null,
    val shotCalibration: ShotCalibrationState = ShotCalibrationState()
)

class YacsiViewModel : ViewModel() {
    private val bluetoothService = BluetoothService()

    private val _uiState = MutableStateFlow(YacsiUiState())
    val uiState: StateFlow<YacsiUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            launch {
                bluetoothService.connectionState.collect { state ->
                    _uiState.update { it.copy(connectionState = state) }
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

    fun sendHelp() {
        bluetoothService.sendCommand("?")
    }

    fun clearWarning() {
        _uiState.update { it.copy(warningMessage = null) }
    }

    fun clearInfo() {
        _uiState.update { it.copy(infoMessage = null) }
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

        // Check if line is a sensor reading
        val reading = parseYacsiLine(line)
        if (reading != null) {
            _uiState.update { it.copy(latestReading = reading) }
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
        super.onCleared()
        bluetoothService.disconnect()
    }
}
