#pragma once
// DXGI Desktop Duplication 抓屏，输出 BGRA。
#include <cstdint>
#include <functional>
#include <string>

class ScreenCapture {
 public:
  using FrameCb = std::function<void(const uint8_t* bgra, int width, int height, int stride)>;

  ScreenCapture();
  ~ScreenCapture();

  // monitor_index: 0 为主显示器；返回 false 时 err 带原因。
  bool Start(int monitor_index, FrameCb cb, std::string* err);
  void Stop();
  bool running() const { return running_; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  void Loop();

  bool running_ = false;
  int width_ = 0;
  int height_ = 0;
  int monitor_index_ = 0;
  FrameCb cb_;
  void* impl_ = nullptr;  // D3D11 资源，定义在 .cpp，避免头文件污染
};
