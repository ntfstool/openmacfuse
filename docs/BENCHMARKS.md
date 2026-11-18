# Benchmarks: OpenMacFUSE and FUSE-T 1.0.49

Every number on this page was measured by the NTFSTool engineering team. Cells that were not measured say "not measured". Nothing is estimated.

## What is compared

Both sides run the same FUSE file system source: the NTFS driver from NTFSTool Pro, a FUSE 2.x high-level file system. Only the library it links against changes.

| Build | Links against | What serves NFS |
|---|---|---|
| OpenMacFUSE | `libopenmacfuse.a` (static) | the file system process itself |
| FUSE-T | `libfuse-t.dylib` from FUSE-T 1.0.49 | the separate NFS server process that FUSE-T ships |

The NTFS driver is not part of this repository, so these tables cannot be re-run from this repository alone. The method is published so that the same comparison can be made with any FUSE 2.x file system that reports stable inode numbers; see [Reproducing](#reproducing).

FUSE-T is its author's project and is not affiliated with OpenMacFUSE or NTFSTool. Version 1.0.49 was the version tested; later versions may behave differently.

## Machines

| Label | Model | CPU | RAM | macOS |
|---|---|---|---|---|
| **A** | Mac14,15 | Apple M2 | 24 GB | 27.0.1 |
| **B** | MacBookPro11,4 | Intel Core i7-4770HQ | 16 GB | 12.7.6 |

macOS 13, 14, 15 and 26: not measured.

## Conditions

- **Disk images** (tables 1 and 2): a raw image formatted NTFS, stored on the internal SSD. The driver reads and writes the image with ordinary file I/O, so the host page cache is in the path and the numbers show the cost of the mount layer plus the file system, not of a disk.
- **Real disks** (table 3): USB drives attached directly. Reads are cold, after unmount and remount.
- **Memory** is physical footprint (`ri_phys_footprint`, the "Memory" column in Activity Monitor), not RSS. For OpenMacFUSE it is the file system process. For FUSE-T it is the file system process plus its NFS server process. Sampler: `bench/fpsample.c`, 0.2 s interval unless stated.
- Ranges are the minimum and maximum over repeated runs.

## 1. Speed, disk image, machine A

| Test | OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|---|
| `stat` + MD5 of every file on the volume | 22.8 s | 796.9 s |
| Delete 10,000 files of 4 KB | 1.86-2.26 s | 11.2-20.7 s |
| 200,000 small-file create/delete operations | 205-218 s | 1365 s |
| Sequential read, 1 GB | 4491 MB/s | 2129 MB/s |
| Sequential write, 1 GB (including `sync`) | 1488 MB/s | 1181 MB/s |

Directory listing, 40,000 files, `time ls dir | wc -l`, machine B, same image, measured on 2026-11-17 with the current defaults:

| Test | OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|---|
| First listing after mount | 4.06 s | 7.90 s |
| Second listing | 0.53 s | 0.53 s |

The first listing includes the work that keeps `._` AppleDouble files out of the listing (`openmacfuse_xattr=adfile`, the default).

**Not listed: small-file creation.** The file system's default durability setting changed after these runs (a write is now acknowledged only after the data has reached the device), which roughly doubles small-file creation time. The comparison has not been repeated with the new default, so no number is given.

## 2. Memory, disk image, machine A

| Workload | OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|---|
| Reading a 300 MB file, peak | 46 MB | 381 MB |
| Writing a 4 GB file, peak | 47.5 MB | 275.6 MB |
| 200,000 small-file create/delete operations, peak | 47.5 MB | 999.6 MB |
| Same, one minute after the workload ended | 46.4 MB | 764 MB |

Six-hour soak, machine B, OpenMacFUSE only (FUSE-T: not measured): continuous read, write, rename and delete with a full verification every 20 minutes. Footprint 45 MB at the start and 51 MB at the end, a slope of +0.27 MB per hour. All verifications matched.

## 3. Real disks

USB SSD on machine A, USB hard disk on machine B.

| Test | OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|---|
| SSD, sequential write 4.5 GB | 293-313 MB/s | 300 MB/s |
| SSD, cold read | 245-251 MB/s | 236-241 MB/s |
| SSD, delete 2,000 small files | 0.82-0.86 s | 8.4-9.7 s |
| SSD, walk the directory tree | 0.25-0.30 s | 4.1-4.6 s |
| Hard disk, sequential write | 70 MB/s | 74 MB/s |
| Hard disk, sequential read | 97 MB/s | 93 MB/s |
| Hard disk, delete small files | 0.53 s | 2.9 s |
| Hard disk, walk the directory tree | 0.03 s | 0.95 s |

Sequential transfers are limited by the USB link and the disk. The differences there are within run-to-run variation.

## 4. Failure handling

Machine B unless stated. "Simulated unplug" removes the backing device from under the driver; "relay" cuts USB power to a real SSD.

| Event | OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|---|
| Unplug during a write: writer gets an error | 0.19 s | 0.35-0.65 s |
| Unplug during a write: mount gone, driver exited | 0.2-0.5 s | does not exit by itself; needs a forced unmount |
| Unplug while idle or reading: mount gone | 1.2-1.4 s | not measured |
| Unplug on machine A (write / read / idle) | 0.27 / 0.27 / 1.25 s | not measured |
| A request makes no progress | unmount and exit after 30 s | not measured |
| Driver killed with SIGKILL | mount removed in about 2 s by the guard process | not measured |

With the guard process disabled, a killed OpenMacFUSE driver leaves the mount in place for 66-70 s, until the client's `deadtimeout` expires; processes touching the mount cannot be interrupted during that time.

## Reproducing

```sh
make                                        # libopenmacfuse.a
cc -O2 -o build/fpsample bench/fpsample.c   # memory sampler: build/fpsample <interval-ms> <pid>...
```

Build a FUSE 2.x file system twice, once against `libopenmacfuse.a` and once against the library you want to compare with. Mount the same volume image with each build in turn and run the same commands: `time ls dir | wc -l` on a large directory, `find . -type f -exec md5 {} +` over the volume, `dd` for sequential transfers (with `sync` for writes, and an unmount and remount before reads), and a create/delete loop for churn, with `fpsample` attached to the mount processes. The driver script used for the tables above is tied to the NTFS driver's build layout and is not included.

The raw logs behind the tables above were recorded in Chinese and contain local paths. They are being translated and will be added to this directory before v1.0.
