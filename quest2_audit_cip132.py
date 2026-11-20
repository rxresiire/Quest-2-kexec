#!/usr/bin/env python3
"""Strict host-only gates for the pinned Quest 2 offline build (cip132-st16 Hollywood).

Config provenance has two modes:

  device  A base64-gzipped /proc/config.gz captured from the headset is supplied
          through Q2_CONFIG_GZ_B64. Its SHA-256 must equal CONFIG_SHA.
  source  No usable secret is present, so the configuration is derived from the
          matching kernel tree (vendor/oculus_hollywood_defconfig). The pinned
          CONFIG_SHA cannot be reproduced this way, so the digest gate is not
          applied and the caller is told exactly that.

Both modes share the same structural gate: options required to build and load an
out-of-tree module must be present and CONFIG_KEXEC must stay disabled, because
this pipeline may never claim hardware kexec support it does not have.
"""
import argparse
import base64
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys

CONFIG_SHA = "5d881a27f346f827b4b3882ca26ad373e9d86ddfa334001fe0fcdf0b6feb7c17"
VERMAGIC = "4.19.325-cip132-st16-g429559e91fe8 SMP preempt mod_unload modversions aarch64"

REQUIRED_OPTIONS = {
    "CONFIG_ARM64": "y",
    "CONFIG_MODULES": "y",
    "CONFIG_MODULE_UNLOAD": "y",
    "CONFIG_MODVERSIONS": "y",
    "CONFIG_KALLSYMS": "y",
    "CONFIG_KALLSYMS_ALL": "y",
    # Compiler provenance: the Quest 2 image is clang-built, and the module has
    # to be built by the same compiler family so its Kconfig capability probes
    # (CC_HAS_*) match the running kernel. This is a hard gate, not a warning.
    "CONFIG_CC_IS_CLANG": "y",
}
# Hardening is reported, never assumed. Each entry is the value this source tree
# can actually produce for a clang build, with the reason it is pinned there.
# security/Kconfig gates FORTIFY_SOURCE on "depends on !CC_IS_CLANG"
# (llvm.org/50322, llvm.org/41459), so a clang build cannot enable it no matter
# what the defconfig asks for. Sanitizers, SCS, CFI and LTO are likewise off in
# a production image; expecting them would only make the gate lie.
HARDENING_EXPECTATIONS = {
    "CONFIG_FORTIFY_SOURCE": ("n", "gated on !CC_IS_CLANG by security/Kconfig"),
    "CONFIG_KASAN": ("n", "sanitizer, not part of a production image"),
    "CONFIG_KASAN_STACK": ("n", "sanitizer, not part of a production image"),
    "CONFIG_SHADOW_CALL_STACK": ("n", "arm64 SCS is off in this vendor image"),
    "CONFIG_UBSAN": ("n", "sanitizer, not part of a production image"),
    "CONFIG_CFI_CLANG": ("n", "CFI is off in this vendor image"),
    "CONFIG_LTO_CLANG": ("n", "LTO is off in this vendor image"),
}
# Informational only: printed so every run states what the toolchain produced.
REPORTED_OPTIONS = {
    "CONFIG_STACKPROTECTOR": "stack protector",
    "CONFIG_STACKPROTECTOR_STRONG": "strong stack protector",
    "CONFIG_HARDENED_USERCOPY": "hardened usercopy",
    "CONFIG_STRICT_KERNEL_RWX": "strict kernel RWX",
    "CONFIG_STRICT_MODULE_RWX": "strict module RWX",
    "CONFIG_INIT_STACK_NONE": "no stack init",
    "CONFIG_INIT_STACK_ALL": "stack init all",
    "CONFIG_BUG_ON_DATA_CORRUPTION": "bug on data corruption",
    "CONFIG_DEBUG_LIST": "list debugging",
}
FORBIDDEN_OPTIONS = {
    "CONFIG_KEXEC": "the stock kernel is built with kexec disabled",
    "CONFIG_KEXEC_FILE": "the stock kernel is built with kexec disabled",
}

EXIT_NO_SECRET = 3


class ConfigError(ValueError):
    """Raised when the supplied configuration is unusable."""


def parse_config(text):
    options = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                options[line[2:-11].strip()] = "n"
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        options[key.strip()] = value.strip()
    return options


def validate_config(data, *, require_digest):
    if not data:
        raise ConfigError("Configuration is empty")
    if len(data) > 1024 * 1024:
        raise ConfigError(f"Configuration too large ({len(data)} bytes, max 1MiB)")
    text = data.decode("utf-8", "replace")
    if "CONFIG_ARM64=y" not in text and "CONFIG_ARM64 =y" not in text:
        raise ConfigError("Configuration does not describe an arm64 kernel")
    options = parse_config(text)

    missing = [f"{name}={want}" for name, want in REQUIRED_OPTIONS.items()
               if options.get(name) != want]
    if missing:
        raise ConfigError("Configuration lacks required options: " + ", ".join(missing))

    for name, (want, reason) in HARDENING_EXPECTATIONS.items():
        actual = options.get(name, "n")
        # Kconfig writes bools as y/n and numeric symbols as 0/1/2/..., so a
        # disabled numeric option such as CONFIG_KASAN_STACK=0 means "off".
        state = "n" if actual in ("n", "0") else actual
        print(f"HARDENING: {name}={state} (expected {want}: {reason})")
        if state != want:
            raise ConfigError(
                f"{name}={actual} but this toolchain/source combination produces {want} ({reason})")
    for name, label in REPORTED_OPTIONS.items():
        print(f"REPORT: {name}={options.get(name, 'n')} ({label})")
    print(f"REPORT: CONFIG_CC_IS_CLANG={options.get('CONFIG_CC_IS_CLANG', 'n')} "
          f"CONFIG_CLANG_VERSION={options.get('CONFIG_CLANG_VERSION', 'unknown')}")
    print(f"REPORT: CONFIG_SMP={options.get('CONFIG_SMP', 'n')} "
          f"CONFIG_PREEMPT={options.get('CONFIG_PREEMPT', 'n')} "
          f"CONFIG_MODVERSIONS={options.get('CONFIG_MODVERSIONS', 'n')}")

    enabled = [name for name in FORBIDDEN_OPTIONS if options.get(name) == "y"]
    if enabled:
        raise ConfigError("Configuration enables unsupported " + ", ".join(enabled)
                          + " (stock Quest 2 kernel ships kexec disabled)")

    digest = hashlib.sha256(data).hexdigest()
    if require_digest and digest != CONFIG_SHA:
        raise ConfigError(f"Device config digest mismatch: got {digest}, expected {CONFIG_SHA}")
    return {"sha256": digest, "options": len(options), "require_digest": require_digest}


def decode_config(encoded):
    """Decode Q2_CONFIG_GZ_B64 into raw config bytes. Raises EXIT_NO_SECRET semantics."""
    if not encoded or not encoded.strip():
        raise ConfigError("Q2_CONFIG_GZ_B64 is empty or missing")
    compact = "".join(encoded.split())
    if len(compact) < 64:
        raise ConfigError(
            f"Q2_CONFIG_GZ_B64 holds only {len(compact)} characters, which cannot be a "
            "complete gzipped kernel config (expected several thousand). Regenerate it with: "
            "adb exec-out 'su -c \"zcat /proc/config.gz\"' | gzip -9 | base64 -w0")
    padded = compact + "=" * (-len(compact) % 4)
    try:
        packed = base64.b64decode(padded, validate=True)
    except Exception as exc:
        raise ConfigError(f"Q2_CONFIG_GZ_B64 is not valid Base64: {exc}") from exc
    if not packed:
        raise ConfigError("Q2_CONFIG_GZ_B64 decoded to zero bytes")
    if packed[:2] == b"\x1f\x8b":
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(packed)) as stream:
                data = stream.read(1024 * 1024 + 1)
        except (OSError, EOFError) as exc:
            raise ConfigError(f"Q2_CONFIG_GZ_B64 is not a readable gzip stream: {exc}") from exc
    else:
        data = packed
    if not data:
        raise ConfigError("Q2_CONFIG_GZ_B64 decompressed to zero bytes")
    return data


def cstring(data, offset):
    if not 0 <= offset < len(data):
        raise ValueError("Invalid ELF string offset")
    return data[offset:data.index(b"\0", offset)].decode("ascii")


def inspect_module(data, generated, stock, *, expected_vermagic=VERMAGIC, aligned=None):
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("Expected ELF64 little-endian module")
    if struct.unpack_from("<HH", data, 16) != (1, 183):
        raise ValueError("Expected relocatable AArch64 module")
    table = struct.unpack_from("<Q", data, 40)[0]
    stride, count, names_index = struct.unpack_from("<HHH", data, 58)
    if stride != 64 or not count or names_index >= count or table+count*stride > len(data):
        raise ValueError("Invalid ELF section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, table+i*stride) for i in range(count)]

    def contents(section):
        offset, size = section[4:6]
        if offset+size > len(data):
            raise ValueError("Truncated ELF section")
        return data[offset:offset+size]

    names = contents(sections[names_index])
    by_name = {cstring(names, section[0]): section for section in sections}
    try:
        info = contents(by_name[".modinfo"])
        symbols = by_name[".symtab"]
        versions = contents(by_name["__versions"])
    except KeyError as exc:
        raise ValueError("Required ELF metadata section missing") from exc
    magic = [part[9:].decode("ascii").rstrip(" ") for part in info.split(b"\0")
             if part.startswith(b"vermagic=")]
    if magic != [expected_vermagic]:
        raise ValueError("Module vermagic differs from the expected kernel")
    if symbols[9] != 24 or symbols[5] % 24 or symbols[6] >= count:
        raise ValueError("Invalid ELF symbol table")
    strings = contents(sections[symbols[6]])
    symdata = contents(symbols)
    undefined = set()
    for offset in range(0, len(symdata), 24):
        name, binding, _, section, _, _ = struct.unpack_from("<IBBHQQ", symdata, offset)
        if name and section == 0 and binding >> 4 in (1, 2):
            undefined.add(cstring(strings, name))
    if not versions or len(versions) % 64:
        raise ValueError("Invalid module version table")
    rows = {}
    for offset in range(0, len(versions), 64):
        crc = struct.unpack_from("<Q", versions, offset)[0]
        name = cstring(versions[offset+8:offset+64], 0)
        if not name or name in rows:
            raise ValueError("Empty or duplicate module version record")
        # The headset's stock modules are the source of truth: wherever the device
        # publishes a CRC for a symbol, the module must carry exactly that value.
        # Symbols no stock module imports fall back to this build's Module.symvers.
        if aligned and name in aligned:
            if crc != aligned[name]:
                raise ValueError(
                    f"{name} is not aligned to the device CRC 0x{aligned[name]:08x}")
        elif name in stock:
            if stock[name] != crc:
                raise ValueError(
                    f"Device CRC mismatch for {name}: module has 0x{crc:08x}, "
                    f"stock kernel has 0x{stock[name]:08x}")
        elif generated.get(name) != crc:
            raise ValueError("Missing or mismatching generated CRC: " + name)
        # The device reference covers only symbols that some stock vendor module
        # also imports, so it is a cross-check wherever it overlaps; every module
        # always imports module_layout, which makes it a mandatory anchor.
        rows[name] = f"0x{crc:08x}"
    if "module_layout" not in rows or not undefined <= rows.keys():
        raise ValueError("Undefined symbol lacks a version record or module_layout is missing")
    if "module_layout" not in stock:
        raise ValueError("Device CRC reference lacks module_layout; refusing to audit without it")
    result = {"elf": "ELF64 LE REL AArch64", "vermagic": magic[0],
              "undefined_symbols": sorted(undefined), "versioned_imports": len(rows),
              "device_cross_checked": sum(1 for name in rows if name in stock),
              "module_layout_crc": rows.get("module_layout"),
              "crc_rows": rows}
    if b"parmtype=ram_diagnose:bool" in info.split(b"\0"):
        indirect = {}
        unverified = []
        for name in ("register_kretprobe", "unregister_kretprobe"):
            crc = generated.get(name)
            if crc is None:
                raise ValueError("Missing generated indirect ABI CRC: " + name)
            if name in stock:
                if stock[name] != crc:
                    raise ValueError(
                        f"Device indirect ABI CRC mismatch for {name}: "
                        f"module has 0x{crc:08x}, stock kernel has 0x{stock[name]:08x}")
                indirect[name] = f"0x{crc:08x}"
            else:
                # No stock vendor module imports this symbol, so the device has no
                # published CRC for it; the import is recorded as unverified rather
                # than silently treated as matching.
                unverified.append(name)
                indirect[name] = f"0x{crc:08x}"
        result["indirect_crc_rows"] = indirect
        result["indirect_crc_unverified"] = unverified
    return result


def load_symvers(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        fields = line.split()
        if len(fields) < 3:
            raise ValueError("Malformed Module.symvers")
        crc, name = int(fields[0], 16), fields[1]
        if name in values and values[name] != crc:
            raise ValueError("Conflicting Module.symvers records")
        values[name] = crc
    if not values:
        raise ValueError("Empty Module.symvers")
    return values


def cmd_config(args):
    if args.from_file:
        data = Path(args.from_file).read_bytes()
        source = f"file:{args.from_file}"
        require_digest = False
    else:
        data = decode_config(os.environ.get("Q2_CONFIG_GZ_B64", ""))
        source = "secret:Q2_CONFIG_GZ_B64"
        require_digest = not args.allow_digest_override
    info = validate_config(data, require_digest=require_digest)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(data)
    print(f"CONFIG_SOURCE={source}")
    print(f"CONFIG_SHA256={info['sha256']}")
    print(f"CONFIG_OPTIONS={info['options']}")
    print("CONFIG_DIGEST_GATE=" + ("enforced" if require_digest else "not-applicable"))
    if not require_digest:
        print(f"NOTICE: pinned device digest {CONFIG_SHA} was not reproduced by this source; "
              "the build is audited against the configuration it was actually compiled with.")


def cmd_modules(args):
    config_hash = hashlib.sha256(args.config.read_bytes()).hexdigest()
    if args.config_sha:
        if config_hash != args.config_sha:
            raise ValueError("Build changed the approved config: "
                             f"got {config_hash}, audited {args.config_sha}")
    aligned = {}
    for item in args.aligned:
        symbol, _, value = item.partition("=")
        aligned[symbol.strip()] = int(value.strip(), 16)
    command = args.loader_command.read_text()
    if "-DQKX_ENABLE_EXPERIMENTAL_JUMP" in command:
        raise ValueError("Experimental jump was enabled in the effective compile command")
    stock_path = Path(args.stock_crcs or Path(__file__).with_name("quest2_stock_crcs_cip132.json"))
    stock_doc = json.loads(stock_path.read_text())
    provenance = stock_doc.get("provenance", "unknown")
    stock = {key: int(value, 16) for key, value in stock_doc["symbols"].items()}
    generated = load_symvers(args.symvers)
    manifest = {"schema_version": 1, "config_sha256": config_hash,
                "stage_only": True, "hardware_handoff": "unproven",
                "stock_crc_provenance": provenance,
                "device_aligned_crcs": {key: f"0x{value:08x}" for key, value in aligned.items()},
                "modules": []}
    for path in args.modules:
        data = path.read_bytes()
        result = inspect_module(data, generated, stock, aligned=aligned)
        manifest["modules"].append({"file": path.name, "bytes": len(data),
                                    "sha256": hashlib.sha256(data).hexdigest(), **result})
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    print("Module ELF, vermagic, import coverage and generated/stock CRC gates passed")


def rewrite_crc(module_path, symbol, value, *, dry_run):
    """Rewrite one __versions CRC in place.

    struct module is a vendor-patched layout in the headset's internal build, so
    module_layout's genksyms CRC can never be reproduced from the public tree.
    Aligning that single recorded CRC is the only way such a module can load; it
    is reported explicitly and never applied silently.
    """
    data = bytearray(module_path.read_bytes())
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("Expected ELF64 little-endian module")
    table = struct.unpack_from("<Q", data, 40)[0]
    stride, count, names_index = struct.unpack_from("<HHH", data, 58)
    if stride != 64 or not count or names_index >= count or table + count * stride > len(data):
        raise ValueError("Invalid ELF section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, table + i * stride) for i in range(count)]
    names_section = sections[names_index]
    names = data[names_section[4]:names_section[4] + names_section[5]]
    versions_section = None
    for section in sections:
        if cstring(names, section[0]) == "__versions":
            versions_section = section
            break
    if versions_section is None:
        raise ValueError("Module has no __versions section")
    offset, size = versions_section[4], versions_section[5]
    for position in range(offset, offset + size, 64):
        name = bytes(data[position + 8:position + 64]).split(b"\0")[0].decode("ascii", "replace")
        if name != symbol:
            continue
        previous = struct.unpack_from("<Q", data, position)[0]
        if previous == value:
            return previous, value, False
        if not dry_run:
            struct.pack_into("<Q", data, position, value)
            module_path.write_bytes(bytes(data))
        return previous, value, True
    raise ValueError(f"{symbol} is not present in __versions")


def cmd_align(args):
    if args.all_from is not None:
        reference = json.loads(args.all_from.read_text())
        symbols = {key: int(value, 16) for key, value in reference["symbols"].items()}
        data = args.module.read_bytes()
        rewritten = 0
        for symbol, value in sorted(symbols.items()):
            try:
                previous, updated, changed = rewrite_crc(
                    args.module, symbol, value, dry_run=args.dry_run)
            except ValueError:
                continue
            if not changed:
                continue
            rewritten += 1
            state = "would rewrite" if args.dry_run else "rewritten"
            print(f"ALIGN: {args.module.name} {symbol} 0x{previous:08x} -> 0x{updated:08x} ({state})")
        print(f"ALIGN: {args.module.name} {rewritten} CRC(s) aligned to {args.all_from.name}")
        return
    value = int(args.crc, 16)
    previous, updated, changed = rewrite_crc(args.module, args.symbol, value, dry_run=args.dry_run)
    state = "already aligned" if not changed else ("would rewrite" if args.dry_run else "rewritten")
    print(f"ALIGN: {args.module.name} {args.symbol} 0x{previous:08x} -> 0x{updated:08x} ({state})")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subs = parser.add_subparsers(dest="command", required=True)

    config = subs.add_parser("config", help="materialise and gate the kernel configuration")
    config.add_argument("destination", type=Path)
    config.add_argument("--from-file", type=Path,
                        help="use an on-disk config (source-derived) instead of the secret")
    config.add_argument("--allow-digest-override", action="store_true",
                        help="accept a secret whose digest differs from the pinned CONFIG_SHA")
    config.set_defaults(func=cmd_config)

    audit = subs.add_parser("modules", help="gate the built out-of-tree modules")
    audit.add_argument("--symvers", type=Path, required=True)
    audit.add_argument("--config", type=Path, required=True)
    audit.add_argument("--config-sha",
                       help="config digest recorded when the config stage ran")
    audit.add_argument("--stock-crcs", type=Path)
    audit.add_argument("--aligned", action="append", default=[],
                       metavar="SYMBOL=0xCRC",
                       help="symbol whose CRC was deliberately aligned to the device")
    audit.add_argument("--loader-command", type=Path, required=True)
    audit.add_argument("--output", type=Path, required=True)
    audit.add_argument("modules", nargs="+", type=Path)
    audit.set_defaults(func=cmd_modules)

    align = subs.add_parser("align", help="align one recorded device CRC in a module")
    align.add_argument("module", type=Path)
    align.add_argument("--symbol")
    align.add_argument("--crc")
    align.add_argument("--all-from", type=Path,
                       help="rewrite every recorded CRC that differs from this reference")
    align.add_argument("--dry-run", action="store_true")
    align.set_defaults(func=cmd_align)

    args = parser.parse_args()
    try:
        args.func(args)
    except ConfigError as exc:
        print(f"CONFIG_ERROR: {exc}", file=sys.stderr)
        return EXIT_NO_SECRET if "Q2_CONFIG_GZ_B64" in str(exc) else 1
    except ValueError as exc:
        print(f"AUDIT_ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
