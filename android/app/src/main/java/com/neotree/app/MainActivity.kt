package com.neotree.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.neotree.app.ui.MainScreen
import com.neotree.app.ui.theme.NeoTreeTheme

class MainActivity : ComponentActivity() {
    private val vm: TreeViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            NeoTreeTheme {
                MainScreen(vm)
            }
        }
    }

    // Only hold a connection while the app is on screen: the tree has 4
    // client slots shared by the whole family's phones.
    override fun onStart() {
        super.onStart()
        vm.onForeground()
    }

    override fun onStop() {
        super.onStop()
        vm.onBackground()
    }
}
