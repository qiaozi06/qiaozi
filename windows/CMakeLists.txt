cmake_minimum_required(VERSION 3.20)
project(crossremote_agent_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg 的 x64-windows-static 依赖用 /MT 编译，本项目也必须用静态运行库，
# 否则链接 ixwebsocket.lib 时报 LNK2038 RuntimeLibrary 不匹配。
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(IXWebSocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(crossremote-agent
  src/main.cpp
  src/capture.cpp
  src/input.cpp
  src/webrtc_peer.cpp
  ../common/cr_e2ee.cpp
)

target_include_directories(crossremote-agent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(crossremote-agent PRIVATE
  ixwebsocket::ixwebsocket
  nlohmann_json::nlohmann_json
  OpenSSL::Crypto
  # vcpkg 的 mbedtls(mbedcrypto) 用 BCryptGenRandom 取随机数
  bcrypt
  d3d11 dxgi
)

# libwebrtc：把官方/自编译产物的路径传进来即可启用 P2P 媒体。
# -DWEBRTC_INCLUDE="C:/webrtc/src;C:/webrtc/src/third_party/libyuv/include"
# -DWEBRTC_LIB="C:/webrtc/src/out/Release/obj/webrtc.lib"
if(WEBRTC_LIB)
  target_compile_definitions(crossremote-agent PRIVATE
    CR_WITH_WEBRTC
    WEBRTC_WIN
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
  )
  target_include_directories(crossremote-agent PRIVATE ${WEBRTC_INCLUDE})
  target_link_libraries(crossremote-agent PRIVATE
    ${WEBRTC_LIB}
    winmm ws2_32 secur32 crypt32 iphlpapi dbghelp
    dmoguids wmcodecdspuuid msdmo strmiids mfuuid shlwapi
    ole32 oleaut32 user32 gdi32 advapi32 normaliz uuid
  )
endif()

target_link_options(crossremote-agent PRIVATE /SUBSYSTEM:CONSOLE)
