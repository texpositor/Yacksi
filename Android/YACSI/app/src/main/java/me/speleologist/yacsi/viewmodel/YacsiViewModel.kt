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
    val isAutoReconnectEnabled: Boolean = false,
    val appMode: AppMode = AppMode.NORMAL,
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

        bluetoothService.sendCommand("R")
        appendLog(">>> Sent Take Reading (R)")
    }

    fun startAccelCalibration() {
        val state = _uiState.value
        if (state.connectionState != ConnectionState.CONNECTED) return
        bluetoothService.sendCommand("A")
        _uiState.update {
            it.copy(
                appMode = AppMode.ACCEL_CALIBRATION
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
                appMode = AppMode.MAG_CALIBRATION
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
        if (upper.contains("CALIBRATION COMPLETE") || upper.contains("OFFSET") || upper.contains("SAVED")) {
            if (_uiState.value.appMode != AppMode.NORMAL) {
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
