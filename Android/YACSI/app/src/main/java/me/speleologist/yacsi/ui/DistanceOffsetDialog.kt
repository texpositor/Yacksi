package me.speleologist.yacsi.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp

// Matches MAX_DISTANCE_OFFSET_MM in the firmware.
private const val MAX_DISTANCE_OFFSET_MM = 2000

@Composable
fun DistanceOffsetDialog(
    currentOffsetMm: Int?,
    onSave: (Int) -> Unit,
    onDismiss: () -> Unit
) {
    var text by remember { mutableStateOf(currentOffsetMm?.toString() ?: "") }
    var edited by remember { mutableStateOf(false) }

    // The current value is requested from the device when the dialog opens, so it
    // may arrive after the dialog is shown; fill it in unless the user has started typing.
    LaunchedEffect(currentOffsetMm) {
        if (!edited && currentOffsetMm != null) text = currentOffsetMm.toString()
    }

    val offsetMm = text.trim().removePrefix("+").toIntOrNull()
    val isValid = offsetMm != null && offsetMm in -MAX_DISTANCE_OFFSET_MM..MAX_DISTANCE_OFFSET_MM

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Distance Offset") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    "Added to every laser distance. To use the rear of the enclosure as the datum, " +
                        "enter the distance from the rear to the laser's own datum. To correct against " +
                        "a steel tape, enter (tape − laser reading).",
                    style = MaterialTheme.typography.bodyMedium
                )
                OutlinedTextField(
                    value = text,
                    onValueChange = {
                        text = it
                        edited = true
                    },
                    label = { Text("Offset (mm)") },
                    placeholder = { if (currentOffsetMm == null) Text("Reading from device…") },
                    singleLine = true,
                    isError = text.isNotBlank() && !isValid,
                    supportingText = {
                        Text(
                            if (text.isNotBlank() && !isValid) {
                                "Enter whole millimetres between -$MAX_DISTANCE_OFFSET_MM and $MAX_DISTANCE_OFFSET_MM."
                            } else {
                                "Negative values shorten the distance."
                            }
                        )
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.fillMaxWidth()
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = { offsetMm?.let(onSave) },
                enabled = isValid
            ) {
                Text("Save")
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("Cancel")
            }
        }
    )
}
