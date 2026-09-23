package com.droidspaces.app.ui.screen

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDownward
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.droidspaces.app.R
import com.droidspaces.app.ui.component.DialogFooterRow
import com.droidspaces.app.ui.component.DsDialog
import com.droidspaces.app.ui.wayland.WaylandNative
import com.droidspaces.app.util.WaylandExtraKey
import com.droidspaces.app.util.defaultWaylandExtraKeys
import com.droidspaces.app.util.waylandExtraKeyPresets

// Modifier state: active means key is held down, locked means it survives
// a non-modifier key press (long-press to lock, like Anland/Termux).
data class ModifierState(val code: Int, val active: Boolean = false, val locked: Boolean = false)

// The key dispatch engine, called by both the bar and the Gboard bridge.
// Returns the updated modifier map. Modifiers wrap non-modifier keys in
// LIFO order, then auto-release unless locked, matching Anland's
// sendWithModifiers.
fun dispatchExtraKey(
    key: WaylandExtraKey,
    modifiers: Map<Int, ModifierState>,
    sendKey: (Int, Int) -> Unit = { code, state -> WaylandNative.nativeSendKey(code, state) },
    sendText: ((String) -> Unit)? = null,
    onSystemCommand: ((String) -> Unit)? = null
): Map<Int, ModifierState> {
    val mods = modifiers.toMutableMap()

    when (key.type) {
        WaylandExtraKey.TYPE_MODIFIER -> {
            val current = mods[key.code]
            if (current != null && current.active) {
                sendKey(key.code, 0)
                mods.remove(key.code)
            } else {
                sendKey(key.code, 1)
                mods[key.code] = ModifierState(key.code, active = true, locked = false)
            }
        }

        WaylandExtraKey.TYPE_COMBO -> {
            val codes = key.comboKeys
            codes.forEach { sendKey(it, 1) }
            codes.reversed().forEach { sendKey(it, 0) }
        }

        WaylandExtraKey.TYPE_TEXT -> {
            if (sendText != null && key.text.isNotBlank()) {
                sendText(key.text)
            }
        }

        WaylandExtraKey.TYPE_SYSTEM -> {
            onSystemCommand?.invoke(key.systemCommand)
        }

        else -> {
            // Regular key: wrap with active modifiers LIFO
            val held = mods.values.filter { it.active }.sortedBy { it.code }
            held.forEach { sendKey(it.code, 1) }
            sendKey(key.code, 1)
            sendKey(key.code, 0)
            held.reversed().forEach { sendKey(it.code, 0) }
            // Auto-release unlocked modifiers
            held.filter { !it.locked }.forEach { mods.remove(it.code) }
        }
    }
    return mods.toMap()
}

fun lockModifier(code: Int, modifiers: Map<Int, ModifierState>): Map<Int, ModifierState> {
    val mods = modifiers.toMutableMap()
    val current = mods[code]
    if (current != null && current.active) {
        mods[code] = current.copy(locked = !current.locked)
    } else {
        WaylandNative.nativeSendKey(code, 1)
        mods[code] = ModifierState(code, active = true, locked = true)
    }
    return mods.toMap()
}

fun releaseAllModifiers(
    modifiers: Map<Int, ModifierState>,
    sendKey: (Int, Int) -> Unit = { code, state -> WaylandNative.nativeSendKey(code, state) }
): Map<Int, ModifierState> {
    modifiers.values.filter { it.active }.forEach { sendKey(it.code, 0) }
    return emptyMap()
}

// Multi-row extra keys bar with modifier combo dispatch and auto-repeat.
@Composable
fun WaylandExtraKeysBar(
    rows: List<List<WaylandExtraKey>>,
    modifiers: Map<Int, ModifierState>,
    onKeyAction: (WaylandExtraKey) -> Unit,
    onKeyLongPress: (WaylandExtraKey) -> Unit,
    onEdit: () -> Unit,
    modifier: Modifier = Modifier
) {
    val haptic = LocalHapticFeedback.current

    Surface(
        modifier = modifier,
        color = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.88f),
        tonalElevation = 0.dp,
        shape = RoundedCornerShape(24.dp),
        border = BorderStroke(
            1.dp,
            MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f)
        )
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(
                modifier = Modifier.weight(1f, fill = false),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                rows.forEachIndexed { rowIdx, row ->
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        row.forEach { key ->
                            val isActive = when (key.type) {
                                WaylandExtraKey.TYPE_MODIFIER ->
                                    modifiers[key.code]?.active == true
                                else -> false
                            }
                            val isLocked = modifiers[key.code]?.locked == true

                            Box(
                                modifier = Modifier
                                    .height(40.dp)
                                    .clip(RoundedCornerShape(10.dp))
                                    .background(
                                        when {
                                            isLocked -> MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.8f)
                                            isActive -> MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.5f)
                                            else -> MaterialTheme.colorScheme.surfaceContainerHigh
                                        }
                                    )
                                    .pointerInput(key) {
                                        detectTapGestures(
                                            onTap = {
                                                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                                onKeyAction(key)
                                            },
                                            onLongPress = {
                                                haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                                                onKeyLongPress(key)
                                            }
                                        )
                                    }
                                    .padding(horizontal = 10.dp),
                                contentAlignment = Alignment.Center
                            ) {
                                Text(
                                    text = key.label,
                                    fontSize = 13.sp,
                                    fontWeight = if (isActive || isLocked) FontWeight.Bold else FontWeight.SemiBold,
                                    color = when {
                                        isLocked -> MaterialTheme.colorScheme.primary
                                        isActive -> MaterialTheme.colorScheme.primary
                                        else -> MaterialTheme.colorScheme.onSurface
                                    },
                                    maxLines = 1
                                )
                            }
                        }
                    }
                }
            }
            IconButton(onClick = onEdit, modifier = Modifier.size(40.dp)) {
                Icon(
                    imageVector = Icons.Default.Edit,
                    contentDescription = stringResource(R.string.wayland_extra_keys_edit),
                    tint = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.size(18.dp)
                )
            }
        }
    }
}

// Bottom dock that rides above the navigation bar and Gboard.
@Composable
fun WaylandExtraKeysDock(
    rows: List<List<WaylandExtraKey>>,
    modifiers: Map<Int, ModifierState>,
    onKeyAction: (WaylandExtraKey) -> Unit,
    onKeyLongPress: (WaylandExtraKey) -> Unit,
    onEdit: () -> Unit,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .navigationBarsPadding()
            .padding(horizontal = 12.dp)
            .padding(bottom = 8.dp),
        contentAlignment = Alignment.Center
    ) {
        WaylandExtraKeysBar(
            rows = rows,
            modifiers = modifiers,
            onKeyAction = onKeyAction,
            onKeyLongPress = onKeyLongPress,
            onEdit = onEdit
        )
    }
}

// Key picker grid: tap to select a key from presets instead of typing evdev codes.
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun KeyPickerGrid(
    onPick: (WaylandExtraKey) -> Unit,
    currentTab: Int,
    onTabChange: (Int) -> Unit
) {
    val tabs = listOf("Keys", "Mods & Nav", "Text")
    Column {
        TabRow(selectedTabIndex = currentTab) {
            tabs.forEachIndexed { index, title ->
                Tab(selected = currentTab == index, onClick = { onTabChange(index) },
                    text = { Text(title, fontSize = 12.sp) })
            }
        }
        Spacer(Modifier.height(8.dp))
        val items = when (currentTab) {
            0 -> waylandExtraKeyPresets.filter {
                it.type == WaylandExtraKey.TYPE_KEY && it.code in listOf(
                    1, 15, 28, 14, 111, 57,
                    59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88
                )
            }
            1 -> waylandExtraKeyPresets.filter {
                it.type == WaylandExtraKey.TYPE_MODIFIER ||
                (it.type == WaylandExtraKey.TYPE_KEY && it.code in listOf(
                    103, 108, 105, 106, 102, 107, 104, 109
                )) ||
                it.type == WaylandExtraKey.TYPE_SYSTEM
            }
            else -> waylandExtraKeyPresets.filter {
                it.type == WaylandExtraKey.TYPE_TEXT
            }
        }
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier.fillMaxWidth()
        ) {
            items.forEach { preset ->
                FilterChip(
                    selected = false,
                    onClick = { onPick(preset) },
                    label = { Text(preset.label, fontSize = 12.sp) }
                )
            }
        }
    }
}

// Full customizer dialog with multi-row editing and key picker.
@Composable
fun WaylandExtraKeysDialog(
    initial: List<List<WaylandExtraKey>>,
    onDismiss: () -> Unit,
    onSave: (List<List<WaylandExtraKey>>) -> Unit
) {
    val rows = remember {
        mutableStateListOf<MutableList<WaylandExtraKey>>().apply {
            initial.forEach { add(it.toMutableList()) }
        }
    }
    var pickerTab by remember { mutableIntStateOf(0) }
    var targetRow by remember { mutableIntStateOf(0) }

    DsDialog(
        onDismiss = onDismiss,
        scrollableContent = false,
        footer = {
            DialogFooterRow(
                dismissLabel = stringResource(R.string.cancel),
                confirmLabel = stringResource(R.string.wayland_extra_keys_save),
                onDismiss = onDismiss,
                onConfirm = { onSave(rows.map { it.toList() }) }
            )
        }
    ) {
        Text(
            text = stringResource(R.string.wayland_extra_keys_title),
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.Bold
        )

        LazyColumn(modifier = Modifier.weight(1f, fill = false)) {
            items(rows.size) { rowIndex ->
                Column(modifier = Modifier.fillMaxWidth()) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            "Row ${rowIndex + 1}",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Row {
                            if (rows.size > 1) {
                                IconButton(onClick = { rows.removeAt(rowIndex) },
                                    modifier = Modifier.size(32.dp)) {
                                    Icon(Icons.Default.Delete, null,
                                        tint = MaterialTheme.colorScheme.error,
                                        modifier = Modifier.size(16.dp))
                                }
                            }
                        }
                    }
                    rows[rowIndex].forEachIndexed { keyIndex, key ->
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(key.label, style = MaterialTheme.typography.bodyMedium)
                                Text(
                                    buildString {
                                        append(key.type)
                                        if (key.code > 0) append(" (${key.code})")
                                        if (key.repeat) append(" ⟳")
                                        if (key.type == WaylandExtraKey.TYPE_MODIFIER) append(" ◆")
                                        if (key.comboKeys.isNotEmpty())
                                            append(" [${key.comboKeys.joinToString("+")}]")
                                        if (key.text.isNotBlank()) append(" \"${key.text}\"")
                                    },
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                                )
                            }
                            IconButton(
                                onClick = {
                                    if (keyIndex > 0) {
                                        val item = rows[rowIndex].removeAt(keyIndex)
                                        rows[rowIndex].add(keyIndex - 1, item)
                                    }
                                },
                                enabled = keyIndex > 0,
                                modifier = Modifier.size(32.dp)
                            ) {
                                Icon(Icons.Default.ArrowUpward, null, modifier = Modifier.size(16.dp))
                            }
                            IconButton(
                                onClick = {
                                    if (keyIndex < rows[rowIndex].size - 1) {
                                        val item = rows[rowIndex].removeAt(keyIndex)
                                        rows[rowIndex].add(keyIndex + 1, item)
                                    }
                                },
                                enabled = keyIndex < rows[rowIndex].size - 1,
                                modifier = Modifier.size(32.dp)
                            ) {
                                Icon(Icons.Default.ArrowDownward, null, modifier = Modifier.size(16.dp))
                            }
                            IconButton(
                                onClick = { rows[rowIndex].removeAt(keyIndex) },
                                modifier = Modifier.size(32.dp)
                            ) {
                                Icon(Icons.Default.Delete, null,
                                    tint = MaterialTheme.colorScheme.error,
                                    modifier = Modifier.size(16.dp))
                            }
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                }
            }
        }

        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Add to:", style = MaterialTheme.typography.labelSmall)
            rows.indices.forEach { i ->
                FilterChip(
                    selected = targetRow == i,
                    onClick = { targetRow = i },
                    label = { Text("R${i + 1}", fontSize = 11.sp) }
                )
            }
            OutlinedButton(onClick = {
                rows.add(mutableListOf())
                targetRow = rows.size - 1
            }) {
                Text("+Row", fontSize = 11.sp)
            }
        }

        KeyPickerGrid(
            onPick = { preset ->
                val idx = targetRow.coerceIn(0, (rows.size - 1).coerceAtLeast(0))
                if (idx < rows.size) {
                    rows[idx].add(preset)
                }
            },
            currentTab = pickerTab,
            onTabChange = { pickerTab = it }
        )

        Spacer(Modifier.height(4.dp))
        TextButton(onClick = {
            rows.clear()
            defaultWaylandExtraKeys().forEach { rows.add(it.toMutableList()) }
            targetRow = 0
        }) {
            Text(stringResource(R.string.wayland_extra_keys_reset))
        }
    }
}
