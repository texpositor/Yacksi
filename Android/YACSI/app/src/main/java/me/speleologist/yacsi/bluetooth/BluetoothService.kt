package me.speleologist.yacsi.bluetooth

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.UUID

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED
}

class BluetoothService {
    private val TAG = "BluetoothService"
    private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private val bluetoothAdapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _connectedDeviceName = MutableStateFlow<String?>(null)
    val connectedDeviceName: StateFlow<String?> = _connectedDeviceName.asStateFlow()

    private val _isAutoReconnectEnabled = MutableStateFlow(false)
    val isAutoReconnectEnabled: StateFlow<Boolean> = _isAutoReconnectEnabled.asStateFlow()

    private val _receivedLines = MutableSharedFlow<String>(replay = 50, extraBufferCapacity = 200)
    val receivedLines: SharedFlow<String> = _receivedLines.asSharedFlow()

    private var socket: BluetoothSocket? = null
    private var readJob: Job? = null
    private var reconnectJob: Job? = null
    private var targetDevice: BluetoothDevice? = null
    private val serviceScope = CoroutineScope(Dispatchers.IO + Job())

    fun getBondedDevices(): Set<BluetoothDevice> {
        return try {
            bluetoothAdapter?.bondedDevices ?: emptySet()
        } catch (e: SecurityException) {
            Log.e(TAG, "SecurityException getting bonded devices", e)
            emptySet()
        }
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice, autoReconnect: Boolean = true) {
        targetDevice = device
        _isAutoReconnectEnabled.value = autoReconnect
        _connectedDeviceName.value = device.name ?: device.address

        reconnectJob?.cancel()
        reconnectJob = serviceScope.launch {
            connectInternal(device)
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun connectInternal(device: BluetoothDevice) {
        closeSocketOnly()

        _connectionState.value = ConnectionState.CONNECTING
        _connectedDeviceName.value = device.name ?: device.address

        try {
            val tmpSocket = device.createRfcommSocketToServiceRecord(SPP_UUID)
            socket = tmpSocket
            // Cancel discovery if running to speed up connection
            bluetoothAdapter?.cancelDiscovery()

            tmpSocket.connect()

            withContext(Dispatchers.Main) {
                _connectionState.value = ConnectionState.CONNECTED
            }

            startReading(tmpSocket)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to connect to device ${device.address}: ${e.message}")
            closeSocketOnly()
            _connectionState.value = ConnectionState.DISCONNECTED

            if (_isAutoReconnectEnabled.value && targetDevice != null) {
                scheduleAutoReconnect()
            }
        }
    }

    private fun scheduleAutoReconnect() {
        reconnectJob?.cancel()
        reconnectJob = serviceScope.launch {
            delay(2500)
            val dev = targetDevice
            if (dev != null && _isAutoReconnectEnabled.value && _connectionState.value == ConnectionState.DISCONNECTED) {
                Log.d(TAG, "Attempting auto-reconnect to ${dev.address}...")
                connectInternal(dev)
            }
        }
    }

    private fun startReading(activeSocket: BluetoothSocket) {
        readJob?.cancel()
        readJob = serviceScope.launch {
            try {
                val inputStream = activeSocket.inputStream
                val reader = BufferedReader(InputStreamReader(inputStream, Charsets.UTF_8))
                val buffer = CharArray(256)
                val lineBuilder = StringBuilder()

                while (isActive) {
                    val readCount = reader.read(buffer)
                    if (readCount == -1) break
                    for (i in 0 until readCount) {
                        val c = buffer[i]
                        if (c == '\r' || c == '\n') {
                            if (lineBuilder.isNotEmpty()) {
                                val line = lineBuilder.toString()
                                lineBuilder.setLength(0)
                                _receivedLines.emit(line)
                            }
                        } else {
                            lineBuilder.append(c)
                        }
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error reading from Bluetooth socket", e)
            } finally {
                onConnectionLost()
            }
        }
    }

    fun sendCommand(command: String) {
        if (_connectionState.value != ConnectionState.CONNECTED) {
            Log.w(TAG, "Cannot send command, not connected")
            return
        }
        serviceScope.launch(Dispatchers.IO) {
            try {
                val outputStream = socket?.outputStream
                if (outputStream != null) {
                    // Protocol: All messages end with \r or \n (app should send \r)
                    val payload = if (command.endsWith("\r") || command.endsWith("\n")) command else "$command\r"
                    outputStream.write(payload.toByteArray(Charsets.UTF_8))
                    outputStream.flush()
                    Log.d(TAG, "Sent command: ${command.trim()}")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to send command: $command", e)
                onConnectionLost()
            }
        }
    }

    private fun onConnectionLost() {
        closeSocketOnly()
        serviceScope.launch(Dispatchers.Main) {
            _connectionState.value = ConnectionState.DISCONNECTED
            if (_isAutoReconnectEnabled.value && targetDevice != null) {
                scheduleAutoReconnect()
            } else {
                _connectedDeviceName.value = null
            }
        }
    }

    private fun closeSocketOnly() {
        readJob?.cancel()
        readJob = null
        try {
            socket?.close()
        } catch (e: Exception) {
            Log.e(TAG, "Error closing socket", e)
        }
        socket = null
    }

    fun disconnect(stopAutoReconnect: Boolean = true) {
        if (stopAutoReconnect) {
            _isAutoReconnectEnabled.value = false
            reconnectJob?.cancel()
            reconnectJob = null
            targetDevice = null
        }
        closeSocketOnly()
        serviceScope.launch(Dispatchers.Main) {
            _connectionState.value = ConnectionState.DISCONNECTED
            _connectedDeviceName.value = null
        }
    }
}
