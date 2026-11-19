package com.rxresiire.kexecinstaller.adb;

import java.io.File;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.GeneralSecurityException;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.Signature;
import java.security.interfaces.RSAPublicKey;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.X509EncodedKeySpec;
import java.util.Base64;

/**
 * Host identity used for the ADB RSA authentication handshake.
 *
 * <p>The key pair is generated on first use and persisted in the app's private
 * storage. The public key is published to adbd in Android's {@code RSAPublicKey}
 * wire format (little-endian 64-bit limbs of n, Montgomery rr, n0inv), which is
 * what the on-device debugging prompt fingerprints.
 */
public final class AdbCrypto {

    private static final String PRIVATE_FILE = "adbkey.pk8";
    private static final String PUBLIC_FILE = "adbkey.pub";

    private final KeyPair keyPair;

    private AdbCrypto(KeyPair keyPair) {
        this.keyPair = keyPair;
    }

    public static AdbCrypto loadOrCreate(File dir) throws GeneralSecurityException, IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("Cannot create key directory " + dir);
        }
        File priv = new File(dir, PRIVATE_FILE);
        File pub = new File(dir, PUBLIC_FILE);
        KeyFactory factory = KeyFactory.getInstance("RSA");
        if (priv.isFile() && pub.isFile()) {
            byte[] pkcs8 = Base64.getMimeDecoder().decode(Files.readAllBytes(priv.toPath()));
            byte[] x509 = Base64.getMimeDecoder().decode(Files.readAllBytes(pub.toPath()));
            return new AdbCrypto(new KeyPair(
                    factory.generatePublic(new X509EncodedKeySpec(x509)),
                    factory.generatePrivate(new PKCS8EncodedKeySpec(pkcs8))));
        }
        KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA");
        generator.initialize(2048);
        KeyPair generated = generator.generateKeyPair();
        Files.write(priv.toPath(), Base64.getEncoder().encode(generated.getPrivate().getEncoded()));
        Files.write(pub.toPath(), Base64.getEncoder().encode(generated.getPublic().getEncoded()));
        return new AdbCrypto(generated);
    }

    public byte[] sign(byte[] token) throws GeneralSecurityException {
        Signature signature = Signature.getInstance("SHA1withRSA");
        signature.initSign(keyPair.getPrivate());
        signature.update(token);
        return signature.sign();
    }

    /** Payload for A_AUTH / AUTH_RSAPUBLICKEY: base64 key, space, identity, NUL. */
    public byte[] publicKeyPayload(String hostName) throws GeneralSecurityException {
        byte[] wire = encodeAdbPublicKey((RSAPublicKey) keyPair.getPublic());
        String encoded = Base64.getEncoder().encodeToString(wire);
        return (encoded + " " + hostName + "@rxresiire\0").getBytes(StandardCharsets.US_ASCII);
    }

    /** Android's struct RSAPublicKey, 524 bytes: len, n0inv, n[64], rr[64], exponent. */
    static byte[] encodeAdbPublicKey(RSAPublicKey key) throws GeneralSecurityException {
        BigInteger n = key.getModulus();
        BigInteger e = key.getPublicExponent();
        byte[] nBytes = toFixedLength(n, 256);
        BigInteger r32 = BigInteger.ONE.shiftLeft(32);
        long n0inv = n.modInverse(r32).negate().mod(r32).longValue();
        BigInteger rr = BigInteger.ONE.shiftLeft(4096).mod(n);
        byte[] rrBytes = toFixedLength(rr, 256);

        ByteBuffer buffer = ByteBuffer.allocate(524).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(524);
        buffer.putInt((int) n0inv);
        putLimbs(buffer, nBytes);
        putLimbs(buffer, rrBytes);
        buffer.putInt(e.intValue());
        return buffer.array();
    }

    private static void putLimbs(ByteBuffer buffer, byte[] bigEndian) {
        for (int limb = 0; limb < 64; limb++) {
            int value = 0;
            for (int byteIndex = 0; byteIndex < 4; byteIndex++) {
                int index = bigEndian.length - 1 - (limb * 4 + byteIndex);
                int octet = index >= 0 ? bigEndian[index] & 0xff : 0;
                value |= octet << (8 * byteIndex);
            }
            buffer.putInt(value);
        }
    }

    private static byte[] toFixedLength(BigInteger value, int length) {
        byte[] raw = value.toByteArray();
        byte[] out = new byte[length];
        if (raw.length >= length) {
            System.arraycopy(raw, raw.length - length, out, 0, length);
        } else {
            System.arraycopy(raw, 0, out, length - raw.length, raw.length);
        }
        return out;
    }
}
