package com.neotree.app.ui

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.neotree.app.TreeViewModel

private const val TAB_HOME = 0
private const val TAB_MODES = 1
private const val TAB_RENDERER = 2
private const val TAB_DEBUG = 3

/** Four pages - Home (the colors), Modes (scenes), Renderer (3D view) and Debug - switched from a bottom bar. */
@Composable
fun AppRoot(vm: TreeViewModel) {
    var tab by rememberSaveable { mutableIntStateOf(TAB_HOME) }
    // The debug and modes pages poll the tree's status only while showing.
    LaunchedEffect(tab) {
        vm.setDebugVisible(tab == TAB_DEBUG)
        vm.setModesVisible(tab == TAB_MODES)
    }
    Scaffold(
        modifier = Modifier.fillMaxSize(),
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == TAB_HOME,
                    onClick = { tab = TAB_HOME },
                    icon = { Icon(Icons.Filled.Home, contentDescription = null) },
                    label = { Text("Home") },
                )
                NavigationBarItem(
                    selected = tab == TAB_MODES,
                    onClick = { tab = TAB_MODES },
                    icon = { Icon(Icons.Filled.PlayArrow, contentDescription = null) },
                    label = { Text("Modes") },
                )
                NavigationBarItem(
                    selected = tab == TAB_RENDERER,
                    onClick = { tab = TAB_RENDERER },
                    icon = { Icon(Icons.Filled.Star, contentDescription = null) },
                    label = { Text("Renderer") },
                )
                NavigationBarItem(
                    selected = tab == TAB_DEBUG,
                    onClick = { tab = TAB_DEBUG },
                    icon = { Icon(Icons.Filled.Build, contentDescription = null) },
                    label = { Text("Debug") },
                )
            }
        },
    ) { padding ->
        when (tab) {
            TAB_HOME -> HomeScreen(vm, padding, onOpenDebug = { tab = TAB_DEBUG })
            TAB_MODES -> ModesScreen(vm, padding)
            TAB_RENDERER -> RendererScreen(vm, padding)
            else -> DebugScreen(vm, padding)
        }
    }
}
