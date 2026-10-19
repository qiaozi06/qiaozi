#include "webrtc_peer.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef CR_WITH_WEBRTC
// libwebrtc m120 头文件与静态库；由 CMake 的 WEBRTC_INCLUDE / WEBRTC_LIB 提供。
#include "api/create_peerconnection_factory.h"
#include "api/data_channel_interface.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "media/base/adapted_video_track_source.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "libyuv.h"
#endif

#include "capture.h"

namespace {

#ifdef CR_WITH_WEBRTC

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xff);
      out += buf;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// DXGI 的 BGRA 帧转 I420 并推进 libwebrtc 的视频源。
class DesktopVideoSource : public rtc::AdaptedVideoTrackSource {
 public:
  void OnBgra(const uint8_t* bgra, int w, int h, int stride) {
    if (!bgra || w <= 0 || h <= 0 || stride <= 0) return;
    rtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(w, h);
    if (libyuv::ARGBToI420(bgra, stride, i420->MutableDataY(), i420->StrideY(),
                           i420->MutableDataU(), i420->StrideU(),
                           i420->MutableDataV(), i420->StrideV(), w, h) != 0) {
      return;
    }
    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(i420)
                                   .set_timestamp_us(rtc::TimeMicros())
                                   .build();
    OnFrame(frame);
  }

  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return true; }
};

struct Impl;

class PeerConnObserver : public webrtc::PeerConnectionObserver {
 public:
  explicit PeerConnObserver(Impl* impl) : impl_(impl) {}

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> dc) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

 private:
  Impl* impl_;
};

class DcObserver : public webrtc::DataChannelObserver {
 public:
  explicit DcObserver(Impl* impl) : impl_(impl) {}

  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;

 private:
  Impl* impl_;
};

struct Impl {
  std::unique_ptr<rtc::Thread> network;
  std::unique_ptr<rtc::Thread> worker;
  std::unique_ptr<rtc::Thread> signaling;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  rtc::scoped_refptr<webrtc::DataChannelInterface> dc;
  rtc::scoped_refptr<webrtc::VideoTrackInterface> track;
  std::unique_ptr<DesktopVideoSource> source;
  std::unique_ptr<ScreenCapture> capture;
  std::unique_ptr<PeerConnObserver> pc_observer;
  std::unique_ptr<DcObserver> dc_observer;
  Peer::Callbacks cb;
  bool data_opened = false;

  void OnLocalIce(const std::string& candidate, const std::string& mid, int mline) {
    if (!cb.on_local_ice) return;
    std::string json = "{\"candidate\":\"" + JsonEscape(candidate) +
                       "\",\"sdpMid\":\"" + JsonEscape(mid) +
                       "\",\"sdpMLineIndex\":" + std::to_string(mline) + "}";
    cb.on_local_ice(json);
  }

  void OnDcMessage(const std::vector<uint8_t>& data) {
    if (cb.on_binary) cb.on_binary(data);
  }

  void OnDcStateChanged() {
    if (!dc) return;
    if (dc->state() == webrtc::DataChannelInterface::kOpen && !data_opened) {
      data_opened = true;
      if (cb.on_data_open) cb.on_data_open();
    } else if (dc->state() == webrtc::DataChannelInterface::kClosed) {
      if (data_opened && cb.on_closed) cb.on_closed();
      data_opened = false;
    }
  }
};

void PeerConnObserver::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  if (!candidate) return;
  std::string sdp;
  if (!candidate->ToString(&sdp)) return;
  impl_->OnLocalIce(sdp, candidate->sdp_mid(), candidate->sdp_mline_index());
}

void PeerConnObserver::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> dc) {
  if (!dc) return;
  impl_->dc = dc;
  if (!impl_->dc_observer) impl_->dc_observer = std::make_unique<DcObserver>(impl_);
  impl_->dc->RegisterObserver(impl_->dc_observer.get());
  impl_->OnDcStateChanged();
}

void PeerConnObserver::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
      new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kClosed) {
    if (impl_->cb.on_closed) impl_->cb.on_closed();
  }
}

void DcObserver::OnMessage(const webrtc::DataBuffer& buffer) {
  const uint8_t* p = buffer.data.data();
  impl_->OnDcMessage(std::vector<uint8_t>(p, p + buffer.data.size()));
}

void DcObserver::OnStateChange() { impl_->OnDcStateChanged(); }

class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  using Cb = std::function<void(webrtc::SessionDescriptionInterface*)>;
  explicit CreateSdpObserver(Cb cb) : cb_(std::move(cb)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    if (cb_) cb_(desc);
  }
  void OnFailure(webrtc::RTCError error) override {
    std::fprintf(stderr, "CreateOffer 失败: %s\n", error.message());
  }

 private:
  Cb cb_;
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) {
      std::fprintf(stderr, "SetLocalDescription 失败: %s\n", error.message());
    }
  }
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) {
      std::fprintf(stderr, "SetRemoteDescription 失败: %s\n", error.message());
    }
  }
};

#endif  // CR_WITH_WEBRTC

}  // namespace

Peer::Peer() = default;

Peer::~Peer() { Close(); }

bool Peer::Init(const std::vector<IceServer>& ice, Callbacks cb, std::string* err) {
#ifdef CR_WITH_WEBRTC
  if (!rtc::InitializeSSL()) {
    if (err) *err = "InitializeSSL 失败";
    return false;
  }

  auto* impl = new Impl();
  impl->cb = std::move(cb);
  impl->network = rtc::Thread::CreateWithSocketServer();
  impl->worker = rtc::Thread::Create();
  impl->signaling = rtc::Thread::Create();
  if (!impl->network->Start() || !impl->worker->Start() || !impl->signaling->Start()) {
    if (err) *err = "WebRTC 线程启动失败";
    rtc::CleanupSSL();
    delete impl;
    return false;
  }

  impl->factory = webrtc::CreatePeerConnectionFactory(
      impl->network.get(), impl->worker.get(), impl->signaling.get());
  if (!impl->factory) {
    if (err) *err = "CreatePeerConnectionFactory 失败";
    impl->signaling->Stop();
    impl->worker->Stop();
    impl->network->Stop();
    rtc::CleanupSSL();
    delete impl;
    return false;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration cfg;
  cfg.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  for (const auto& s : ice) {
    webrtc::PeerConnectionInterface::IceServer server;
    for (const auto& u : s.urls) server.urls.push_back(u);
    server.username = s.username;
    server.password = s.credential;
    cfg.servers.push_back(server);
  }

  impl->pc_observer = std::make_unique<PeerConnObserver>(impl);
  webrtc::PeerConnectionDependencies deps(impl->pc_observer.get());
  auto result = impl->factory->CreatePeerConnectionOrError(cfg, std::move(deps));
  if (!result.ok()) {
    if (err) *err = result.error().message();
    impl->signaling->Stop();
    impl->worker->Stop();
    impl->network->Stop();
    rtc::CleanupSSL();
    delete impl;
    return false;
  }
  impl->pc = result.value();

  impl->source = std::make_unique<DesktopVideoSource>();
  impl->track = impl->factory->CreateVideoTrack("screen", impl->source.get());
  auto add = impl->pc->AddTrack(impl->track, {"stream0"});
  if (!add.ok()) {
    if (err) *err = add.error().message();
    impl->pc->Close();
    impl->signaling->Stop();
    impl->worker->Stop();
    impl->network->Stop();
    rtc::CleanupSSL();
    delete impl;
    return false;
  }

  webrtc::DataChannelInit dci;
  dci.ordered = true;
  auto dc = impl->pc->CreateDataChannelOrError("control-channel", &dci);
  if (!dc.ok()) {
    if (err) *err = dc.error().message();
    impl->pc->Close();
    impl->signaling->Stop();
    impl->worker->Stop();
    impl->network->Stop();
    rtc::CleanupSSL();
    delete impl;
    return false;
  }
  impl->dc = dc.value();
  impl->dc_observer = std::make_unique<DcObserver>(impl);
  impl->dc->RegisterObserver(impl->dc_observer.get());

  impl_ = impl;
  return true;
#else
  (void)ice;
  (void)cb;
  if (err) *err = "未启用 libwebrtc（编译需 -DCR_WITH_WEBRTC 并链接 WEBRTC_LIB），当前为信令联调模式";
  return false;
#endif
}

void Peer::StartScreen(int monitor_index, int fps, int max_bitrate_kbps) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl || !impl->source) return;
  (void)max_bitrate_kbps;
  impl->capture = std::make_unique<ScreenCapture>();
  std::string err;
  impl->capture->Start(
      monitor_index,
      [src = impl->source.get()](const uint8_t* p, int w, int h, int stride) {
        src->OnBgra(p, w, h, stride);
      },
      &err);
  if (!err.empty()) std::fprintf(stderr, "抓屏启动失败: %s\n", err.c_str());
  (void)fps;
#else
  (void)monitor_index;
  (void)fps;
  (void)max_bitrate_kbps;
#endif
}

bool Peer::CreateOffer(std::string* sdp, std::string* err) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl || !impl->pc) {
    if (err) *err = "peer 未初始化";
    return false;
  }
  auto observer = rtc::make_ref_counted<CreateSdpObserver>(
      [impl, sdp](webrtc::SessionDescriptionInterface* desc) {
        if (!desc) return;
        std::string out;
        if (!desc->ToString(&out)) return;
        *sdp = out;
        impl->pc->SetLocalDescription(
            std::unique_ptr<webrtc::SessionDescriptionInterface>(desc),
            rtc::make_ref_counted<SetLocalObserver>());
      });
  impl->pc->CreateOffer(observer.get(),
                        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  return true;
#else
  if (err) *err = "未启用 libwebrtc";
  (void)sdp;
  return false;
#endif
}

void Peer::SetRemoteAnswer(const std::string& sdp) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl || !impl->pc) return;
  webrtc::SdpParseError perr;
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc(
      webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &perr));
  if (!desc) {
    std::fprintf(stderr, "解析 answer 失败: %s\n", perr.description.c_str());
    return;
  }
  impl->pc->SetRemoteDescription(std::move(desc),
                                 rtc::make_ref_counted<SetRemoteObserver>());
#else
  (void)sdp;
#endif
}

void Peer::AddRemoteIce(const std::string& candidate_json, const std::string& mid,
                        int mline) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl || !impl->pc) return;
  webrtc::SdpParseError perr;
  std::unique_ptr<webrtc::IceCandidateInterface> cand(
      webrtc::CreateIceCandidate(mid, mline, candidate_json, &perr));
  if (!cand) {
    std::fprintf(stderr, "解析 ICE 失败: %s\n", perr.description.c_str());
    return;
  }
  impl->pc->AddIceCandidate(cand.get());
#else
  (void)candidate_json;
  (void)mid;
  (void)mline;
#endif
}

void Peer::SendBinary(const std::vector<uint8_t>& data) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (impl && impl->dc && impl->dc->state() == webrtc::DataChannelInterface::kOpen) {
    webrtc::DataBuffer buf(rtc::CopyOnWriteBuffer(data.data(), data.size()), true);
    impl->dc->Send(buf);
  }
#else
  (void)data;
#endif
}

void Peer::Close() {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl) return;
  if (impl->capture) impl->capture->Stop();
  if (impl->dc) {
    if (impl->dc_observer) impl->dc->UnregisterObserver();
    impl->dc->Close();
  }
  if (impl->pc) impl->pc->Close();
  impl->signaling->Stop();
  impl->worker->Stop();
  impl->network->Stop();
  rtc::CleanupSSL();
  delete impl;
#endif
  impl_ = nullptr;
  data_open_ = false;
}
