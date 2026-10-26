package me.speleologist.yacsi.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.speleologist.yacsi.bluetooth.ConnectionState
import me.speleologist.yacsi.viewmodel.AppMode
import me.speleologist.yacsi.viewmodel.YacsiViewModel
import java.time.LocalDate

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
    var showEraseShotConfirmDialog by remember { mutableStateOf(false) }
    var showDistanceOffsetDialog by remember { mutableStateOf(false) }

    var selectedTab by rememberSaveable { mutableIntStateOf(0) }

    val listState = rememberLazyListState()
    val clipboardManager = LocalClipboardManager.current

    val newSurveyLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("text/csv")
    ) { uri -> uri?.let { viewModel.createSurvey(it) } }
    val openSurveyLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> uri?.let { viewModel.openSurvey(it) } }

    // Auto-scroll logs to bottom when new lines arrive
    LaunchedEffect(uiState.rawLogs.size, selectedTab) {
        if (selectedTab == 1 && uiState.rawLogs.isNotEmpty()) {
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

    uiState.infoMessage?.let { msg ->
        AlertDialog(
            onDismissRequest = { viewModel.clearInfo() },
            title = { Text("Calibration") },
            text = { Text(msg) },
            confirmButton = {
                TextButton(onClick = { viewModel.clearInfo() }) {
                    Text("OK")
                }
            }
        )
    }

    if (showEraseShotConfirmDialog) {
        AlertDialog(
            onDismissRequest = { showEraseShotConfirmDialog = false },
            title = { Text("Erase Shot Calibration") },
            text = { Text("Stop using the shot calibration and fall back to the rough accelerometer/magnetometer calibration? The stored shots are kept, so Refit Stored Shots can restore it.") },
            confirmButton = {
                TextButton(
                    onClick = {
                        showEraseShotConfirmDialog = false
                        viewModel.eraseShotCalibration()
                    }
                ) {
                    Text("Erase", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showEraseShotConfirmDialog = false }) {
                    Text("Cancel")
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

    if (showDistanceOffsetDialog) {
        DistanceOffsetDialog(
            currentOffsetMm = uiState.distanceOffsetMm,
            onSave = { offsetMm ->
                showDistanceOffsetDialog = false
                viewModel.setDistanceOffset(offsetMm)
            },
            onDismiss = { showDistanceOffsetDialog = false }
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
                        val statusText = when (uiState.connectionState) {
                            ConnectionState.CONNECTED -> "Connected to ${uiState.connectedDeviceName ?: "Yacksi"}"
                            ConnectionState.CONNECTING -> "Connecting to ${uiState.connectedDeviceName ?: "Yacksi"}…"
                            ConnectionState.DISCONNECTED -> {
                                if (uiState.isAutoReconnectEnabled) {
                                    "Auto-reconnecting to ${uiState.connectedDeviceName ?: "Yacksi"}…"
                                } else {
                                    "Disconnected"
                                }
                            }
                        }
                        val statusColor = when {
                            uiState.connectionState == ConnectionState.CONNECTED -> MaterialTheme.colorScheme.primary
                            uiState.connectionState == ConnectionState.CONNECTING || uiState.isAutoReconnectEnabled -> MaterialTheme.colorScheme.secondary
                            else -> MaterialTheme.colorScheme.error
                        }
                        Text(
                            text = statusText,
                            style = MaterialTheme.typography.bodySmall,
                            color = statusColor
                        )
                    }
                },
                actions = {
                    // Connect / Disconnect button
                    val isConnected = uiState.connectionState == ConnectionState.CONNECTED
                    val isConnecting = uiState.connectionState == ConnectionState.CONNECTING
                    val isAutoConnecting = uiState.connectionState == ConnectionState.DISCONNECTED && uiState.isAutoReconnectEnabled
                    val isActiveState = isConnected || isConnecting || isAutoConnecting

                    Button(
                        onClick = {
                            if (isActiveState) {
                                viewModel.disconnect()
                            } else {
                                onRequestPermissions()
                                showDeviceDialog = true
                            }
                        },
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (isConnected) MaterialTheme.colorScheme.errorContainer
                            else if (isConnecting || isAutoConnecting) MaterialTheme.colorScheme.secondaryContainer
                            else MaterialTheme.colorScheme.primary
                        ),
                        modifier = Modifier.padding(end = 8.dp)
                    ) {
                        Text(
                            text = when {
                                isConnected -> "Disconnect"
                                isConnecting || isAutoConnecting -> "Cancel"
                                else -> "Connect"
                            },
                            color = when {
                                isConnected -> MaterialTheme.colorScheme.onErrorContainer
                                isConnecting || isAutoConnecting -> MaterialTheme.colorScheme.onSecondaryContainer
                                else -> MaterialTheme.colorScheme.onPrimary
                            }
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
                                text = { Text("Distance Offset") },
                                onClick = {
                                    showMenu = false
                                    viewModel.requestDistanceOffset()
                                    showDistanceOffsetDialog = true
                                }
                            )
                            HorizontalDivider()
                            DropdownMenuItem(
                                text = { Text("Shot Calibration") },
                                onClick = {
                                    showMenu = false
                                    viewModel.startShotCalibration()
                                }
                            )
                            DropdownMenuItem(
                                text = { Text("Refit Stored Shots") },
                                onClick = {
                                    showMenu = false
                                    viewModel.refitStoredShots()
                                }
                            )
                            DropdownMenuItem(
                                text = { Text("Show Calibration Data") },
                                onClick = {
                                    showMenu = false
                                    viewModel.dumpShotCalibration()
                                }
                            )
                            DropdownMenuItem(
                                text = { Text("Erase Shot Calibration") },
                                onClick = {
                                    showMenu = false
                                    showEraseShotConfirmDialog = true
                                }
                            )
                            HorizontalDivider()
                            DropdownMenuItem(
                                text = { Text("Rough Accelerometer Calibration") },
                                onClick = {
                                    showMenu = false
                                    viewModel.startAccelCalibration()
                                }
                            )
                            DropdownMenuItem(
                                text = { Text("Rough Magnetometer Calibration") },
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
            if (uiState.appMode == AppMode.SHOT_CALIBRATION) {
                ShotCalibrationCard(
                    state = uiState.shotCalibration,
                    onTakeShot = { viewModel.takeCalibrationShot() },
                    onUndo = { viewModel.undoCalibrationShot() },
                    onFinishEarly = { viewModel.finishShotCalibrationEarly() },
                    onAbort = { viewModel.abortShotCalibration() },
                    onAccept = { viewModel.acceptShotCalibration() },
                    onDiscard = { viewModel.discardShotCalibration() }
                )
            } else if (uiState.appMode != AppMode.NORMAL) {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.tertiaryContainer
                    ),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text(
                            text = when (uiState.appMode) {
                                AppMode.ACCEL_CALIBRATION -> "Mode: Rough Accelerometer Calibration"
                                else -> "Mode: Rough Magnetometer Calibration"
                            },
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(
                            text = when (uiState.appMode) {
                                AppMode.ACCEL_CALIBRATION -> "Place device in requested orientation and press Continue."
                                else -> "Rotate instrument through all orientations continuously until complete."
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                        if (uiState.appMode == AppMode.ACCEL_CALIBRATION) {
                            Spacer(modifier = Modifier.height(12.dp))
                            Button(
                                onClick = { viewModel.sendCalibrationContinue() },
                                modifier = Modifier.fillMaxWidth()
                            ) {
                                Text("Continue to Next Position")
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

                    if (reading?.calibration != null) {
                        Spacer(modifier = Modifier.height(4.dp))
                        // Under the shot calibration the field magnitude is 1.0; a large
                        // deviation suggests nearby iron or electrical interference.
                        val interference = reading.calibration == "SHOT" &&
                            reading.mLen != null && kotlin.math.abs(reading.mLen - 1f) > 0.05f
                        Text(
                            text = buildString {
                                append(if (reading.calibration == "SHOT") "Shot calibration" else "Rough calibration only")
                                reading.mLen?.let { append("  ·  field %.3f".format(it)) }
                                reading.laserMm?.let { laserMm ->
                                    val offsetMm = (reading.distMm - laserMm).toInt()
                                    if (offsetMm != 0) append("  ·  distance offset %+d mm".format(offsetMm))
                                }
                                if (interference) append("  ·  possible magnetic interference")
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = if (interference || reading.calibration != "SHOT") {
                                MaterialTheme.colorScheme.error
                            } else {
                                MaterialTheme.colorScheme.outline
                            }
                        )
                    }
                }
            }

            // Take Reading Button
            val isConnected = uiState.connectionState == ConnectionState.CONNECTED
            Button(
                onClick = { viewModel.takeReading() },
                enabled = isConnected && uiState.appMode == AppMode.NORMAL,
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.primary
                ),
                modifier = Modifier
                    .fillMaxWidth()
                    .height(50.dp)
            ) {
                Text(
                    text = "Take Reading (R)",
                    style = MaterialTheme.typography.titleMedium
                )
            }

            PrimaryTabRow(selectedTabIndex = selectedTab) {
                Tab(selected = selectedTab == 0, onClick = { selectedTab = 0 }, text = { Text("Survey") })
                Tab(selected = selectedTab == 1, onClick = { selectedTab = 1 }, text = { Text("Log") })
            }

            if (selectedTab == 0) {
                SurveyPanel(
                    state = uiState.survey,
                    onNewSurvey = { newSurveyLauncher.launch("survey-${LocalDate.now()}.csv") },
                    onOpenSurvey = {
                        openSurveyLauncher.launch(arrayOf("text/*", "application/csv", "application/octet-stream"))
                    },
                    onFromChange = viewModel::setFromStation,
                    onToChange = viewModel::setToStation,
                    onLrudChange = viewModel::setLrud,
                    onTargetChange = viewModel::setShotTarget,
                    onLrudBeforeLegChange = viewModel::setLrudBeforeLeg,
                    onUndoLastLeg = viewModel::undoLastLeg,
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f)
                )
            } else {
                // Scrolling Output Window (Monospace)
                Card(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f),
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
                ) {
                    Column(modifier = Modifier.fillMaxSize().padding(8.dp)) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = "Output Log",
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                            if (uiState.rawLogs.isNotEmpty()) {
                                IconButton(
                                    onClick = {
                                        clipboardManager.setText(AnnotatedString(uiState.rawLogs.joinToString("\n")))
                                    },
                                    modifier = Modifier.size(24.dp)
                                ) {
                                    Icon(
                                        imageVector = Icons.Default.ContentCopy,
                                        contentDescription = "Copy Output Log",
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.size(16.dp)
                                    )
                                }
                            }
                        }
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
