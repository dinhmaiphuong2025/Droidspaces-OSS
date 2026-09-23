package com.droidspaces.app.ui.screen

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ArrowDownward
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
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

// Horizontal extra keys bar docked at the bottom, Termux style.
// Sticky keys latch: first tap holds the key down, second tap releases it.
@Composable
fun WaylandExtraKeysBar(
    keys: List<WaylandExtraKey>,
    latchedCodes: Set<Int>,
    onTapKey: (WaylandExtraKey) -> Unit,
    onEdit: () -> Unit,
    modifier: Modifier = Modifier
) {
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
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            LazyRow(
                modifier = Modifier.weight(1f, fill = false),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                items(keys, key = { "${it.label}:${it.code}" }) { key ->
                    val latched = latchedCodes.contains(key.code)
                    Box(
                        modifier = Modifier
                            .height(48.dp)
                            .clip(RoundedCornerShape(12.dp))
                            .background(
                                if (latched) MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.6f)
                                else MaterialTheme.colorScheme.surfaceContainerHigh
                            )
                            .clickable { onTapKey(key) }
                            .padding(horizontal = 12.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = key.label,
                            fontSize = 14.sp,
                            fontWeight = FontWeight.SemiBold,
                            color = if (latched) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurface
                        )
                    }
                }
            }
            IconButton(onClick = onEdit, modifier = Modifier.size(48.dp)) {
                Icon(
                    imageVector = Icons.Default.Edit,
                    contentDescription = stringResource(R.string.wayland_extra_keys_edit),
                    tint = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.size(20.dp)
                )
            }
        }
    }
}

fun tapWaylandExtraKey(key: WaylandExtraKey, latched: Set<Int>): Set<Int> {
    val next = latched.toMutableSet()
    if (key.sticky) {
        if (next.contains(key.code)) {
            WaylandNative.nativeSendKey(key.code, 0)
            next.remove(key.code)
        } else {
            WaylandNative.nativeSendKey(key.code, 1)
            next.add(key.code)
        }
    } else {
        WaylandNative.nativeSendKey(key.code, 1)
        WaylandNative.nativeSendKey(key.code, 0)
    }
    return next.toSet()
}

// Full-screen customizer: reorder, delete, add from presets or by hand.
@Composable
fun WaylandExtraKeysDialog(
    initial: List<WaylandExtraKey>,
    onDismiss: () -> Unit,
    onSave: (List<WaylandExtraKey>) -> Unit
) {
    val keys = remember { mutableStateListOf<WaylandExtraKey>().apply { addAll(initial) } }
    var label by remember { mutableStateOf("") }
    var codeText by remember { mutableStateOf("") }
    var sticky by remember { mutableStateOf(false) }
    var presetExpanded by remember { mutableStateOf(false) }

    val code = codeText.toIntOrNull()
    val canAdd = label.isNotBlank() && code != null && code in 1..255

    DsDialog(
        onDismiss = onDismiss,
        scrollableContent = false,
        footer = {
            DialogFooterRow(
                dismissLabel = stringResource(R.string.cancel),
                confirmLabel = stringResource(R.string.wayland_extra_keys_save),
                onDismiss = onDismiss,
                onConfirm = { onSave(keys.toList()) }
            )
        }
    ) {
        Text(
            text = stringResource(R.string.wayland_extra_keys_title),
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.Bold
        )

        LazyColumn(modifier = Modifier.weight(1f, fill = false)) {
            items(keys.size, key = { index -> "$index:${keys[index].label}:${keys[index].code}" }) { index ->
                val key = keys[index]
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(text = key.label, style = MaterialTheme.typography.bodyMedium)
                        Text(
                            text = if (key.sticky) "${key.code} (sticky)" else "${key.code}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                        )
                    }
                    IconButton(
                        onClick = { if (index > 0) keys.add(index - 1, keys.removeAt(index)) },
                        enabled = index > 0
                    ) {
                        Icon(Icons.Default.ArrowUpward, contentDescription = null)
                    }
                    IconButton(
                        onClick = { if (index < keys.size - 1) keys.add(index + 1, keys.removeAt(index)) },
                        enabled = index < keys.size - 1
                    ) {
                        Icon(Icons.Default.ArrowDownward, contentDescription = null)
                    }
                    IconButton(onClick = { keys.removeAt(index) }) {
                        Icon(
                            Icons.Default.Delete,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.error
                        )
                    }
                }
            }
        }

        OutlinedTextField(
            value = label,
            onValueChange = { label = it },
            label = { Text(stringResource(R.string.wayland_extra_key_label)) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedTextField(
                value = codeText,
                onValueChange = { codeText = it.filter(Char::isDigit).take(3) },
                label = { Text(stringResource(R.string.wayland_extra_key_code)) },
                modifier = Modifier.weight(1f),
                singleLine = true
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(checked = sticky, onCheckedChange = { sticky = it })
                Text(
                    text = stringResource(R.string.wayland_extra_key_sticky),
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box {
                OutlinedButton(onClick = { presetExpanded = true }) {
                    Text(stringResource(R.string.wayland_extra_key_preset))
                }
                DropdownMenu(
                    expanded = presetExpanded,
                    onDismissRequest = { presetExpanded = false }
                ) {
                    waylandExtraKeyPresets.forEach { preset ->
                        DropdownMenuItem(
                            text = { Text("${preset.label} (${preset.code})") },
                            onClick = {
                                label = preset.label
                                codeText = "${preset.code}"
                                sticky = preset.sticky
                                presetExpanded = false
                            }
                        )
                    }
                }
            }
            IconButton(onClick = {
                if (canAdd) {
                    keys.add(WaylandExtraKey(label.trim(), code!!, sticky))
                    label = ""
                    codeText = ""
                    sticky = false
                }
            }) {
                Icon(Icons.Default.Add, contentDescription = stringResource(R.string.add))
            }
            TextButton(onClick = {
                keys.clear()
                keys.addAll(defaultWaylandExtraKeys())
            }) {
                Text(stringResource(R.string.wayland_extra_keys_reset))
            }
        }
    }
}

// Bottom dock wrapper: keeps the bar clear of the navigation bar.
@Composable
fun WaylandExtraKeysDock(
    keys: List<WaylandExtraKey>,
    latchedCodes: Set<Int>,
    onTapKey: (WaylandExtraKey) -> Unit,
    onEdit: () -> Unit,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .navigationBarsPadding()
            .padding(horizontal = 12.dp, vertical = 0.dp)
            .padding(bottom = 12.dp)
            .heightIn(max = 64.dp),
        contentAlignment = Alignment.Center
    ) {
        WaylandExtraKeysBar(
            keys = keys,
            latchedCodes = latchedCodes,
            onTapKey = onTapKey,
            onEdit = onEdit
        )
    }
}
