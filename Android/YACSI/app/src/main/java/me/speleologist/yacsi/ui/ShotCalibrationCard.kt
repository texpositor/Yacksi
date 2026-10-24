package me.speleologist.yacsi.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import me.speleologist.yacsi.viewmodel.ShotCalibrationResult
import me.speleologist.yacsi.viewmodel.ShotCalibrationState
import me.speleologist.yacsi.viewmodel.ShotPhase

@Composable
fun ShotCalibrationCard(
    state: ShotCalibrationState,
    onTakeShot: () -> Unit,
    onUndo: () -> Unit,
    onFinishEarly: () -> Unit,
    onAbort: () -> Unit,
    onAccept: () -> Unit,
    onDiscard: () -> Unit
) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "Shot Calibration",
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onTertiaryContainer
            )
            Spacer(modifier = Modifier.height(8.dp))
            when (state.phase) {
                ShotPhase.AIMING, ShotPhase.SAMPLING -> AimingContent(
                    state, onTakeShot, onUndo, onFinishEarly, onAbort
                )
                ShotPhase.FITTING -> BusyRow("Fitting calibration…")
                ShotPhase.REVIEW -> ReviewContent(state.result, onAccept, onDiscard)
            }
        }
    }
}

@Composable
private fun AimingContent(
    state: ShotCalibrationState,
    onTakeShot: () -> Unit,
    onUndo: () -> Unit,
    onFinishEarly: () -> Unit,
    onAbort: () -> Unit
) {
    val onCard = MaterialTheme.colorScheme.onTertiaryContainer
    Text(
        text = "Shot ${state.shot} of ${state.totalShots}  ·  Target ${state.group} of ${state.groups}  ·  Roll ${state.roll} of ${state.rolls}",
        style = MaterialTheme.typography.bodySmall,
        color = onCard
    )
    Spacer(modifier = Modifier.height(4.dp))
    LinearProgressIndicator(
        progress = { state.shotsTaken.toFloat() / state.totalShots.coerceAtLeast(1) },
        modifier = Modifier.fillMaxWidth()
    )
    Spacer(modifier = Modifier.height(8.dp))
    Text(
        text = state.instruction,
        style = MaterialTheme.typography.bodyLarge,
        fontWeight = FontWeight.Medium,
        color = onCard
    )
    Spacer(modifier = Modifier.height(4.dp))
    Text(
        text = "Aim the laser at a clearly marked point at least 2 m away, brace the instrument and hold still. The laser switches off briefly while the shot is taken.",
        style = MaterialTheme.typography.bodySmall,
        color = onCard
    )
    state.notice?.let {
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = it,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = if (state.noticeIsError) FontWeight.Bold else FontWeight.Normal,
            color = if (state.noticeIsError) MaterialTheme.colorScheme.error else onCard
        )
    }
    Spacer(modifier = Modifier.height(12.dp))
    if (state.phase == ShotPhase.SAMPLING) {
        BusyRow("Hold still…")
    } else {
        Button(
            onClick = onTakeShot,
            modifier = Modifier
                .fillMaxWidth()
                .height(56.dp)
        ) {
            Text("Take Shot", style = MaterialTheme.typography.titleMedium)
        }
    }
    Spacer(modifier = Modifier.height(8.dp))
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        val idle = state.phase == ShotPhase.AIMING
        OutlinedButton(
            onClick = onUndo,
            enabled = idle && state.shotsTaken > 0,
            modifier = Modifier.weight(1f)
        ) { Text("Undo") }
        OutlinedButton(
            onClick = onFinishEarly,
            enabled = idle && state.canFinishEarly,
            modifier = Modifier.weight(1f)
        ) { Text("Fit Now") }
        TextButton(onClick = onAbort, modifier = Modifier.weight(1f)) {
            Text("Abort", color = MaterialTheme.colorScheme.error)
        }
    }
    if (!state.canFinishEarly) {
        Text(
            text = "Fit Now is available after ${state.minGroups} complete targets.",
            style = MaterialTheme.typography.bodySmall,
            color = onCard
        )
    }
}

@Composable
private fun ReviewContent(
    result: ShotCalibrationResult?,
    onAccept: () -> Unit,
    onDiscard: () -> Unit
) {
    val onCard = MaterialTheme.colorScheme.onTertiaryContainer
    if (result == null) {
        Text("No result received.", color = onCard)
    } else {
        val qualityColor = when (result.quality) {
            "GOOD" -> Color(0xFF2E7D32)
            "OK" -> Color(0xFFF9A825)
            else -> MaterialTheme.colorScheme.error
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "RMS error %.2f°".format(result.rmsError),
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold,
                color = onCard
            )
            Spacer(modifier = Modifier.size(12.dp))
            Text(
                text = result.quality,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = qualityColor
            )
        }
        Text(
            text = "Worst shot %.2f° (shot %d)  ·  Magnetic dip %.1f°  ·  %d shots".format(
                result.maxError, result.maxShot, result.dip, result.shots
            ),
            style = MaterialTheme.typography.bodySmall,
            color = onCard
        )
        if (result.groupErrors.isNotEmpty()) {
            Spacer(modifier = Modifier.height(8.dp))
            Text("Error by target (°):", style = MaterialTheme.typography.bodySmall, color = onCard)
            val worst = result.groupErrors.withIndex().sortedByDescending { it.value }.take(2).map { it.index }.toSet()
            result.groupErrors.withIndex().chunked(7).forEach { row ->
                Row(modifier = Modifier.fillMaxWidth()) {
                    row.forEach { (index, error) ->
                        Text(
                            text = "%d:%.2f".format(index + 1, error),
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            fontWeight = if (index in worst) FontWeight.Bold else FontWeight.Normal,
                            color = if (index in worst && result.quality != "GOOD") {
                                MaterialTheme.colorScheme.error
                            } else {
                                onCard
                            },
                            modifier = Modifier.weight(1f)
                        )
                    }
                    repeat(7 - row.size) { Spacer(modifier = Modifier.weight(1f)) }
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = when (result.quality) {
                "GOOD" -> "Good calibration. Save it to apply it to all readings."
                "OK" -> "Usable, but a repeat with steadier, more careful aiming may do better."
                else -> "Poor result. Check for nearby metal and that each group was aimed at the same point, then repeat."
            },
            style = MaterialTheme.typography.bodySmall,
            color = onCard
        )
    }
    Spacer(modifier = Modifier.height(12.dp))
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Button(onClick = onAccept, enabled = result != null, modifier = Modifier.weight(1f)) {
            Text("Save & Apply")
        }
        OutlinedButton(onClick = onDiscard, modifier = Modifier.weight(1f)) {
            Text("Discard")
        }
    }
}

@Composable
private fun BusyRow(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        CircularProgressIndicator(modifier = Modifier.size(24.dp), strokeWidth = 3.dp)
        Spacer(modifier = Modifier.size(12.dp))
        Text(text, color = MaterialTheme.colorScheme.onTertiaryContainer)
    }
}
