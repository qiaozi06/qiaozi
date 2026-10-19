# CrossRemote / kkdesk 原生被控端

远程桌面被控端源码，三端共用同一套业务协议与 E2EE 实现。

| 目录 | 平台 | 技术栈 | 构建方式 |
| --- | --- | --- | --- |
| `android/` | Android 8.0+ | Kotlin + WebRTC SDK 144 | Gradle 8.9 / JDK 17 |
| `windows/` | Windows 10/11 x64 | C++17 + DXGI + IXWebSocket | CMake + MSVC 2022 |
| `macos/` | macOS 11+ | Objective-C++ + WebRTC.framework | make + Xcode CLT |
| `common/` | 三端共用 | C++ 协议与 E2EE | — |

服务端：`wss://desk.qiaozi06.icu/ws?role=agent`，Web 控制端 `https://desk.qiaozi06.icu`。

## 已构建产物

- Android：`kkdesk-android-0.1.0-debug.apk`（debug 签名，未真机验证）
- Windows/macOS：由 `.github/workflows/build-clients.yml` 在 GitHub Actions 构建，产物在 workflow 的 Artifacts 中

## 本地构建

### Android

```bash
cd android
# 需要 JDK 17、Android SDK platform 34 / build-tools 34.0.0
gradle assembleDebug
# 产物 app/build/outputs/apk/debug/app-debug.apk
```

### Windows

```bat
vcpkg install ixwebsocket:x64-windows nlohmann-json:x64-windows openssl:x64-windows
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

> 未提供 `webrtc.lib` 时为**信令联调模式**：可注册设备、收发信令，但不产生屏幕视频流。
> 启用完整媒体需再加 `-DWEBRTC_INCLUDE=... -DWEBRTC_LIB=...`。

### macOS

```bash
cd macos
make bundle \
  WEBRTC_DIR=/path/to/WebRTC.xcframework/macos-arm64_x86_64 \
  OPENSSL_PREFIX=$(brew --prefix openssl@3)
```

## CI

`.github/workflows/build-clients.yml` 包含三个 job：`android`、`windows`、`macos`，手动触发或推送 `main` 分支时执行。

macOS job 使用 `webrtc-sdk` 预编译的 `WebRTC.xcframework` 144.7559.15；Windows job 使用 vcpkg 静态依赖构建信令联调版。

## 已知限制

- Android APK 仅完成编译/打包/签名验证，未在真机或模拟器运行
- Windows 暂无公开可用的预编译 `libwebrtc`，CI 产物无媒体流
- macOS 产物为 ad-hoc 签名，未公证；首次运行需授予屏幕录制与辅助功能权限
- 三端均未做代码签名证书分发
