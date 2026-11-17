# Contributing to OpenMacFUSE

Thanks for looking. OpenMacFUSE is small on purpose. Changes that keep it small, bounded and correct are the easiest to merge.

## Before you open an issue

- Run with `NFUSE_LOG=/tmp/nfuse.log NFUSE_VERBOSE=1` and attach the relevant part of the log.
- Include `sw_vers`, `uname -m`, and the output of `nfsstat -m` for the mount.
- Say which transport was used (the log's mount line shows `proto=ticotsord` for the unix socket or `tcp`).
- Remove anything private from logs (paths, file names, user names) before posting.

Security issues: email service@ntfstool.com instead of opening a public issue. We reply within 72 hours.

## Pull requests

1. One change per pull request. Explain the problem first, then the fix.
2. **Clean room.** Work only from the public RFCs (7530, 7531, 5531, 4506), the public FUSE API description, and your own observation of the macOS NFS client. Do not copy, paraphrase or port code from other NFS servers or FUSE implementations, and do not disassemble or decompile third-party binaries. Say in the pull request what you based the change on.
3. **No third-party code or dependencies.** The library builds with the system compiler and system libraries only.
4. **Bounded memory.** Every new table, cache or queue needs a fixed cap and an eviction or reclaim rule. Nothing may grow with file size or with the number of operations.
5. **Tests.** Add or extend a test that fails without your change. Run `make test` and `tests/m0_smoke.sh` on at least one macOS version, and list the versions you ran in the pull request.
6. **Measured claims only.** If a change is about speed or memory, include before/after numbers (memory with `bench/fpsample.c`), with machine model, macOS build and the exact command. Untested cells say "not measured".
7. **Style.** C11, tabs, kernel-style braces, `-Wall -Wextra` clean. Comments and log messages in English. Explain *why* in comments, especially for behaviour that exists because of a specific macOS client version.
8. **License.** By submitting a pull request you agree that your contribution is licensed under the MIT License. New files start with:

   ```c
   /*
    * Copyright 2026 The OpenMacFUSE Authors
    * SPDX-License-Identifier: MIT
    */
   ```

   Sign off your commits (`git commit -s`) to certify the [Developer Certificate of Origin](https://developercertificate.org/).

## Scope

In scope: NFSv4.0 server correctness against the macOS client, FUSE 2.x API coverage, macOS version compatibility, bounded resource use, tests and benchmarks.

Out of scope for this repository: specific file systems (NTFS or otherwise), GUI, installers.
