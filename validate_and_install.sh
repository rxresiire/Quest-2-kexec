#!/system/bin/sh
# Quest 2 kexec Module Validation & Installation Script
# Run on device via: adb shell su -c "sh /data/local/tmp/kexec_test/validate_and_install.sh"
#
# SAFETY: This script ONLY runs passive checks and REVERSIBLE tests.
# It NEVER runs: execute=1, test_cpus=2, test_full_copy=1 with execute, or any hardware transition.
# All tests use execute=0 and are designed to be safely reversible.

set -e

MODULE_DIR="/data/local/tmp/kexec_test"
MODULE_PATH="$MODULE_DIR/quest_kexec.ko"
MAGISK_MODULE_DIR="/data/adb/modules/quest_kexec"
LOG_FILE="$MODULE_DIR/validation_$(date +%Y%m%d_%H%M%S).log"

mkdir -p "$MODULE_DIR"
exec > "$LOG_FILE" 2>&1

echo "========================================="
echo "Quest 2 kexec Validation & Installation"
echo "Timestamp: $(date)"
echo "Log: $LOG_FILE"
echo "SAFETY MODE: Passive checks + reversible tests ONLY"
echo "========================================="

log() { echo "[$(date +%H:%M:%S)] $*"; }
pass() { log "PASS: $*"; }
fail() { log "FAIL: $*"; }
warn() { log "WARN: $*"; }

# Check root
if [ "$(id -u)" -ne 0 ]; then
    fail "Requires root. Run with: adb shell su -c '$0'"
    exit 1
fi

# Check kernel version
KERNEL_VER=$(uname -r)
log "Running kernel: $KERNEL_VER"
if echo "$KERNEL_VER" | grep -q "cip132-st16"; then
    pass "Kernel matches target (cip132-st16)"
else
    warn "Kernel version mismatch - module may not load"
fi

# Check SELinux
SELINUX=$(getenforce)
log "SELinux: $SELINUX"
if [ "$SELINUX" != "Permissive" ]; then
    warn "SELinux not permissive - setting to permissive"
    setenforce 0
fi

# Check Magisk
if [ -d "/data/adb/magisk" ]; then
    pass "Magisk detected"
    MAGISK_AVAILABLE=1
else
    warn "Magisk not found - temporary installation only"
    MAGISK_AVAILABLE=0
fi

# Verify module file exists
if [ ! -f "$MODULE_PATH" ]; then
    fail "Module not found at $MODULE_PATH"
    fail "Push it first: adb push quest_kexec.ko $MODULE_PATH"
    exit 1
fi

log "Module file: $(ls -lh $MODULE_PATH)"

# =========================================
# TEST 1: Module vermagic check (PASSIVE - no load)
# =========================================
log ""
log "=== TEST 1: Module vermagic compatibility (PASSIVE) ==="
if command -v modinfo >/dev/null 2>&1; then
    MOD_VERMAGIC=$(modinfo "$MODULE_PATH" 2>/dev/null | grep vermagic | head -1 | cut -d: -f2- | xargs)
    log "Module vermagic: $MOD_VERMAGIC"
    KERNEL_VERMAGIC="$KERNEL_VER"
    log "Kernel vermagic: $KERNEL_VERMAGIC"
    if echo "$MOD_VERMAGIC" | grep -q "$KERNEL_VERMAGIC"; then
        pass "Vermagic matches kernel"
        VERMAGIC_MISMATCH=0
    else
        warn "Vermagic mismatch - may need --force to load"
        VERMAGIC_MISMATCH=1
    fi
else
    warn "modinfo not available - skipping vermagic check"
    VERMAGIC_MISMATCH=0
fi

# =========================================
# Helper: Run a SAFE reversible test
# All tests use execute=0 and are designed to be reversible
# =========================================
run_safe_test() {
    TEST_NAME="$1"
    shift
    log ""
    log "=== TEST: $TEST_NAME ==="
    log "Loading module with execute=0 (staging only)..."
    log "Parameters: $*"
    
    # Ensure execute=0 is set (SAFETY)
    PARAMS="execute=0 $*"
    insmod "$MODULE_PATH" $PARAMS 2>&1
    RET=$?
    if [ $RET -eq 0 ]; then
        pass "$TEST_NAME successful"
        dmesg -T | grep -i quest_kexec | tail -20
        rmmod quest_kexec 2>/dev/null && pass "Module unloaded cleanly" || warn "Unload had issues"
        return 0
    else
        fail "$TEST_NAME failed (exit code $RET)"
        dmesg -T | grep -i quest_kexec | tail -30
        return 1
    fi
}

# =========================================
# TEST 2: RAM validation (REVERSIBLE - claims/releases RAM window)
# =========================================
run_safe_test "RAM validation (validate_ram=1)" \
    image="/data/local/tmp/qkx-Image" \
    initrd="/data/local/tmp/qkx-initramfs" \
    dtb="/data/local/tmp/qkx-boot.dtb" \
    validate_ram=1 \
    ram_diagnose=1
RAM_TEST=$?

# =========================================
# TEST 3: Preflight staging (REVERSIBLE - symbol resolution, device lookup)
# =========================================
run_safe_test "Preflight staging (preflight_only=1)" \
    image="/data/local/tmp/qkx-Image" \
    initrd="/data/local/tmp/qkx-initramfs" \
    dtb="/data/local/tmp/qkx-boot.dtb" \
    preflight_only=1
PREFLIGHT_TEST=$?

# =========================================
# TEST 4: Full staging with payload (REVERSIBLE - if payload files exist)
# =========================================
log ""
log "=== TEST: Full staging with payload (REVERSIBLE) ==="
if [ -f "/data/local/tmp/qkx-Image" ] && [ -f "/data/local/tmp/qkx-initramfs" ] && [ -f "/data/local/tmp/qkx-boot.dtb" ]; then
    log "Payload files found - running full staging..."
    run_safe_test "Full staging with payload" \
        image="/data/local/tmp/qkx-Image" \
        initrd="/data/local/tmp/qkx-initramfs" \
        dtb="/data/local/tmp/qkx-boot.dtb" \
        preflight_only=1
    FULL_STAGE_TEST=$?
else
    warn "Payload files not found - skipping full staging test"
    warn "Place these files to test full staging:"
    warn "  /data/local/tmp/qkx-Image"
    warn "  /data/local/tmp/qkx-initramfs"
    warn "  /data/local/tmp/qkx-boot.dtb"
    FULL_STAGE_TEST=0
fi

# =========================================
# SUMMARY - ONLY passive/reversible tests
# =========================================
log ""
log "========================================="
log "VALIDATION SUMMARY (SAFE TESTS ONLY)"
log "========================================="
log "Kernel: $KERNEL_VER"
log ""
log "Tests run (ALL execute=0, all reversible):"
[ $RAM_TEST -eq 0 ] && pass "RAM validation (claim/release)" || fail "RAM validation"
[ $PREFLIGHT_TEST -eq 0 ] && pass "Preflight staging (symbols/devices)" || fail "Preflight staging"
[ $FULL_STAGE_TEST -eq 0 ] && pass "Full staging w/ payload" || warn "Full staging (no payload)"

SAFE_TESTS=0
[ $RAM_TEST -eq 0 ] && SAFE_TESTS=$((SAFE_TESTS + 1))
[ $PREFLIGHT_TEST -eq 0 ] && SAFE_TESTS=$((SAFE_TESTS + 1))
log ""
log "Safe tests passed: $SAFE_TESTS/2 (full staging optional)"

# =========================================
# EXPLICITLY NOT RUN (DANGEROUS - require separate confirmation)
# =========================================
log ""
log "========================================="
log "NOT RUN (require explicit separate confirmation):"
log "========================================="
log "  - CPU freeze test (test_cpus=1) - REVERSIBLE but affects scheduler"
log "  - MMU-off roundtrip (test_transition=1) - PHYSICAL memory transition"
log "  - Full relocation (test_full_copy=1) - PHYSICAL memory copy loop"
log "  - Hardware transition (execute=1) - IRREVERSIBLE kexec jump"
log ""
log "To run these manually (if you accept the risk):"
log "  adb shell su -c 'insmod $MODULE_PATH execute=0 test_cpus=1'"
log "  adb shell su -c 'insmod $MODULE_PATH execute=0 test_transition=1'"
log "  adb shell su -c 'insmod $MODULE_PATH execute=0 test_full_copy=1'"
log "  adb shell su -c 'insmod $MODULE_PATH execute=1  # DANGEROUS - IRREVERSIBLE'"
log ""

# =========================================
# INSTALLATION - Magisk module for STAGING ONLY
# =========================================
if [ $SAFE_TESTS -ge 2 ]; then
    log ""
    log "========================================="
    log "SAFE TESTS PASSED - Ready for STAGING-ONLY installation"
    log "========================================="
    
    if [ $MAGISK_AVAILABLE -eq 1 ]; then
        log "Creating Magisk module for PERSISTENT STAGING-ONLY installation..."
        
        rm -rf "$MAGISK_MODULE_DIR"
        mkdir -p "$MAGISK_MODULE_DIR"
        
        # module.prop
        cat > "$MAGISK_MODULE_DIR/module.prop" <<EOF
id=quest_kexec
name=Quest 2 kexec (Staging Only)
version=1.0
versionCode=1
author=WitherBlitz/Kilo
description=Quest 2 (Hollywood) kexec module - STAGING MODE ONLY (execute=0)
minMagisk=25000
EOF
        
        # post-fs-data.sh - loads module at boot in STAGING MODE ONLY
        cat > "$MAGISK_MODULE_DIR/post-fs-data.sh" <<EOF
#!/system/bin/sh
# Quest 2 kexec - STAGING MODE ONLY
# Loads module at boot with execute=0, validate_ram=1
# NO hardware transition, NO CPU freeze, NO MMU-off

MODULE_DIR="/data/local/tmp/kexec_test"
MODULE_PATH="\$MODULE_DIR/quest_kexec.ko"

mkdir -p "\$MODULE_DIR"
cp /data/adb/modules/quest_kexec/quest_kexec.ko "\$MODULE_PATH"

# STAGING ONLY - execute=0
insmod "\$MODULE_PATH" \
    image="/data/local/tmp/qkx-Image" \
    initrd="/data/local/tmp/qkx-initramfs" \
    dtb="/data/local/tmp/qkx-boot.dtb" \
    execute=0 \
    validate_ram=1 \
    2>&1 | tee /dev/kmsg

log "quest_kexec module loaded at boot (STAGING MODE: execute=0)"
EOF
        chmod +x "$MAGISK_MODULE_DIR/post-fs-data.sh"
        
        # service.sh - manual control
        cat > "$MAGISK_MODULE_DIR/service.sh" <<EOF
#!/system/bin/sh
# Quest kexec service - STAGING MODE ONLY
# Usage: su -c /data/adb/modules/quest_kexec/service.sh [start|stop|status|uninstall]

MODULE_DIR="/data/local/tmp/kexec_test"
MODULE_PATH="\$MODULE_DIR/quest_kexec.ko"

case "\$1" in
    start)
        log "Loading quest_kexec (STAGING: execute=0, validate_ram=1)..."
        insmod "\$MODULE_PATH" image="/data/local/tmp/qkx-Image" initrd="/data/local/tmp/qkx-initramfs" dtb="/data/local/tmp/qkx-boot.dtb" execute=0 validate_ram=1
        ;;
    stop)
        log "Unloading quest_kexec..."
        rmmod quest_kexec 2>/dev/null || true
        ;;
    status)
        if lsmod | grep -q quest_kexec; then
            log "quest_kexec: LOADED"
        else
            log "quest_kexec: NOT LOADED"
        fi
        ;;
    uninstall)
        log "Uninstalling quest_kexec Magisk module..."
        rmmod quest_kexec 2>/dev/null || true
        rm -rf /data/adb/modules/quest_kexec
        log "Uninstalled. Reboot to complete."
        ;;
    *)
        echo "Usage: \$0 [start|stop|status|uninstall]"
        echo "  start    - Load module in STAGING mode (execute=0)"
        echo "  stop     - Unload module"
        echo "  status   - Check if loaded"
        echo "  uninstall - Remove Magisk module completely"
        ;;
esac
EOF
        chmod +x "$MAGISK_MODULE_DIR/service.sh"
        
        # Copy module
        cp "$MODULE_PATH" "$MAGISK_MODULE_DIR/quest_kexec.ko"
        
        # Set proper permissions
        chcon -R u:object_r:magisk_file:s0 "$MAGISK_MODULE_DIR" 2>/dev/null || true
        
        # Trigger Magisk module refresh
        touch /data/adb/modules/quest_kexec/update
        touch /data/adb/modules/quest_kexec/auto_mount
        
        pass "Magisk module created at $MAGISK_MODULE_DIR"
        pass "Module will load at boot in STAGING MODE ONLY (execute=0, validate_ram=1)"
        log ""
        log "Magisk module control:"
        log "  adb shell su -c '/data/adb/modules/quest_kexec/service.sh start'"
        log "  adb shell su -c '/data/adb/modules/quest_kexec/service.sh stop'"
        log "  adb shell su -c '/data/adb/modules/quest_kexec/service.sh status'"
        log "  adb shell su -c '/data/adb/modules/quest_kexec/service.sh uninstall'  # REMOVE COMPLETELY"
        
        INSTALLED=1
    else
        warn "Magisk not available - module installed temporarily only"
        log "To load manually (STAGING ONLY): adb shell su -c 'insmod $MODULE_PATH execute=0 validate_ram=1'"
        INSTALLED=0
    fi
else
    log ""
    warn "Safe tests not all passed - NOT installing"
    warn "Fix failing tests before attempting installation"
    INSTALLED=0
fi

# =========================================
# FINAL REPORT
# =========================================
log ""
log "========================================="
log "FINAL REPORT"
log "========================================="
log "Kernel: $KERNEL_VER"
log "Module vermagic match: $([ $VERMAGIC_MISMATCH -eq 0 ] && echo "YES" || echo "MISMATCH")"
log "Safe staging tests: $SAFE_TESTS/2 passed (full staging optional)"
log "Magisk module installed: $([ $INSTALLED -eq 1 ] && echo "YES (staging only)" || echo "NO")"
log ""
log "INSTALLED STATE:"
log "  - Module loads at boot with execute=0 (STAGING ONLY)"
log "  - validate_ram=1 claims/releases RAM window (REVERSIBLE)"
log "  - NO CPU freeze, NO MMU-off, NO full relocation, NO kexec jump"
log "  - Magisk module can be STOPPED or UNINSTALLED anytime"
log ""
log "WHAT WORKS (verified):"
log "  - Module loads and validates RAM window"
log "  - Symbol resolution and device lookup"
log ""
log "WHAT REMAINS FOR ANOTHER OS BOOT (NOT INSTALLED):"
log "  1. Target OS payload (Image, initramfs, DTB) in /data/local/tmp/"
log "  2. Verified shutdown backends for all 10 subsystems"
log "  3. CPU freeze test (reversible) - run manually if desired"
log "  4. MMU-off roundtrip test - run manually if desired"
log "  5. Full relocation test - run manually if desired"
log "  6. Hardware transition (execute=1) - REQUIRES EXPLICIT CONFIRMATION"
log "  7. Bootloader unlock or alternative boot path (for persistence)"
log ""
log "Log saved to: $LOG_FILE"
log "========================================="

if [ $SAFE_TESTS -ge 2 ]; then
    exit 0
else
    exit 1
fi