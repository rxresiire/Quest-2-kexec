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

CONFIG_SHA = "87e00f44625e573a199fefc9824acd20b722306288e3d12b3fda2a0464246cd9"
VERMAGIC = "4.19.325-cip132-st16-g429559e91fe8 SMP preempt mod_unload modversions aarch64"

REQUIRED_OPTIONS = {
    "CONFIG_ARM64": "y",
    "CONFIG_MODULES": "y",
    "CONFIG_MODULE_UNLOAD": "y",
    "CONFIG_MODVERSIONS": "y",
    "CONFIG_KALLSYMS": "y",
    "CONFIG_KALLSYMS_ALL": "y",
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


def inspect_module(data, generated, stock, *, expected_vermagic=VERMAGIC):
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
        if generated.get(name) != crc or stock.get(name) != crc:
            raise ValueError("Missing or mismatching generated/stock CRC: " + name)
        rows[name] = f"0x{crc:08x}"
    if "module_layout" not in rows or not undefined <= rows.keys():
        raise ValueError("Undefined symbol lacks a version record or module_layout is missing")
    result = {"elf": "ELF64 LE REL AArch64", "vermagic": magic[0],
              "undefined_symbols": sorted(undefined), "versioned_imports": len(rows),
              "crc_rows": rows}
    if b"parmtype=ram_diagnose:bool" in info.split(b"\0"):
        indirect = {}
        for name in ("register_kretprobe", "unregister_kretprobe"):
            crc = stock.get(name)
            if crc is None or generated.get(name) != crc:
                raise ValueError("Missing or mismatching indirect ABI CRC: " + name)
            indirect[name] = f"0x{crc:08x}"
        result["indirect_crc_rows"] = indirect
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
    command = args.loader_command.read_text()
    if "-DQKX_ENABLE_EXPERIMENTAL_JUMP" in command:
        raise ValueError("Experimental jump was enabled in the effective compile command")
    stock_path = Path(args.stock_crcs or Path(__file__).with_name("quest2_stock_crcs_cip132.json"))
    stock = json.loads(stock_path.read_text())
    stock = {key: int(value, 16) for key, value in stock["symbols"].items()}
    generated = load_symvers(args.symvers)
    manifest = {"schema_version": 1, "config_sha256": config_hash,
                "stage_only": True, "hardware_handoff": "unproven",
                "stock_crc_provenance": stock.get("provenance", "unknown"), "modules": []}
    for path in args.modules:
        data = path.read_bytes()
        result = inspect_module(data, generated, stock)
        manifest["modules"].append({"file": path.name, "bytes": len(data),
                                    "sha256": hashlib.sha256(data).hexdigest(), **result})
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    print("Module ELF, vermagic, import coverage and generated/stock CRC gates passed")


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
    audit.add_argument("--loader-command", type=Path, required=True)
    audit.add_argument("--output", type=Path, required=True)
    audit.add_argument("modules", nargs="+", type=Path)
    audit.set_defaults(func=cmd_modules)

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
