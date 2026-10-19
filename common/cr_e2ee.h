// CrossRemote 端到端加密（E2EE）：ECDH P-256 + AES-256-GCM
//
// 与 web/pb.js 的 PBCodec.E2EE 字节级一致：
//   - 公钥帧：[8][65B 未压缩点 0x04||X||Y]
//   - 加密帧：[9][innerType][12B IV][密文||16B tag]
//   - 密钥：ECDH 共享秘密的 X 坐标直接作为 AES-256 密钥（WebCrypto deriveKey 行为）
//
// 依赖 OpenSSL 1.1.1+（libcrypto）。Windows 可用 vcpkg 安装 openssl，macOS 可用 Homebrew。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cr {

class E2ee {
 public:
  E2ee();
  ~E2ee();
  E2ee(const E2ee&) = delete;
  E2ee& operator=(const E2ee&) = delete;

  // 生成 P-256 密钥对；失败返回 false（此时 Available() 为 false）
  bool Init();
  bool Available() const { return pkey_ != nullptr; }
  bool Ready() const { return key_.size() == 32; }

  // 公钥帧 [8][65B]，未 Init 时返回空
  std::vector<uint8_t> KeyFrame() const;

  // 接收对端 65B 未压缩公钥并派生 AES 密钥
  bool AcceptPeerKey(const uint8_t* raw, size_t n);

  // 加密：[9][type][12B IV][密文||tag]；未就绪时返回 nullptr（调用方回退明文）
  bool Encrypt(uint8_t type, const uint8_t* data, size_t n, std::vector<uint8_t>* out) const;

  // 解密：输入 [9][type][12B IV][密文||tag]
  bool Decrypt(const uint8_t* pkt, size_t n, uint8_t* type, std::vector<uint8_t>* out) const;

 private:
  void FreeKey();
  void* pkey_ = nullptr;             // EVP_PKEY*
  std::vector<uint8_t> key_;         // 32B AES 密钥
};

}  // namespace cr
