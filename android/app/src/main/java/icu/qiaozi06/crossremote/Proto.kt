package icu.qiaozi06.crossremote

import java.io.ByteArrayOutputStream

/**
 * DataChannel 业务协议，与 web/pb.js、agents/common/cr_proto.h 字节级兼容。
 *
 * 帧格式：[1B frame_type][payload]
 * 1=InputEvent 2=ClipboardMsg 3=FileTransferMsg 4=SessionCtrlMsg
 * 5=Meta(JSON) 8=ECDH 公钥 9=加密帧
 */
object Proto {
    const val INPUT = 1
    const val CLIPBOARD = 2
    const val FILE = 3
    const val CTRL = 4
    const val META = 5
    const val KEY = 8
    const val ENC = 9

    const val CTRL_CLOSE = 0
    const val CTRL_SWITCH_MONITOR = 1
    const val CTRL_BLACK_ON = 2
    const val CTRL_BLACK_OFF = 3

    class Writer {
        private val out = ByteArrayOutputStream()

        fun varint(value: Long) {
            var v = value
            while ((v and 0x7FL.inv()) != 0L) {
                out.write(((v and 0x7F) or 0x80).toInt())
                v = v ushr 7
            }
            out.write((v and 0x7F).toInt())
        }

        fun tag(field: Int, wire: Int) = varint(((field shl 3) or wire).toLong())

        fun int32(field: Int, value: Int) {
            tag(field, 0)
            varint(value.toLong())
        }

        fun bool(field: Int, value: Boolean) {
            tag(field, 0)
            varint(if (value) 1L else 0L)
        }

        fun bytes(field: Int, value: ByteArray) {
            tag(field, 2)
            varint(value.size.toLong())
            out.write(value)
        }

        fun string(field: Int, value: String) = bytes(field, value.toByteArray(Charsets.UTF_8))

        fun toByteArray(): ByteArray = out.toByteArray()
    }

    class Reader(private val buf: ByteArray, private var pos: Int = 0, private val end: Int = buf.size) {
        fun eof() = pos >= end

        fun varint(): Long {
            var result = 0L
            var shift = 0
            while (pos < end) {
                val b = buf[pos++].toInt() and 0xFF
                result = result or ((b and 0x7F).toLong() shl shift)
                if (b and 0x80 == 0) return result
                shift += 7
            }
            return result
        }

        fun int32(): Int = varint().toInt()
        fun u32(): Int = varint().toInt()
        fun bool(): Boolean = varint() != 0L

        fun bytes(): ByteArray {
            val n = varint().toInt().coerceIn(0, end - pos)
            val out = buf.copyOfRange(pos, pos + n)
            pos += n
            return out
        }

        fun string(): String = String(bytes(), Charsets.UTF_8)

        fun skip(wire: Int) {
            when (wire) {
                0 -> varint()
                1 -> pos += 8
                2 -> pos += varint().toInt()
                5 -> pos += 4
                else -> pos = end
            }
            if (pos > end) pos = end
        }
    }

    data class InputEvent(
        val kind: Int,
        val x: Int = 0,
        val y: Int = 0,
        val button: Int = 0,
        val pressed: Boolean = false,
        val deltaY: Int = 0,
        val keycode: Int = 0,
    ) {
        companion object {
            const val KIND_NONE = 0
            const val KIND_MOVE = 1
            const val KIND_BUTTON = 2
            const val KIND_WHEEL = 3
            const val KIND_KEY = 4
        }
    }

    data class CtrlMsg(val cmd: Int, val monitorIndex: Int = 0)

    data class FileChunk(
        val fileId: String,
        val fileName: String,
        val totalSize: Long,
        val chunkIndex: Int,
        val chunkData: ByteArray,
        val isFinish: Boolean,
    )

    fun decodeInput(payload: ByteArray): InputEvent? {
        val r = Reader(payload)
        while (!r.eof()) {
            val t = r.varint()
            val field = (t ushr 3).toInt()
            val wire = (t and 7).toInt()
            if (wire != 2) {
                r.skip(wire)
                continue
            }
            val sub = r.bytes()
            when (field) {
                1 -> {
                    var x = 0
                    var y = 0
                    val s = Reader(sub)
                    while (!s.eof()) {
                        val st = s.varint()
                        val f = (st ushr 3).toInt()
                        val w = (st and 7).toInt()
                        when {
                            f == 1 && w == 0 -> x = s.int32()
                            f == 2 && w == 0 -> y = s.int32()
                            else -> s.skip(w)
                        }
                    }
                    return InputEvent(InputEvent.KIND_MOVE, x = x, y = y)
                }
                2 -> {
                    var x = 0
                    var y = 0
                    var button = 0
                    var pressed = false
                    val s = Reader(sub)
                    while (!s.eof()) {
                        val st = s.varint()
                        val f = (st ushr 3).toInt()
                        val w = (st and 7).toInt()
                        when {
                            f == 1 && w == 0 -> x = s.int32()
                            f == 2 && w == 0 -> y = s.int32()
                            f == 3 && w == 0 -> button = s.int32()
                            f == 4 && w == 0 -> pressed = s.bool()
                            else -> s.skip(w)
                        }
                    }
                    return InputEvent(InputEvent.KIND_BUTTON, x = x, y = y, button = button, pressed = pressed)
                }
                3 -> {
                    var delta = 0
                    val s = Reader(sub)
                    while (!s.eof()) {
                        val st = s.varint()
                        val f = (st ushr 3).toInt()
                        val w = (st and 7).toInt()
                        if (f == 1 && w == 0) delta = s.int32() else s.skip(w)
                    }
                    return InputEvent(InputEvent.KIND_WHEEL, deltaY = delta)
                }
                4 -> {
                    var keycode = 0
                    var pressed = false
                    val s = Reader(sub)
                    while (!s.eof()) {
                        val st = s.varint()
                        val f = (st ushr 3).toInt()
                        val w = (st and 7).toInt()
                        when {
                            f == 1 && w == 0 -> keycode = s.u32()
                            f == 2 && w == 0 -> pressed = s.bool()
                            else -> s.skip(w)
                        }
                    }
                    return InputEvent(InputEvent.KIND_KEY, keycode = keycode, pressed = pressed)
                }
            }
        }
        return null
    }

    fun encodeMove(x: Int, y: Int): ByteArray = frame(INPUT, Writer().apply {
        val m = Writer()
        m.int32(1, x)
        m.int32(2, y)
        bytes(1, m.toByteArray())
    }.toByteArray())

    fun encodeButton(x: Int, y: Int, button: Int, pressed: Boolean): ByteArray = frame(INPUT, Writer().apply {
        val m = Writer()
        m.int32(1, x)
        m.int32(2, y)
        m.int32(3, button)
        m.bool(4, pressed)
        bytes(2, m.toByteArray())
    }.toByteArray())

    fun encodeWheel(deltaY: Int): ByteArray = frame(INPUT, Writer().apply {
        val m = Writer()
        m.int32(1, deltaY)
        bytes(3, m.toByteArray())
    }.toByteArray())

    fun encodeKey(vk: Int, pressed: Boolean): ByteArray = frame(INPUT, Writer().apply {
        val m = Writer()
        m.int32(1, vk)
        m.bool(2, pressed)
        bytes(4, m.toByteArray())
    }.toByteArray())

    fun encodeClipboard(text: String): ByteArray = frame(CLIPBOARD, Writer().apply {
        string(1, text)
    }.toByteArray())

    fun encodeCtrl(cmd: Int, monitorIndex: Int = 0): ByteArray = frame(CTRL, Writer().apply {
        int32(1, cmd)
        int32(2, monitorIndex)
    }.toByteArray())

    fun encodeFile(chunk: FileChunk): ByteArray = frame(FILE, Writer().apply {
        string(1, chunk.fileId)
        string(2, chunk.fileName)
        int32(3, chunk.totalSize.toInt())
        int32(4, chunk.chunkIndex)
        bytes(5, chunk.chunkData)
        bool(6, chunk.isFinish)
    }.toByteArray())

    fun decodeCtrl(payload: ByteArray): CtrlMsg {
        var cmd = 0
        var monitor = 0
        val r = Reader(payload)
        while (!r.eof()) {
            val t = r.varint()
            val field = (t ushr 3).toInt()
            val wire = (t and 7).toInt()
            when {
                field == 1 && wire == 0 -> cmd = r.int32()
                field == 2 && wire == 0 -> monitor = r.int32()
                else -> r.skip(wire)
            }
        }
        return CtrlMsg(cmd, monitor)
    }

    fun decodeFile(payload: ByteArray): FileChunk {
        var id = ""
        var name = ""
        var total = 0L
        var index = 0
        var data = ByteArray(0)
        var finish = false
        val r = Reader(payload)
        while (!r.eof()) {
            val t = r.varint()
            val field = (t ushr 3).toInt()
            val wire = (t and 7).toInt()
            when (wire) {
                0 -> when (field) {
                    3 -> total = r.varint()
                    4 -> index = r.int32()
                    6 -> finish = r.bool()
                    else -> r.skip(wire)
                }
                2 -> when (field) {
                    1 -> id = r.string()
                    2 -> name = r.string()
                    5 -> data = r.bytes()
                    else -> r.skip(wire)
                }
                else -> r.skip(wire)
            }
        }
        return FileChunk(id, name, total, index, data, finish)
    }

    fun frame(type: Int, payload: ByteArray): ByteArray {
        val out = ByteArray(payload.size + 1)
        out[0] = type.toByte()
        System.arraycopy(payload, 0, out, 1, payload.size)
        return out
    }

    fun metaFrame(json: String): ByteArray = frame(META, json.toByteArray(Charsets.UTF_8))

    /** 拆出 [type, body]；空帧返回 null。 */
    fun split(frame: ByteArray): Pair<Int, ByteArray>? {
        if (frame.isEmpty()) return null
        return Pair(frame[0].toInt() and 0xFF, frame.copyOfRange(1, frame.size))
    }
}
