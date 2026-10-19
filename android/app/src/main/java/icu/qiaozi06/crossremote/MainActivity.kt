package icu.qiaozi06.crossremote

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    companion object {
        private const val PREFS = "agent"
        private const val KEY_SERVER = "server"
        private const val KEY_CODE = "code"
        private const val KEY_PASSWORD = "password"
        private const val DEFAULT_SERVER = "wss://desk.qiaozi06.icu/ws?role=agent"
    }

    private lateinit var prefs: SharedPreferences
    private lateinit var etServer: EditText
    private lateinit var etCode: EditText
    private lateinit var etPassword: EditText
    private lateinit var tvStatus: TextView

    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        val data = result.data
        if (result.resultCode == RESULT_OK && data != null) {
            val service = Intent(this, ScreenCaptureService::class.java)
                .putExtra(ScreenCaptureService.EXTRA_DATA, data)
            ContextCompat.startForegroundService(this, service)
        } else {
            appendLog("用户取消了屏幕录制授权")
        }
    }

    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) appendLog("未授予通知权限，前台服务通知可能不显示")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        etServer = findViewById(R.id.etServer)
        etCode = findViewById(R.id.etCode)
        etPassword = findViewById(R.id.etPassword)
        tvStatus = findViewById(R.id.tvStatus)

        etServer.setText(prefs.getString(KEY_SERVER, DEFAULT_SERVER))
        etCode.setText(prefs.getString(KEY_CODE, ""))
        etPassword.setText(prefs.getString(KEY_PASSWORD, ""))

        AgentRuntime.setLogSink { line ->
            runOnUiThread { appendLog(line) }
        }

        findViewById<Button>(R.id.btnConnect).setOnClickListener { connect() }
        findViewById<Button>(R.id.btnCapture).setOnClickListener { requestCapture() }
        findViewById<Button>(R.id.btnAccessibility).setOnClickListener {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }
        findViewById<Button>(R.id.btnDisconnect).setOnClickListener {
            AgentRuntime.disconnect()
            appendLog("已断开")
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private fun connect() {
        val server = etServer.text.toString().trim()
        val inputCode = etCode.text.toString().trim()
        val inputPassword = etPassword.text.toString()
        if (server.isEmpty()) {
            appendLog("请填写信令地址")
            return
        }
        prefs.edit().putString(KEY_SERVER, server).apply()
        AgentRuntime.connect(this, server, inputCode, inputPassword) { code, password ->
            runOnUiThread {
                etCode.setText(code)
                prefs.edit().putString(KEY_CODE, code).apply()
                if (password.isNotEmpty()) {
                    etPassword.setText(password)
                    prefs.edit().putString(KEY_PASSWORD, password).apply()
                }
                appendLog("设备码：$code" + if (password.isNotEmpty()) "，密码：$password" else "")
            }
        }
    }

    private fun requestCapture() {
        val manager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projectionLauncher.launch(manager.createScreenCaptureIntent())
    }

    private fun appendLog(line: String) {
        tvStatus.append(line + "\n")
    }
}
