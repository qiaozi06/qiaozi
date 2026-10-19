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
  d3d11 dxgi
)

# libwebrtc：把官方预编译库的路径传进来即可启用 P2P 媒体。
# cmake -DWEBRTC_LIB="C:/webrtc/lib/webrtc.lib" -DWEBRTC_INCLUDE="C:/webrtc/include" ..
if(WEBRTC_LIB)
  target_compile_definitions(crossremote-agent PRIVATE CR_WITH_WEBRTC)
  target_include_directories(crossremote-agent PRIVATE ${WEBRTC_INCLUDE})
  target_link_libraries(crossremote-agent PRIVATE ${WEBRTC_LIB} libyuv)
endif()

target_link_options(crossremote-agent PRIVATE /SUBSYSTEM:CONSOLE)
