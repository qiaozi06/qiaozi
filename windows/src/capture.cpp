#include "capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

struct CaptureImpl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGIOutputDuplication> dup;
  ComPtr<ID3D11Texture2D> staging;
  UINT width = 0;
  UINT height = 0;
};

DXGI_FORMAT PickFormat() { return DXGI_FORMAT_B8G8R8A8_UNORM; }

}  // namespace

ScreenCapture::ScreenCapture() = default;
ScreenCapture::~ScreenCapture() { Stop(); }

bool ScreenCapture::Start(int monitor_index, FrameCb cb, std::string* err) {
  if (running_) return true;
  monitor_index_ = monitor_index;
  cb_ = std::move(cb);

  auto* impl = new CaptureImpl();
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                 D3D11_SDK_VERSION, impl->device.GetAddressOf(), nullptr,
                                 impl->ctx.GetAddressOf());
  if (FAILED(hr)) {
    if (err) *err = "D3D11CreateDevice failed: " + std::to_string(hr);
    delete impl;
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIOutput> output;
  impl->device.As(&dxgi_device);
  dxgi_device->GetAdapter(adapter.GetAddressOf());
  if (FAILED(adapter->EnumOutputs(static_cast<UINT>(monitor_index), output.GetAddressOf()))) {
    if (err) *err = "EnumOutputs failed (monitor_index=" + std::to_string(monitor_index) + ")";
    delete impl;
    return false;
  }

  ComPtr<IDXGIOutput1> output1;
  output.As(&output1);
  hr = output1->DuplicateOutput(impl->device.Get(), impl->dup.GetAddressOf());
  if (FAILED(hr)) {
    if (err) *err = "DuplicateOutput failed: " + std::to_string(hr) + "（部分系统需在无独占全屏时运行）";
    delete impl;
    return false;
  }

  DXGI_OUTDUPL_DESC desc{};
  impl->dup->GetDesc(&desc);
  impl->width = desc.ModeDesc.Width;
  impl->height = desc.ModeDesc.Height;
  width_ = static_cast<int>(impl->width);
  height_ = static_cast<int>(impl->height);

  D3D11_TEXTURE2D_DESC td{};
  td.Width = impl->width;
  td.Height = impl->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = PickFormat();
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = impl->device->CreateTexture2D(&td, nullptr, impl->staging.GetAddressOf());
  if (FAILED(hr)) {
    if (err) *err = "CreateTexture2D(staging) failed: " + std::to_string(hr);
    delete impl;
    return false;
  }

  impl_ = impl;
  running_ = true;
  std::thread([this] { Loop(); }).detach();
  return true;
}

void ScreenCapture::Loop() {
  auto* impl = static_cast<CaptureImpl*>(impl_);
  while (running_) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = impl->dup->AcquireNextFrame(50, &info, resource.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
    if (FAILED(hr)) {
      // 分辨率切换、锁屏、UAC 切换时需重建；这里退避等待，由外部重启抓屏。
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(resource.As(&tex))) {
      impl->ctx->CopyResource(impl->staging.Get(), tex.Get());
      D3D11_MAPPED_SUBRESOURCE map{};
      if (SUCCEEDED(impl->ctx->Map(impl->staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        if (cb_) {
          cb_(static_cast<const uint8_t*>(map.pData), static_cast<int>(impl->width),
              static_cast<int>(impl->height), static_cast<int>(map.RowPitch));
        }
        impl->ctx->Unmap(impl->staging.Get(), 0);
      }
    }
    impl->dup->ReleaseFrame();
  }
}

void ScreenCapture::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  auto* impl = static_cast<CaptureImpl*>(impl_);
  delete impl;
  impl_ = nullptr;
}
