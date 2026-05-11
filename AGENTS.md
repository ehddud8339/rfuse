# Repository Guidelines

## Project Structure & Module Organization
This repository is the RFUSE artifact for kernel/user-space filesystem
experiments. Kernel drivers live in `driver/rfuse/` and `driver/fuse/`.
User-space libraries are in `lib/librfuse/` and `lib/libfuse/`. Example
filesystems are under `filesystems/nullfs/` and `filesystems/stackfs/`.
Benchmarks and workloads live in `bench/unit/`, `bench/fio/`,
`bench/scale_fio/`, `bench/fxmark/`, and `bench/filebench/`. The `linux/` tree
is vendored Linux 5.15.0 source; avoid unrelated churn there.

## Build, Test, and Development Commands
- `cd lib/librfuse && ./librfuse_install.sh`: rebuilds and installs the RFUSE
  user library with Meson/Ninja.
- `cd lib/libfuse && ./libfuse_install.sh`: rebuilds and installs the baseline
  FUSE user library.
- `cd driver/rfuse && make`: builds the RFUSE kernel module against the running
  kernel headers.
- `cd driver/fuse && make`: builds the baseline FUSE kernel module.
- `cd filesystems/nullfs && make`: builds `NullFS`.
- `cd filesystems/stackfs && make`: builds `StackFS_ll`.
- `cd bench/unit && make && ./unit 1`: builds and runs the latency unit test
  after the relevant filesystem is mounted.

Install, module insertion, mount, and benchmark commands often require root.
Review `DEVICE_NAME` in benchmark `driver.sh` scripts before running them.

## Coding Style & Naming Conventions
Most owned code is C. Follow the surrounding style: tabs in kernel-style code,
short helper functions, lower_snake_case names, and uppercase macros such as
`RFUSE_NUM_IQUEUE`. Keep kernel changes compatible with Linux 5.15 APIs.
`NullFS` uses `-Wall -Werror`, so warnings must be fixed.

## Testing Guidelines
Use the smallest relevant test first: rebuild the touched driver, library, or
filesystem, then run `bench/unit` for latency path changes. For performance or
behavioral changes, run the applicable `bench/*/driver.sh`. Record hardware,
kernel version, mount options, device names, and logs when comparing FUSE and
RFUSE.

## Commit & Pull Request Guidelines
Recent history uses short, direct subjects, sometimes in Korean, such as
`page cache 우회 path 추가` or `로그 추가`. Keep subjects focused on changed
behavior. Before opening a PR, discuss substantial changes via an issue or
maintainer contact, describe motivation and test coverage, list required system
configuration, and avoid committing generated kernel build artifacts such as
`*.o`, `*.ko`, `.cmd`, `Module.symvers`, or `modules.order`.

## Agent-Specific Instructions
Do not clean or revert unrelated working-tree changes. Treat generated benchmark
logs and kernel build outputs as local artifacts unless the user explicitly asks
to manage them.
