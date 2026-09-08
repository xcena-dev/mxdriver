# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Linux out-of-tree kernel module (`mx_dma.ko`) for XCENA MX-DMA PCI devices. Provides DMA transfer capabilities between host memory and CXL (Compute Express Link) memory devices. Supports both CXL-enabled and standalone (non-CXL) configurations.

## Build Commands

```bash
# Build with CXL support (default)
make

# Build without CXL support (standalone mode)
make WO_CXL=1

# Build without the ftrace tracepoints (drops trace.o)
make MX_DMA_DISABLE_TRACE=1

# Clean build artifacts
make clean

# Install onto this machine (auto-detects CXL via /sys/firmware/acpi/tables/CEDT)
sudo ./install.sh

# Install for another kernel or machine (image build). sudo drops the
# environment, so the variables go to sudo, not to the shell that calls it.
sudo XCENA_TARGET_KVER=<version> XCENA_TARGET_KDIR=<build-tree> \
     XCENA_TARGET_HAS_CXL=true|false ./install.sh

# Uninstall (always acts on the running kernel)
sudo ./uninstall.sh

# Build against a specific kernel, and stage into a rootfs
make BUILDSYSTEM_DIR=/lib/modules/<version>/build
make install INSTALL_MOD_PATH=<rootfs>
```

The module requires kernel headers at `/lib/modules/$(uname -r)/build`. There is no test suite in this repository; testing is done through userspace applications in the parent `sdk_release` repo.

### Install Pipeline

`install.sh` resolves the target kernel and the target's CXL answer first, then installs:

- **Target kernel** — `XCENA_TARGET_KVER` / `XCENA_TARGET_KDIR`, defaulting to the running kernel and `/lib/modules/<kver>/build`. Given both, they must agree; the tree's real release comes from `include/config/kernel.release` (a header tree's `make kernelrelease` recomputes the version and drops the distro suffix).
- **CXL answer** — `XCENA_TARGET_HAS_CXL=true|false`, otherwise this machine's `/sys/firmware/acpi/tables/CEDT`. Selects `WO_CXL=1`. `dkms.conf` reads the same variable, and falls back to CEDT on a kernel-upgrade autoinstall where the variable is not set.
- **DKMS, or legacy** — DKMS when available (sources copied to `/usr/src/mx_dma-<version>`, `POST_INSTALL=scripts/dkms-post-install.sh`), else `make install` plus `depmod -a`.
- **Boot** — `/etc/modules-load.d/mx_dma.conf`. CXL builds also get `softdep cxl_pci pre: mx_dma` in `/etc/modprobe.d/mx_dma-order.conf` and an initramfs entry, so the PCI bus notifier is registered before `cxl_pci` binds XCENA devices. Standalone builds use `pci_register_driver` and skip both.

`uninstall.sh` always acts on the running kernel; it takes no `XCENA_TARGET_*` input.

## Architecture

### Module Structure

The driver registers as a PCI driver for vendor `0x20A6` (XCENA). Each PCI device creates 5 character devices under `/dev/mx_dma/`:

| Device Node | Purpose | Key Operations |
|---|---|---|
| `mx_dma{N}_data` | Bulk data DMA transfers | read/write (scatter-gather, parallel) |
| `mx_dma{N}_context` | Context/control transfers | read/write (single transfer) |
| `mx_dma{N}_ioctl` | Mailbox and control commands | ioctl |
| `mx_dma{N}_event` | MSI interrupt events | poll |
| `mx_dma{N}_bdf` | PCI BDF information | read |

### Hardware Revision Abstraction

The driver supports two hardware revisions selected at probe time via `pdev->revision`:

- **Revision 1** (`core_v1.c`): Custom mailbox protocol with direct BAR MMIO. Uses 1KB (`SINGLE_DMA_SIZE = 1 << 10`) DMA granularity. SQ/CQ are MMIO-mapped mailbox regions in BAR space.
- **Revision 2** (`core_v2.c`): NVMe-like admin/IO queue model with doorbell-based submission. Uses 4KB (`SINGLE_DMA_SIZE = PAGE_SIZE`) DMA granularity. SQ/CQ are DMA-coherent host memory buffers. Admin queue sets up IO queues via create/delete commands.

Both revisions implement two hook sets:

- `struct mx_operations` — device level: `init_queue`, `release_queue`, `create_command_sg`, `create_command_ctrl`, `create_command_passthru`, `bar_mmap`. Registered via `register_mx_ops_v1/v2`.
- `struct mx_queue_ops` — queue level, driven by the common handlers in `core_common.c`: `is_pushable`, `push_command`, `post_submit`, `is_popable`, `pop_completion`, `post_complete`, `build_ping_command`.

### Source File Responsibilities

- **`init.c`** — Module init/exit, PCI probe/remove, character device creation, CXL device discovery. CXL mode uses `bus_register_notifier` on `pci_bus_type`; non-CXL mode uses standard `pci_register_driver`.
- **`fops.c`** — File operations for all 5 character device types. Routes reads/writes through `mxdma_device_prepare()` magic validation.
- **`transfer.c`** — DMA transfer lifecycle: user page pinning (`pin_user_pages_fast`), scatter-gather mapping, split-transfer construction, completion waiting, zombie cleanup. Module params (all sysfs-writable, 0644): `timeout_ms` (60000), `parallel_count` (6), `parallel_split_ratio` (50), `zombie_grace_ms` (60000).
- **`ioctl.c`** — IOCTL handlers for mailbox management, passthru, and the HIO protocol path. Defines `MX_IOCTL_MAGIC 'X'` with 10 ioctl commands.
- **`mbox.c`** — Mailbox ring buffer utilities (empty/full checks, index arithmetic with phase-bit wraparound).
- **`helper.c`** — Global transfer ID management via IDR with 16-bit cyclic allocation.
- **`core_common.c`** — Revision-independent core: descriptor-list sizing and construction (`mx_get_list_count`, `mx_desc_list_init`), SG offset lookup (`mx_sg_locate`), the unified `mx_submit_handler` / `mx_complete_handler` threads with `poll_backoff`, and the transport liveness watchdog.
- **`core_v1.c` / `core_v2.c`** — Per-revision queue init/release, command creation, and the `mx_queue_ops` hooks the common handlers drive.
- **`trace.c` / `trace.h`** — ftrace tracepoints (`mx_dma_*`) for the ioctl and transfer paths. Compiled in by default; `MX_DMA_DISABLE_TRACE=1` drops `trace.o` and stubs the call sites. `static_assert`s tie the string tables to the opcode and wait-state enums.

### Key Data Flow

```
User read/write → fops.c (magic validation)
  → transfer.c: alloc_mx_transfers() builds split-transfers (parallel_split_ratio,
                or legacy per-page splitting capped at parallel_count when 0)
  → transfer.c: map_user_addr_to_sg() pins pages + DMA maps once per shared sg context
  → core_vN.c: create_command_sg() builds hw command with PRP/desc lists
  → transfer.c: mx_transfer_queue_parallel() enqueues to io_queue
  → core_common.c: mx_submit_handler thread pushes commands to hardware
  → core_common.c: mx_complete_handler thread polls completions
  → transfer.c: mx_transfer_wait() with interruptible timeout
```

### Concurrency Model

- **submit_thread / complete_thread** — Per-device kthreads (`core_common.c`) that poll SQ/CQ with `swait_queue_head` and `POLLING_INTERVAL_MSEC` (4ms) timeout. While hardware is unresponsive `poll_backoff()` spins with `cond_resched()` for `BACKOFF_SPIN_ITERS` (100), then sleeps exponentially 125µs → 16ms.
- **Liveness watchdog** — Runs in the submit thread while IO is outstanding. Pings a stalled queue and moves `lv_health` ALIVE → SUSPECT → DEAD; thresholds are the per-device sysfs `liveness_stall_ms` (1000) / `liveness_dead_ms` (5000). All state is atomics, no lock.
- **sq_lock** (spinlock) — Protects the submission queue list.
- **Mailbox mutexes** — Per-mailbox mutex in `struct mx_mbox` for ioctl command serialization.
- **IDR id_lock** (spinlock) — Protects global transfer ID allocation/lookup.
- **zombie_lock** (spinlock) — Protects zombie transfer list; zombie_cleanup_thread wakes every `ZOMBIE_POLL_INTERVAL_MSEC` (1000) and reclaims after the `zombie_grace_ms` grace period (60s default, 0 = immediate).

### CXL vs Standalone Mode

Controlled by `CONFIG_WO_CXL` (set via `make WO_CXL=1`):
- **CXL mode** (default): Uses PCI bus notifier to detect CXL-bound devices. Device ID derived from CXL memory device name (`mem{N}`). The bound driver is `cxl_pci`, so its `driver_data` is never touched. The driver's own embedded registry maps `struct pci_dev` to refcounted `mx_pci_dev` objects that may outlive unbind while a file/VMA/DMA proof is held.
- **Standalone mode** (`WO_CXL=1`): Uses standard `pci_register_driver` and the same private registry/lifetime path. Device IDs are auto-incremented.

### IOCTL Interface

Magic: `'X'`, commands defined in `ioctl.c`:
- `MX_IOCTL_REGISTER_MBOX` (1) — Register SQ/CQ mailbox pair (up to 80 pairs)
- `MX_IOCTL_INIT_MBOX` (2) — Reset mailbox context
- `MX_IOCTL_SEND_CMD_WITH_DATA` (3) — Send command with optional data write
- `MX_IOCTL_RECV_CMDS` (4) — Receive commands from CQ mailbox
- `MX_IOCTL_SEND_CMDS` (5) — Batch send commands to SQ mailbox
- `MX_IOCTL_READ_DATA` (6) / `MX_IOCTL_WRITE_DATA` (7) — Direct parallel data transfers
- `MX_IOCTL_PASSTHRU_CMD` (8) — Passthru command
- `MX_IOCTL_HIO_SEND` (9) / `MX_IOCTL_HIO_RECV` (10) — HIO protocol commands

### Sysfs Interface

The `mx_dma{N}_ioctl` node carries a `liveness/` attribute group (`device_create_with_groups`, `drvdata` = `mx_pdev`): `enable`, `stall_ms`, `dead_ms` (0644) and `health`, `rtt_ns` (read-only).

## Kernel Version Compatibility

Every version check pairs `LINUX_VERSION_CODE` with `RHEL_RELEASE_CODE`, because RHEL ships a 5.14 kernel that backports later mainline APIs, so a bare version test picks the wrong branch. `mx_dma.h` stubs both RHEL macros off RHEL such that each comparison loses; that stub makes `RHEL_RELEASE_VERSION(a, b)` valid only against `RHEL_RELEASE_CODE`.

| API | Old branch | New branch (mainline / RHEL) |
|---|---|---|
| `mxdma_devnode` arg (`init.c`) | `struct device *` | `const struct device *` — 6.1.6 / 9.6 |
| `class_create` (`init.c`) | takes `THIS_MODULE` | name only — 6.3.3 / 9.6 |
| `match_mem_prefix` arg (`init.c`, CXL only) | `void *data` | `const void *data` — 6.12.0 / 9.6 |
| vma flags (`core_v1.c` mmap) | `vma->vm_flags \|=` | `vm_flags_set()` — 6.3.0 / 9.6 |

## Critical Areas

Changes to the following require extra care:
- **DMA mapping/unmapping** (`transfer.c`) — Must maintain proper pin/unpin and map/unmap pairing to avoid memory corruption.
- **Zombie transfer handling** — Prevents use-after-free when transfers timeout or are interrupted.
- **PRP/descriptor list construction** (`core_v1.c`, `core_v2.c`) — Linked-list DMA descriptor chains must maintain correct bus addresses.
- **Mailbox index arithmetic** (`mbox.c`) — Phase-bit wraparound logic is subtle; `depth` must be power-of-2.
- **Liveness watchdog** (`core_common.c`) — Lock-free health transitions racing the completion path; the `cmpxchg` source states are what keep a resolving pong from being clobbered.
- **Tracepoint string tables** (`trace.h`) — Guarded by `static_assert`s against the opcode and wait-state enums; reorder an enum and the build fails by design.
- **Kernel version ifdefs** — Must be kept in sync when targeting new kernel versions, and each one gates on `RHEL_RELEASE_CODE` as well.
