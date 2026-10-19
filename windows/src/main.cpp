// CrossRemote Windows 被控端
// - WSS 信令（与 server/protocol.go 对齐）
// - DXGI 抓屏 + WebRTC 推流（需 libwebrtc）
// - DataChannel 键鼠注入 / 剪贴板 / 文件
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "cr_e2ee.h"
#include "cr_proto.h"
#include "capture.h"
#include "input.h"
#include "webrtc_peer.h"

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

struct Config {
  std::string server = "wss://desk.qiaozi06.icu/ws";
  std::string device_code;
  std::string password;
  std::string device_name;
  bool auto_accept = true;
  int fps = 30;
  int bitrate_kbps = 4000;
};

std::string ConfigPath() {
  const char* appdata = getenv("APPDATA");
  std::string dir = appdata ? std::string(appdata) + "\\CrossRemote" : ".";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\agent.json";
}

Config LoadConfig() {
  Config c;
  char host[256] = {0};
  DWORD n = sizeof(host);
  if (GetComputerNameA(host, &n)) c.device_name = host;
  std::ifstream in(ConfigPath());
  if (in) {
    try {
      json j;
      in >> j;
      c.server = j.value("server", c.server);
      c.device_code = j.value("device_code", "");
      c.password = j.value("password", "");
      c.device_name = j.value("device_name", c.device_name);
      c.auto_accept = j.value("auto_accept", true);
      c.fps = j.value("fps", 30);
      c.bitrate_kbps = j.value("bitrate_kbps", 4000);
    } catch (...) {
    }
  }
  return c;
}

void SaveConfig(const Config& c) {
  json j = {{"server", c.server},       {"device_code", c.device_code},
            {"password", c.password},   {"device_name", c.device_name},
            {"auto_accept", c.auto_accept}, {"fps", c.fps},
            {"bitrate_kbps", c.bitrate_kbps}};
  std::ofstream out(ConfigPath());
  out << j.dump(2);
}

std::string JsonDump(const json& j) { return j.dump(); }

}  // namespace

class Agent {
 public:
  explicit Agent(Config cfg) : cfg_(std::move(cfg)) {}

  void Run() {
    ws_.setUrl(cfg_.server + "?role=agent");
    ws_.setPingInterval(20);
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) { OnWs(msg); });
    ws_.start();
    // 主线程保持存活，WebSocket 自带重连。
    while (true) std::this_thread::sleep_for(1s);
  }

 private:
  void Send(const std::string& type, const json& payload) {
    json j = {{"msg_type", type}, {"payload", payload}};
    std::lock_guard<std::mutex> lk(send_mu_);
    ws_.send(JsonDump(j));
  }

  void OnWs(const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      json reg = {{"device_code", cfg_.device_code},
                  {"password", cfg_.password},
                  {"os_type", 1},
                  {"device_name", cfg_.device_name},
                  {"version", "1.0.0"},
                  {"screen_resolution", Resolution()},
                  {"e2ee", true}};
      Send("register", reg);
      running_ = true;
    } else if (msg->type == ix::WebSocketMessageType::Close) {
      running_ = false;
    } else if (msg->type == ix::WebSocketMessageType::Message) {
      try {
        OnSignal(json::parse(msg->str));
      } catch (const std::exception& e) {
        fprintf(stderr, "bad signal: %s\n", e.what());
      }
    }
  }

  void OnSignal(const json& j) {
    std::string type = j.value("msg_type", "");
    json p = j.value("payload", json::object());
    if (type == "register_ok") {
      cfg_.device_code = p.value("device_code", cfg_.device_code);
      if (p.contains("password")) cfg_.password = p.value("password", cfg_.password);
      SaveConfig(cfg_);
      ice_.clear();
      for (const auto& s : p.value("ice_servers", json::array())) {
        IceServer is;
        for (const auto& u : s.value("urls", json::array())) is.urls.push_back(u.get<std::string>());
        is.username = s.value("username", "");
        is.credential = s.value("credential", "");
        ice_.push_back(is);
      }
      printf("已注册，设备码 %s，密码 %s\n", cfg_.device_code.c_str(), cfg_.password.c_str());
    } else if (type == "call") {
      session_id_ = p.value("session_id", "");
      if (!cfg_.auto_accept) {
        Send("call_resp", {{"session_id", session_id_}, {"accept", false}});
        return;
      }
      Send("call_resp", {{"session_id", session_id_}, {"accept", true}});
      StartSession();
    } else if (type == "sdp_answer") {
      if (peer_) peer_->SetRemoteAnswer(p.value("sdp", ""));
    } else if (type == "ice_candidate") {
      if (peer_ && p.contains("candidate")) {
        const auto& c = p["candidate"];
        std::string cand = c.is_string() ? c.get<std::string>() : c.value("candidate", "");
        std::string mid = c.is_object() ? c.value("sdpMid", p.value("sdpMid", ""))
                                        : p.value("sdpMid", "");
        int mline = c.is_object() ? c.value("sdpMLineIndex", p.value("sdpMLineIndex", 0))
                                  : p.value("sdpMLineIndex", 0);
        if (!cand.empty()) peer_->AddRemoteIce(cand, mid, mline);
      }
    } else if (type == "session_close") {
      EndSession(p.value("reason", "对端结束会话"));
    } else if (type == "heartbeat_ack") {
      // no-op
    } else if (type == "error") {
      fprintf(stderr, "服务端错误: %s\n", p.dump().c_str());
    }
  }

  void StartSession() {
    EndSession("重建会话");
    peer_ = std::make_unique<Peer>();
    // 每个会话重新生成 ECDH 密钥对；控制端在 DataChannel open 后先发 KEY 帧。
    if (!e2ee_.Init()) fprintf(stderr, "E2EE 初始化失败，将回退明文帧\n");
    Peer::Callbacks cb;
    cb.on_binary = [this](const std::vector<uint8_t>& d) { OnData(d); };
    cb.on_local_ice = [this](const std::string& cand) {
      // cand 已是 {"candidate","sdpMid","sdpMLineIndex"} 的 JSON，Web 端直接喂给 addIceCandidate。
      Send("ice_candidate", {{"session_id", session_id_}, {"candidate", json::parse(cand)}});
    };
    cb.on_data_open = [this] { SendScreenInfo(); };
    std::string err;
    if (!peer_->Init(ice_, cb, &err)) {
      fprintf(stderr, "WebRTC 初始化失败: %s\n", err.c_str());
    }
    peer_->StartScreen(0, cfg_.fps, cfg_.bitrate_kbps);
    std::string sdp;
    if (peer_->CreateOffer(&sdp, &err)) {
      Send("sdp_offer", {{"session_id", session_id_}, {"sdp", sdp}, {"type", "offer"}});
    } else {
      fprintf(stderr, "创建 offer 失败: %s\n", err.c_str());
    }
  }

  void EndSession(const std::string& reason) {
    if (peer_) {
      peer_->Close();
      peer_.reset();
    }
    if (!session_id_.empty()) printf("会话结束: %s (%s)\n", session_id_.c_str(), reason.c_str());
    session_id_.clear();
  }

  void SendScreenInfo() {
    int w = 0, h = 0;
    PrimaryScreenSize(&w, &h);
    json data = {{"action", "screen_info"},
                 {"width", w},
                 {"height", h},
                 {"monitors", json::array({{{"index", 0}, {"width", w}, {"height", h}}})}};
    std::string text = data.dump();
    SendFrame(cr::kMeta, std::vector<uint8_t>(text.begin(), text.end()));
  }

  // 已协商 E2EE 时发加密帧，否则回退明文帧。
  void SendFrame(uint8_t type, const std::vector<uint8_t>& body) {
    if (!peer_) return;
    std::vector<uint8_t> out;
    if (e2ee_.Ready() && e2ee_.Encrypt(type, body.data(), body.size(), &out)) {
      peer_->SendBinary(out);
      return;
    }
    out.reserve(body.size() + 1);
    out.push_back(type);
    out.insert(out.end(), body.begin(), body.end());
    peer_->SendBinary(out);
  }

  void OnData(const std::vector<uint8_t>& d) {
    if (d.empty()) return;
    if (d[0] == cr::kKey) {
      // [8][65B 未压缩 P-256 公钥]：派生密钥后立即回自己的公钥帧。
      if (d.size() == 66 && e2ee_.AcceptPeerKey(d.data() + 1, 65)) {
        auto kf = e2ee_.KeyFrame();
        if (!kf.empty() && peer_) peer_->SendBinary(kf);
      }
      return;
    }
    uint8_t type = d[0];
    std::vector<uint8_t> body;
    if (type == cr::kEnc) {
      if (!e2ee_.Ready() || !e2ee_.Decrypt(d.data(), d.size(), &type, &body)) return;
    } else {
      body.assign(d.begin() + 1, d.end());
    }
    switch (type) {
      case cr::kInput:
        InjectInput(cr::decodeInput(body));
        break;
      case cr::kClipboard: {
        cr::Reader r(body);
        std::string text;
        while (!r.eof()) {
          uint64_t t = r.varint();
          int f = static_cast<int>(t >> 3), w = static_cast<int>(t & 7);
          if (f == 1 && w == 2) text = r.string();
          else r.skip(w);
        }
        SetClipboardText(text);
        break;
      }
      case cr::kFile:
        SaveFileChunk(cr::decodeFile(body));
        break;
      case cr::kCtrl: {
        auto c = cr::decodeCtrl(body);
        if (c.cmd == cr::kCtrlClose) EndSession("控制端关闭");
        break;
      }
      default:
        break;
    }
  }

  void SaveFileChunk(const cr::FileChunk& f) {
    char tmp[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tmp);
    std::string path = std::string(tmp) + f.file_name;
    FILE* fp = fopen(path.c_str(), f.chunk_index == 0 ? "wb" : "ab");
    if (!fp) return;
    if (!f.chunk_data.empty()) fwrite(f.chunk_data.data(), 1, f.chunk_data.size(), fp);
    fclose(fp);
  }

  std::string Resolution() {
    int w = 0, h = 0;
    PrimaryScreenSize(&w, &h);
    return std::to_string(w) + "x" + std::to_string(h);
  }

  Config cfg_;
  ix::WebSocket ws_;
  std::vector<IceServer> ice_;
  std::unique_ptr<Peer> peer_;
  cr::E2ee e2ee_;
  std::string session_id_;
  std::atomic<bool> running_{false};
  std::mutex send_mu_;
};

int main() {
  SetConsoleOutputCP(CP_UTF8);
  Config cfg = LoadConfig();
  printf("CrossRemote Windows Agent\n信令: %s\n", cfg.server.c_str());
  Agent agent(cfg);
  agent.Run();
  return 0;
}
