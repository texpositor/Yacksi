package me.speleologist.yacsi.ui

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

@SuppressLint("MissingPermission")
@Composable
fun PairedDevicesDialog(
    bondedDevices: Set<BluetoothDevice>,
    onDeviceSelected: (BluetoothDevice) -> Unit,
    onDismiss: () -> Unit
) {
    val sortedDevices = bondedDevices.sortedByDescending { 
        it.name?.equals("Yacksi", ignoreCase = true) == true 
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Select Bluetooth Device") },
        text = {
            Column(modifier = Modifier.fillMaxWidth().heightIn(max = 300.dp)) {
                if (sortedDevices.isEmpty()) {
                    Text(
                        "No paired Bluetooth devices found.\nPlease pair 'Yacksi' in Android Bluetooth settings first.",
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.padding(vertical = 16.dp)
                    )
                } else {
                    Text(
                        "Select a paired device to connect:",
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(bottom = 8.dp)
                    )
                    LazyColumn(modifier = Modifier.fillMaxWidth()) {
                        items(sortedDevices) { device ->
                            val deviceName = device.name ?: "Unknown Device"
                            val isYacksi = deviceName.equals("Yacksi", ignoreCase = true)
                            
                            Card(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(vertical = 4.dp)
                                    .clickable { onDeviceSelected(device) },
                                colors = if (isYacksi) {
                                    CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)
                                } else {
                                    CardDefaults.cardColors()
                                }
                            ) {
                                Row(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(12.dp),
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Column(modifier = Modifier.weight(1f)) {
                                        Text(
                                            text = deviceName + if (isYacksi) " (Default)" else "",
                                            fontWeight = if (isYacksi) FontWeight.Bold else FontWeight.Normal,
                                            color = if (isYacksi) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurface
                                        )
                                        Spacer(modifier = Modifier.height(2.dp))
                                        Text(
                                            text = device.address,
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.outline
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text("Cancel")
            }
        }
    )
}
