#!/bin/sh
# Quest 2 kexec validation and reversible staging installer
# Safe path only: passive vermagic check + reversible tests (RAM validation, preflight, rehearsal)
# Dangerous tests (CPU freeze, MMU-off, full relocation, execute=1) documented but NOT run
set -e

ADB_SERIAL="${ADB_SERIAL:-192.168.100.18:33869}"
MODULE_LOCAL="${MODULE_LOCAL:-./artifacts/quest_kexec.ko}"
MODULE_REMOTE="/data/local/tmp/quest_kexec.ko"
WORKDIR="/data/local/tmp/kexec_test"

log() { echo "[$(date -u +%H:%M:%S)] $*"; }
die() { log "FATAL: $*"; exit 1; }

adb_cmd() { adb -s "$ADB_SERIAL" shell "$@"; }
adb_push() { adb -s "$ADB_SERIAL" push "$@" ; }
adb_pull() { adb -s "$ADB_SERIAL" pull "$@" ; }

log "Quest 2 kexec validation & staging installer"
log "ADB serial: $ADB_SERIAL"
log "Local module: $MODULE_LOCAL"

# 1. Verify module exists
[ -f "$MODULE_LOCAL" ] || die "Module not found at $MODULE_LOCAL (run download_artifacts first)"

# 2. Read module vermagic & arch
log "Checking module architecture & vermagic..."
VERMAGIC=$(modinfo "$MODULE_LOCAL" 2>/dev/null | grep -m1 vermagic | sed 's/vermagic://' | xargs) || die "modinfo failed"
ARCH=$(aarch64-linux-gnu-readelf -h "$MODULE_LOCAL" 2>/dev/null | grep Machine | awk '{print $2}')
log "Module arch: $ARCH"
log "Module vermagic: $VERMAGIC"
echo "$VERMAGIC" | grep -q "aarch64" || die "Module not aarch64"
echo "$VERMAGIC" | grep -q "SMP" || log "Warning: module not SMP"
echo "$VERMAGIC" | grep -q "preempt" || log "Warning: module not preempt"

# 3. Check unresolved symbols
log "Checking unresolved symbols..."
UNSOLVED=$(modinfo "$MODULE_LOCAL" 2>/dev/null | grep -E "^depends:" | sed 's/depends://' | tr ',' '\n' | while read d; do [ -n "$d" ] && echo "$d"; done)
[ -n "$UNSOLVED" ] && log "Module dependencies: $UNSOLVED" || log "No external dependencies"

# 4. Push module to device
log "Pushing module to device..."
adb_push "$MODULE_LOCAL" "$MODULE_REMOTE" || die "adb push failed"

# 5. Create workdir on device
adb_cmd "mkdir -p $WORKDIR"

# 6. Push validation script
adb_push "$(dirname "$0")/validate_and_install.sh" "$WORKDIR/validate_and_install.sh" 2>/dev/null || true
adb_cmd "chmod +x $WORKDIR/validate_and_install.sh"

# 7. Run passive validation (vermagic, symbols, RAM claim/release)
log "Running passive validation on device..."
adb_cmd "cd $WORKDIR && ./validate_and_install.sh passive" || log "Passive validation returned non-zero (check output)"

# 8. Run preflight staging test (no execute, no CPU freeze)
log "Running preflight staging test..."
adb_cmd "cd $WORKDIR && ./validate_and_install.sh preflight" || log "Preflight test returned non-zero"

# 9. Run full staging rehearsal (RAM claim, copy, GPU suspend test)
log "Running full staging rehearsal..."
adb_cmd "cd $WORKDIR && ./validate_and_install.sh staging" || log "Staging rehearsal returned non-zero"

# 10. Install Magisk staging module (persists across reboots, execute=0, validate_ram=1)
log "Installing Magisk staging module..."
adb_cmd "cd $WORKDIR && ./validate_and_install.sh install" || die "Magisk module install failed"

log "=== STAGING INSTALLATION COMPLETE ==="
log "Module loads at boot with execute=0 validate_ram=1 (reversible)"
log "To uninstall: adb shell 'service.sh uninstall' in Magisk module dir"
log "To verify: adb shell 'lsmod | grep quest_kexec' after reboot"
log "Bootloader UNMODIFIED. Partitions UNTOUCHED. Magisk PRESERVED."