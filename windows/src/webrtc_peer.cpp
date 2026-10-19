#include "webrtc_peer.h"

#include <atomic>
#include <memory>
#include <mutex>

#ifdef CR_WITH_WEBRTC
// 需要 libwebrtc（建议 m120 及以上）头文件与静态库。
#include "api/create_peerconnection_factory.h"
#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "media/base/video_broadcaster.h"
#include "rtc_base/thread.h"
#include "rtc_base/ssl_adapter.h"
#endif

#include "capture.h"

namespace {

#ifdef CR_WITH_WEBRTC

// 把 DXGI 的 BGRA 帧转 I420 并推给 libwebrtc。
// 生产实现建议在 capture.cpp 内直接用 libyuv::ARGBToI420，避免额外拷贝。
class DesktopVideoSource : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
 public:
  void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!sink) return;
    sink_ = sink;
    wants_ = wants;
  }
  void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (sink_ == sink) sink_ = nullptr;
  }

  void OnBgra(const uint8_t* bgra, int w, int h, int stride) {
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink;
    {
      std::lock_guard<std::mutex> lk(mu_);
      sink = sink_;
    }
    if (!sink) return;
    rtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(w, h);
    libyuv::ARGBToI420(bgra, stride, i420->MutableDataY(), i420->StrideY(),
                       i420->MutableDataU(), i420->StrideU(),
                       i420->MutableDataV(), i420->StrideV(), w, h);
    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(i420)
                                   .set_timestamp_us(rtc::TimeMicros())
                                   .build();
    sink->OnFrame(frame);
  }

 private:
  std::mutex mu_;
  rtc::VideoSinkInterface<webrtc::VideoFrame>* sink_ = nullptr;
  rtc::VideoSinkWants wants_;
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
};

class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  using Cb = std::function<void(webrtc::SessionDescriptionInterface*)>;
  explicit CreateSdpObserver(Cb cb) : cb_(std::move(cb)) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override { cb_(desc); }
  void OnFailure(webrtc::RTCError error) override {}
 private:
  Cb cb_;
};

#endif  // CR_WITH_WEBRTC

}  // namespace

Peer::Peer() = default;

Peer::~Peer() { Close(); }

bool Peer::Init(const std::vector<IceServer>& ice, Callbacks cb, std::string* err) {
#ifdef CR_WITH_WEBRTC
  rtc::InitializeSSL();
  auto* impl = new Impl();
  impl->network = rtc::Thread::CreateWithSocketServer();
  impl->worker = rtc::Thread::Create();
  impl->signaling = rtc::Thread::Create();
  impl->network->Start();
  impl->worker->Start();
  impl->signaling->Start();

  impl->factory = webrtc::CreatePeerConnectionFactory(
      impl->network.get(), impl->worker.get(), impl->signaling.get(), nullptr,
      webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(),
      nullptr, nullptr);

  webrtc::PeerConnectionInterface::RTCConfiguration cfg;
  cfg.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  for (const auto& s : ice) {
    webrtc::PeerConnectionInterface::IceServer server;
    for (const auto& u : s.urls) server.urls.push_back(u);
    server.username = s.username;
    server.password = s.credential;
    cfg.servers.push_back(server);
  }

  webrtc::PeerConnectionDependencies deps(nullptr);
  deps.observer = nullptr;  // 生产实现：自定义 PeerConnectionObserver 转发 ICE/状态/DC
  auto result = impl->factory->CreatePeerConnectionOrError(cfg, std::move(deps));
  if (!result.ok()) {
    if (err) *err = result.error().message();
    delete impl;
    return false;
  }
  impl->pc = result.value();

  impl->source = std::make_unique<DesktopVideoSource>();
  impl->track = impl->factory->CreateVideoTrack("screen", impl->source.get());
  impl->pc->AddTrack(impl->track, {"stream0"});

  webrtc::DataChannelInit dci;
  dci.ordered = true;
  auto dc = impl->pc->CreateDataChannelOrError("control-channel", &dci);
  if (!dc.ok()) {
    if (err) *err = dc.error().message();
    delete impl;
    return false;
  }
  impl->dc = dc.value();
  // 注册 dc->RegisterObserver(...)，OnMessage 时回调 cb.on_binary。
  // 注册 pc->RegisterObserver(...)，OnIceCandidate 时序列化 candidate 后回调 cb.on_local_ice。

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
  (void)fps;
  (void)max_bitrate_kbps;
  impl->capture = std::make_unique<ScreenCapture>();
  std::string err;
  impl->capture->Start(monitor_index,
                       [src = impl->source.get()](const uint8_t* p, int w, int h, int stride) {
                         src->OnBgra(p, w, h, stride);
                       },
                       &err);
#else
  (void)monitor_index; (void)fps; (void)max_bitrate_kbps;
#endif
}

bool Peer::CreateOffer(std::string* sdp, std::string* err) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl || !impl->pc) { if (err) *err = "peer 未初始化"; return false; }
  auto observer = rtc::make_ref_counted<CreateSdpObserver>(
      [impl, sdp](webrtc::SessionDescriptionInterface* desc) {
        std::string out;
        desc->ToString(&out);
        *sdp = out;
        // SetLocalDescription 的重载在不同 libwebrtc 版本间有差异：
        // m120 之前可用 (desc, nullptr)，之后建议改用 SetLocalDescriptionObserverInterface。
        impl->pc->SetLocalDescription(
            std::unique_ptr<webrtc::SessionDescriptionInterface>(desc), nullptr);
      });
  impl->pc->CreateOffer(observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
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
  auto* desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &perr);
  if (desc) impl->pc->SetRemoteDescription(desc);
#else
  (void)sdp;
#endif
}

void Peer::AddRemoteIce(const std::string& candidate_json, const std::string& mid, int mline) {
#ifdef CR_WITH_WEBRTC
  auto* impl = static_cast<Impl*>(impl_);
  if (!impl || !impl->pc) return;
  webrtc::SdpParseError perr;
  auto cand = webrtc::CreateIceCandidate(mid, mline, candidate_json, &perr);
  if (cand) impl->pc->AddIceCandidate(cand.get());
#else
  (void)candidate_json; (void)mid; (void)mline;
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
  if (impl->dc) impl->dc->Close();
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
