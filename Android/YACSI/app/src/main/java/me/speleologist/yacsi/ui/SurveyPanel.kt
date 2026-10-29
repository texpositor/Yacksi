package me.speleologist.yacsi.ui

import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import me.speleologist.yacsi.survey.LrudSide
import me.speleologist.yacsi.survey.StationLrud
import me.speleologist.yacsi.survey.SurveyLeg
import me.speleologist.yacsi.viewmodel.ShotTarget
import me.speleologist.yacsi.viewmodel.SurveyState

@Composable
fun SurveyPanel(
    state: SurveyState,
    canTakeReading: Boolean,
    onTakeReading: () -> Unit,
    onNewSurvey: () -> Unit,
    onOpenSurvey: () -> Unit,
    onAtChange: (String) -> Unit,
    onToChange: (String) -> Unit,
    onLrudChange: (LrudSide, String) -> Unit,
    onTargetChange: (ShotTarget) -> Unit,
    onUndoLastLeg: () -> Unit,
    lrudFocusRequests: Flow<LrudSide> = emptyFlow(),
    modifier: Modifier = Modifier
) {
    var showUndoConfirm by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val lrudFocus = remember { LrudSide.entries.associateWith { FocusRequester() } }

    // Move to the next LRUD field after one is filled by voice
    LaunchedEffect(lrudFocusRequests) {
        lrudFocusRequests.collect { side ->
            // The controls are item 1; bring them back if they have scrolled away
            if (listState.layoutInfo.visibleItemsInfo.none { it.index == 1 }) {
                listState.scrollToItem(1)
                withFrameNanos { }
            }
            // Not attached when no survey is open
            runCatching { lrudFocus.getValue(side).requestFocus() }
        }
    }

    if (showUndoConfirm) {
        val leg = state.legs.lastOrNull()
        AlertDialog(
            onDismissRequest = { showUndoConfirm = false },
            title = { Text("Remove Last Leg") },
            text = { Text("Remove ${leg?.from} → ${leg?.to} from the survey file? Its stations and LRUD are put back so it can be shot again.") },
            confirmButton = {
                TextButton(onClick = {
                    showUndoConfirm = false
                    onUndoLastLeg()
                }) {
                    Text("Remove", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showUndoConfirm = false }) {
                    Text("Cancel")
                }
            }
        )
    }

    // Controls and legs share one LazyColumn so the whole tab scrolls together
    LazyColumn(state = listState, modifier = modifier) {
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = state.fileName ?: "No survey open",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f)
                )
                TextButton(onClick = onNewSurvey) { Text("New") }
                TextButton(onClick = onOpenSurvey) { Text("Open") }
            }
        }

        if (!state.isOpen) {
            item {
                Text(
                    "Start a new survey file or open an existing one to record legs.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.outline
                )
            }
            return@LazyColumn
        }

        item {
            SurveyControls(
                state = state,
                canTakeReading = canTakeReading,
                onTakeReading = onTakeReading,
                onAtChange = onAtChange,
                onToChange = onToChange,
                onLrudChange = onLrudChange,
                onTargetChange = onTargetChange,
                onUndoLeg = { showUndoConfirm = true },
                lrudFocus = lrudFocus
            )
        }

        // LRUD waiting at stations with no leg from them yet, e.g. passage ends
        items(state.stationLrud.values.toList()) { station -> StationLrudRow(station) }
        items(state.legs.asReversed()) { leg -> LegRow(leg) }
    }
}

@Composable
private fun SurveyControls(
    state: SurveyState,
    canTakeReading: Boolean,
    onTakeReading: () -> Unit,
    onAtChange: (String) -> Unit,
    onToChange: (String) -> Unit,
    onLrudChange: (LrudSide, String) -> Unit,
    onTargetChange: (ShotTarget) -> Unit,
    onUndoLeg: () -> Unit,
    lrudFocus: Map<LrudSide, FocusRequester>
) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        val stationKeyboard = KeyboardOptions(
            capitalization = KeyboardCapitalization.Characters,
            autoCorrectEnabled = false,
            keyboardType = KeyboardType.Ascii
        )
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            CompactTextField(
                value = state.atStation,
                onValueChange = onAtChange,
                label = "At",
                keyboardOptions = stationKeyboard,
                modifier = Modifier.weight(1f)
            )
            Text("→", style = MaterialTheme.typography.titleMedium)
            CompactTextField(
                value = state.toStation,
                onValueChange = onToChange,
                label = "To",
                keyboardOptions = stationKeyboard,
                modifier = Modifier.weight(1f)
            )
            // Readings are normally taken with the device button, so this stays small
            FilledTonalButton(
                onClick = onTakeReading,
                enabled = canTakeReading,
                contentPadding = PaddingValues(horizontal = 12.dp)
            ) {
                Text("Read")
            }
        }

        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            ShotTarget.entries.forEach { target ->
                FilterChip(
                    selected = state.target == target,
                    onClick = { onTargetChange(target) },
                    label = { Text(target.label) }
                )
            }
            Spacer(modifier = Modifier.weight(1f))
            TextButton(
                onClick = onUndoLeg,
                enabled = state.legs.isNotEmpty(),
                contentPadding = PaddingValues(horizontal = 8.dp)
            ) {
                Text("Undo Leg")
            }
        }

        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            LrudSide.entries.forEach { side ->
                CompactTextField(
                    value = state.lrud[side] ?: "",
                    onValueChange = { onLrudChange(side, it) },
                    label = side.label,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.weight(1f).focusRequester(lrudFocus.getValue(side))
                )
            }
        }

        state.notice?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = if (state.noticeIsError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary
            )
        }

        HorizontalDivider()
        Text(
            "${state.legs.size} legs  (distance m, bearing °, inclination °, L R U D at the At station)",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.outline
        )
    }
}

/**
 * An outlined text field with tighter inner padding and no minimum height, so the
 * station and LRUD rows take less vertical space than a standard OutlinedTextField.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CompactTextField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    keyboardOptions: KeyboardOptions,
    modifier: Modifier = Modifier
) {
    val interactionSource = remember { MutableInteractionSource() }
    val colors = OutlinedTextFieldDefaults.colors()
    BasicTextField(
        value = value,
        onValueChange = onValueChange,
        singleLine = true,
        keyboardOptions = keyboardOptions,
        textStyle = MaterialTheme.typography.bodyMedium.copy(color = MaterialTheme.colorScheme.onSurface),
        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
        interactionSource = interactionSource,
        modifier = modifier
    ) { innerTextField ->
        OutlinedTextFieldDefaults.DecorationBox(
            value = value,
            innerTextField = innerTextField,
            enabled = true,
            singleLine = true,
            visualTransformation = VisualTransformation.None,
            interactionSource = interactionSource,
            label = { Text(label) },
            colors = colors,
            contentPadding = PaddingValues(horizontal = 8.dp, vertical = 6.dp),
            container = {
                OutlinedTextFieldDefaults.Container(
                    enabled = true,
                    isError = false,
                    interactionSource = interactionSource,
                    colors = colors
                )
            }
        )
    }
}

private fun formatLrudColumns(lrud: Map<LrudSide, Double>) = LrudSide.entries.joinToString(" ") { side ->
    lrud[side]?.let { "%.2f".format(it) } ?: "-"
}

@Composable
private fun StationLrudRow(station: StationLrud) {
    Text(
        text = "%-6s %-19s  %s".format(station.station, "(station)", formatLrudColumns(station.lrud)),
        fontFamily = FontFamily.Monospace,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.outline,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis
    )
}

@Composable
private fun LegRow(leg: SurveyLeg) {
    Text(
        text = "%-6s %-6s %7.2f %5.1f %5.1f  %s".format(
            leg.from, leg.to, leg.distanceM, leg.bearing, leg.inclination, formatLrudColumns(leg.lrud)
        ),
        fontFamily = FontFamily.Monospace,
        style = MaterialTheme.typography.bodySmall,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis
    )
}
