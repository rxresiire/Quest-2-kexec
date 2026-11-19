# RXRESIIRE KEXEC INSTALLER (Quest 2 / Hollywood)

Genuine Android application for the Meta Quest 2 running
`4.19.325-cip132-st16-g429559e91fe8` (build `52322490022300150`).

It connects to the headset's adbd over TCP, authenticates with the standard ADB
RSA handshake, uses Magisk `su` for privileged steps, validates a
`quest_kexec.ko` against the running kernel, and installs it as a reversible
Magisk module in staging mode.

## What this app actually does

| Step | Implementation |
| ---- | -------------- |
| Transport | Native ADB wire protocol over TCP: `CNXN`, `AUTH`, `OPEN`, `WRTE`, `OKAY`, `CLSE` |
| Host identity | 2048-bit RSA key generated on first run, published in Android's `RSAPublicKey` limb format, token signed with `SHA1withRSA` |
| Root | `su -c` via Magisk; `id -u` must report `0` before any write |
| Verification | Local SHA-256 plus the module's `vermagic` parsed straight out of its `.modinfo` ELF section |
| Install | Copies the module to `/data/adb/modules/rxresiire_kexec`, writes `module.prop`, `post-fs-data.sh`, `service.sh`, `uninstall.sh` |
| Load | `insmod ... execute=0 validate_ram=1`; verifies the entry appears in `/proc/modules` |
| Rollback | `rmmod`, module directory removal, staging file removal |

Every privileged path goes through Magisk `su`; the app never writes partitions
and never touches the bootloader, AVB or the verified-boot chain.

## What this app deliberately does not do

* **No kernel transition.** The stock kernel is built with `CONFIG_KEXEC=n`, so
  `kexec_load`, `machine_kexec`, the relocation code and the purgatory are not
  present in the image. A module cannot add them. The installer stages and
  validates the module only; `execute=1` is never set.
* **No SPAKE2 pairing.** Android 11+ "Wireless debugging → Pair device with
  pairing code" uses a SPAKE2 exchange over TLS that is a separate service from
  adbd. This app speaks the classic ADB TCP transport and therefore needs adbd
  already reachable on a TCP port (wireless debugging enabled, or `adb tcpip`).
  The default address is `192.168.100.18:33869`.

## Build

```bash
cd app
./gradlew assembleDebug
# app/installer/build/outputs/apk/debug/installer-debug.apk
```

Requirements: JDK 17, Android SDK platform 34, build-tools 34.0.0. The CI
workflow builds, badging-dumps and signature-verifies the same APK on every
push.

## Usage

1. Enable wireless debugging / TCP adbd on the headset and confirm the port.
2. Launch the app, keep `host:port`, press **Connect** and accept the RSA
   fingerprint prompt on the headset.
3. Press **Root** and approve the Magisk prompt.
4. Press **Probe** to confirm device, build, kernel string, Magisk version and
   the `CONFIG_KEXEC` state.
5. Press **Install** and choose the `quest_kexec.ko` produced by the CI build.
6. **Load** to `insmod` in staging mode, **Uninstall** to remove everything.
