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
import me.speleologist.yacsi.protocol.YacsiReading
import me.speleologist.yacsi.protocol.parseYacsiLine

enum class AppMode {
    NORMAL,
    ACCEL_CALIBRATION,
    MAG_CALIBRATION
}

data class YacsiUiState(
    val connectionState: ConnectionState = ConnectionState.DISCONNECTED,
    val connectedDeviceName: String? = null,
    val appMode: AppMode = AppMode.NORMAL,
    val isRunning: Boolean = false,
    val latestReading: YacsiReading? = null,
    val rawLogs: List<String> = emptyList(),
    val warningMessage: String? = null
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
                    if (state == ConnectionState.DISCONNECTED) {
                        _uiState.update { it.copy(isRunning = false, appMode = AppMode.NORMAL) }
                    }
                }
            }
            launch {
                bluetoothService.connectedDeviceName.collect { name ->
                    _uiState.update { it.copy(connectedDeviceName = name) }
                }
            }
            launch {
                bluetoothService.receivedLines.collect { line ->
                    handleReceivedLine(line)
                }
            }
        }
    }

    fun getBondedDevices(): Set<BluetoothDevice> {
        return bluetoothService.getBondedDevices()
    }

    fun connect(device: BluetoothDevice) {
        bluetoothService.connect(device)
    }

    fun disconnect() {
        bluetoothService.disconnect()
    }

    fun toggleRun() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) return

        if (state.appMode != AppMode.NORMAL) {
            _uiState.update { it.copy(warningMessage = "Cannot change Run/Stop state while in calibration mode.") }
            return
        }

        if (state.isRunning) {
            // Stop
            bluetoothService.sendCommand("S")
            _uiState.update { it.copy(isRunning = false) }
        } else {
            // Run
            bluetoothService.sendCommand("R")
            _uiState.update { it.copy(isRunning = true) }
        }
    }

    fun startAccelCalibration() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) return
        bluetoothService.sendCommand("A")
        _uiState.update {
            it.copy(
                appMode = AppMode.ACCEL_CALIBRATION,
                isRunning = false
            )
        }
        appendLog(">>> Sent Accel Calibration (A)")
    }

    fun startMagCalibration() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) return
        bluetoothService.sendCommand("M")
        _uiState.update {
            it.copy(
                appMode = AppMode.MAG_CALIBRATION,
                isRunning = false
            )
        }
        appendLog(">>> Sent Mag Calibration (M)")
    }

    fun eraseMagnetometerCalibration() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) return
        bluetoothService.sendCommand("X")
        appendLog(">>> Sent Erase Magnetometer Calibration (X)")
    }

    fun sendCalibrationContinue() {
        // Sends \r for accelerometer calibration step
        bluetoothService.sendCommand("\r")
        appendLog(">>> Sent Enter (\\r)")
    }

    fun sendMagCalibrationFinish() {
        // Sends S to finish mag calibration
        bluetoothService.sendCommand("S")
        _uiState.update { it.copy(appMode = AppMode.NORMAL) }
        appendLog(">>> Sent Finish Magnetometer Calibration (S)")
    }

    fun cancelCalibration() {
        bluetoothService.sendCommand("q")
        _uiState.update { it.copy(appMode = AppMode.NORMAL) }
        appendLog(">>> Cancelled calibration (q)")
    }

    fun sendHelp() {
        bluetoothService.sendCommand("?")
    }

    fun clearWarning() {
        _uiState.update { it.copy(warningMessage = null) }
    }

    private fun handleReceivedLine(line: String) {
        appendLog(line)

        // Check if line is a sensor reading
        val reading = parseYacsiLine(line)
        if (reading != null) {
            _uiState.update { it.copy(latestReading = reading) }
        }

        // Check calibration completion or state cues from device
        val upper = line.uppercase()
        if (upper.contains("CALIBRATION COMPLETE") || upper.contains("OFFSET") && _uiState.value.appMode == AppMode.MAG_CALIBRATION) {
            // Check if mag cal finished or accel cal finished
            if (_uiState.value.appMode == AppMode.ACCEL_CALIBRATION && upper.contains("SAVED")) {
                _uiState.update { it.copy(appMode = AppMode.NORMAL) }
            }
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
