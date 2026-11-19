package com.rxresiire.kexecinstaller.install;

import com.rxresiire.kexecinstaller.adb.AdbConnection;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Installs the kexec staging module on a Quest 2 (hollywood) build.
 *
 * <p>Scope is deliberately narrow. The shipped 4.19.325-cip132-st16 kernel has
 * CONFIG_KEXEC=n, so no module can enable the kernel's kexec_load transition:
 * machine_kexec, the relocation code and the purgatory are simply absent. This
 * installer therefore stages and validates the out-of-tree module, sets it up as
 * a reversible Magisk module, and never triggers a kernel handoff. It also never
 * writes partitions or touches the bootloader.
 */
public final class KexecInstaller {

    public static final String EXPECTED_KERNEL = "4.19.325-cip132-st16-g429559e91fe8";
    public static final String MODULE_ID = "rxresiire_kexec";
    public static final String MODULE_DIR = "/data/adb/modules/" + MODULE_ID;
    public static final String STAGE_PATH = "/data/local/tmp/quest_kexec.ko";

    private final AdbConnection adb;

    public KexecInstaller(AdbConnection adb) {
        this.adb = adb;
    }

    public String probe() throws IOException {
        StringBuilder report = new StringBuilder();
        String kernel = adb.exec("cat /proc/version").trim();
        String device = adb.exec("getprop ro.product.device").trim();
        String incremental = adb.exec("getprop ro.build.version.incremental").trim();
        String sdk = adb.exec("getprop ro.build.version.sdk").trim();
        boolean root = adb.hasRoot();
        String magisk = adb.execRoot("magisk -V 2>/dev/null || echo unavailable").trim();
        String kexecConfig = adb.execRoot(
                "if [ -r /proc/config.gz ]; then zcat /proc/config.gz | grep -E '^CONFIG_KEXEC'; "
                        + "else echo 'unreadable'; fi").trim();

        report.append("device      : ").append(device.isEmpty() ? "?" : device).append('\n');
        report.append("build       : ").append(incremental.isEmpty() ? "?" : incremental)
                .append(" (API ").append(sdk).append(")\n");
        report.append("kernel      : ").append(kernel).append('\n');
        report.append("kernel match: ").append(kernel.contains(EXPECTED_KERNEL) ? "yes" : "NO").append('\n');
        report.append("magisk root : ").append(root ? "granted" : "denied").append('\n');
        report.append("magisk ver  : ").append(magisk).append('\n');
        report.append("kexec cfg   : ").append(kexecConfig.isEmpty() ? "unknown" : kexecConfig).append('\n');
        if (kexecConfig.contains("CONFIG_KEXEC=y")) {
            report.append("transition  : kernel reports kexec support, still not exercised by this app\n");
        } else {
            report.append("transition  : unavailable - stock kernel is built with CONFIG_KEXEC=n\n");
        }
        return report.toString();
    }

    public String stage(byte[] module, String expectedSha256) throws IOException {
        String localSha = sha256(module);
        if (expectedSha256 != null && !expectedSha256.equalsIgnoreCase(localSha)) {
            throw new IOException("Selected file hashes to " + localSha + " but the manifest expects "
                    + expectedSha256);
        }
        String vermagic = ElfModinfo.vermagic(module);
        if (!vermagic.startsWith(EXPECTED_KERNEL)) {
            throw new IOException("Module vermagic '" + vermagic + "' does not match " + EXPECTED_KERNEL);
        }
        String kernel = adb.exec("cat /proc/version").trim();
        if (!kernel.contains(EXPECTED_KERNEL)) {
            throw new IOException("Headset is not running " + EXPECTED_KERNEL + ": " + kernel);
        }
        if (!adb.hasRoot()) {
            throw new IOException("Magisk root was not granted; approve the su request on the headset");
        }

        adb.writeFile(module, STAGE_PATH, true);
        String remoteSha = adb.execRoot("sha256sum " + STAGE_PATH + " | cut -d' ' -f1").trim()
                .toLowerCase(Locale.ROOT);
        if (!remoteSha.equals(localSha)) {
            throw new IOException("Copy verification failed: local " + localSha + ", device " + remoteSha);
        }

        String base = "set -e; mkdir -p " + MODULE_DIR + "/system/lib/modules";
        requireOk(adb.execRoot(base), "prepare module directory");
        adb.writeFile(module, MODULE_DIR + "/system/lib/modules/quest_kexec.ko", true);
        adb.writeFile(moduleProp().getBytes(StandardCharsets.UTF_8), MODULE_DIR + "/module.prop", true);
        adb.writeFile(postFsData().getBytes(StandardCharsets.UTF_8),
                MODULE_DIR + "/post-fs-data.sh", true);
        adb.writeFile(serviceScript().getBytes(StandardCharsets.UTF_8), MODULE_DIR + "/service.sh", true);
        adb.writeFile(uninstallScript().getBytes(StandardCharsets.UTF_8),
                MODULE_DIR + "/uninstall.sh", true);
        requireOk(adb.execRoot("chmod 0755 " + MODULE_DIR + "/post-fs-data.sh "
                + MODULE_DIR + "/service.sh " + MODULE_DIR + "/uninstall.sh"), "set script modes");

        return "Staged " + MODULE_ID + " (" + module.length + " bytes, sha256 " + localSha + ")\n"
                + "vermagic: " + vermagic + "\n"
                + "mode: staging only (execute=0, validate_ram=1)\n"
                + "kexec transition: not available (CONFIG_KEXEC=n); nothing was booted or remapped\n"
                + "rollback: use Uninstall in this app, or delete " + MODULE_DIR;
    }

    public String loadStaged() throws IOException {
        String loaded = adb.execRoot("lsmod | grep -c '^quest_kexec' || true").trim();
        if (!"0".equals(loaded)) {
            return "quest_kexec is already loaded";
        }
        String result = adb.execRoot("insmod " + MODULE_DIR
                + "/system/lib/modules/quest_kexec.ko execute=0 validate_ram=1");
        String modules = adb.execRoot("grep -c '^quest_kexec' /proc/modules || true").trim();
        if ("0".equals(modules)) {
            throw new IOException("insmod did not register the module: " + result.trim());
        }
        return "quest_kexec loaded in staging mode\n" + result.trim();
    }

    public String uninstall() throws IOException {
        String result = adb.execRoot("rmmod quest_kexec 2>/dev/null; rm -rf " + MODULE_DIR
                + "; rm -f " + STAGE_PATH + "; echo removed");
        if (!result.contains("removed")) {
            throw new IOException("Uninstall did not complete: " + result.trim());
        }
        return "Removed " + MODULE_ID + " and " + STAGE_PATH + "; reboot to return to a clean boot path";
    }

    private static void requireOk(String response, String action) throws IOException {
        String trimmed = response.trim();
        if (!trimmed.isEmpty()) {
            throw new IOException("Failed to " + action + ": " + trimmed);
        }
    }

    private static String sha256(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder hex = new StringBuilder(digest.length * 2);
            for (byte value : digest) {
                hex.append(String.format(Locale.ROOT, "%02x", value));
            }
            return hex.toString();
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static String moduleProp() {
        return "id=" + MODULE_ID + "\n"
                + "name=RXRESIIRE KEXEC INSTALLER (staging)\n"
                + "version=1.0.0\n"
                + "versionCode=1\n"
                + "author=rxresiire\n"
                + "description=Quest 2 Hollywood kexec staging module. Stock kernel has CONFIG_KEXEC=n;"
                + " no kernel transition is performed and the boot path is untouched.\n";
    }

    private static String postFsData() {
        return "#!/system/bin/sh\n"
                + "MODDIR=${0%/*}\n"
                + "[ -f \"$MODDIR/system/lib/modules/quest_kexec.ko\" ] || exit 0\n"
                + "insmod \"$MODDIR/system/lib/modules/quest_kexec.ko\" execute=0 validate_ram=1 "
                + ">> /data/local/tmp/rxresiire_kexec.log 2>&1\n";
    }

    private static String serviceScript() {
        return "#!/system/bin/sh\n"
                + "MODDIR=${0%/*}\n"
                + "grep -q '^quest_kexec' /proc/modules || "
                + "insmod \"$MODDIR/system/lib/modules/quest_kexec.ko\" execute=0 validate_ram=1 "
                + ">> /data/local/tmp/rxresiire_kexec.log 2>&1\n";
    }

    private static String uninstallScript() {
        return "#!/system/bin/sh\n"
                + "rmmod quest_kexec 2>/dev/null\n";
    }
}
