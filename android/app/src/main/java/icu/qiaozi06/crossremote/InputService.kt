package icu.qiaozi06.crossremote

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.view.accessibility.AccessibilityEvent
import java.io.IOException

/**
 * Android 输入注入。
 *
 * Android 没有给普通应用提供"鼠标指针移动"能力，因此：
 * - MOVE 事件被忽略（触摸屏没有悬停概念）；
 * - BUTTON 按下事件转换为点按，右键转换为长按；
 * - KEY 事件优先用 root 的 `input keyevent`，没有 root 时只映射 Home/返回/最近任务。
 *
 * 生产级实现应由系统签名应用通过 InputManager.injectInputEvent 或 uinput 虚拟设备完成。
 */
class InputService : AccessibilityService() {

    companion object {
        @Volatile
        var instance: InputService? = null
            private set

        /** 32 位 Windows VK -> Android KeyEvent KEYCODE_* */
        private val VK_TO_ANDROID = mapOf(
            0x0D to 66,  // Enter
            0x1B to 4,   // Esc -> BACK
            0x08 to 67,  // Backspace
            0x09 to 61,  // Tab
            0x20 to 62,  // Space
            0x25 to 21,  // Left
            0x26 to 19,  // Up
            0x27 to 22,  // Right
            0x28 to 20,  // Down
            0x2E to 112, // Delete -> FORWARD_DEL
            0x24 to 122, // Home
            0x23 to 123, // End -> MOVE_END
            0x21 to 92,  // PageUp
            0x22 to 93,  // PageDown
        )

        private fun vkToAndroid(vk: Int): Int? {
            if (vk in 0x30..0x39) return 7 + (vk - 0x30)          // 0-9
            if (vk in 0x41..0x5A) return 29 + (vk - 0x41)         // A-Z
            return VK_TO_ANDROID[vk]
        }
    }

    private var lastX = 0f
    private var lastY = 0f
    private var hasPoint = false

    override fun onServiceConnected() {
        super.onServiceConnected()
        instance = this
        AgentRuntime.log("无障碍输入服务已连接")
    }

    override fun onDestroy() {
        instance = null
        super.onDestroy()
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit

    override fun onInterrupt() = Unit

    fun handle(event: Proto.InputEvent) {
        when (event.kind) {
            Proto.InputEvent.KIND_MOVE -> {
                lastX = event.x.toFloat()
                lastY = event.y.toFloat()
                hasPoint = true
            }
            Proto.InputEvent.KIND_BUTTON -> {
                lastX = event.x.toFloat()
                lastY = event.y.toFloat()
                hasPoint = true
                if (event.pressed) {
                    when (event.button) {
                        1 -> tap(event.x.toFloat(), event.y.toFloat(), 600) // 右键 -> 长按
                        0 -> tap(event.x.toFloat(), event.y.toFloat(), 60)  // 左键 -> 点按
                    }
                }
            }
            Proto.InputEvent.KIND_WHEEL -> {
                if (hasPoint) {
                    val dy = if (event.deltaY > 0) -240f else 240f
                    swipe(lastX, lastY, lastX, lastY + dy, 180)
                }
            }
            Proto.InputEvent.KIND_KEY -> {
                if (!event.pressed) return
                val code = vkToAndroid(event.keycode)
                when (event.keycode) {
                    0x24 -> performGlobalAction(GLOBAL_ACTION_HOME)
                    0x1B -> performGlobalAction(GLOBAL_ACTION_BACK)
                    0x09 -> performGlobalAction(GLOBAL_ACTION_RECENTS)
                    else -> {
                        if (code != null && !sendKeyEvent(code)) {
                            AgentRuntime.log("键盘注入需要 root：VK=${event.keycode}")
                        }
                    }
                }
            }
        }
    }

    private fun tap(x: Float, y: Float, durationMs: Long) {
        val path = Path().apply { moveTo(x, y) }
        val gesture = GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(path, 0, durationMs))
            .build()
        if (!dispatchGesture(gesture, null, null)) {
            AgentRuntime.log("手势注入失败（服务未就绪）")
        }
    }

    private fun swipe(x1: Float, y1: Float, x2: Float, y2: Float, durationMs: Long) {
        val path = Path().apply {
            moveTo(x1, y1)
            lineTo(x2, y2)
        }
        val gesture = GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(path, 0, durationMs))
            .build()
        dispatchGesture(gesture, null, null)
    }

    private fun sendKeyEvent(androidKeyCode: Int): Boolean {
        return try {
            val process = Runtime.getRuntime().exec(
                arrayOf("su", "-c", "input keyevent $androidKeyCode")
            )
            process.waitFor() == 0
        } catch (e: IOException) {
            false
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        } catch (e: SecurityException) {
            false
        }
    }
}
