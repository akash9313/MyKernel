# AstraOS Software Repository Specification (`packages.idx`)

## 1. Overview & Repository Architecture

The AstraOS Software Repository Infrastructure defines the official publication, indexing, transport, and authentication architecture for distributing binary software packages to AstraOS systems.

AstraOS repositories operate as static, highly verifiable content-addressable software catalogs that can be hosted over:
1. **Network Endpoints**: Secure TLS 1.3 HTTPS servers (`https://pkg.astraos.org/v1/`).
2. **Local / Offline Media**: Bootable installation ISOs, USB drives, or local filesystems (`file:///pkg/repo/`).

---

## 2. Repository Index Specification (`packages.idx`)

The repository catalog is defined by an index manifest titled `packages.idx`. It provides an authenticated manifest of all available packages, their cryptographic fingerprints, dependency graphs, and command mapping associations.

### 2.1 File Format & Encoding
- **Encoding**: UTF-8 plaintext with UNIX newlines (`\n`).
- **Entry Separator**: Blank line (`\n\n`) separating discrete package records.
- **Field Syntax**: Key-value pairs formatted as `<Field>: <Value>`.

### 2.2 Record Schema & Fields

| Field | Required | Description | Example |
|:---|:---:|:---|:---|
| `Package` | **Yes** | Unique package identifier (lowercase, alphanumeric, dashes) | `Package: calc-cli` |
| `Version` | **Yes** | Semantic version string (`MAJOR.MINOR.PATCH`) | `Version: 1.2.0` |
| `Architecture` | **Yes** | Target instruction set architecture (`x86_64`) | `Architecture: x86_64` |
| `Installed-Size` | **Yes** | Total uncompressed disk space required in bytes | `Installed-Size: 45056` |
| `Download-Size` | **Yes** | Total `.apkg` archive transfer size in bytes | `Download-Size: 18432` |
| `Depends` | No | Comma-separated list of required package prerequisites | `Depends: libc, libmath` |
| `Commands` | **Yes** | Comma-separated list of binary commands provided | `Commands: calc, bc` |
| `SHA256` | **Yes** | Hex-encoded SHA-256 digest of the `.apkg` archive | `SHA256: 7f83b165...` |
| `Filename` | **Yes** | Relative path to the `.apkg` binary file | `Filename: pool/calc-cli-1.2.0.apkg` |
| `Description` | **Yes** | Single-line human-readable summary | `Description: Arbitrary precision CLI calculator` |

### 2.3 Reference `packages.idx` Example

```ini
Package: coreutils
Version: 1.0.0
Architecture: x86_64
Installed-Size: 65536
Download-Size: 24576
Depends: 
Commands: cat, ls, touch, rm, cp, mv
SHA256: 4a5b6c7d8e9f0123456789abcdef0123456789abcdef0123456789abcdef0123
Filename: pool/coreutils-1.0.0.apkg
Description: Essential core system command-line utilities

Package: net-tools
Version: 1.0.0
Architecture: x86_64
Installed-Size: 49152
Download-Size: 19456
Depends: coreutils
Commands: ping, ifconfig, nslookup, curl
SHA256: 8f9e0a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f
Filename: pool/net-tools-1.0.0.apkg
Description: TCP/IP networking diagnostic and transfer utilities

Package: editor-pro
Version: 2.1.0
Architecture: x86_64
Installed-Size: 81920
Download-Size: 32768
Depends: coreutils
Commands: nano, vim, edit
SHA256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
Filename: pool/editor-pro-2.1.0.apkg
Description: Advanced terminal and text editing suite
```

---

## 3. Network Transport & Repository Endpoints

### 3.1 Official Network Hierarchy
AstraOS packages are fetched via HTTP/1.1 or TLS 1.3 HTTPS using the native networking stack (`kernel/net/`):

- **Index Endpoint**: `https://pkg.astraos.org/v1/packages.idx`
- **Index Signature**: `https://pkg.astraos.org/v1/packages.idx.sig`
- **Binary Archive**: `https://pkg.astraos.org/v1/pool/<package>-<version>.apkg`

### 3.2 Offline Media Fallback
For air-gapped or offline installations, the package manager checks:
1. `file:///media/iso/packages/packages.idx`
2. `file:///pkg/cache/packages.idx`

---

## 4. Package Publication, Signing & Distribution Workflow

```mermaid
flowchart LR
    A["Source Code & Binaries"] --> B["Build Staging Tree (/bin, /lib)"]
    B --> C["apkg-pack Tool"]
    C --> D["Generate .apkg Archive"]
    D --> E["Calculate SHA-256 Digest"]
    E --> F["Sign with AstraOS Release Key (RSA-2048)"]
    F --> G["Embed Digital Signature Block"]
    G --> H["Update & Sign packages.idx"]
    H --> I["Publish to Repository Mirror"]
```

### Packaging Steps:
1. **Directory Staging**: Binaries are compiled and arranged in root-relative directory layouts (`bin/`, `lib/`, `share/`).
2. **Payload Packing**: Files are serialized into the `.apkg` data payload block.
3. **Digest Computation**: The SHA-256 hash of the payload is computed.
4. **Digital Signing**: The release authority signs the SHA-256 digest with the AstraOS Release Signing Private Key (RSA-2048 PKCS#1 v1.5).
5. **Catalog Registration**: The package entry, checksum, and commands are appended to `packages.idx`, and the index signature is updated.

---

## 5. Security & Verification Guarantees

1. **Replay & Rollback Defense**: The repository index includes monotonic serial numbers and timestamps. The package manager rejects repository updates with timestamps older than the installed local index.
2. **Man-in-the-Middle Protection**: All network communication requires TLS 1.3 with strong cipher suites (`TLS_AES_128_GCM_SHA256`, `TLS_CHACHA20_POLY1305_SHA256`) and X.509 certificate validation against the embedded Root CA.
3. **Payload Tamper Resistance**: Regardless of the transport channel (even over unauthenticated HTTP or untrusted mirrors), a package cannot be installed unless its payload matches its internal SHA-256 digest and that digest is verified by the cryptographic RSA signature of the AstraOS Release Authority.

