#include "cr_e2ee.h"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#include <cstring>

namespace cr {

namespace {
constexpr uint8_t kFrameKey = 8;
constexpr uint8_t kFrameEnc = 9;
constexpr size_t kIvLen = 12;
constexpr size_t kTagLen = 16;
constexpr size_t kPubLen = 65;
}  // namespace

E2ee::E2ee() = default;

E2ee::~E2ee() { FreeKey(); }

void E2ee::FreeKey() {
  if (pkey_) {
    EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_));
    pkey_ = nullptr;
  }
  key_.clear();
}

bool E2ee::Init() {
  FreeKey();
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (!ctx) return false;
  EVP_PKEY* pkey = nullptr;
  bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) == 1 &&
            EVP_PKEY_keygen(ctx, &pkey) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || !pkey) {
    if (pkey) EVP_PKEY_free(pkey);
    return false;
  }
  pkey_ = pkey;
  return true;
}

std::vector<uint8_t> E2ee::KeyFrame() const {
  std::vector<uint8_t> out;
  if (!pkey_) return out;
  const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(static_cast<EVP_PKEY*>(pkey_));
  if (!ec) return out;
  std::vector<uint8_t> raw(kPubLen);
  size_t n = EC_POINT_point2oct(EC_KEY_get0_group(ec), EC_KEY_get0_public_key(ec),
                                POINT_CONVERSION_UNCOMPRESSED, raw.data(), raw.size(), nullptr);
  if (n != kPubLen) return out;
  out.reserve(1 + n);
  out.push_back(kFrameKey);
  out.insert(out.end(), raw.begin(), raw.end());
  return out;
}

bool E2ee::AcceptPeerKey(const uint8_t* raw, size_t n) {
  if (!pkey_ || !raw || n != kPubLen || raw[0] != 0x04) return false;
  EC_KEY* peer_ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (!peer_ec) return false;
  const EC_GROUP* group = EC_KEY_get0_group(peer_ec);
  EC_POINT* point = EC_POINT_new(group);
  bool ok = point && EC_POINT_oct2point(group, point, raw, n, nullptr) == 1 &&
            EC_KEY_set_public_key(peer_ec, point) == 1;
  if (point) EC_POINT_free(point);
  EVP_PKEY* peer = ok ? EVP_PKEY_new() : nullptr;
  ok = ok && peer && EVP_PKEY_assign_EC_KEY(peer, peer_ec) == 1;
  if (!ok) {
    if (peer) EVP_PKEY_free(peer);
    else EC_KEY_free(peer_ec);
    return false;
  }

  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(static_cast<EVP_PKEY*>(pkey_), nullptr);
  size_t len = 0;
  ok = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
       EVP_PKEY_derive(ctx, nullptr, &len) == 1 && len > 0;
  std::vector<uint8_t> secret(len ? len : 0);
  ok = ok && EVP_PKEY_derive(ctx, secret.data(), &len) == 1;
  if (ctx) EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(peer);
  if (!ok || len < 32) return false;

  // WebCrypto deriveKey(ECDH→AES-GCM-256) 取共享秘密前 32 字节（即 X 坐标）
  key_.assign(secret.begin(), secret.begin() + 32);
  return true;
}

bool E2ee::Encrypt(uint8_t type, const uint8_t* data, size_t n,
                   std::vector<uint8_t>* out) const {
  if (!Ready() || !out) return false;
  std::vector<uint8_t> iv(kIvLen);
  if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) return false;

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  std::vector<uint8_t> ct(n + kTagLen);
  int len = 0, total = 0;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvLen), nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv.data()) == 1;
  if (ok && n > 0) {
    ok = EVP_EncryptUpdate(ctx, ct.data(), &len, data, static_cast<int>(n)) == 1;
    total = len;
  }
  ok = ok && EVP_EncryptFinal_ex(ctx, ct.data() + total, &len) == 1;
  total += ok ? len : 0;
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen),
                                 ct.data() + total) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return false;

  ct.resize(static_cast<size_t>(total) + kTagLen);
  out->clear();
  out->reserve(2 + kIvLen + ct.size());
  out->push_back(kFrameEnc);
  out->push_back(type);
  out->insert(out->end(), iv.begin(), iv.end());
  out->insert(out->end(), ct.begin(), ct.end());
  return true;
}

bool E2ee::Decrypt(const uint8_t* pkt, size_t n, uint8_t* type,
                   std::vector<uint8_t>* out) const {
  if (!Ready() || !pkt || !out || n < 2 + kIvLen + kTagLen || pkt[0] != kFrameEnc) return false;
  if (type) *type = pkt[1];
  const uint8_t* iv = pkt + 2;
  const uint8_t* ct = pkt + 2 + kIvLen;
  const size_t ct_len = n - 2 - kIvLen - kTagLen;

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  out->assign(ct_len + kTagLen, 0);
  int len = 0, total = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvLen), nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv) == 1;
  if (ok && ct_len > 0) {
    ok = EVP_DecryptUpdate(ctx, out->data(), &len, ct, static_cast<int>(ct_len)) == 1;
    total = len;
  }
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen),
                                 const_cast<uint8_t*>(ct + ct_len)) == 1;
  ok = ok && EVP_DecryptFinal_ex(ctx, out->data() + total, &len) == 1;
  total += ok ? len : 0;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    out->clear();
    return false;
  }
  out->resize(static_cast<size_t>(total));
  return true;
}

}  // namespace cr
