// CrossRemote DataChannel 业务协议（与 web/pb.js 字节级兼容）
// 帧格式：[1B type][payload]
//  1=InputEvent 2=ClipboardMsg 3=FileTransferMsg 4=SessionCtrlMsg
//  5=Meta(JSON) 8=ECDH 公钥 9=加密帧 [9][innerType][12B IV][AES-GCM+tag]
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cr {

enum FrameType : uint8_t {
  kInput = 1,
  kClipboard = 2,
  kFile = 3,
  kCtrl = 4,
  kMeta = 5,
  kKey = 8,
  kEnc = 9,
};

enum CtrlCmd {
  kCtrlClose = 0,
  kCtrlSwitchMonitor = 1,
  kCtrlBlackOn = 2,
  kCtrlBlackOff = 3,
};

struct InputEvent {
  enum Kind { kNone, kMove, kButton, kWheel, kKey } kind = kNone;
  int32_t x = 0;
  int32_t y = 0;
  int32_t button = 0;   // 0=左 1=右 2=中
  bool pressed = false;
  int32_t delta_y = 0;
  uint32_t keycode = 0; // Windows VK
};

class Writer {
 public:
  void varint(uint64_t v) {
    while (v >= 0x80) {
      buf_.push_back(static_cast<uint8_t>(v) | 0x80);
      v >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(v));
  }
  void tag(int field, int wire) { varint((static_cast<uint64_t>(field) << 3) | wire); }
  void int32f(int field, int32_t v) {
    tag(field, 0);
    varint(static_cast<uint64_t>(static_cast<int64_t>(v)));
  }
  void boolf(int field, bool v) {
    tag(field, 0);
    varint(v ? 1 : 0);
  }
  void bytesf(int field, const uint8_t* p, size_t n) {
    tag(field, 2);
    varint(n);
    buf_.insert(buf_.end(), p, p + n);
  }
  void bytesf(int field, const std::vector<uint8_t>& v) { bytesf(field, v.data(), v.size()); }
  void stringf(int field, const std::string& s) {
    bytesf(field, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  const std::vector<uint8_t>& data() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  explicit Reader(const std::vector<uint8_t>& v) : Reader(v.data(), v.size()) {}
  bool eof() const { return p_ >= end_; }
  uint64_t varint() {
    uint64_t r = 0;
    int s = 0;
    while (p_ < end_) {
      uint8_t b = *p_++;
      r |= static_cast<uint64_t>(b & 0x7f) << s;
      if (!(b & 0x80)) return r;
      s += 7;
    }
    return r;
  }
  int32_t int32() { return static_cast<int32_t>(static_cast<int64_t>(varint())); }
  uint32_t u32() { return static_cast<uint32_t>(varint()); }
  bool boolean() { return varint() != 0; }
  std::vector<uint8_t> bytes() {
    size_t n = static_cast<size_t>(varint());
    if (p_ + n > end_) n = static_cast<size_t>(end_ - p_);
    std::vector<uint8_t> out(p_, p_ + n);
    p_ += n;
    return out;
  }
  std::string string() {
    auto b = bytes();
    return std::string(b.begin(), b.end());
  }
  void skip(int wire) {
    switch (wire) {
      case 0: varint(); break;
      case 1: p_ += 8; break;
      case 2: p_ += static_cast<size_t>(varint()); break;
      case 5: p_ += 4; break;
      default: p_ = end_; break;
    }
    if (p_ > end_) p_ = end_;
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

inline InputEvent decodeInput(const std::vector<uint8_t>& in) {
  InputEvent ev;
  Reader r(in);
  while (!r.eof()) {
    uint64_t t = r.varint();
    int field = static_cast<int>(t >> 3);
    int wire = static_cast<int>(t & 7);
    if (wire != 2) { r.skip(wire); continue; }
    auto sub = r.bytes();
    if (field == 1) {
      ev.kind = InputEvent::kMove;
      Reader s(sub);
      while (!s.eof()) {
        uint64_t st = s.varint();
        int f = static_cast<int>(st >> 3), w = static_cast<int>(st & 7);
        if (f == 1 && w == 0) ev.x = s.int32();
        else if (f == 2 && w == 0) ev.y = s.int32();
        else s.skip(w);
      }
    } else if (field == 2) {
      ev.kind = InputEvent::kButton;
      Reader s(sub);
      while (!s.eof()) {
        uint64_t st = s.varint();
        int f = static_cast<int>(st >> 3), w = static_cast<int>(st & 7);
        if (f == 1 && w == 0) ev.x = s.int32();
        else if (f == 2 && w == 0) ev.y = s.int32();
        else if (f == 3 && w == 0) ev.button = s.int32();
        else if (f == 4 && w == 0) ev.pressed = s.boolean();
        else s.skip(w);
      }
    } else if (field == 3) {
      ev.kind = InputEvent::kWheel;
      Reader s(sub);
      while (!s.eof()) {
        uint64_t st = s.varint();
        int f = static_cast<int>(st >> 3), w = static_cast<int>(st & 7);
        if (f == 1 && w == 0) ev.delta_y = s.int32();
        else s.skip(w);
      }
    } else if (field == 4) {
      ev.kind = InputEvent::kKey;
      Reader s(sub);
      while (!s.eof()) {
        uint64_t st = s.varint();
        int f = static_cast<int>(st >> 3), w = static_cast<int>(st & 7);
        if (f == 1 && w == 0) ev.keycode = s.u32();
        else if (f == 2 && w == 0) ev.pressed = s.boolean();
        else s.skip(w);
      }
    }
  }
  return ev;
}

inline std::vector<uint8_t> encodeMove(int32_t x, int32_t y) {
  Writer w;
  Writer m;
  m.int32f(1, x);
  m.int32f(2, y);
  w.bytesf(1, m.data());
  return w.data();
}

inline std::vector<uint8_t> encodeButton(int32_t x, int32_t y, int32_t button, bool pressed) {
  Writer w, m;
  m.int32f(1, x);
  m.int32f(2, y);
  m.int32f(3, button);
  m.boolf(4, pressed);
  w.bytesf(2, m.data());
  return w.data();
}

inline std::vector<uint8_t> encodeWheel(int32_t delta_y) {
  Writer w, m;
  m.int32f(1, delta_y);
  w.bytesf(3, m.data());
  return w.data();
}

inline std::vector<uint8_t> encodeKey(uint32_t vk, bool pressed) {
  Writer w, m;
  m.int32f(1, static_cast<int32_t>(vk));
  m.boolf(2, pressed);
  w.bytesf(4, m.data());
  return w.data();
}

inline std::vector<uint8_t> encodeClipboard(const std::string& text) {
  Writer w;
  w.stringf(1, text);
  return w.data();
}

struct FileChunk {
  std::string file_id;
  std::string file_name;
  int64_t total_size = 0;
  int32_t chunk_index = 0;
  std::vector<uint8_t> chunk_data;
  bool is_finish = false;
};

inline FileChunk decodeFile(const std::vector<uint8_t>& in) {
  FileChunk f;
  Reader r(in);
  while (!r.eof()) {
    uint64_t t = r.varint();
    int field = static_cast<int>(t >> 3), wire = static_cast<int>(t & 7);
    if (wire == 0) {
      if (field == 3) f.total_size = static_cast<int64_t>(r.varint());
      else if (field == 4) f.chunk_index = r.int32();
      else if (field == 6) f.is_finish = r.boolean();
      else r.skip(wire);
    } else if (wire == 2) {
      if (field == 1) f.file_id = r.string();
      else if (field == 2) f.file_name = r.string();
      else if (field == 5) f.chunk_data = r.bytes();
      else r.skip(wire);
    } else {
      r.skip(wire);
    }
  }
  return f;
}

inline std::vector<uint8_t> encodeFile(const FileChunk& f) {
  Writer w;
  w.stringf(1, f.file_id);
  w.stringf(2, f.file_name);
  w.int32f(3, static_cast<int32_t>(f.total_size));
  w.int32f(4, f.chunk_index);
  w.bytesf(5, f.chunk_data);
  w.boolf(6, f.is_finish);
  return w.data();
}

inline std::vector<uint8_t> encodeCtrl(int32_t cmd, int32_t monitor_index) {
  Writer w;
  w.int32f(1, cmd);
  w.int32f(2, monitor_index);
  return w.data();
}

struct CtrlMsg {
  int32_t cmd = 0;
  int32_t monitor_index = 0;
};

inline CtrlMsg decodeCtrl(const std::vector<uint8_t>& in) {
  CtrlMsg c;
  Reader r(in);
  while (!r.eof()) {
    uint64_t t = r.varint();
    int field = static_cast<int>(t >> 3), wire = static_cast<int>(t & 7);
    if (field == 1 && wire == 0) c.cmd = r.int32();
    else if (field == 2 && wire == 0) c.monitor_index = r.int32();
    else r.skip(wire);
  }
  return c;
}

inline std::vector<uint8_t> frame(uint8_t type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out;
  out.reserve(payload.size() + 1);
  out.push_back(type);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

inline std::vector<uint8_t> metaFrame(const std::string& json) {
  return frame(kMeta, std::vector<uint8_t>(json.begin(), json.end()));
}

}  // namespace cr
