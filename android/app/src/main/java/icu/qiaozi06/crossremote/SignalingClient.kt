package icu.qiaozi06.crossremote

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * 信令 WebSocket 客户端。只负责收发 JSON，业务状态机在 AgentSession。
 */
class SignalingClient(
    private val url: String,
    private val listener: Listener,
) {
    interface Listener {
        fun onOpen()
        fun onMessage(type: String, payload: JSONObject)
        fun onClosed(reason: String)
        fun onFailure(error: String)
    }

    private val http = OkHttpClient.Builder()
        .pingInterval(20, TimeUnit.SECONDS)
        .build()

    @Volatile
    private var socket: WebSocket? = null

    @Volatile
    var closed = false
        private set

    fun connect() {
        closed = false
        val request = Request.Builder().url(url).build()
        socket = http.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                listener.onOpen()
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                try {
                    val json = JSONObject(text)
                    listener.onMessage(
                        json.optString("msg_type"),
                        json.optJSONObject("payload") ?: JSONObject(),
                    )
                } catch (e: Exception) {
                    listener.onFailure("信令消息解析失败: ${e.message}")
                }
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(1000, null)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                closed = true
                listener.onClosed("连接关闭($code) $reason")
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                closed = true
                listener.onFailure("连接失败: ${t.message ?: t.javaClass.simpleName}")
            }
        })
    }

    fun send(type: String, payload: JSONObject): Boolean {
        val ws = socket ?: return false
        val msg = JSONObject()
            .put("msg_type", type)
            .put("payload", payload)
        return ws.send(msg.toString())
    }

    fun close() {
        closed = true
        socket?.close(1000, "agent shutdown")
        socket = null
    }
}
