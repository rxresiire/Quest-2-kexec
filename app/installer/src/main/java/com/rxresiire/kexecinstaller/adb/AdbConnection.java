package com.rxresiire.kexecinstaller.adb;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.Base64;
import java.util.zip.CRC32;

/**
 * Minimal-but-real ADB client speaking the native ADB wire protocol over TCP.
 *
 * <p>Supports CNXN negotiation, RSA AUTH (token signature and public-key push),
 * and stream OPEN/WRTE/OKAY/CLSE for {@code shell:} services. It intentionally
 * does not implement the separate SPAKE2/TLS "wireless debugging" pairing
 * service; it authenticates against an adbd that is already listening on TCP.
 */
public final class AdbConnection implements Closeable {

    private static final int A_CNXN = 0x4e584e43;
    private static final int A_AUTH = 0x48545541;
    private static final int A_OPEN = 0x4e45504f;
    private static final int A_OKAY = 0x59414b4f;
    private static final int A_CLSE = 0x45534c43;
    private static final int A_WRTE = 0x45545257;

    private static final int AUTH_TOKEN = 1;
    private static final int AUTH_SIGNATURE = 2;
    private static final int AUTH_RSAPUBLICKEY = 3;

    private static final int CONNECT_VERSION = 0x01000001;
    private static final int MAX_PAYLOAD = 256 * 1024;
    private static final int PUSH_CHUNK = 12 * 1024;

    private final Socket socket;
    private final DataInputStream input;
    private final DataOutputStream output;
    private final String hostName;
    private int nextLocalId = 1;

    private AdbConnection(Socket socket, String hostName) throws IOException {
        this.socket = socket;
        this.hostName = hostName;
        this.input = new DataInputStream(socket.getInputStream());
        this.output = new DataOutputStream(socket.getOutputStream());
    }

    public static AdbConnection open(String host, int port, int timeoutMs, AdbCrypto crypto,
                                     String hostName) throws IOException, GeneralSecurityException {
        Socket socket = new Socket();
        try {
            socket.connect(new InetSocketAddress(host, port), timeoutMs);
            socket.setSoTimeout(timeoutMs);
            socket.setTcpNoDelay(true);
            AdbConnection connection = new AdbConnection(socket, hostName);
            connection.handshake(crypto);
            return connection;
        } catch (IOException | GeneralSecurityException failure) {
            try {
                socket.close();
            } catch (IOException ignored) {
                // best effort
            }
            throw failure;
        }
    }

    private void handshake(AdbCrypto crypto) throws IOException, GeneralSecurityException {
        send(A_CNXN, CONNECT_VERSION, MAX_PAYLOAD,
                ("host::" + hostName).getBytes(StandardCharsets.UTF_8));
        int authStep = 0;
        while (true) {
            Packet packet = read();
            if (packet.command == A_CNXN) {
                socket.setSoTimeout(0);
                return;
            }
            if (packet.command != A_AUTH) {
                throw new IOException("Unexpected ADB packet 0x"
                        + Integer.toHexString(packet.command) + " during handshake");
            }
            if (packet.arg0 != AUTH_TOKEN) {
                throw new IOException("adbd rejected host authentication (status " + packet.arg0 + ")");
            }
            if (authStep == 0) {
                send(A_AUTH, AUTH_SIGNATURE, 0, crypto.sign(packet.payload));
                authStep = 1;
            } else if (authStep == 1) {
                send(A_AUTH, AUTH_RSAPUBLICKEY, 0, crypto.publicKeyPayload(hostName));
                authStep = 2;
            } else {
                throw new IOException("adbd did not accept the key; accept the debugging prompt on the headset");
            }
        }
    }

    /** Run a command through the device shell as the shell user. */
    public String exec(String command) throws IOException {
        return runShell("shell:" + command);
    }

    /** Run a command as root through Magisk's su. */
    public String execRoot(String command) throws IOException {
        return runShell("shell:su -c " + quote(command));
    }

    /** True when Magisk su grants uid 0. */
    public boolean hasRoot() throws IOException {
        String uid = execRoot("id -u").trim();
        return "0".equals(uid);
    }

    /**
     * Write bytes to a device path using base64 staging, so no binary-safe
     * stdin channel is required. When {@code asRoot} the write runs via su.
     */
    public void writeFile(byte[] data, String remotePath, boolean asRoot) throws IOException {
        String base64 = Base64.getEncoder().encodeToString(data);
        String create = "umask 022; : > " + quote(remotePath);
        String setup = asRoot ? execRoot(create) : exec(create);
        if (!setup.trim().isEmpty()) {
            throw new IOException("Cannot create " + remotePath + ": " + setup.trim());
        }
        for (int offset = 0; offset < base64.length(); offset += PUSH_CHUNK) {
            String chunk = base64.substring(offset, Math.min(base64.length(), offset + PUSH_CHUNK));
            String append = "printf %s " + quote(chunk) + " | base64 -d >> " + quote(remotePath);
            String response = asRoot ? execRoot(append) : exec(append);
            if (!response.trim().isEmpty() && !response.contains("Applet not found")) {
                throw new IOException("Device reported an error while staging: " + response.trim());
            }
        }
    }

    private String runShell(String service) throws IOException {
        final int localId = nextLocalId++;
        send(A_OPEN, localId, 0, service.getBytes(StandardCharsets.UTF_8));
        ByteArrayOutputStream collected = new ByteArrayOutputStream();
        boolean opened = false;
        while (true) {
            Packet packet;
            try {
                packet = read();
            } catch (EOFException end) {
                break;
            }
            if (packet.arg1 != localId && packet.arg0 != localId) {
                continue;
            }
            switch (packet.command) {
                case A_OKAY:
                    opened = true;
                    break;
                case A_WRTE:
                    collected.write(packet.payload);
                    send(A_OKAY, localId, packet.arg0, new byte[0]);
                    break;
                case A_CLSE:
                    if (opened || collected.size() > 0) {
                        return collected.toString(StandardCharsets.UTF_8.name());
                    }
                    throw new IOException("Device refused service " + service);
                default:
                    break;
            }
        }
        return collected.toString(StandardCharsets.UTF_8.name());
    }

    private void send(int command, int arg0, int arg1, byte[] payload) throws IOException {
        int length = payload == null ? 0 : payload.length;
        CRC32 crc = new CRC32();
        if (length > 0) {
            crc.update(payload);
        }
        ByteBuffer header = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        header.putInt(command);
        header.putInt(arg0);
        header.putInt(arg1);
        header.putInt(length);
        header.putInt((int) crc.getValue());
        header.putInt(command ^ 0xffffffff);
        output.write(header.array());
        if (length > 0) {
            output.write(payload);
        }
        output.flush();
    }

    private Packet read() throws IOException {
        byte[] header = new byte[24];
        input.readFully(header);
        ByteBuffer buffer = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int command = buffer.getInt();
        int arg0 = buffer.getInt();
        int arg1 = buffer.getInt();
        int length = buffer.getInt();
        buffer.getInt();
        int magic = buffer.getInt();
        if ((command ^ 0xffffffff) != magic) {
            throw new IOException("ADB packet magic mismatch");
        }
        if (length < 0 || length > MAX_PAYLOAD) {
            throw new IOException("ADB payload length out of range: " + length);
        }
        byte[] payload = new byte[length];
        if (length > 0) {
            input.readFully(payload);
        }
        return new Packet(command, arg0, arg1, payload);
    }

    private static String quote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    @Override
    public void close() {
        try {
            socket.close();
        } catch (IOException ignored) {
            // best effort
        }
    }

    private static final class Packet {
        final int command;
        final int arg0;
        final int arg1;
        final byte[] payload;

        Packet(int command, int arg0, int arg1, byte[] payload) {
            this.command = command;
            this.arg0 = arg0;
            this.arg1 = arg1;
            this.payload = payload;
        }
    }
}
