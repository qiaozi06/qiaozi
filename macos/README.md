# CrossRemote macOS 被控端（Objective-C++）

## 能力
- `NSURLSessionWebSocketTask` 接入 WSS 信令，断线 3 秒后自动重连
- `CGDisplayStream` 抓主屏，`RTCVideoSource` 推送 H.264
- DataChannel `control-channel`：`CGEventPost` 注入鼠标/键盘，`NSPasteboard` 同步文本
- 设备码/密码保存到 `~/Library/Application Support/CrossRemote/agent.json`
- 文件分片写入 `~/Downloads/`

## 依赖
- macOS 11+，Xcode Command Line Tools
- `WebRTC.framework`（GoogleWebRTC，可 `pod 'GoogleWebRTC'` 或从 libwebrtc 构建产物获取）
- OpenSSL（`brew install openssl`），用于 E2EE；Makefile 默认取 `brew --prefix openssl`

## 构建
```bash
cd agents/macos
# 方式一：framework 放在 /Library/Frameworks
make WEBRTC_DIR=/Library/Frameworks WEBRTC_FRAMEWORK=WebRTC

# 方式二：指定绝对目录
make WEBRTC_DIR=$HOME/webrtc/out/mac/Release
make bundle   # 生成 CrossRemote.app（ad-hoc 签名）
```

## 运行与授权
1. 启动 `CrossRemote.app`（或 `./crossremote-agent`）
2. 首次运行需在「系统设置 → 隐私与安全性 → 屏幕录制」勾选本应用并重启
3. 在「辅助功能」中勾选本应用，否则 CGEventPost 注入无效
4. 控制台会打印 9 位设备码与密码；用 `https://desk.qiaozi06.icu` 连接

配置文件示例：
```json
{
  "server": "wss://desk.qiaozi06.icu/ws?role=agent",
  "device_code": "123456789",
  "password": "abcd1234",
  "device_name": "MacBook-Pro",
  "auto_accept": true
}
```

## 已知限制
- `CGDisplayStream` 在 macOS 15 已标记废弃，后续建议迁移 ScreenCaptureKit
- 键码映射表为常用键子集（`CGCodeForVK`），特殊键需补表
- E2EE 已接入（`../common/cr_e2ee.cpp`，与控制端 `[8]`/`[9]` 帧格式一致）；未协商成功时回退明文，媒体仍由 DTLS-SRTP 保护。
  本机为 Linux，未用 Xcode 编译验证 E2EE 接入。
- 本次补齐了连接建立后的 `register` 发送（此前仅处理 `register_ok`，不会注册），
  并在注册负载中上报 `"e2ee": true`；同样未经编译验证。
- 未做公证（notarization），分发给其他机器需开发者证书签名
- 本仓库在 Linux 上无法编译验证
