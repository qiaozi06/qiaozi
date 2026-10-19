# CrossRemote Android 被控端（源码，未编译验证）

本目录是 Android 被控端源码。与 Windows/macOS 端一样，**当前服务器是 Linux 环境，没有 Android SDK，
未执行过 Gradle 编译与真机运行**，请按“未验证源码”对待。

## 能力

- WSS 信令：`wss://<域名>/ws?role=agent`，注册 / 心跳 / 呼叫 / SDP / ICE 流程与 Web 控制端一致
- 屏幕共享：`MediaProjection` + `ScreenCapturerAndroid` → WebRTC 视频轨
- 控制通道：DataChannel `control-channel`，业务帧与 `web/pb.js`、`agents/common/cr_proto.h` 字节级兼容
  - 输入：点按 / 长按 / 滚动（Android 无鼠标悬停，MOVE 被忽略）
  - 剪贴板：写入系统剪贴板
  - 文件：控制端下发的文件分块保存到 `getExternalFilesDir("received")`
- 前台服务保活 + 断线自动重连（最多 5 次）

## 目录

```
android/
├── settings.gradle.kts
├── build.gradle.kts
├── gradle.properties
├── app/
│   ├── build.gradle.kts
│   ├── proguard-rules.pro
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── java/icu/qiaozi06/crossremote/
│       │   ├── MainActivity.kt         # 配置、授权入口
│       │   ├── AgentRuntime.kt         # 进程级会话单例
│       │   ├── AgentSession.kt         # 信令状态机 + WebRTC
│       │   ├── SignalingClient.kt      # OkHttp WebSocket
│       │   ├── Proto.kt                # 业务协议编解码
│       │   ├── CrE2ee.kt               # E2EE（ECDH P-256 + AES-256-GCM）
│       │   ├── ScreenCaptureService.kt # 前台服务
│       │   └── InputService.kt         # 无障碍输入注入
│       └── res/
│           ├── layout/activity_main.xml
│           ├── values/strings.xml
│           └── xml/accessibility_service_config.xml
└── README.md
```

## 构建

前置：

- JDK 17
- Android SDK Platform 34 + Build-Tools
- Gradle 8.9+（仓库未附带 wrapper，可执行 `gradle wrapper --gradle-version 8.9` 生成）

```bash
cd agents/android
gradle wrapper --gradle-version 8.9   # 或直接用 Android Studio 打开本目录
./gradlew assembleDebug
# 产物 app/build/outputs/apk/debug/app-debug.apk
```

依赖说明：

- WebRTC 使用 `org.webrtc:google-webrtc:1.0.32006`（Maven Central 上最后的官方发布，API 较旧）。
  若需新版，可替换为 `io.github.webrtc-sdk:android` 或自行编译 libwebrtc，并适配
  `ScreenCapturerAndroid` / `PeerConnectionFactory` 的构造方式。
- 若网络拉取 Gradle 依赖缓慢，可在 `settings.gradle.kts` 的仓库列表前加国内镜像。

## 使用

1. 安装 APK，填写信令地址（默认 `wss://desk.qiaozi06.icu/ws?role=agent`）。
2. 设备码/密码留空则服务器自动生成，注册成功后界面会回填并保存到 SharedPreferences。
3. 点“开启屏幕共享”，在系统弹窗中允许录屏；前台服务会保持会话存活。
4. 点“开启无障碍输入服务”，在系统设置中打开 CrossRemote 的无障碍开关（用于注入手势）。
5. 在 Web 控制端输入设备码与密码连接。

## 已知限制（重要）

- **未编译、未真机验证**：本源码仅做了协议与 API 层面的静态编写。
- **E2EE 已实现但未真机验证**：与 Web/Windows/macOS 使用同一套 `[8]`/`[9]` 帧格式，
  未协商成功时自动回退明文；`CrE2ee.kt` 仅通过代码审查，未在设备上实测。
- **无音频**：只共享屏幕视频。
- **鼠标悬停不可注入**：Android 普通应用没有系统级鼠标指针；MOVE 事件被忽略，
  左键 = 点按、右键 = 长按，滚轮 = 纵向滑动。
- **键盘注入需要 root**：非 root 时仅 Home/返回/最近任务通过 `performGlobalAction` 生效；
  生产级方案需系统签名应用 `InputManager.injectInputEvent` 或 uinput 虚拟输入设备。
- **单显示器**：`SWITCH_MONITOR` / 黑屏指令暂未实现。
- 文件保存目录为应用私有外部目录，卸载应用会一并删除。
