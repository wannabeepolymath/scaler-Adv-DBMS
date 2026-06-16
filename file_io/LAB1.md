# Lab 1 — File I/O in C++: the kernel journey

**Goal:** trace what actually happens when a C++ program opens and reads a file,
from `std::ifstream` down through libc, the syscall boundary, the VFS, the inode
layer, and the page cache.

## Program

[`reader.cpp`](reader.cpp) opens `test.txt`, reads it line by line, and prints it.

```bash
g++ -std=c++17 -o reader reader.cpp
printf 'hello from lab 1\nsecond line\n' > test.txt
./reader
```

## The inode, observed on disk

`std::ifstream("test.txt")` ultimately resolves a path to an **inode** — the
filesystem's canonical record of a file. On this machine (APFS/macOS):

```
$ stat -f 'inode=%i size=%z blocks=%b links=%l mode=%Sp' test.txt
inode=36299572  size=29  blocks=8  links=1  mode=-rw-r--r--
```

The filename is just a directory entry pointing at inode `36299572`; the inode
holds the size, block map, permissions, link count, and timestamps. `fstat()`
reads exactly this record.

## The syscall sequence

On Linux, `strace -e trace=openat,read,close,fstat,mmap ./reader` shows the
user→kernel boundary directly:

| Syscall  | Role |
|----------|------|
| `openat` | path resolution → inode lookup → permission check → allocate an fd |
| `fstat`  | fetch inode metadata (size, mode, timestamps) for the fd |
| `read`   | copy bytes from the file into a user-space buffer |
| `mmap`   | map the C++ runtime / shared libraries into the address space |
| `close`  | release the fd, drop the inode's open reference |

```
openat(AT_FDCWD, "test.txt", O_RDONLY) = 3
fstat(3, {st_mode=S_IFREG|0644, st_size=29, ...}) = 0
read(3, "hello from lab 1\nsecond line\n", 4096) = 29
read(3, "", 4096)                       = 0    # EOF
close(3)                                = 0
```

> **macOS note:** there is no `strace`/`/proc` here. The equivalent is
> `sudo dtruss -t open,read,close,fstat ./reader` (needs root, and System
> Integrity Protection must allow tracing). The syscall names differ slightly
> (`open` vs `openat`) but the journey is identical.

## What `openat` does inside the kernel

1. **Path resolution** — walk the directory tree from `AT_FDCWD`, component by component.
2. **Inode lookup** — each directory entry maps a name → inode number; the VFS fetches the inode from the filesystem driver (APFS / ext4 / btrfs).
3. **Permission check** — compare the process UID/GID against the inode's mode bits.
4. **fd allocation** — install a `struct file` (current offset + inode pointer) in the process's open-file table and return its index (here, fd `3`).

## Kernel layers

```
std::ifstream
   │  fread / libc buffering
   ▼
read() syscall            ← user / kernel boundary
   ▼
VFS (virtual filesystem switch)
   ▼
filesystem driver (APFS / ext4 / btrfs)
   ▼
page cache                ← warm reads served from RAM, no disk I/O
   ▼
block device driver → disk (only on a cache miss)
```

- The **page cache** means a second read of the same file hits RAM, not the disk.
- `fstat` is cheap: inode metadata lives in the kernel's inode cache.

## Verifying the open fd at runtime

- **Linux:** add a `sleep`, then `ls -l /proc/<pid>/fd` and `stat /proc/<pid>/fd/3`.
- **macOS:** `lsof -p <pid>` lists the same open descriptors and their inodes.

## Takeaways

- Every `std::ifstream` open becomes an `openat`/`open` that traverses the VFS and resolves an inode.
- The fd is a per-process handle; the inode is the kernel's one true record of the file.
- Tracing (`strace`/`dtruss`) exposes the exact syscall boundary between C++ and the kernel.
- Repeated reads are absorbed by the page cache; only cold reads reach the disk.
