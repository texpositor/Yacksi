package me.speleologist.yacsi.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.speleologist.yacsi.protocol.FirmwareInfo
import me.speleologist.yacsi.protocol.ScanSample
import me.speleologist.yacsi.protocol.SideQuality
import me.speleologist.yacsi.viewmodel.ScanState
import java.util.Locale

/** A laser reading older than this is shown as stale. */
private const val STALE_Z_MS = 1500

@Composable
fun ScanPanel(
    state: ScanState,
    firmware: FirmwareInfo?,
    canGo: Boolean,
    canStop: Boolean,
    onGo: () -> Unit,
    onStop: () -> Unit,
    onChooseFolder: () -> Unit,
    modifier: Modifier = Modifier
) {
    Column(modifier = modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        LatestReadings(state.latest)

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = onGo,
                enabled = canGo,
                modifier = Modifier.weight(1f).height(64.dp)
            ) {
                Text("Go", fontSize = 24.sp, fontWeight = FontWeight.Bold)
            }
            Button(
                onClick = onStop,
                enabled = canStop,
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.error,
                    contentColor = MaterialTheme.colorScheme.onError
                ),
                modifier = Modifier.weight(1f).height(64.dp)
            ) {
                Text("Stop", fontSize = 24.sp, fontWeight = FontWeight.Bold)
            }
        }

        RunStatus(state, firmware, onChooseFolder)

        Card(
            modifier = Modifier.fillMaxWidth().weight(1f),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
        ) {
            Column(modifier = Modifier.fillMaxSize().padding(horizontal = 8.dp, vertical = 4.dp)) {
                SampleRow("N", "Z m", "Y m", "Hdg°", "Tilt°", header = true)
                HorizontalDivider()
                if (state.samples.isEmpty()) {
                    Text(
                        text = if (state.isRunning) "Waiting for samples…" else "Press Go (or the instrument's button) to start a run.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.outline,
                        modifier = Modifier.padding(8.dp)
                    )
                } else {
                    // Newest first, so the latest stays at the top without scrolling
                    LazyColumn(modifier = Modifier.fillMaxSize()) {
                        items(state.samples.asReversed()) { sample ->
                            SampleRow(
                                n = sample.n.toString(),
                                z = sample.zMm?.let(::metres) ?: "—",
                                y = sideText(sample),
                                heading = sample.heading?.let(::degrees) ?: "—",
                                tilt = sample.tilt?.let(::degrees) ?: "—",
                                yColor = sideColor(sample.sideQuality),
                                yItalic = sample.sideQuality == SideQuality.DOUBTFUL
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun LatestReadings(latest: ScanSample?) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            val zStale = (latest?.zAgeMs ?: 0) > STALE_Z_MS
            MetricItem(
                label = if (zStale) "Down Z (old)" else "Down Z",
                value = latest?.zMm?.let { "${metres(it)} m" } ?: "---",
                valueColor = if (zStale) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
                modifier = Modifier.weight(1f)
            )
            MetricItem(
                label = "Side Y",
                value = latest?.let { sideText(it).let { text -> if (text == "—") "---" else "$text m" } } ?: "---",
                valueColor = latest?.let { sideColor(it.sideQuality) } ?: MaterialTheme.colorScheme.primary,
                modifier = Modifier.weight(1f)
            )
            MetricItem(
                label = "Heading",
                value = latest?.heading?.let { "${degrees(it)}°" } ?: "---",
                modifier = Modifier.weight(1f)
            )
            MetricItem(
                label = "Tilt",
                value = latest?.tilt?.let { "${degrees(it)}°" } ?: "---",
                modifier = Modifier.weight(1f)
            )
        }
    }
}

@Composable
private fun RunStatus(state: ScanState, firmware: FirmwareInfo?, onChooseFolder: () -> Unit) {
    Column {
        val recording = when {
            state.isRunning && state.fileName != null -> "Recording to ${state.fileName}"
            state.isRunning -> "Running, not saving"
            state.fileName != null -> "Last run: ${state.fileName}"
            else -> "No run yet"
        }
        val counts = if (state.sampleCount > 0 || state.isRunning) {
            " · ${state.sampleCount} samples" + if (state.missedSamples > 0) " · ${state.missedSamples} missed" else ""
        } else ""
        Text(
            text = recording + counts,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = if (state.isRunning) FontWeight.Bold else FontWeight.Normal,
            color = if (state.missedSamples > 0) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
        )
        state.notice?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = if (state.noticeIsError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outline
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "Folder: ${state.folderName ?: "not chosen"}" +
                    (firmware?.let { " · ${it.name} ${it.version}" } ?: ""),
                style = MaterialTheme.typography.bodySmall,
                color = if (state.folderName == null) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outline,
                modifier = Modifier.weight(1f)
            )
            TextButton(onClick = onChooseFolder, enabled = !state.isRunning) {
                Text(if (state.folderName == null) "Choose…" else "Change…")
            }
        }
    }
}

@Composable
private fun SampleRow(
    n: String,
    z: String,
    y: String,
    heading: String,
    tilt: String,
    header: Boolean = false,
    yColor: Color = Color.Unspecified,
    yItalic: Boolean = false
) {
    val style = if (header) MaterialTheme.typography.labelMedium else MaterialTheme.typography.bodyMedium
    val color = if (header) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
        listOf(n, z, y, heading, tilt).forEachIndexed { index, text ->
            Text(
                text = text,
                style = style,
                fontFamily = if (header) null else FontFamily.Monospace,
                fontStyle = if (index == 2 && yItalic) FontStyle.Italic else null,
                color = if (index == 2 && !header && yColor != Color.Unspecified) yColor else color,
                textAlign = TextAlign.End,
                modifier = Modifier.weight(if (index == 0) 0.8f else 1f)
            )
        }
    }
}

/** Good side readings in full, doubtful ones marked with "?", invalid ones not shown. */
private fun sideText(sample: ScanSample): String = when (sample.sideQuality) {
    SideQuality.GOOD -> metres(sample.yMm!!)
    SideQuality.DOUBTFUL -> metres(sample.yMm!!) + "?"
    SideQuality.INVALID, SideQuality.NONE -> "—"
}

@Composable
private fun sideColor(quality: SideQuality): Color = when (quality) {
    SideQuality.GOOD -> MaterialTheme.colorScheme.primary
    SideQuality.DOUBTFUL -> MaterialTheme.colorScheme.tertiary
    SideQuality.INVALID, SideQuality.NONE -> MaterialTheme.colorScheme.outline
}

private fun metres(mm: Int) = "%.2f".format(Locale.US, mm / 1000.0)

private fun degrees(value: Float) = "%.0f".format(Locale.US, value)
