#!/usr/bin/env python3
# =============================================================================
# aeon_keygen.py — Sovereign Ed25519 Keypair Generator
# DelgadoLogic | AeonBrowser Sovereign Infrastructure
#
# Generates Ed25519 keypairs for:
#   1. Trust Store Signing  (AEON_TRUST_SIGNER_PUBKEY)
#   2. P2P Update Signing   (trusted_signer in AeonP2PUpdateConfig)
#   3. Manifest Signing     (AeonHive sovereign manifests)
#
# Output:
#   - sovereign-keys/aeon_trust_signing.key   (64-byte private key, KEEP SECRET)
#   - sovereign-keys/aeon_trust_signing.pub   (32-byte public key)
#   - sovereign-keys/aeon_update_signing.key  (64-byte private key, KEEP SECRET)
#   - sovereign-keys/aeon_update_signing.pub  (32-byte public key)
#   - sovereign-keys/aeon_manifest_signing.key
#   - sovereign-keys/aeon_manifest_signing.pub
#   - sovereign-keys/pubkeys.h               (C header with embedded public keys)
#   - sovereign-keys/KEY_INVENTORY.md         (human-readable inventory)
#
# Security Notes:
#   - Private keys (.key) must NEVER be committed to git
#   - .gitignore should exclude *.key in sovereign-keys/
#   - Production builds embed only pubkeys.h (public keys only)
#   - Key ceremony should be performed on an air-gapped machine
#
# Usage:
#   python sovereign-keys/aeon_keygen.py
#   python sovereign-keys/aeon_keygen.py --force   (regenerate even if keys exist)
# =============================================================================

import os
import sys
import hashlib
import time
import argparse
from pathlib import Path

# Use nacl if available, otherwise fallback to pure-python Ed25519
try:
    from nacl.signing import SigningKey
    from nacl.encoding import RawEncoder
    HAS_NACL = True
except ImportError:
    HAS_NACL = False

# Pure-python Ed25519 fallback using hashlib (simplified key generation)
# For production, PyNaCl is strongly recommended.
try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import (
        Encoding, PublicFormat, PrivateFormat, NoEncryption
    )
    HAS_CRYPTOGRAPHY = True
except ImportError:
    HAS_CRYPTOGRAPHY = False


# ---------------------------------------------------------------------------
# Key Purposes
# ---------------------------------------------------------------------------
KEY_PURPOSES = {
    "aeon_trust_signing": {
        "description": "Trust Store Bundle Signing",
        "usage": "Signs Ed25519-verified CA trust bundles distributed via AeonHive",
        "header_const": "AEON_TRUST_SIGNER_PUBKEY",
        "header_file": "sovereign/aeon_tls.h",
        "consumers": ["AeonTLS::ApplyTrustBundle()", "AeonTLSImpl::VerifyBundleSignature()"],
    },
    "aeon_update_signing": {
        "description": "P2P Update Manifest Signing",
        "usage": "Signs update manifests for AeonP2PUpdate distribution",
        "header_const": "AEON_UPDATE_SIGNER_PUBKEY",
        "header_file": "updater/aeon_p2p_update.h",
        "consumers": ["AeonP2PUpdateImpl::VerifyManifest()"],
    },
    "aeon_manifest_signing": {
        "description": "Sovereign Manifest Signing",
        "usage": "Signs AeonHive sovereign manifests (version broadcasts, config updates)",
        "header_const": "AEON_MANIFEST_SIGNER_PUBKEY",
        "header_file": "hive/aeon_hive.h",
        "consumers": ["AeonHive::BroadcastManifest()"],
    },
}


def generate_keypair(purpose_name: str, output_dir: Path, force: bool = False):
    """Generate an Ed25519 keypair for the given purpose."""
    key_path = output_dir / f"{purpose_name}.key"
    pub_path = output_dir / f"{purpose_name}.pub"

    if key_path.exists() and not force:
        print(f"  [SKIP] {purpose_name} — key already exists (use --force to regenerate)")
        # Read existing public key
        with open(pub_path, "rb") as f:
            return f.read()

    if HAS_NACL:
        sk = SigningKey.generate()
        private_bytes = bytes(sk) + bytes(sk.verify_key)  # 64 bytes: seed + pubkey
        public_bytes = bytes(sk.verify_key)  # 32 bytes
    elif HAS_CRYPTOGRAPHY:
        sk = Ed25519PrivateKey.generate()
        private_bytes = sk.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
        public_bytes = sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        # Ed25519 private key in raw format is 32 bytes (seed), we store seed + pubkey
        private_bytes = private_bytes + public_bytes  # 64 bytes
    else:
        # Last resort: use os.urandom for the seed and explain that
        # this needs real Ed25519 to be production-ready
        print(f"  [WARN] No Ed25519 library found — generating raw seed (NOT PRODUCTION SAFE)")
        print(f"         Install: pip install PyNaCl  OR  pip install cryptography")
        seed = os.urandom(32)
        # Derive a deterministic "public key" via SHA-512 (placeholder only)
        h = hashlib.sha512(seed).digest()
        private_bytes = seed + h[:32]  # 64 bytes
        public_bytes = h[:32]  # 32 bytes (NOT a real Ed25519 pubkey)

    # Write private key (secret!)
    with open(key_path, "wb") as f:
        f.write(private_bytes)
    os.chmod(str(key_path), 0o600)  # Owner-only read/write

    # Write public key
    with open(pub_path, "wb") as f:
        f.write(public_bytes)

    fingerprint = hashlib.sha256(public_bytes).hexdigest()[:16]
    print(f"  [DONE] {purpose_name}")
    print(f"         Private: {key_path} ({len(private_bytes)} bytes)")
    print(f"         Public:  {pub_path} ({len(public_bytes)} bytes)")
    print(f"         SHA-256: {fingerprint}...")

    return public_bytes


def format_c_array(data: bytes, indent: str = "    ") -> str:
    """Format binary data as a C uint8_t array initializer."""
    lines = []
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"{indent}{hex_vals},")
    return "\n".join(lines)


def generate_header(keys: dict, output_dir: Path):
    """Generate the C header with all public keys embedded."""
    header_path = output_dir / "pubkeys.h"

    timestamp = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())

    lines = [
        "// =============================================================================",
        "// pubkeys.h — Aeon Sovereign Signing Public Keys (AUTO-GENERATED)",
        f"// Generated: {timestamp}",
        "// Tool: sovereign-keys/aeon_keygen.py",
        "//",
        "// This file contains ONLY public keys. It is safe to commit to version control.",
        "// The corresponding private keys (.key files) must NEVER be committed.",
        "// =============================================================================",
        "",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
    ]

    for purpose_name, purpose_info in KEY_PURPOSES.items():
        pub_bytes = keys.get(purpose_name)
        if not pub_bytes:
            continue

        const_name = purpose_info["header_const"]
        description = purpose_info["description"]
        fingerprint = hashlib.sha256(pub_bytes).hexdigest()

        lines.append(f"// ── {description} {'─' * (60 - len(description))}")
        lines.append(f"// Used by: {', '.join(purpose_info['consumers'])}")
        lines.append(f"// SHA-256: {fingerprint}")
        lines.append(f"static const uint8_t {const_name}[32] = {{")
        lines.append(format_c_array(pub_bytes))
        lines.append("};")
        lines.append("")

    with open(header_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"\n  [HEADER] {header_path}")


def generate_inventory(keys: dict, output_dir: Path):
    """Generate human-readable key inventory."""
    inventory_path = output_dir / "KEY_INVENTORY.md"

    timestamp = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())

    lines = [
        "# Aeon Sovereign Key Inventory",
        "",
        f"**Generated:** {timestamp}  ",
        f"**Generator:** `sovereign-keys/aeon_keygen.py`  ",
        "",
        "> [!CAUTION]",
        "> Private keys (`.key` files) must NEVER be committed to version control.",
        "> Store them in a secure offline location. Loss of private keys means",
        "> inability to sign trust bundles or update manifests.",
        "",
        "## Keys",
        "",
        "| Purpose | Constant | Public Key SHA-256 | Consumers |",
        "|---------|----------|--------------------|-----------|",
    ]

    for purpose_name, purpose_info in KEY_PURPOSES.items():
        pub_bytes = keys.get(purpose_name)
        if not pub_bytes:
            continue

        fingerprint = hashlib.sha256(pub_bytes).hexdigest()[:32] + "..."
        consumers = ", ".join(f"`{c}`" for c in purpose_info["consumers"])
        lines.append(
            f"| {purpose_info['description']} | `{purpose_info['header_const']}` "
            f"| `{fingerprint}` | {consumers} |"
        )

    lines.extend([
        "",
        "## File Layout",
        "",
        "```",
        "sovereign-keys/",
        "├── aeon_keygen.py           # This generator script",
        "├── pubkeys.h                # C header (safe to commit)",
        "├── KEY_INVENTORY.md         # This file (safe to commit)",
        "├── .gitignore               # Excludes .key files",
        "├── aeon_trust_signing.key   # 🔒 SECRET - trust store signing",
        "├── aeon_trust_signing.pub   # Public key",
        "├── aeon_update_signing.key  # 🔒 SECRET - P2P update signing",
        "├── aeon_update_signing.pub  # Public key",
        "├── aeon_manifest_signing.key # 🔒 SECRET - manifest signing",
        "└── aeon_manifest_signing.pub # Public key",
        "```",
        "",
        "## Integration",
        "",
        "After generating keys, update the following files:",
        "",
    ])

    for purpose_name, purpose_info in KEY_PURPOSES.items():
        lines.append(f"### {purpose_info['description']}")
        lines.append(f"- Replace `{purpose_info['header_const']}` in `{purpose_info['header_file']}`")
        lines.append(f"  with the values from `pubkeys.h`")
        lines.append("")

    lines.extend([
        "## Rotation Policy",
        "",
        "- **Trust Store Keys:** Rotate annually. Old key must co-sign new key's first bundle.",
        "- **Update Signing Keys:** Rotate every 6 months. Include transition period.",
        "- **Manifest Keys:** Rotate every 6 months. Old manifests remain valid until expiry.",
        "",
        "## Emergency Key Revocation",
        "",
        "If a private key is compromised:",
        "1. Generate new keypair immediately",
        "2. Sign a revocation notice with the old key (if still available)",
        "3. Broadcast revocation via AeonHive `SovereignManifest` topic",
        "4. Push emergency update with new public key embedded",
        "5. Update `pubkeys.h` and all consuming headers",
        "",
    ])

    with open(inventory_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"  [INVENTORY] {inventory_path}")


def generate_gitignore(output_dir: Path):
    """Ensure .key files are excluded from version control."""
    gitignore_path = output_dir / ".gitignore"

    content = [
        "# Sovereign signing keys — NEVER commit private keys",
        "*.key",
        "",
        "# Allow public keys and generated headers",
        "!*.pub",
        "!pubkeys.h",
        "!KEY_INVENTORY.md",
        "!aeon_keygen.py",
        "!.gitignore",
        "",
    ]

    with open(gitignore_path, "w", encoding="utf-8") as f:
        f.write("\n".join(content))

    print(f"  [GITIGNORE] {gitignore_path}")


def main():
    # Fix Windows console encoding
    import io
    if sys.stdout.encoding != 'utf-8':
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

    parser = argparse.ArgumentParser(
        description="Generate Ed25519 signing keypairs for Aeon sovereign infrastructure"
    )
    parser.add_argument("--force", action="store_true",
                        help="Regenerate keys even if they already exist")
    parser.add_argument("--output", type=str, default=None,
                        help="Output directory (default: sovereign-keys/ relative to script)")
    args = parser.parse_args()

    # Determine output directory
    if args.output:
        output_dir = Path(args.output)
    else:
        output_dir = Path(__file__).resolve().parent

    output_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("  Aeon Sovereign Ed25519 Key Generator")
    print("  DelgadoLogic -- AeonBrowser Infrastructure")
    print("=" * 60)
    print()

    if not HAS_NACL and not HAS_CRYPTOGRAPHY:
        print("  [WARN] No Ed25519 library detected!")
        print("         Install one of:")
        print("           pip install PyNaCl")
        print("           pip install cryptography")
        print("         Proceeding with placeholder generation...")
        print()

    # Generate all keypairs
    keys = {}
    for purpose_name, purpose_info in KEY_PURPOSES.items():
        print(f"\n-- {purpose_info['description']} --")
        pub = generate_keypair(purpose_name, output_dir, force=args.force)
        keys[purpose_name] = pub

    # Generate C header
    print()
    generate_header(keys, output_dir)

    # Generate inventory
    generate_inventory(keys, output_dir)

    # Generate .gitignore
    generate_gitignore(output_dir)

    print()
    print("=" * 60)
    print("  Key generation complete!")
    print()
    print("  Next steps:")
    print("    1. Copy pubkeys.h constants into the consuming headers")
    print("    2. Back up .key files to secure offline storage")
    print("    3. NEVER commit .key files to git")
    print("=" * 60)


if __name__ == "__main__":
    main()
