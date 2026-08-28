#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

if rg -n 'devm_|dmam_' --glob '*.c' --glob '!*.mod.c' .; then
    echo "foreign-device devres allocation is forbidden" >&2
    exit 1
fi

if rg -n 'dev_(set|get)_drvdata\(&pdev->dev|pci_(set|get)_drvdata' \
        --glob '*.[ch]' .; then
    echo "mx_dma must not overwrite/read the bound PCI driver's drvdata" >&2
    exit 1
fi

rg -q 'pci_dev_get\(pdev\)' init.c
rg -q 'try_module_get\(THIS_MODULE\)' init.c
rg -q 'mxdma_enumerate_bound_devices' init.c
rg -q 'device_lock\(&pdev->dev\)' init.c
rg -q 'strcmp\(pdev->dev.driver->name, "cxl_pci"\)' init.c
rg -Fq 'saved_dma_mask = *pdev->dev.dma_mask' init.c
rg -Fq 'saved_coherent_dma_mask = pdev->dev.coherent_dma_mask' init.c
rg -Fq 'dma_set_mask(&pdev->dev, mx_pdev->saved_dma_mask)' init.c
rg -Fq 'mx_pdev->saved_coherent_dma_mask)' init.c
rg -Fq 'saved_min_align_mask = dma_get_min_align_mask(&pdev->dev)' init.c
rg -Fq 'required_min_align_mask = mx_pdev->saved_min_align_mask |' init.c
rg -Fq 'mx_pdev->saved_min_align_mask);' init.c
rg -q 'MX_LEASE_CAP_PERSISTENT_STATE_ANCHOR' lease.c
rg -q 'MX_LEASE_CAP_PRIVILEGED_FRESH_ANCHOR' lease.c
rg -q 'MX_LEASE_CAP_PRIVILEGED_PUBLISHER' lease.c
rg -q 'MX_LEASE_CAP_WORKLOAD_PROOF_BINDING' lease.c
rg -q 'MX_LEASE_CAP_CANONICAL_SLOT_OFD_PROOF' lease.c
if rg -q 'mx_lease_detach_idle_anchor_locked' lease.c; then
    echo "sandbox state anchor must survive a holder-free restart" >&2
    exit 1
fi
rg -q 'mx_lease_sm_validate_anchored_acquire' lease.c
rg -q 'mx_lease_sm_admit_direct' lease.c
rg -q 'lease->state_path_valid' lease.c
rg -q 'path_get\(&state_file->f_path\)' lease.c
rg -q 'mx_lease_transfer_get\(ctx, transfer\)' transfer.c
rg -q 'mx_lease_transfer_put\(transfer\)' transfer.c
rg -q 'get_file\(ctx->slot_proof_file\)' lease.c
rg -q 'get_file\(ctx->lifetime_proof_file\)' lease.c
rg -q 'ctx->transfer_count' lease.c
rg -q 'ctx->direct_count' lease.c
rg -q 'mx_lease_direct_begin\(ctx\)' fops.c
rg -q 'mx_lease_direct_end\(ctx\)' fops.c
rg -q 'mx_lease_anchor_slot_domain' lease.c
rg -q 'file_inode\(slot_file\) != lease->slot_domain_inode' lease.c
rg -q 'mx_lease_clone_with_ofd_lock\(source, F_RDLCK' lease.c
rg -q 'F_WRLCK, 0, OFFSET_MAX' lease.c
rg -q 'ctx->direct_count, ctx->transfer_count' lease.c
rg -q 'mx_lease_authorize_no_completion\(file_ctx\)' ioctl.c
rg -Fq 'return ctx ? -EOPNOTSUPP : -EINVAL' lease.c
if rg -q 'time_after[^;]*zombie_timestamp|zombie_timestamp[^;]*time_after' \
        transfer.c; then
    echo "time alone must never authorize zombie DMA reclamation" >&2
    exit 1
fi
rg -Fq 'if (!READ_ONCE(mx_pdev->dma_reclaim_safe))' transfer.c
rg -Fq 'atomic_read(&transfer->wait_claimed) == 1' transfer.c
rg -Fq 'TRACE_EVENT(mx_dma_xfer_post_submit' trace.h
post_submit_line=$(rg -n 'ops->post_submit\(q\)' core_common.c | cut -d: -f1)
proof_trace_line=$(rg -n 'trace_mx_dma_xfer_post_submit\(q->' core_common.c | cut -d: -f1)
if [ -z "$post_submit_line" ] || [ -z "$proof_trace_line" ] ||
   [ "$proof_trace_line" -le "$post_submit_line" ]; then
    echo "hardware-visible proof trace must follow the post-submit doorbell" >&2
    exit 1
fi
push_timeout_block=$(sed -n \
    '/Timeout waiting for pushable admin queue/,/push_mx_command(queue, c)/p' \
    core_v2.c)
if grep -Eq 'admin_desynced|protocol_poisoned' <<<"$push_timeout_block"; then
    echo "an admin command that was never submitted must remain recoverable" >&2
    exit 1
fi
completion_timeout_block=$(sed -n \
    '/Timeout waiting for admin completion/,/return false/p' core_v2.c)
if ! grep -q 'admin_desynced' <<<"$completion_timeout_block" ||
   ! grep -q 'protocol_poisoned' <<<"$completion_timeout_block"; then
    echo "a doorbelled admin command without completion must fail closed" >&2
    exit 1
fi
publisher_check=$(rg -n -A1 \
    'mx_lease_profile_is_publisher\(req\.profile\) &&' lease.c | \
    rg -c '!capable\(CAP_SYS_RAWIO\)')
if [ "$publisher_check" -ne 1 ]; then
    echo "every publisher acquisition must require host CAP_SYS_RAWIO" >&2
    exit 1
fi

echo "lifecycle source invariants: PASS"
