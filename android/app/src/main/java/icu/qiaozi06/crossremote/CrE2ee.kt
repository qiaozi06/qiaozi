package icu.qiaozi06.crossremote

import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.SecureRandom
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * 端到端加密：ECDH P-256 + AES-256-GCM。
 *
 * 与 web/pb.js、agents/common/cr_e2ee.h 字节级一致：
 *  - 公钥帧 [8][65B 未压缩点 0x04||X||Y]
 *  - 加密帧 [9][innerType][12B IV][密文||16B tag]
 *  - AES 密钥 = ECDH 共享秘密（X 坐标）32 字节
 */
class CrE2ee {
    private var keyPair: KeyPair? = null
    private var params: ECParameterSpec? = null
    private var aesKey: ByteArray? = null

    val available: Boolean get() = keyPair != null
    val ready: Boolean get() = aesKey != null

    fun init(): Boolean {
        aesKey = null
        return try {
            val gen = KeyPairGenerator.getInstance("EC")
            gen.initialize(ECGenParameterSpec("secp256r1"))
            val kp = gen.generateKeyPair()
            keyPair = kp
            params = (kp.public as java.security.interfaces.ECPublicKey).params
            true
        } catch (e: Exception) {
            keyPair = null
            params = null
            false
        }
    }

    /** [8][65B]；未初始化时返回空数组。 */
    fun keyFrame(): ByteArray {
        val kp = keyPair ?: return ByteArray(0)
        val w = (kp.public as java.security.interfaces.ECPublicKey).w
        val raw = ByteArray(65)
        raw[0] = 0x04
        writeFixed(w.affineX, raw, 1)
        writeFixed(w.affineY, raw, 33)
        return ByteArray(66).also {
            it[0] = Proto.KEY.toByte()
            System.arraycopy(raw, 0, it, 1, 65)
        }
    }

    /** 接收 65B 未压缩公钥并派生 AES 密钥。 */
    fun acceptPeerKey(raw: ByteArray): Boolean {
        val kp = keyPair ?: return false
        val p = params ?: return false
        if (raw.size != 65 || raw[0] != 0x04.toByte()) return false
        return try {
            val point = ECPoint(
                BigInteger(1, raw.copyOfRange(1, 33)),
                BigInteger(1, raw.copyOfRange(33, 65))
            )
            val pub = KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, p))
            val ka = KeyAgreement.getInstance("ECDH")
            ka.init(kp.private)
            ka.doPhase(pub, true)
            val secret = ka.generateSecret()
            // 共享秘密为 X 坐标，个别实现可能去掉前导 0，统一补齐到 32B。
            aesKey = when {
                secret.size >= 32 -> secret.copyOfRange(secret.size - 32, secret.size)
                else -> ByteArray(32).also { System.arraycopy(secret, 0, it, 32 - secret.size, secret.size) }
            }
            true
        } catch (e: Exception) {
            aesKey = null
            false
        }
    }

    /** 加密为 [9][type][12B IV][ct||tag]；未就绪返回 null。 */
    fun encrypt(type: Int, data: ByteArray): ByteArray? {
        val key = aesKey ?: return null
        return try {
            val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, iv))
            val ct = cipher.doFinal(data)
            ByteArray(2 + iv.size + ct.size).also {
                it[0] = Proto.ENC.toByte()
                it[1] = type.toByte()
                System.arraycopy(iv, 0, it, 2, iv.size)
                System.arraycopy(ct, 0, it, 2 + iv.size, ct.size)
            }
        } catch (e: Exception) {
            null
        }
    }

    /** 解密 [9][type][12B IV][ct||tag]，返回 (type, body)；失败返回 null。 */
    fun decrypt(packet: ByteArray): Pair<Int, ByteArray>? {
        val key = aesKey ?: return null
        if (packet.size < 2 + 12 + 16 || packet[0] != Proto.ENC.toByte()) return null
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(
                Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"),
                GCMParameterSpec(128, packet.copyOfRange(2, 14))
            )
            val pt = cipher.doFinal(packet.copyOfRange(14, packet.size))
            Pair(packet[1].toInt() and 0xFF, pt)
        } catch (e: Exception) {
            null
        }
    }

    private fun writeFixed(v: BigInteger, out: ByteArray, offset: Int) {
        val b = v.toByteArray()
        val src = if (b.size > 32) b.copyOfRange(b.size - 32, b.size) else b
        System.arraycopy(src, 0, out, offset + (32 - src.size), src.size)
    }
}
