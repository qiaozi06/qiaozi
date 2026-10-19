#pragma once
// WebRTC 会话封装。真正建链依赖 libwebrtc，编译时通过 -DCR_WITH_WEBRTC 与
// WEBRTC_LIB 引入（见 README）。未启用时仅做信令联调，不产生媒体流。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cr_proto.h"

struct IceServer {
  std::vector<std::string> urls;
  std::string username;
  std::string credential;
};

class Peer {
 public:
  struct Callbacks {
    std::function<void(const std::vector<uint8_t>&)> on_binary;   // DataChannel 收到二进制
    std::function<void(const std::string&)> on_local_sdp;         // 本地 offer
    std::function<void(const std::string&)> on_local_ice;         // 本地 ICE（candidate JSON）
    std::function<void()> on_data_open;
    std::function<void()> on_closed;
  };

  Peer();
  ~Peer();

  bool Init(const std::vector<IceServer>& ice, Callbacks cb, std::string* err);
  void StartScreen(int monitor_index, int fps, int max_bitrate_kbps);
  bool CreateOffer(std::string* sdp, std::string* err);
  void SetRemoteAnswer(const std::string& sdp);
  void AddRemoteIce(const std::string& candidate_json, const std::string& mid, int mline);
  void SendBinary(const std::vector<uint8_t>& data);
  void Close();
  bool data_open() const { return data_open_; }

 private:
  void* impl_ = nullptr;
  bool data_open_ = false;
};
