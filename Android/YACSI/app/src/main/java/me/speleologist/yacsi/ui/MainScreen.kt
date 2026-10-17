package me.speleologist.yacsi.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.speleologist.yacsi.bluetooth.ConnectionState
import me.speleologist.yacsi.viewmodel.AppMode
import me.speleologist.yacsi.viewmodel.YacsiViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
    viewModel: YacsiViewModel,
    onRequestPermissions: () -> Unit
) {
    val uiState by viewModel.uiState.collectAsState()
    var showDeviceDialog by remember { mutableStateOf(false) }
    var showMenu by remember { mutableStateOf(false) }
    var showEraseConfirmDialog by remember { mutableStateOf(false) }

    val listState = rememberLazyListState()

    // Auto-scroll logs to bottom when new lines arrive
    LaunchedEffect(uiState.rawLogs.size) {
        if (uiState.rawLogs.isNotEmpty()) {
            listState.animateScrollToItem(uiState.rawLogs.size - 1)
        }
    }

    // Warning snackbar or dialog if warningMessage is present
    uiState.warningMessage?.let { msg ->
        AlertDialog(
            onDismissRequest = { viewModel.clearWarning() },
            title = { Text("Warning") },
            text = { Text(msg) },
            confirmButton = {
                TextButton(onClick = { viewModel.clearWarning() }) {
                    Text("OK")
                }
            }
        )
    }

    if (showEraseConfirmDialog) {
        AlertDialog(
            onDismissRequest = { showEraseConfirmDialog = false },
            title = { Text("Erase Magnetometer Calibration") },
            text = { Text("Are you sure you want to erase the stored magnetometer calibration on the device? This cannot be undone.") },
            confirmButton = {
                TextButton(
                    onClick = {
                        showEraseConfirmDialog = false
                        viewModel.eraseMagnetometerCalibration()
                    }
                ) {
                    Text("Erase", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showEraseConfirmDialog = false }) {
                    Text("Cancel")
                }
            }
        )
    }

    if (showDeviceDialog) {
        val bondedDevices = viewModel.getBondedDevices()
        PairedDevicesDialog(
            bondedDevices = bondedDevices,
            onDeviceSelected = { device ->
                showDeviceDialog = false
                viewModel.connect(device)
            },
            onDismiss = { showDeviceDialog = false }
        )
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("YACSI - Cave Survey")
                        Text(
                            text = when (uiState.connectionState) {
                                ConnectionState.DISCONNECTED -> "Disconnected"
                                ConnectionState.CONNECTING -> "Connecting…"
                                ConnectionState.CONNECTED -> "Connected to ${uiState.connectedDeviceName ?: "Yacksi"}"
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = when (uiState.connectionState) {
                                ConnectionState.CONNECTED -> MaterialTheme.colorScheme.primary
                                ConnectionState.CONNECTING -> MaterialTheme.colorScheme.secondary
                                ConnectionState.DISCONNECTED -> MaterialTheme.colorScheme.error
                            }
                        )
                    }
                },
                actions = {
                    // Connect / Disconnect button
                    val isConnected = uiState.connectionState == ConnectionState.CONNECTED
                    val isConnecting = uiState.connectionState == ConnectionState.CONNECTING

                    Button(
                        onClick = {
                            if (isConnected || isConnecting) {
                                viewModel.disconnect()
                            } else {
                                onRequestPermissions()
                                showDeviceDialog = true
                            }
                        },
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (isConnected) MaterialTheme.colorScheme.errorContainer
                            else MaterialTheme.colorScheme.primary
                        ),
                        modifier = Modifier.padding(end = 8.dp)
                    ) {
                        Text(
                            text = when (uiState.connectionState) {
                                ConnectionState.CONNECTED -> "Disconnect"
                                ConnectionState.CONNECTING -> "Cancel"
                                ConnectionState.DISCONNECTED -> "Connect"
                            },
                            color = if (isConnected) MaterialTheme.colorScheme.onErrorContainer
                            else MaterialTheme.colorScheme.onPrimary
                        )
                    }

                    // Overflow Menu
                    Box {
                        IconButton(
                            onClick = { showMenu = true },
                            enabled = isConnected && uiState.appMode == AppMode.NORMAL
                        ) {
                            Icon(Icons.Default.MoreVert, contentDescription = "Menu")
                        }
                        DropdownMenu(
                            expanded = showMenu,
                            onDismissRequest = { showMenu = false }
                        ) {
                            DropdownMenuItem(
                                text = { Text("Calibrate Accelerometer") },
                                onClick = {
                                    showMenu = false
                                    viewModel.startAccelCalibration()
                                }
                            )
                            DropdownMenuItem(
                                text = { Text("Calibrate Magnetometer") },
                                onClick = {
                                    showMenu = false
                                    viewModel.startMagCalibration()
                                }
                            )
                            DropdownMenuItem(
                                text = { Text("Erase Magnetometer Calibration") },
                                onClick = {
                                    showMenu = false
                                    showEraseConfirmDialog = true
                                }
                            )
                        }
                    }
                }
            )
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Mode Banner / Calibration Controls if in calibration mode
            if (uiState.appMode != AppMode.NORMAL) {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.tertiaryContainer
                    ),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text(
                            text = if (uiState.appMode == AppMode.ACCEL_CALIBRATION)
                                "Mode: Accelerometer Calibration"
                            else
                                "Mode: Magnetometer Calibration",
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(
                            text = if (uiState.appMode == AppMode.ACCEL_CALIBRATION)
                                "Place device in requested orientation and press Continue."
                            else
                                "Rotate instrument through all orientations continuously, then press Finish.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                        Spacer(modifier = Modifier.height(12.dp))
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            if (uiState.appMode == AppMode.ACCEL_CALIBRATION) {
                                Button(
                                    onClick = { viewModel.sendCalibrationContinue() },
                                    modifier = Modifier.weight(1f)
                                ) {
                                    Text("Continue to Next Position")
                                }
                            } else {
                                Button(
                                    onClick = { viewModel.sendMagCalibrationFinish() },
                                    modifier = Modifier.weight(1f)
                                ) {
                                    Text("Finish Calibration (S)")
                                }
                            }
                            OutlinedButton(
                                onClick = { viewModel.cancelCalibration() }
                            ) {
                                Text("Cancel")
                            }
                        }
                    }
                }
            }

            // Parsed Data Display
            val reading = uiState.latestReading
            Card(
                modifier = Modifier.fillMaxWidth(),
                elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        text = "Live Readings",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        MetricItem(
                            label = "Distance",
                            value = if (reading != null) "${reading.distMm.toInt()} mm" else "---",
                            modifier = Modifier.weight(1f)
                        )
                        MetricItem(
                            label = "Bearing",
                            value = if (reading != null) {
                                if (reading.bearing != null) "%.1f°".format(reading.bearing) else "nan"
                            } else "---",
                            modifier = Modifier.weight(1f)
                        )
                        MetricItem(
                            label = "Inclination",
                            value = if (reading != null) "%.1f°".format(reading.incl) else "---",
                            modifier = Modifier.weight(1f)
                        )
                    }

                    Spacer(modifier = Modifier.height(8.dp))
                    HorizontalDivider()
                    Spacer(modifier = Modifier.height(8.dp))

                    // Secondary Panel: Raw AX, AY, AZ, MX, MY, MZ
                    Text(
                        text = "Raw Sensors (AX, AY, AZ, MX, MY, MZ)",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.outline
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = if (reading != null) {
                            "A: [${reading.ax}, ${reading.ay}, ${reading.az}]  |  M: [${reading.mx}, ${reading.my}, ${reading.mz}]"
                        } else {
                            "A: [---] | M: [---]"
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }

            // Run / Stop Toggle Button
            val isConnected = uiState.connectionState == ConnectionState.CONNECTED
            Button(
                onClick = { viewModel.toggleRun() },
                enabled = isConnected && uiState.appMode == AppMode.NORMAL,
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (uiState.isRunning) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.primary
                ),
                modifier = Modifier
                    .fillMaxWidth()
                    .height(50.dp)
            ) {
                Text(
                    text = if (uiState.isRunning) "Stop Readings (S)" else "Start Continuous Readings (R)",
                    style = MaterialTheme.typography.titleMedium
                )
            }

            // Scrolling Output Window (Monospace)
            Card(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
            ) {
                Column(modifier = Modifier.fillMaxSize().padding(8.dp)) {
                    Text(
                        text = "Output Log",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(MaterialTheme.colorScheme.surface)
                            .padding(8.dp)
                    ) {
                        if (uiState.rawLogs.isEmpty()) {
                            Text(
                                text = "Connected output will appear here...",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.outline,
                                modifier = Modifier.align(Alignment.Center)
                            )
                        } else {
                            LazyColumn(
                                state = listState,
                                modifier = Modifier.fillMaxSize()
                            ) {
                                items(uiState.rawLogs) { logLine ->
                                    Text(
                                        text = logLine,
                                        fontFamily = FontFamily.Monospace,
                                        fontSize = 12.sp,
                                        color = MaterialTheme.colorScheme.onSurface
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun MetricItem(label: String, value: String, modifier: Modifier = Modifier) {
    Column(modifier = modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.outline
        )
        Spacer(modifier = Modifier.height(2.dp))
        Text(
            text = value,
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )
    }
}
