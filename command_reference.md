# AstraOS Native Command Framework — Comprehensive Command Reference

This document provides complete, definitive reference documentation for all command-line utilities and built-in commands supported by the native AstraOS Shell (`astrashell`) and command runtime environment.

---

## 1. System Information & Environment Commands

### `help`
- **Syntax**: `help [command]`
- **Description**: Displays built-in help information, command categories, or detailed syntax for a specific command.
- **Return Code**: `0` on success.
- **Examples**:
  ```bash
  help
  help pkg
  help ls
  ```

---

### `clear`
- **Syntax**: `clear`
- **Description**: Clears the visible display grid and resets cursor positioning to row 0, column 0 in the active terminal context.
- **Return Code**: `0` on success.

---

### `echo`
- **Syntax**: `echo [arguments...]`
- **Description**: Prints arguments separated by single spaces, followed by a newline, to the standard output or redirected stream. Supports variable expansion (`$VAR`, `$?`).
- **Return Code**: `0` on success.
- **Examples**:
  ```bash
  echo Hello, AstraOS!
  echo "Current user is: $USER"
  echo "Last command status: $?"
  ```

---

### `pwd`
- **Syntax**: `pwd`
- **Description**: Prints the normalized absolute path of the current working directory (`cwd`).
- **Return Code**: `0` on success.

---

### `whoami`
- **Syntax**: `whoami`
- **Description**: Outputs the username associated with the current session context (defaults to `root`).
- **Return Code**: `0` on success.

---

### `date`
- **Syntax**: `date`
- **Description**: Queries the hardware Real-Time Clock (RTC) and formats the current date and time in ISO-8601 UTC standard (`YYYY-MM-DD HH:MM:SS UTC`).
- **Return Code**: `0` on success, `1` on hardware query failure.

---

### `uname`
- **Syntax**: `uname [-a | -s | -r | -m]`
- **Description**: Prints operating system and kernel release information.
  - `-a`: Print all information (OS name, release, architecture).
  - `-s`: Print OS kernel name (`AstraOS`).
  - `-r`: Print kernel version (`1.0.0-release`).
  - `-m`: Print machine architecture (`x86_64`).
- **Return Code**: `0` on success.

---

### `hostname`
- **Syntax**: `hostname [new_name]`
- **Description**: Prints the system network hostname, or updates the hostname when provided with an argument.
- **Return Code**: `0` on success.

---

### `env`
- **Syntax**: `env`
- **Description**: Prints all active environment variable key-value pairs stored in the shell context.
- **Return Code**: `0` on success.

---

### `history`
- **Syntax**: `history`
- **Description**: Lists recent commands executed in the active session with line index numbering.
- **Return Code**: `0` on success.

---

### `uptime`
- **Syntax**: `uptime`
- **Description**: Displays system uptime computed from the Local APIC monotonic timer, including seconds elapsed and scheduling cycle statistics.
- **Return Code**: `0` on success.

---

### `cpuinfo`
- **Syntax**: `cpuinfo`
- **Description**: Queries CPUID and SMP topology, reporting active CPU core count, Local APIC IDs, and supported hardware features (NXE, PGE, PCID, SSE, AVX).
- **Return Code**: `0` on success.

---

### `free`
- **Syntax**: `free [-m | -k]`
- **Description**: Displays physical memory statistics retrieved directly from the Physical Page Manager (PMM) and kernel heap allocator.
  - `-m`: Display values in Megabytes (MiB).
  - `-k`: Display values in Kilobytes (KiB) (default).
- **Output Fields**: Total physical RAM, allocated frames, free frames, heap arena capacity, and heap in-use memory.
- **Return Code**: `0` on success.

---

### `exit`
- **Syntax**: `exit [status]`
- **Description**: Terminates the current interactive shell session. In the graphical desktop environment, closes the active Terminal application window.
- **Return Code**: Specified integer or `0`.

---

## 2. Filesystem Management Commands

### `ls`
- **Syntax**: `ls [-l | -a] [path]`
- **Description**: Lists files and directories located at `path` (or current directory if omitted).
  - `-l`: Long listing format showing node type (`DIR` vs `FILE`), byte size, and filename.
  - `-a`: Include hidden files starting with `.`.
- **Return Code**: `0` on success, `1` if path does not exist.

---

### `cd`
- **Syntax**: `cd [path]`
- **Description**: Changes the current working directory to `path`.
  - Supports absolute paths (`/usr/bin`), relative paths (`../docs`), current directory (`.`), and parent (`..`).
  - `cd` without arguments defaults to `/`.
- **Return Code**: `0` on success, `1` if directory is not found.

---

### `cat`
- **Syntax**: `cat <file1> [file2...]`
- **Description**: Concatenates and prints the contents of one or more files to stdout.
- **Return Code**: `0` on success, `1` if file not found or path is a directory.

---

### `touch`
- **Syntax**: `touch <file>`
- **Description**: Creates a new empty file at `<file>` if it does not already exist, or updates its metadata.
- **Return Code**: `0` on success, `1` on VFS error.

---

### `rm`
- **Syntax**: `rm <path>`
- **Description**: Deletes a file from the mutable RAMFS or AstraFS filesystem via `vfs_unlink()`.
- **Return Code**: `0` on success, `1` if file does not exist or cannot be unlinked.

---

### `cp`
- **Syntax**: `cp <source> <destination>`
- **Description**: Copies contents of `<source>` file to `<destination>`.
- **Return Code**: `0` on success, `1` on read/write failure.

---

### `mv`
- **Syntax**: `mv <source> <destination>`
- **Description**: Renames or moves `<source>` to `<destination>` via `vfs_rename()`.
- **Return Code**: `0` on success, `1` on error.

---

### `stat`
- **Syntax**: `stat <path>`
- **Description**: Displays detailed metadata about a file or directory node, including node type, byte size, and internal handle attributes.
- **Return Code**: `0` on success, `1` if target node does not exist.

---

### `df`
- **Syntax**: `df`
- **Description**: Displays storage volume statistics across mounted filesystems (Root RAMFS, AstraFS persistent volume, DevFS). Reports total capacity, used blocks, and available storage.
- **Return Code**: `0` on success.

---

## 3. Process Management Commands

### `ps`
- **Syntax**: `ps`
- **Description**: Inspects the kernel process table (`kernel/process/process.c`), displaying:
  - `PID`: Process Identifier.
  - `NAME`: Binary image name.
  - `STATE`: Current state (`READY`, `RUNNING`, `SLEEPING`, `WAITING`, `DEAD`).
  - `CR3`: Page directory physical base address.
  - `CPU`: Bound APIC CPU core.
- **Return Code**: `0` on success.

---

## 4. Network Diagnostic & Transfer Commands

### `ifconfig` / `ip`
- **Syntax**: `ifconfig` or `ip addr`
- **Description**: Queries the active network interface (`net_get_interface()`), reporting:
  - Network interface name (`eth0`).
  - IPv4 address, Subnet Mask, Gateway, DNS server.
  - Physical MAC address (`xx:xx:xx:xx:xx:xx`).
  - Link operational status (`UP` / `DOWN`).
- **Return Code**: `0` on success.

---

### `ping`
- **Syntax**: `ping <host_or_ip> [-c count]`
- **Description**: Resolves domain name via DNS (if hostname provided), transmits ICMP Echo Request packets, and reports round-trip latency and packet loss.
  - `-c count`: Number of packets to transmit (default: 4).
- **Return Code**: `0` if at least one reply received, `1` if destination unreachable or 100% loss.

---

### `nslookup`
- **Syntax**: `nslookup <domain>`
- **Description**: Performs an explicit DNS query against the configured DNS resolver and displays the corresponding IPv4 A-record.
- **Return Code**: `0` on successful lookup, `1` on NXDOMAIN or timeout.

---

### `curl`
- **Syntax**: `curl [-o destination] <url>`
- **Description**: Fetches resource data over HTTP/1.1 or TLS 1.3 HTTPS.
  - If `-o destination` is specified, writes payload to the given file.
  - Otherwise, dumps textual content directly to terminal stdout.
- **Return Code**: `0` on HTTP 200 OK, `1` on connection or protocol failure.

---

## 5. Native Package Management (`pkg`)

The native package manager (`kernel/pkg/pkg.c`) provides secure, transactional package administration:

### `pkg list`
- **Syntax**: `pkg list`
- **Description**: Lists all currently installed software packages and their versions on AstraOS.

### `pkg search <query>`
- **Syntax**: `pkg search <query>`
- **Description**: Queries the repository index catalog (`packages.idx`) for packages matching `<query>`.

### `pkg info <package>`
- **Syntax**: `pkg info <package>`
- **Description**: Displays detailed metadata about `<package>`, including version, description, download size, dependencies, and digital signature status.

### `pkg install <package>`
- **Syntax**: `pkg install <package>`
- **Description**: Downloads the `.apkg` archive, verifies the SHA-256 payload digest, validates the RSA-2048 PKCS#1 v1.5 digital signature against AstraOS Root Trust, verifies dependencies, stages files in RAMFS, and atomically commits files into `/pkg/bin` and `/pkg/lib`.
- **Return Code**: `0` on successful installation, non-zero error code on failure (with automatic rollback).

### `pkg remove <package>`
- **Syntax**: `pkg remove <package>`
- **Description**: Safely unlinks package binaries and metadata via `vfs_unlink()`, rejecting removal if other installed packages depend on it.

### `pkg update`
- **Syntax**: `pkg update`
- **Description**: Refreshes the local repository index cache from the official AstraOS software repository.

### `pkg upgrade`
- **Syntax**: `pkg upgrade`
- **Description**: Identifies installed packages with newer versions available in the repository index and upgrades them transactionally.

### `pkg clean`
- **Syntax**: `pkg clean`
- **Description**: Cleans temporary staging artifacts and cached `.apkg` archives from `/tmp`.

---

## 6. Graphical Application Launchers

Built-in launcher commands enable users to launch native desktop applications directly from the terminal prompt:

- `terminal`: Spawns a new independent Terminal window instance.
- `editor`: Opens the native AstraOS Graphical Text Editor.
- `browser`: Launches the AstraOS Graphical Web Browser.
- `filemanager`: Launches the AstraOS Desktop File Manager.

---

## 7. Cross-Platform Interoperability Guidance

When foreign operating system commands are entered, AstraOS provides transparent educational guidance rather than misleading mock outputs:

| Foreign Command | Host OS | AstraOS Response / Native Alternative |
|:---|:---|:---|
| `apt` / `apt-get` | Debian / Ubuntu Linux | *"AstraOS Compatibility: 'apt' is a Debian/Ubuntu package manager. Use 'pkg' on AstraOS."* |
| `sudo` | Linux / UNIX | *"AstraOS operates with root supervisor privileges in the native shell."* |
| `systemctl` | Linux (systemd) | *"AstraOS manages kernel services deterministically; use native process tools ('ps')."* |
| `dir` | Windows CMD | *"AstraOS Compatibility: 'dir' is a Windows CMD command. Use AstraOS 'ls'."* |
| `cls` | Windows CMD | *"AstraOS Compatibility: 'cls' is a Windows command. Use AstraOS 'clear'."* |
| `cmd` / `powershell` | Windows | *"AstraOS Compatibility: Running native AstraOS interactive shell. Type 'help' for commands."* |

