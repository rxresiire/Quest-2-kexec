# Quest 2 kexec — Rxresiire / PROJECT_ASTRO

Kernel-exec (kexec) port for Meta Quest 2 (`hollywood`) running the stock
`4.19.325-cip132-st16` kernel (`build 52322490022300150`). Goal: load a custom
kernel Image + initramfs + DTB from the running Quest OS and jump to it without
the bootloader.

**Current status: blocked.** Bootloader unlock requires Meta's internal tool
(https://oculus.com/unlock — employee-only). `fastboot flashing unlock` returns
`Unsupported Operation`; `fastboot oem unlock` redirects to that page. No public
exploit works on this firmware build. Without an unlocked bootloader, no custom
kernel can be flashed and kexec from the stock kernel panics on every jump
attempt.

---

## What works

| Component | Status | Details |
|---|---|---|
| `quest_kexec.ko` module load (staging) | Works | `insmod execute=0` loads, stages all 3 segments (Image, initramfs, DTB), passes CRC/vermagic wall |
| `execute=0 skip_shutdown=0` | Works | No crash, entry point printed `0x90100000`, ~8414 copy pages staged |
| Module symbol lookups | Works | `kprobe` + `kallsyms_lookup_name` bypasses `kptr_restrict=2` |
| `CONFIG_RELOCATABLE=y` | Stock kernel has it | Kernel can self-relocate from code0 to link address at boot |
| CI pipeline | Works | Builds kernel image + module via GitHub Actions, audits config/module CRCs, builds installer APK |
| Installer APK | Built | `app/installer/build/outputs/apk/debug/installer-debug.apk`, connects via ADB TCP, installs as Magisk module |
| Device root | Works | Magisk auto-roots ~15s after reboot; `/product/bin/su` returns `uid=0` |
| Startup script | Installed | `/data/adb/service.d/qkx-boot.sh` — Magisk service.d, auto-runs on boot |
| Payload persistence | On device | `/data/local/tmp/qkx-{Image,quest_kexec_q2.ko,initramfs,boot.dtb}` |

---

## What does NOT work

| Component | Status | Blocker |
|---|---|---|
| `execute=1` kernel jump | Panics every time | q2fork trampoline (`qkx_enter`/`transition.S`) — TTBR0 switch to incomplete page table, no valid stack pointer, missing identity mapping for kernel image at `QKX_ENTRY` |
| `CONFIG_KEXEC=y` native syscall | CI audit rejects | Stock Quest 2 kernel ships kexec disabled; audit script blocks it |
| `fastboot flashing unlock` | `Unsupported Operation` | Qualcomm bootloader doesn't expose unlock via this command |
| `fastboot oem unlock` | Redirects to `oculus.com/unlock` | Meta-internal tool, not publicly available |
| Bootloader unlock exploit | Patched | CVE-2021-1931 fixed in firmware > V29 (May 2021); device is on Sep 2026 build |
| Downgrade to vulnerable firmware | Blocked | Efuses blown on V51+ prevent downgrade |
| `/dev/mem` / `/proc/kcore` | Not present | No direct kernel memory read from userspace |
| `kptr_restrict=2` bypass | Partial | `kprobe` resolves symbols but no arbitrary memory read |

---

## Repository layout

```
clean_repo/
├── .github/workflows/quest2-kexec-cip132-build.yml   # CI: kernel build + module build + APK + audit
├── app/                                               # RXRESIIRE KEXEC INSTALLER APK (Android)
│   ├── installer/                                     # Gradle project
│   └── README.md                                      # APK docs
├── config/
│   ├── loader-q2fork-fixed.c                          # Module loader (stop_machine handoff, identity map, stack fix)
│   ├── q2-stock.config                                # Stock kernel config from /proc/config.gz
│   └── q2-device-crcs.json                            # Device symbol CRCs for module alignment
├── quest2_audit_cip132.py                              # CI audit: config + module symbol CRC gate
└── README.md                                          # This file
```

---

## Key files

### `config/loader-q2fork-fixed.c`
The q2fork loader module. Current version uses `stop_machine()` to park secondary
CPUs, then CPU0 performs an MMU-on TTBR0 switch to an identity-mapped QKX
destination page table (`QKX_LO`–`QKX_HI`) with a dedicated jump stack at
`QKX_HI`, then branches to `qkx_entry` (`QKX_ENTRY + text_offset`).

Known issue: the jump still panics. The page table only maps the QKX window and
jump stack — the new kernel's own page tables are not set up before the branch,
so the first memory access after `br` faults.

### `config/q2-stock.config`
Device's own `/proc/config.gz`, committed as the authoritative config. All
module builds must match this for vermagic/CRC consistency.

### `.github/workflows/quest2-kexec-cip132-build.yml`
CI pipeline: kernel build (clang-r450784e), module build, `quest2_audit_cip132.py`
audit (config SHA gate + module CRC alignment), APK build.

---

## Device info

| Property | Value |
|---|---|
| Codename | `hollywood` (DVT) |
| USB serial | `1WMHHA6JFM2175` |
| Kernel | `4.19.325-cip132-st16-g429559e91fe8` |
| Build | `52322490022300150` (Sep 2026) |
| Bootloader | LOCKED (`Device unlock allowed: true` but no public unlock path) |
| Active slot | `_b` |
| Root | Magisk, auto-roots ~15s after reboot |
| SELinux | Permissive |
| `kptr_restrict` | 2 |
| `CONFIG_KEXEC` | Not set |
| `CONFIG_RELOCATABLE` | y |
| `CONFIG_KALLSYMS_ALL` | y |
| `CONFIG_MODULE_UNLOAD` | y |

---

## Payload addresses (q2fork constants)

| Symbol | Address |
|---|---|
| `QKX_LO` | `0x90000000` |
| `QKX_HI` | `0x93400000` |
| `QKX_ENTRY` (Image load) | `0x90080000` |
| Kernel entry | `QKX_ENTRY + text_offset` = `0x90100000` |
| `QKX_INITRD` | `0x93000000` |
| `QKX_DTB` | `0x93200000` |

Kernel is hard-linked for `0x14688000` (QCOM default). With `CONFIG_RELOCATABLE=y`,
the kernel can copy itself from code0 (`0x90080000`) to its link address
(`0x14688000`) during early boot. `CONFIG_PHYSICAL_START` should be `0x14688000`.

---

## How to reproduce the current state

1. Build via CI: push to `master` on `rxresiire/Quest-2-kexec`
2. Download `quest_kexec.ko` and `Image` from CI artifacts
3. Push to device: `adb push Image /data/local/tmp/qkx-Image` (`.tmp` then `mv`)
4. Stage only: `insmod quest_kexec_q2.ko execute=0 skip_shutdown=0` — works, no crash
5. Jump: `insmod quest_kexec_q2.ko execute=1 skip_shutdown=1` — panics, device reboots

---

## What needs to happen next (open problems)

1. **Bootloader unlock** — Meta's `oculus.com/unlock` is internal-only. Without it,
   no custom kernel can be flashed. This is the fundamental blocker.
2. **kexec jump** — Even with an unlocked bootloader, the q2fork trampoline needs
   a complete page table for the new kernel before branching, not just the QKX
   window mapping. Native `kexec_load` syscall would be preferable but requires
   `CONFIG_KEXEC=y` which CI audit currently blocks.
3. **KASLR leak** — `kptr_restrict=2` hides kernel pointers from root; `kprobe`
   resolves symbols but no arbitrary memory read is available.

---

## License

MIT — see `LICENSE`. Third-party components retain their own licenses.

---

## Disclaimer

This is research code. Unlocking/modifying your Quest 2 bootloader can brick the
device and void warranty. Meta does not provide public bootloader unlock codes.
Use at your own risk.
