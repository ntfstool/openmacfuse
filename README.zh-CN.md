# OpenMacFUSE

[English](README.md) | **简体中文** | [日本語](README.ja.md) | [Deutsch](README.de.md) | [Français](README.fr.md) | [Español](README.es.md)

**一个兼容 FUSE 2.x 的 macOS 库，不需要内核扩展，也不需要安装任何东西。**

OpenMacFUSE 在你的文件系统进程内运行一个 NFSv4.0 服务器，并用 macOS 自带的 NFS 客户端挂载它。你的 `fuse_operations` 回调保持不变；只需链接一个静态库 `libnfuse.a`，取代原来的 FUSE 库。

它是 [NTFSTool Pro](https://ntfstool.com) 内置的挂载引擎，承载 NTFS、ext2/3/4、exFAT 和 BitLocker 卷。它是为一款正在发售的产品而写的，下文的测量数据都来自这项工作。

> **状态：v0.9 预览版。** 已在 macOS 12（Intel）和 macOS 27（Apple 芯片）上完整测试。macOS 13 至 26 未经测试。见[已测试的系统](#已测试的系统)。

![请求路径：典型的双进程 NFS 式 FUSE 层与 OpenMacFUSE 的对比](docs/images/architecture.svg)

## 为什么

自 macOS 11 起，第三方内核扩展只有在用户进入恢复模式降低安全级别之后才能加载。对面向普通用户的软件来说，这已经不是一个现实的要求。用户态文件系统可以改走 macOS 本来就有的 NFS 客户端：在同一台机器上运行一个 NFS 服务器，把每个 NFS 请求翻译成一次文件系统调用，再用 `mount_nfs` 挂载。

OpenMacFUSE 走的就是这条路，并且把整个服务器放进文件系统进程里：

- **无需安装。** 没有内核扩展，没有系统扩展，没有安装器，在 Apple 芯片上也不必切到“降低安全性”。只有一个静态库。
- **单进程。** NFS 服务器和文件系统共用一个地址空间。请求从协议解码器到你的回调只是一次函数调用，没有第二个进程，也没有额外拷贝。
- **内存有上限。** 固定大小的缓冲池，节点表有上限，目录快照有上限。内存不随文件大小、文件数量或运行时间增长。
- **出错即卸载（fail closed）。** 设备消失、进程卡死或被杀掉时，挂载会在几秒内被撤下，不会让应用阻塞在一个已经失效的卷上。
- **Mac 语义由库处理。** 扩展属性不在卷上留下 `._` 文件，查找不区分大小写，文件名兼容 NFC/NFD。
- **净室实现的 C 代码。** 约 17,000 行，依据 RFC 7530、7531、5531、4506 和公开的 FUSE 2.x API 编写。不含第三方代码。MIT 许可。

## 测量数据

所有数字均由 NTFSTool 工程团队测得。同一份 FUSE 文件系统源码（NTFSTool Pro 的 NTFS 驱动）构建两次，一次链接 OpenMacFUSE，一次链接 FUSE-T 1.0.49，在同一台机器上用同一个卷镜像运行。方法、机器和完整表格见 [docs/BENCHMARKS.md](docs/BENCHMARKS.md)。

![元数据密集型负载的耗时，OpenMacFUSE 与 FUSE-T 1.0.49](docs/images/bench-speed.svg)

![内存峰值，OpenMacFUSE 与 FUSE-T 1.0.49](docs/images/bench-memory.svg)

在真实 USB 磁盘上，大文件顺序传输受磁盘本身限制，两者持平：

| 真实磁盘 | OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|---|
| USB SSD，顺序写入 4.5 GB | 293-313 MB/s | 300 MB/s |
| USB SSD，冷读 | 245-251 MB/s | 236-241 MB/s |
| USB SSD，删除 2,000 个小文件 | 0.82-0.86 s | 8.4-9.7 s |
| USB SSD，遍历目录树 | 0.25-0.30 s | 4.1-4.6 s |
| USB 机械硬盘，写入 / 读取 | 70 / 97 MB/s | 74 / 93 MB/s |

小文件创建没有列出：这些测试跑完之后，文件系统默认的持久化设置改过，这项对比还没有重做。

同一个卷镜像挂载在运行 macOS 12.7.6 的 Intel Mac 上，列出一个含 40,000 个文件的目录，然后读取挂载进程的内存占用：

| OpenMacFUSE | FUSE-T 1.0.49 |
|---|---|
| ![终端：OpenMacFUSE 列出 40,000 个文件用时 4.06 s，占用 43 MB](docs/images/terminal-openmacfuse.png) | ![终端：FUSE-T 列出 40,000 个文件用时 7.90 s，占用 209 MB](docs/images/terminal-fuse-t.png) |
| 首次列目录 4.06 s，内存占用 43 MB | 首次列目录 7.90 s，内存占用 209 MB |

### 出问题的时候

| 事件 | 结果 |
|---|---|
| 写入过程中拔掉磁盘 | 写入方收到错误，挂载在 0.2-0.5 s 内消失 |
| 空闲或读取时拔掉磁盘 | 约 1.2-1.4 s 内卸载 |
| 某个请求 30 s 没有进展 | 看门狗让待处理的请求失败，强制卸载，然后退出 |
| 文件系统进程崩溃或被杀掉 | 看护进程在约 2 s 内移除挂载 |
| 连续六小时混合负载 | 内存开始时 45 MB，结束时 51 MB（+0.27 MB/小时） |

没有看护进程时，NFS 服务器死掉后挂载会一直留着，直到客户端的 `deadtimeout` 到期（约 70 s），这期间访问它的进程无法被中断。

## 快速开始

要求：装有 Xcode Command Line Tools 的 macOS。除此之外不需要别的。

```sh
git clone https://github.com/ntfstool/openmacfuse
cd openmacfuse
make              # build/libnfuse.a (+ build/nfuse-m0, an in-memory test server)
make universal    # arm64 + x86_64
make -C examples/hello
mkdir -p /tmp/hello && examples/hello/hello /tmp/hello
cat /tmp/hello/hello.txt      # Hello from nfuse
umount /tmp/hello             # hello exits after the unmount
```

第一次挂载时，macOS 可能会为调用方应用弹出“网络卷宗”权限提示。这是正常的：它本来就是一次 NFS 挂载。

库及其标识符都以 `nfuse` 命名（`libnfuse.a`、`nfuse.h`、`NFUSE_*`）。OpenMacFUSE 是项目的名字。

## 在你的文件系统中使用

```c
#define FUSE_USE_VERSION 26
#include <fuse.h>
```

```sh
cc -I/path/to/openmacfuse/include -c myfs.c
cc -o myfs myfs.o /path/to/openmacfuse/build/libnfuse.a \
   -lpthread -framework CoreFoundation -framework IOKit
```

FUSE 2.x 高层 API 的两种用法都支持：

```
fuse_main(argc, argv, &ops, NULL)
```

```
fuse_mount → fuse_new → fuse_set_signal_handlers → [daemon()] → fuse_loop → fuse_unmount → fuse_destroy
```

`fuse_new` 返回时挂载已经完成。`fuse_loop` 在卷被卸载后、设备被报告丢失后，或收到 SIGINT/SIGTERM/SIGHUP 时返回。

[`examples/hello/hello.c`](examples/hello/hello.c) 是一个完整的只读文件系统，不到 150 行。文件系统只要提供了 `write` 回调，并且挂载时没有带 `-o ro`，就是可读写的。

### 你的文件系统必须做到的两件事

1. **`getattr` 要为每个对象返回稳定、唯一的 `st_ino`。** NFS 文件句柄是用 inode 号构造的。如果你的文件系统把 `st_ino` 留为零，库会按路径分配稳定的编号。
2. **让 `fsync` 的含义与你希望 `COMMIT` 具有的含义一致。** 库在处理 NFS `COMMIT` 和 `FILE_SYNC` 写入时调用 `fsync`，除此之外没有别的持久化信号。

### 挂载选项（`-o`）

| 选项 | 作用 |
|---|---|
| `volname=NAME` | macOS 显示的卷名 |
| `ro` / `rdonly` | 只读挂载 |
| `rwsize=N` | NFS 读写块大小（默认 262144） |
| `nobrowse` | 不在 Finder 中显示该卷 |
| `nosuid`, `nodev`, `noexec` | 原样传给挂载 |
| `nfuse_case=insensitive\|sensitive` | 查找不区分大小写（默认）。卷上已经存在仅大小写不同的名字时，自动回退到 `sensitive` |
| `nfuse_xattr=adfile\|appledouble\|streams` | 扩展属性存放在哪里。`adfile`（默认）：库把客户端的 `._` 文件转成对你的 `setxattr`/`getxattr` 的调用，卷上不会多存任何东西。`appledouble`：普通的 `._` 文件。`streams`：NFSv4 命名属性，实验性，见“已知限制” |
| `nfuse_nfc=1\|0` | 新名字以 Unicode NFC 形式存储（默认 1）。无论取哪个值，查找都同时接受 NFC 和 NFD |

常见的 FUSE 选项（`local`、`allow_other`、`fsname`、…）会被接受并忽略。通过 `fuse_main` 进入的程序，除非自己传入这些选项，否则使用保守的默认值（`nfuse_xattr=appledouble`、`nfuse_case=sensitive`）。

### 环境变量

| 变量 | 默认值 | 作用 |
|---|---|---|
| `NFUSE_TRANSPORT` | `auto` | `unix`、`tcp` 或 `auto`（macOS 26+ 用 unix 套接字，更早的版本用 127.0.0.1 TCP） |
| `NFUSE_LOG` | — | 把日志追加写入这个文件 |
| `NFUSE_WATCHDOG_S` | 30 | 请求多少秒没有进展就算卡死；0 表示禁用 |
| `NFUSE_WATCHDOG_MAX_S` | 600 | 单个请求的硬性时限；0 表示禁用 |
| `NFUSE_GUARD` | 1 | 0 = 不启动看护进程 |
| `NFUSE_GUARD_S` | 20 | 多少秒没有心跳，看护进程就认为进程已冻结 |
| `NFUSE_MOUNT_TIMEOUT_S` | 30 | `mount_nfs` 的超时时间 |

可选的扩展回调（大小写折叠、按范围读写 xattr、供看门狗使用的进度计数器、挂载信息）声明在 [`include/nfuse.h`](include/nfuse.h) 中。

## 工作原理

macOS 的 NFS 客户端历史悠久、稳定，而且系统里一定有。OpenMacFUSE 通过本地 unix 套接字（macOS 26 及以后）或 127.0.0.1 TCP（更早的版本，此时会拒绝来自其他本地进程的连接）与它用 NFSv4.0 通信，并把每个 COMPOUND 请求翻译成 FUSE 高层回调。

决定其行为的几项设计：

- **有上限的节点表。** 文件句柄不是指向内存对象的指针。节点表有上限；被淘汰的句柄在下次使用时按路径重新解析。把上限强制压到 64 项并连续做 301 次重命名，对旧句柄的 7,839 次查找全部解析成功，没有一次返回 `NFS4ERR_STALE`。
- **重复请求缓存。** 客户端在超时后会重发请求。对非幂等操作（`CREATE`、`REMOVE`、`RENAME`、…）的应答会被缓存并重放，因此操作只执行一次。
- **默认不使用命名属性。** NFSv4 命名属性本是存放扩展属性最自然的地方，但在开发过程中，启用它们导致过 macOS NFS 客户端内核崩溃（kernel panic，在 macOS 27.0.1 和 12.7.6 上都出现过）。默认路径只使用客户端最常走到的那部分功能。

设计说明与协议上的取舍：[docs/DESIGN.md](docs/DESIGN.md)。

## 已测试的系统

| macOS | 硬件 | 结果 |
|---|---|---|
| 12.7.6 | Intel (MacBookPro11,4) | 完整测试套件，869 项自动化检查通过 |
| 27.0.1 | Apple 芯片 (M2) | 读写、模拟拔盘、杀进程、Mac 语义套件通过 |
| 13, 14, 15, 26 | — | **未经测试** |

完整测试套件驱动的是真实的文件系统（NTFS、ext、exFAT），它们不在本仓库中。在本仓库里可以运行的有：

```sh
make test               # unit tests, no mount
tests/m0_smoke.sh       # mount the in-memory server, read, unmount
```

## 已知限制

- **未经测试的 macOS 版本**：13、14、15、26。
- **对 macOS 而言，这个卷是网络卷。** 没有废纸篓，Finder 第一次打开非常大的目录（40,000 项以上）会比较慢。
- **macOS NFS 客户端里的内核崩溃无法在用户态捕获。** 开发中遇到过的两条路径（命名属性；中断一个服务器不应答的挂载）默认都已避开。
- **API 覆盖范围**：FUSE 2.x 高层 API。未提供：低层 API、FUSE 3。
- **回调串行执行**（一把全局锁）。慢设备上的一个慢请求会拖住同一挂载上的其他请求。
- FUSE 2.x **没有 inode 代数（generation number）**：如果你的文件系统在删除后复用 inode 号，持有旧句柄的客户端可能访问到新文件。
- **文件标志**：隐藏和归档标志会传到你的 `chflags` 回调。Finder 的“已锁定”标志（`uchg`）不会；macOS NFS 客户端直接拒绝它，不发送请求。

## 相关项目

- [NTFSTool Pro](https://ntfstool.com)：Mac 上的 NTFS 读写工具，支持 ext2/3/4、exFAT 恢复和 BitLocker。OpenMacFUSE 是它内置的挂载引擎。
- [ntfstool/ntfstool](https://github.com/ntfstool/ntfstool)：免费、开源的 NTFSTool，基于 Apple FSKit 构建。

OpenMacFUSE 是为数不多的无需内核扩展的开源 macOS FUSE 实现之一。它是独立项目，与 macFUSE 及其作者没有关联，也与 libfuse、FUSE-T 项目没有关联。“FUSE”指的是这套 API。

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请联系：service@ntfstool.com。

## 许可证

MIT。见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。
