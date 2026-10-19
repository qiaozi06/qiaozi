package icu.qiaozi06.crossremote

import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * 进程级单例：Activity / Service 之间共享当前 AgentSession。
 */
object AgentRuntime {
    private const val TAG = "CrossRemote"

    @Volatile
    private var session: AgentSession? = null

    @Volatile
    private var logSink: ((String) -> Unit)? = null

    fun setLogSink(sink: ((String) -> Unit)?) {
        logSink = sink
    }

    fun log(message: String) {
        Log.i(TAG, message)
        logSink?.invoke(message)
    }

    fun connect(
        context: Context,
        serverUrl: String,
        code: String,
        password: String,
        onRegistered: (code: String, password: String) -> Unit,
    ) {
        disconnect()
        session = AgentSession(
            context.applicationContext,
            serverUrl,
            code,
            password,
            onRegistered,
            ::log,
        ).also { it.connect() }
    }

    fun attachProjection(intent: Intent) {
        val s = session
        if (s == null) {
            log("尚未连接信令，请先连接后重新授权屏幕共享")
        } else {
            s.attachProjection(intent)
        }
    }

    fun disconnect() {
        session?.disconnect()
        session = null
    }

    fun isConnected(): Boolean = session != null
}
