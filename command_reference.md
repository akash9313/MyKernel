# AstraOS Native Command Framework — Comprehensive Command Reference

This document provides definitive reference documentation for all command-line utilities, stream processing tools, networking diagnostics, and package management capabilities supported by the native AstraOS Shell (`astrashell`) and command runtime environment.

---

## 1. Shell Control, Execution & Syntax

The AstraOS Shell is a native interactive command interpreter featuring command chaining, in-memory pipelining, stream redirection, and interactive package discovery.

### Command Chaining Operators
- `;` **Sequential Unconditional Execution**:
  Executes commands sequentially from left to right regardless of exit status.
  ```bash
  echo "First" ; echo "Second" ; pwd
  ```
- `&&` **Logical AND Conditional Execution**:
  Executes the right-hand command only if the preceding command succeeded with exit status `0`.
  ```bash
  touch /tmp/test.txt && echo "File created successfully"
  ```
- `||` **Logical OR Conditional Execution**:
  Executes the right-hand command only if the preceding command failed with non-zero exit status.
  ```bash
  cat /nonexistent.txt || echo "File not found fallback"
  ```

### Pipelining (`|`)
Connects the standard output (`stdout`) of a producing command to the standard input (`stdin`) of a consuming command using an 8 KiB in-memory circular pipe buffer (`g_pipe_buf`).
- Pipes execute synchronously without requiring kernel context switches or temp files.
- Example:
  ```bash
  dmesg | grep "APIC"
  ls -l /bin | wc -l
  cat /etc/hosts | sort -r | uniq
  ```

### Stream Redirection
- `> <file>` **Output Redirection (Truncate/Create)**:
  Redirects stdout to `<file>`, creating it if it does not exist or truncating existing content.
  ```bash
  echo "Hello World" > /tmp/hello.txt
  ```
- `>> <file>` **Output Redirection (Append)**:
  Appends stdout to the end of `<file>`.
  ```bash
  date >> /var/log/system.log
  ```
- `< <file>` **Input Redirection**:
  Feeds the content of `<file>` into stdin of the command.
  ```bash
  wc -l < /etc/resolv.conf
  ```

### Interactive Missing-Command Package Flow
When a command is entered that is not present in `/bin` or built-in, but is available as an official signed package in the local repository catalog (`calc`, `htop`), the shell halts execution and renders an interactive prompt:
```text
astrashell: command 'calc' not found
Package 'calc' (v1.0.0, 48 KB) provides this command.
Do you wish to install it?
  [1] Install package
  [2] View package information
  [3] Cancel
Select option (1-3):
```
- **Option `[1]`**: Invokes `pkg install`, validates RSA-2048 PKCS#1 v1.5 cryptographic signatures, unpacks the binary, registers the command in path lookup, and allows immediate execution.
- **Option `[2]`**: Displays comprehensive package metadata (name, version, size, digest, description) and re-prompts with installation options `[1]` and `[3]`.
- **Option `[3]`**: Cancels the prompt cleanly and restores the interactive shell prompt without altering system state.

---

## 2. Terminal Keybindings & Cursor Navigation

The native AstraOS Graphical Terminal application provides full cursor positioning with automatic line wrapping and scrollback buffer management:

| Key / Shortcut | Action | Description |
|:---|:---|:---|
| **Left Arrow** | Cursor Left | Moves cursor one character left within the active input buffer. |
| **Right Arrow** | Cursor Right | Moves cursor one character right within the active input buffer. |
| **Home** (`0x47`) | Start of Line | Jumps cursor immediately to index 0 of the input line. |
| **End** (`0x4F`) | End of Line | Jumps cursor immediately to the end of the input line (`strlen(buffer)`). |
| **Delete** (`0x53`)| Delete Forward | Deletes the character directly under the cursor position. |
| **Backspace** | Delete Backward | Deletes the character preceding the cursor position. |
| **Up Arrow** | History Previous | Recalls earlier command from shell session history. |
| **Down Arrow** | History Next | Recalls later command from shell session history. |
| **Enter** | Execute | Submits line buffer to `shell_execute()`. |
| **Ctrl + C** | Cancel | Aborts active input buffer and prints a fresh prompt. |

---

## 3. Group A: Basic Shell & Environment Commands

### `help`
- **Syntax**: `help [command]`
- **Description**: Displays built-in help information, command categories, or detailed syntax for a specific command.
- **Return Code**: `0` on success.

### `clear`
- **Syntax**: `clear`
- **Description**: Clears the visible display grid and resets cursor positioning to row 0, column 0 in the active terminal context.
- **Return Code**: `0` on success.

### `echo`
- **Syntax**: `echo [-n] [-e] [args...]`
- **Description**: Prints arguments to stdout separated by spaces.
  - `-n`: Do not output trailing newline.
  - `-e`: Interpret backslash escapes (`\n` newline, `\t` tab, `\\` backslash).
- **Return Code**: `0` on success.

### `pwd`
- **Syntax**: `pwd`
- **Description**: Prints the normalized absolute path of the current working directory (`cwd`).
- **Return Code**: `0` on success.

### `whoami`
- **Syntax**: `whoami`
- **Description**: Outputs current username (defaults to `root`).
- **Return Code**: `0` on success.

### `hostname`
- **Syntax**: `hostname [new_name]`
- **Description**: Queries or sets system network hostname.
- **Return Code**: `0` on success.

### `date`
- **Syntax**: `date`
- **Description**: Queries hardware Real-Time Clock (RTC) and prints formatted ISO-8601 UTC date and time (`YYYY-MM-DD HH:MM:SS UTC`).
- **Return Code**: `0` on success.

### `uname`
- **Syntax**: `uname [-a | -s | -r | -m | -v]`
- **Description**: Prints operating system and kernel release information.
  - `-a`: Print all system information.
  - `-s`: Print OS kernel name (`AstraOS`).
  - `-r`: Print kernel version (`1.0.0-release`).
  - `-m`: Print machine architecture (`x86_64`).
  - `-v`: Print build timestamp and release qualification tag.
- **Return Code**: `0` on success.

### `env`
- **Syntax**: `env`
- **Description**: Prints all active environment variables (`PATH`, `USER`, `SHELL`, `HOME`, `OS`).
- **Return Code**: `0` on success.

### `history`
- **Syntax**: `history`
- **Description**: Lists recent commands executed in the current session with index numbering.
- **Return Code**: `0` on success.

### `exit`
- **Syntax**: `exit [code]`
- **Description**: Terminates current shell or closes terminal window.
- **Return Code**: Specified integer or `0`.

### `which`
- **Syntax**: `which <command>`
- **Description**: Searches system `PATH` and built-ins for the location of `<command>`.
- **Return Code**: `0` if found, `1` if not found.

### `type`
- **Syntax**: `type <command>`
- **Description**: Reports whether `<command>` is a built-in shell command, alias, or executable binary.
- **Return Code**: `0` on success.

### `alias`
- **Syntax**: `alias [name=value]`
- **Description**: Defines a command alias or lists all currently active aliases.
- **Return Code**: `0` on success.

### `unalias`
- **Syntax**: `unalias <name>`
- **Description**: Removes alias `<name>`.
- **Return Code**: `0` on success, `1` if alias does not exist.

### `true`
- **Syntax**: `true`
- **Description**: Does nothing; returns success status `0`.
- **Return Code**: `0`.

### `false`
- **Syntax**: `false`
- **Description**: Does nothing; returns failure status `1`.
- **Return Code**: `1`.

### `sleep`
- **Syntax**: `sleep <seconds>`
- **Description**: Pauses execution for `<seconds>` using the kernel monotonic timer.
- **Return Code**: `0` on success.

---

## 4. Group B: Filesystem Navigation & File Manipulation

### `ls`
- **Syntax**: `ls [-l] [-a] [-h] [path]`
- **Description**: Lists directory entries.
  - `-l`: Long format showing permissions, size, and type (`DIR`/`FILE`).
  - `-a`: Include hidden entries starting with `.`.
  - `-h`: Human-readable file sizes (B, K, M).
- **Return Code**: `0` on success, `1` on error.

### `cd`
- **Syntax**: `cd [path | - | ~]`
- **Description**: Changes working directory.
  - `cd -`: Returns to previous working directory (`old_pwd`).
  - `cd ~`: Returns to root `/`.
  - `cd ..`: Moves to parent directory.
- **Return Code**: `0` on success, `1` if path not found.

### `cat`
- **Syntax**: `cat [file...]`
- **Description**: Concatenates file contents to stdout. If no file is specified, reads from stdin.
- **Return Code**: `0` on success, `1` on error.

### `touch`
- **Syntax**: `touch <file>`
- **Description**: Creates empty file or updates timestamp.
- **Return Code**: `0` on success, `1` on failure.

### `mkdir`
- **Syntax**: `mkdir [-p] <path>`
- **Description**: Creates a new directory.
  - `-p`: Create parent directories as needed without failing if directory exists.
- **Return Code**: `0` on success, `1` on error.

### `rmdir`
- **Syntax**: `rmdir <path>`
- **Description**: Removes an empty directory node.
- **Return Code**: `0` on success, `1` if directory not empty or not found.

### `rm`
- **Syntax**: `rm [-r] [-f] <path>`
- **Description**: Removes file or directory nodes.
  - `-r`: Recursive removal.
  - `-f`: Force removal without prompt.
- **Return Code**: `0` on success, `1` on error.

### `cp`
- **Syntax**: `cp <source> <destination>`
- **Description**: Copies file data from `<source>` to `<destination>`.
- **Return Code**: `0` on success, `1` on read/write failure.

### `mv`
- **Syntax**: `mv <source> <destination>`
- **Description**: Renames or moves a filesystem node.
- **Return Code**: `0` on success, `1` on error.

### `stat`
- **Syntax**: `stat <path>`
- **Description**: Prints node metadata (inode, size, blocks, permissions, timestamps).
- **Return Code**: `0` on success, `1` if not found.

### `df`
- **Syntax**: `df`
- **Description**: Displays disk space usage across all mounted filesystems (RAMFS, AstraFS, DevFS).
- **Return Code**: `0` on success.

### `du`
- **Syntax**: `du [-h] [-s] [path]`
- **Description**: Summarizes disk usage of files and directories.
  - `-h`: Human readable units.
  - `-s`: Summary total only.
- **Return Code**: `0` on success.

### `find`
- **Syntax**: `find [path] [-name pattern]`
- **Description**: Traverses directory hierarchy searching for filenames matching `pattern`.
- **Return Code**: `0` on success.

### `tree`
- **Syntax**: `tree [path]`
- **Description**: Displays recursive directory structure in an indented visual hierarchy.
- **Return Code**: `0` on success.

### `head`
- **Syntax**: `head [-n lines] [file]`
- **Description**: Prints first `n` lines of file or stdin (default: 10 lines).
- **Return Code**: `0` on success.

### `tail`
- **Syntax**: `tail [-n lines] [file]`
- **Description**: Prints last `n` lines of file or stdin (default: 10 lines).
- **Return Code**: `0` on success.

### `wc`
- **Syntax**: `wc [-l] [-w] [-c] [file]`
- **Description**: Counts lines (`-l`), words (`-w`), and bytes (`-c`) in file or stdin.
- **Return Code**: `0` on success.

### `grep`
- **Syntax**: `grep [-n] [-i] [-v] <pattern> [file]`
- **Description**: Searches input for matching regular text pattern.
  - `-n`: Prefix lines with 1-based line number.
  - `-i`: Case-insensitive comparison.
  - `-v`: Invert match (output non-matching lines).
- **Return Code**: `0` if matches found, `1` if no match.

### `sort`
- **Syntax**: `sort [-r] [file]`
- **Description**: Sorts lines of text alphabetically.
  - `-r`: Reverse sort order.
- **Return Code**: `0` on success.

### `uniq`
- **Syntax**: `uniq [-c] [-d] [file]`
- **Description**: Filters adjacent duplicate lines.
  - `-c`: Prefix lines with repetition count.
  - `-d`: Only output duplicate lines.
- **Return Code**: `0` on success.

### `diff`
- **Syntax**: `diff <file1> <file2>`
- **Description**: Compares two files line by line and displays unified differences.
- **Return Code**: `0` if files identical, `1` if different.

### `file`
- **Syntax**: `file <path>`
- **Description**: Determines file type via header magic bytes (e.g., ELF64 binary, AstraFS package `.apkg`, text file, directory).
- **Return Code**: `0` on success.

### `basename`
- **Syntax**: `basename <path>`
- **Description**: Strips leading directory path, returning only the filename component.
- **Return Code**: `0` on success.

### `dirname`
- **Syntax**: `dirname <path>`
- **Description**: Strips trailing filename, returning directory path component.
- **Return Code**: `0` on success.

### `realpath`
- **Syntax**: `realpath <path>`
- **Description**: Resolves relative path references (`.`, `..`) into normalized absolute path.
- **Return Code**: `0` on success.

### `readlink`
- **Syntax**: `readlink <path>`
- **Description**: Prints target value of symbolic link.
- **Return Code**: `0` on success.

---

## 5. Group C: Text & Stream Processing Commands

### `printf`
- **Syntax**: `printf <format> [args...]`
- **Description**: Formats and prints text according to format specifiers (`%s`, `%d`, `%x`, `\n`, `\t`).
- **Return Code**: `0` on success.

### `tee`
- **Syntax**: `tee [-a] <file>`
- **Description**: Reads from stdin and writes simultaneously to stdout and `<file>`.
  - `-a`: Append to file rather than overwrite.
- **Return Code**: `0` on success.

### `cut`
- **Syntax**: `cut -d <delim> -f <field> | cut -c <list>`
- **Description**: Extracts sections from lines of text.
  - `-d <delim>`: Field delimiter character (default: tab).
  - `-f <field>`: Field index (1-based).
  - `-c <range>`: Character column slice (`e.g., 1-5`).
- **Return Code**: `0` on success.

### `tr`
- **Syntax**: `tr [-d] <set1> [set2]`
- **Description**: Translates or deletes characters from stdin to stdout.
  - `-d`: Delete characters matching `set1`.
- **Return Code**: `0` on success.

### `paste`
- **Syntax**: `paste [file1] [file2]`
- **Description**: Merges lines of files sequentially separated by tabs.
- **Return Code**: `0` on success.

### `nl`
- **Syntax**: `nl [file]`
- **Description**: Numbers lines of file or stdin.
- **Return Code**: `0` on success.

### `fold`
- **Syntax**: `fold [-w width] [file]`
- **Description**: Wraps input lines to specified character column width (default: 80).
- **Return Code**: `0` on success.

### `rev`
- **Syntax**: `rev [file]`
- **Description**: Reverses lines character-by-character.
- **Return Code**: `0` on success.

### `strings`
- **Syntax**: `strings [-n min_len] <file>`
- **Description**: Scans binary file and prints sequences of printable characters (default min length: 4).
- **Return Code**: `0` on success.

### `hexdump`
- **Syntax**: `hexdump [-C] <file>`
- **Description**: Displays file data in hex and ASCII format.
  - `-C`: Canonical hex+ASCII side-by-side display.
- **Return Code**: `0` on success.

---

## 6. Group D: System Monitoring & Diagnostics

### `ps`
- **Syntax**: `ps`
- **Description**: Displays active kernel tasks and processes with PID, state, bound CPU core, CR3 page directory, and memory allocation.
- **Return Code**: `0` on success.

### `top`
- **Syntax**: `top`
- **Description**: Real-time process and CPU utilization monitor showing dynamic task scheduling metrics.
- **Return Code**: `0` on success.

### `free`
- **Syntax**: `free [-h | -m | -k]`
- **Description**: Displays physical memory (PMM) and heap utilization.
  - `-h`: Human-readable units (KiB, MiB).
  - `-m`: Megabytes.
  - `-k`: Kilobytes.
- **Return Code**: `0` on success.

### `uptime`
- **Syntax**: `uptime`
- **Description**: Displays elapsed time since kernel boot and APIC tick count.
- **Return Code**: `0` on success.

### `cpuinfo`
- **Syntax**: `cpuinfo`
- **Description**: Queries CPUID and SMP topology, displaying detected cores, APIC IDs, and hardware CPU features (NX, PGE, PCID, SSE, AVX).
- **Return Code**: `0` on success.

### `meminfo`
- **Syntax**: `meminfo`
- **Description**: Displays detailed memory subsystem metrics including PMM frame pools, heap arena size, slab caches, and page tables.
- **Return Code**: `0` on success.

### `dmesg`
- **Syntax**: `dmesg`
- **Description**: Prints kernel circular log ring buffer messages from boot and device discovery.
- **Return Code**: `0` on success.

### `lsblk`
- **Syntax**: `lsblk`
- **Description**: Lists block storage devices (ATA primary/secondary, AHCI SATA ports, NVMe, RAM disks) and partition layouts.
- **Return Code**: `0` on success.

### `mount`
- **Syntax**: `mount`
- **Description**: Lists all currently mounted filesystems, mount points, and filesystem drivers (`ramfs`, `astrafs`, `devfs`).
- **Return Code**: `0` on success.

### `umount`
- **Syntax**: `umount <path>`
- **Description**: Safely unmounts filesystem volume at `<path>`, ensuring all dirty blocks are flushed to storage.
- **Return Code**: `0` on success, `1` if busy or not found.

---

## 7. Group E: Network Diagnostics & Transfers

### `ip`
- **Syntax**: `ip [addr | route | link]`
- **Description**: Multi-purpose network inspection tool.
  - `ip addr`: Displays IP addresses, subnet masks, and interface state.
  - `ip route`: Displays IPv4 default gateway and routing table.
  - `ip link`: Displays MAC addresses and hardware link status.
- **Return Code**: `0` on success.

### `ifconfig`
- **Syntax**: `ifconfig`
- **Description**: Displays network interface configuration (`eth0`, IP, Netmask, Gateway, MAC address).
- **Return Code**: `0` on success.

### `ping`
- **Syntax**: `ping [-c count] <host_or_ip>`
- **Description**: Sends ICMP Echo Request packets to test network connectivity and measures round-trip latency.
  - `-c <count>`: Number of packets (default: 4).
- **Return Code**: `0` on reply, `1` on timeout.

### `nslookup`
- **Syntax**: `nslookup <domain>`
- **Description**: Queries configured DNS server for IPv4 A-record of `<domain>`.
- **Return Code**: `0` on resolution, `1` on NXDOMAIN or failure.

### `curl`
- **Syntax**: `curl [-o destination] <url>`
- **Description**: Fetches web resources over HTTP/1.1 or TLS 1.3 HTTPS.
  - `-o <file>`: Save response payload to `<file>` instead of stdout.
- **Return Code**: `0` on HTTP 200, `1` on connection or transport error.

### `wget`
- **Syntax**: `wget [-O destination] <url>`
- **Description**: Downloads resource from HTTP/HTTPS URL and writes to disk.
  - `-O <file>`: Output filename destination.
- **Return Code**: `0` on success, `1` on error.

### `netstat`
- **Syntax**: `netstat`
- **Description**: Lists active TCP/UDP sockets, connection states (`LISTEN`, `ESTABLISHED`, `TIME_WAIT`), and local/remote IP endpoints.
- **Return Code**: `0` on success.

---

## 8. Native Package Manager (`pkg`)

The AstraOS package manager enforces cryptographic integrity and transaction safety:

### `pkg list`
- **Syntax**: `pkg list`
- **Description**: Lists installed software packages with version and installation timestamp.

### `pkg search <query>`
- **Syntax**: `pkg search <query>`
- **Description**: Searches package repository index for matching terms.

### `pkg info <package>`
- **Syntax**: `pkg info <package>`
- **Description**: Displays package metadata: version, size, dependencies, description, and cryptographic digest.

### `pkg install <package>`
- **Syntax**: `pkg install <package>`
- **Description**: Locates `.apkg` archive, validates RSA-2048 PKCS#1 v1.5 signature against Root CA, extracts binary to `/pkg/bin`, registers dependencies, and commits transaction.

### `pkg remove <package>`
- **Syntax**: `pkg remove <package>`
- **Description**: Safely removes package binary and unregisters dependencies.

### `pkg update`
- **Syntax**: `pkg update`
- **Description**: Refreshes repository metadata catalog. In offline freestanding environments, returns honest status:
  `BLOCKED: Offline freestanding environment; remote host unreachable`.

### `pkg upgrade`
- **Syntax**: `pkg upgrade`
- **Description**: Upgrades installed packages with available newer releases. In offline environments, reports:
  `BLOCKED: Offline freestanding environment; remote host unreachable`.

### `pkg clean`
- **Syntax**: `pkg clean`
- **Description**: Deletes cached archives and temporary staging data from `/tmp`.

### `pkg repo list`
- **Syntax**: `pkg repo list`
- **Description**: Displays configured software repository endpoints and security trust roots.

---

## 9. Cross-Platform Interoperability Guidance

When foreign operating system commands are entered, AstraOS provides educational guidance rather than misleading mock outputs:

| Foreign Command | Host OS | AstraOS Response / Native Alternative |
|:---|:---|:---|
| `apt` / `apt-get` | Debian / Ubuntu Linux | *"AstraOS Compatibility: 'apt' is a Debian/Ubuntu package manager. Use 'pkg' on AstraOS."* |
| `sudo` | Linux / UNIX | *"AstraOS operates with root supervisor privileges in the native shell."* |
| `systemctl` | Linux (systemd) | *"AstraOS manages kernel services deterministically; use native process tools ('ps')."* |
| `dir` | Windows CMD | *"AstraOS Compatibility: 'dir' is a Windows CMD command. Use AstraOS 'ls'."* |
| `cls` | Windows CMD | *"AstraOS Compatibility: 'cls' is a Windows command. Use AstraOS 'clear'."* |
| `cmd` / `powershell` | Windows | *"AstraOS Compatibility: Running native AstraOS interactive shell. Type 'help' for commands."* |
