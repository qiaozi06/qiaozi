package icu.qiaozi06.crossremote

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.Looper
import android.util.DisplayMetrics
import android.view.WindowManager
import org.json.JSONArray
import org.json.JSONObject
import org.webrtc.DataChannel
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpReceiver
import org.webrtc.ScreenCapturerAndroid
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer

/**
 * 被控端会话：信令注册 + WebRTC 屏幕共享 + DataChannel 指令。
 *
 * E2EE 可选：控制端在 DataChannel open 后先发 KEY 帧，本端派生密钥并回发公钥；
 * 未协商成功时保持明文帧，互通不受影响。
 */
class AgentSession(
    private val app: Context,
    private val serverUrl: String,
    private val codeInput: String,
    private val passwordInput: String,
    private val onRegistered: (code: String, password: String) -> Unit,
    private val log: (String) -> Unit,
) : SignalingClient.Listener {

    private val handler = Handler(Looper.getMainLooper())
    private var signaling: SignalingClient? = null
    private var stopped = false
    private var retries = 0

    private var sessionId = ""
    private var iceServers: List<PeerConnection.IceServer> = emptyList()

    private var eglBase: EglBase? = null
    private var factory: PeerConnectionFactory? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var capturer: ScreenCapturerAndroid? = null
    private var surfaceHelper: SurfaceTextureHelper? = null
    private var peer: PeerConnection? = null
    private var dataChannel: DataChannel? = null
    private val e2ee = CrE2ee()

    private var projectionIntent: Intent? = null
    private val fileHandles = HashMap<String, RandomAccessFile>()

    private val heartbeat = object : Runnable {
        override fun run() {
            if (stopped) return
            signaling?.send("heartbeat", JSONObject())
            handler.postDelayed(this, 20_000)
        }
    }

    fun connect() {
        stopped = false
        val client = SignalingClient(serverUrl, this)
        signaling = client
        client.connect()
    }

    fun disconnect() {
        stopped = true
        handler.removeCallbacks(heartbeat)
        signaling?.send("session_close", JSONObject().put("session_id", sessionId).put("reason", "agent shutdown"))
        signaling?.close()
        closePeer()
        for ((_, raf) in fileHandles) {
            runCatching { raf.close() }
        }
        fileHandles.clear()
    }

    /** 用户完成 MediaProjection 授权后调用。 */
    fun attachProjection(intent: Intent) {
        projectionIntent = intent
        log("已获得屏幕录制授权")
        if (peer != null) startCaptureIfNeeded()
    }

    // ---------------- 信令 ----------------

    override fun onOpen() {
        log("信令已连接：$serverUrl")
        retries = 0
        val screen = screenSize()
        val payload = JSONObject()
            .put("device_code", codeInput)
            .put("password", passwordInput)
            .put("os_type", 3)
            .put("device_name", "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}")
            .put("version", "0.1.0")
            .put("screen_resolution", "${screen.first}x${screen.second}")
            .put("e2ee", true)
        signaling?.send("register", payload)
    }

    override fun onMessage(type: String, payload: JSONObject) {
        when (type) {
            "register_ok" -> {
                val code = payload.optString("device_code")
                val pw = payload.optString("password")
                log("注册成功，设备码 $code${if (pw.isNotEmpty()) "，临时密码 $pw" else ""}")
                onRegistered(code, pw)
                parseIceServers(payload.optJSONArray("ice_servers"))
                handler.removeCallbacks(heartbeat)
                handler.post(heartbeat)
            }
            "heartbeat_ack" -> Unit
            "call" -> {
                sessionId = payload.optString("session_id")
                log("收到呼叫：$sessionId，自动接受")
                closePeer()
                signaling?.send(
                    "call_resp",
                    JSONObject().put("session_id", sessionId).put("accept", true)
                )
                handler.postDelayed({ startPeer() }, 200)
            }
            "sdp_answer" -> {
                val sdp = payload.optString("sdp")
                if (sdp.isNotEmpty()) {
                    peer?.setRemoteDescription(SimpleSdpObserver("answer"), SessionDescription(
                        SessionDescription.Type.ANSWER, sdp
                    ))
                }
            }
            "ice_candidate" -> {
                val c = payload.optJSONObject("candidate")
                val candidate = c?.optString("candidate").orEmpty()
                if (candidate.isNotEmpty()) {
                    peer?.addIceCandidate(
                        IceCandidate(
                            c.optString("sdpMid", payload.optString("sdpMid")),
                            c.optInt("sdpMLineIndex", payload.optInt("sdpMLineIndex")),
                            candidate
                        )
                    )
                }
            }
            "session_close" -> {
                log("会话结束：${payload.optString("reason")}")
                closePeer()
            }
            "error" -> log("服务器错误：${payload.optInt("code")} ${payload.optString("msg")}")
        }
    }

    override fun onClosed(reason: String) {
        log(reason)
        if (!stopped) scheduleReconnect()
    }

    override fun onFailure(error: String) {
        log(error)
        if (!stopped) scheduleReconnect()
    }

    private fun scheduleReconnect() {
        if (retries >= 5) {
            log("重连次数已达上限，请手动重新连接")
            return
        }
        retries++
        log("${3 * retries} 秒后重连（$retries/5）")
        handler.postDelayed({
            if (!stopped) {
                closePeer()
                signaling?.close()
                connect()
            }
        }, 3000L * retries)
    }

    private fun parseIceServers(array: JSONArray?) {
        val list = ArrayList<PeerConnection.IceServer>()
        if (array == null) {
            log("未收到 ice_servers，将仅使用主机候选")
            iceServers = list
            return
        }
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            val urls = item.optJSONArray("urls") ?: continue
            for (j in 0 until urls.length()) {
                val url = urls.optString(j)
                if (url.isEmpty()) continue
                val builder = PeerConnection.IceServer.builder(url)
                val user = item.optString("username")
                val cred = item.optString("credential")
                if (user.isNotEmpty()) builder.setUsername(user)
                if (cred.isNotEmpty()) builder.setPassword(cred)
                list.add(builder.createIceServer())
            }
        }
        iceServers = list
        log("ICE 服务器 ${list.size} 条")
    }

    // ---------------- WebRTC ----------------

    private fun startPeer() {
        if (!e2ee.init()) log("E2EE 初始化失败，将回退明文帧")
        try {
            val ctx = app.applicationContext
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(ctx)
                    .setEnableInternalTracer(false)
                    .createInitializationOptions()
            )
            val egl = EglBase.create()
            eglBase = egl
            val pf = PeerConnectionFactory.builder()
                .setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true))
                .setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext))
                .createPeerConnectionFactory()
            factory = pf

            val config = PeerConnection.RTCConfiguration(iceServers).apply {
                sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
                bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
                continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
            }
            val pc = pf.createPeerConnection(config, peerObserver)
                ?: throw IllegalStateException("createPeerConnection 返回 null")
            peer = pc

            val (w, h) = screenSize()
            val source = pf.createVideoSource(true)
            videoSource = source
            val track = pf.createVideoTrack("screen", source)
            videoTrack = track
            pc.addTrack(track, listOf("stream0"))

            if (projectionIntent != null) {
                startCaptureIfNeeded()
            } else {
                log("尚未授权屏幕共享，先建立控制通道（无视频）")
            }

            val dcInit = DataChannel.Init()
            dcInit.ordered = true
            val dc = pc.createDataChannel("control-channel", dcInit)
            dataChannel = dc
            dc.registerObserver(dataChannelObserver)

            pc.createOffer(object : SimpleSdpObserver("offer") {
                override fun onCreateSuccess(sdp: SessionDescription) {
                    pc.setLocalDescription(object : SimpleSdpObserver("local-offer") {
                        override fun onSetSuccess() {
                            signaling?.send(
                                "sdp_offer",
                                JSONObject()
                                    .put("session_id", sessionId)
                                    .put("sdp", sdp.description)
                                    .put("type", "offer")
                            )
                            sendScreenInfo(w, h)
                        }
                    }, sdp)
                }
            }, MediaConstraints())
        } catch (e: Exception) {
            log("建立 WebRTC 失败：${e.message}")
        }
    }

    private fun startCaptureIfNeeded() {
        val intent = projectionIntent ?: return
        if (capturer != null) return
        val pf = factory ?: return
        val source = videoSource ?: return
        try {
            val egl = eglBase ?: return
            val helper = SurfaceTextureHelper.create("CrossRemoteCapture", egl.eglBaseContext)
            surfaceHelper = helper
            val cap = ScreenCapturerAndroid(intent, object : MediaProjection.Callback() {
                override fun onStop() {
                    log("系统已停止屏幕录制")
                }
            })
            capturer = cap
            val (w, h) = screenSize()
            cap.initialize(helper, app.applicationContext, source.capturerObserver)
            cap.startCapture(w, h, 30)
            log("屏幕共享已开始 ${w}x$h@30")
        } catch (e: Exception) {
            log("启动屏幕捕获失败：${e.message}")
        }
    }

    private val peerObserver = object : PeerConnection.Observer {
        override fun onSignalingChange(newState: PeerConnection.SignalingState) = Unit
        override fun onIceConnectionChange(newState: PeerConnection.IceConnectionState) {
            log("ICE 状态：$newState")
        }
        override fun onIceConnectionReceivingChange(receiving: Boolean) = Unit
        override fun onIceGatheringChange(newState: PeerConnection.IceGatheringState) = Unit
        override fun onIceCandidate(candidate: IceCandidate) {
            val c = JSONObject()
                .put("candidate", candidate.sdp)
                .put("sdpMid", candidate.sdpMid)
                .put("sdpMLineIndex", candidate.sdpMLineIndex)
            signaling?.send(
                "ice_candidate",
                JSONObject()
                    .put("session_id", sessionId)
                    .put("candidate", c)
                    .put("sdpMid", candidate.sdpMid)
                    .put("sdpMLineIndex", candidate.sdpMLineIndex)
            )
        }
        override fun onIceCandidatesRemoved(candidates: Array<out IceCandidate>) = Unit
        override fun onAddStream(stream: MediaStream) = Unit
        override fun onRemoveStream(stream: MediaStream) = Unit
        override fun onDataChannel(channel: DataChannel) = Unit
        override fun onRenegotiationNeeded() = Unit
        override fun onAddTrack(receiver: RtpReceiver, mediaStreams: Array<out MediaStream>) = Unit
    }

    private val dataChannelObserver = object : DataChannel.Observer {
        override fun onBufferedAmountChange(previousAmount: Long) = Unit
        override fun onStateChange() {
            log("控制通道状态：${dataChannel?.state()}")
        }
        override fun onMessage(buffer: DataChannel.Buffer) {
            val bytes = ByteArray(buffer.data.remaining())
            buffer.data.get(bytes)
            onControlFrame(bytes)
        }
    }

    private fun onControlFrame(frameBytes: ByteArray) {
        if (frameBytes.isEmpty()) return
        if (frameBytes[0].toInt() and 0xFF == Proto.KEY) {
            // [8][65B 未压缩 P-256 公钥]：派生密钥后立即回自己的公钥帧。
            if (frameBytes.size == 66 && e2ee.acceptPeerKey(frameBytes.copyOfRange(1, 66))) {
                sendRaw(e2ee.keyFrame())
            }
            return
        }
        val (type, body) = if (frameBytes[0].toInt() and 0xFF == Proto.ENC) {
            e2ee.decrypt(frameBytes) ?: return
        } else {
            Proto.split(frameBytes) ?: return
        }
        when (type) {
            Proto.INPUT -> {
                val ev = Proto.decodeInput(body) ?: return
                val service = InputService.instance
                if (service == null) {
                    log("未启用无障碍服务，忽略输入事件")
                } else {
                    service.handle(ev)
                }
            }
            Proto.CLIPBOARD -> {
                val text = Proto.Reader(body).let { r ->
                    var s = ""
                    while (!r.eof()) {
                        val t = r.varint()
                        val field = (t ushr 3).toInt()
                        val wire = (t and 7).toInt()
                        if (field == 1 && wire == 2) s = r.string() else r.skip(wire)
                    }
                    s
                }
                val cm = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("CrossRemote", text))
                log("剪贴板已更新（${text.length} 字符）")
            }
            Proto.CTRL -> {
                val ctrl = Proto.decodeCtrl(body)
                when (ctrl.cmd) {
                    Proto.CTRL_CLOSE -> {
                        signaling?.send(
                            "session_close",
                            JSONObject().put("session_id", sessionId).put("reason", "agent closed")
                        )
                        closePeer()
                    }
                    else -> log("收到控制指令 ${ctrl.cmd}（Android 暂未实现）")
                }
            }
            Proto.FILE -> saveFileChunk(Proto.decodeFile(body))
            Proto.META -> Unit
            else -> Unit
        }
    }

    private fun saveFileChunk(chunk: Proto.FileChunk) {
        try {
            val dir = File(app.getExternalFilesDir(null), "received").apply { mkdirs() }
            val safeName = File(chunk.fileName.ifEmpty { "unnamed.bin" }).name
            val raf = fileHandles.getOrPut(chunk.fileId) {
                RandomAccessFile(File(dir, safeName), "rw")
            }
            raf.seek(chunk.chunkIndex.toLong() * 64 * 1024)
            raf.write(chunk.chunkData)
            if (chunk.isFinish) {
                raf.close()
                fileHandles.remove(chunk.fileId)
                log("文件接收完成：${File(dir, safeName).absolutePath}")
            }
        } catch (e: Exception) {
            log("保存文件失败：${e.message}")
        }
    }

    private fun sendScreenInfo(width: Int, height: Int) {
        val data = JSONObject()
            .put("action", "screen_info")
            .put("width", width)
            .put("height", height)
            .put(
                "monitors",
                JSONArray().put(JSONObject().put("index", 0).put("width", width).put("height", height))
            )
        signaling?.send(
            "screen_info",
            JSONObject().put("session_id", sessionId).put("data", data)
        )
        dataChannel?.let { dc ->
            if (dc.state() == DataChannel.State.OPEN) {
                sendFrame(Proto.META, data.toString().toByteArray(Charsets.UTF_8))
            }
        }
    }

    private fun sendRaw(bytes: ByteArray) {
        val dc = dataChannel ?: return
        if (bytes.isEmpty() || dc.state() != DataChannel.State.OPEN) return
        dc.send(DataChannel.Buffer(ByteBuffer.wrap(bytes), false))
    }

    // 已协商 E2EE 时发加密帧，否则回退明文帧。
    private fun sendFrame(type: Int, body: ByteArray) {
        val enc = if (e2ee.ready) e2ee.encrypt(type, body) else null
        sendRaw(enc ?: Proto.frame(type, body))
    }

    private fun screenSize(): Pair<Int, Int> {
        val wm = app.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        wm.defaultDisplay.getRealMetrics(metrics)
        return Pair(metrics.widthPixels, metrics.heightPixels)
    }

    private fun closePeer() {
        dataChannel?.let {
            it.unregisterObserver()
            it.close()
        }
        dataChannel = null
        peer?.let {
            it.close()
            it.dispose()
        }
        peer = null
        capturer?.let {
            runCatching { it.stopCapture() }
            it.dispose()
        }
        capturer = null
        surfaceHelper?.dispose()
        surfaceHelper = null
        videoTrack?.dispose()
        videoTrack = null
        videoSource?.dispose()
        videoSource = null
        factory?.dispose()
        factory = null
        eglBase?.release()
        eglBase = null
    }

    private open class SimpleSdpObserver(private val tag: String) : SdpObserver {
        override fun onCreateSuccess(sdp: SessionDescription) = Unit
        override fun onSetSuccess() = Unit
        override fun onCreateFailure(error: String) {
            AgentRuntime.log("$tag createFailure: $error")
        }
        override fun onSetFailure(error: String) {
            AgentRuntime.log("$tag setFailure: $error")
        }
    }
}
