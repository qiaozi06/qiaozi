# CrossRemote Windows 被控端（C++17）

## 能力
- WSS 接入 `desk.qiaozi06.icu` 信令，注册设备码后自动上报心跳
- DXGI Desktop Duplication 抓屏（BGRA → I420）经 WebRTC 推送
- DataChannel `control-channel` 接收键鼠/剪贴板/文件，`SendInput` 注入
- 设备码与密码保存在 `%APPDATA%\CrossRemote\agent.json`
- 断线自动重连（IXWebSocket）

## 依赖
| 组件 | 用途 | 获取 |
| --- | --- | --- |
| Visual Studio 2022 (MSVC x64) | 编译 | — |
| vcpkg | ixwebsocket / nlohmann-json | `vcpkg install ixwebsocket nlohmann-json` |
| libwebrtc | 媒体与 DataChannel | 官方 `depot_tools` 构建，或自备预编译 `webrtc.lib` + 头文件 |
| libyuv | BGRA→I420 | libwebrtc 构建产物或单独编译 |
| OpenSSL 1.1.1+ | E2EE（ECDH P-256 + AES-256-GCM） | `vcpkg install openssl` |

> 未提供 `libwebrtc` 时仍可编译出“信令联调模式”程序：能注册、能收发信令，但不产生视频流。

## 构建
```bat
git clone <repo> && cd agents\windows
cmake -B build -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DWEBRTC_INCLUDE=C:\webrtc\include ^
  -DWEBRTC_LIB=C:\webrtc\lib\webrtc.lib
cmake --build build --config Release
```

产物：`build\Release\crossremote-agent.exe`

## 运行
```bat
crossremote-agent.exe
```
首次运行会在 `%APPDATA%\CrossRemote\agent.json` 生成配置并注册设备码，控制台会打印 9 位设备码与密码；
用 Web 控制端 `https://desk.qiaozi06.icu` 输入即可连接。可手工编辑 `agent.json`：

```json
{
  "server": "wss://desk.qiaozi06.icu/ws",
  "device_code": "123456789",
  "password": "abcd1234",
  "device_name": "OFFICE-PC",
  "auto_accept": true,
  "fps": 30,
  "bitrate_kbps": 4000
}
```

## 关键实现位置
- `src/main.cpp`：信令状态机（register / call / sdp / ice / session_close）、DataChannel 分发、文件落盘
- `src/webrtc_peer.cpp`：PeerConnection 与 DataChannel（`CR_WITH_WEBRTC` 宏控制）
- `src/capture.cpp`：DXGI 抓屏，锁屏/UAC/分辨率切换需重建 duplication（当前退避重试）
- `src/input.cpp`：绝对坐标 `SendInput`，支持多显示器虚拟桌面
- `../common/cr_proto.h`：与 `web/pb.js` 字节级一致的业务帧编解码

## 已知限制
- E2EE 已接入（`../common/cr_e2ee.cpp`，与控制端 `[8]`/`[9]` 帧格式一致）；未协商成功时回退明文帧，媒体仍由 DTLS-SRTP 保护。
  本机为 Linux，未用 MSVC 编译验证 E2EE 接入。
- 剪贴板仅文本；文件保存到 `%TEMP%`，未做路径/NUL 清洗
- 未签名 exe 会被 SmartScreen 拦截；正式分发需代码签名
- 本仓库在 Linux 服务器上无法编译验证，需在 Windows 环境按上述步骤构建
