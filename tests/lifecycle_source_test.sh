#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

if command -v rg >/dev/null 2>&1; then
    search_re() { rg -q -- "$1" "$2"; }
    search_fixed() { rg -Fq -- "$1" "$2"; }
    search_lines() { rg -n -- "$1" "$2"; }
    search_context() { rg -n -A1 -- "$1" "$2"; }
    count_re() { rg -c -- "$1"; }
    scan_c_sources() {
        rg -n --glob '*.c' --glob '!*.mod.c' -- "$1" .
    }
    scan_c_or_h_sources() {
        rg -n --glob '*.[ch]' -- "$1" .
    }
    count_dma_set_void_guards() {
        rg -U -c \
            'LINUX_VERSION_CODE >= KERNEL_VERSION\(6, 10, 0\) \|\| \\\n\s*RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION\(9, 6\)' init.c
    }
else
    search_re() { grep -Eq -- "$1" "$2"; }
    search_fixed() { grep -Fq -- "$1" "$2"; }
    search_lines() { grep -nE -- "$1" "$2"; }
    search_context() { grep -nE -A1 -- "$1" "$2"; }
    count_re() { grep -Ec -- "$1"; }
    scan_c_sources() {
        local file found=1
        while IFS= read -r -d '' file; do
            if grep -nHE -- "$1" "$file"; then
                found=0
            fi
        done < <(find . -type f -name '*.c' ! -name '*.mod.c' -print0)
        return "$found"
    }
    scan_c_or_h_sources() {
        local file found=1
        while IFS= read -r -d '' file; do
            if grep -nHE -- "$1" "$file"; then
                found=0
            fi
        done < <(find . -type f \( -name '*.c' -o -name '*.h' \) -print0)
        return "$found"
    }
    count_dma_set_void_guards() {
        awk '
            previous ~ /^#if LINUX_VERSION_CODE >= KERNEL_VERSION\(6, 10, 0\) \|\| \\$/ &&
            $0 ~ /^[[:space:]]*RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION\(9, 6\)$/ {
                count++
            }
            { previous = $0 }
            END { print count + 0 }
        ' init.c
    }
fi

if scan_c_sources 'devm_|dmam_'; then
    echo "foreign-device devres allocation is forbidden" >&2
    exit 1
fi

if scan_c_or_h_sources \
        'dev_(set|get)_drvdata\(&pdev->dev|pci_(set|get)_drvdata'; then
    echo "mx_dma must not overwrite/read the bound PCI driver's drvdata" >&2
    exit 1
fi

search_re 'pci_dev_get\(pdev\)' init.c
search_re 'try_module_get\(THIS_MODULE\)' init.c
search_re 'mxdma_enumerate_bound_devices' init.c
search_re 'device_lock\(&pdev->dev\)' init.c
search_re 'strcmp\(pdev->dev.driver->name, "cxl_pci"\)' init.c
# Keep main's single-owner detach rule together with the lease branch's
# ability to retain an ambiguous device for later recovery.
detach_block=$(sed -n '/^static void mxdma_detach_device(/,/^}/p' init.c)
unlink_line=$(awk '/list_del_init\(/ { print NR; exit }' <<<"$detach_block")
unlock_line=$(awk '/mutex_unlock\(/ { print NR; exit }' <<<"$detach_block")
destroy_line=$(awk '/if \(!destroy_mx_pdev\(/ { print NR; exit }' <<<"$detach_block")
[[ -n $unlink_line && -n $unlock_line && -n $destroy_line ]]
(( unlink_line < unlock_line && unlock_line < destroy_line ))
grep -Fq 'list_add_tail(&mx_pdev->registry_entry' <<<"$detach_block"
create_block=$(sed -n '/^static int create_mx_pdev(/,/^}/p' init.c)
revision_line=$(awk '/switch \(pdev->revision\)/ { print NR; exit }' <<<"$create_block")
allocation_line=$(awk '/mx_pdev = kzalloc\(/ { print NR; exit }' <<<"$create_block")
[[ -n $revision_line && -n $allocation_line ]]
(( revision_line < allocation_line ))
grep -Fq '*out_pdev = NULL;' <<<"$create_block"
search_fixed 'saved_dma_mask = *pdev->dev.dma_mask' init.c
search_fixed 'saved_coherent_dma_mask = pdev->dev.coherent_dma_mask' init.c
search_fixed 'dma_set_mask(&pdev->dev, mx_pdev->saved_dma_mask)' init.c
search_fixed 'mx_pdev->saved_coherent_dma_mask)' init.c
search_fixed 'saved_min_align_mask = dma_get_min_align_mask(&pdev->dev)' init.c
search_fixed 'required_min_align_mask = mx_pdev->saved_min_align_mask |' init.c
search_fixed 'mx_pdev->saved_min_align_mask);' init.c
dma_set_void_guards=$(count_dma_set_void_guards)
if [ "$dma_set_void_guards" -ne 2 ]; then
    echo "both dma_set_max_seg_size call sites need the RHEL 9.6 void guard" >&2
    exit 1
fi
search_re 'static inline int mx_sg_alloc_table' transfer.c
search_re 'RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION\(9, 2\)' transfer.c
search_re 'ret = mx_sg_alloc_table\(sgt' transfer.c
search_re 'MX_LEASE_CAP_PERSISTENT_STATE_ANCHOR' lease.c
search_re 'MX_LEASE_CAP_PRIVILEGED_FRESH_ANCHOR' lease.c
search_re 'MX_LEASE_CAP_PRIVILEGED_PUBLISHER' lease.c
search_re 'MX_LEASE_CAP_WORKLOAD_PROOF_BINDING' lease.c
search_re 'MX_LEASE_CAP_CANONICAL_SLOT_OFD_PROOF' lease.c
if search_re 'mx_lease_detach_idle_anchor_locked' lease.c; then
    echo "sandbox state anchor must survive a holder-free restart" >&2
    exit 1
fi
search_re 'mx_lease_sm_validate_anchored_acquire' lease.c
search_re 'mx_lease_sm_admit_direct' lease.c
search_re 'lease->state_path_valid' lease.c
search_re 'path_get\(&state_file->f_path\)' lease.c
search_re 'mx_lease_transfer_get\(ctx, transfer\)' transfer.c
search_re 'mx_lease_transfer_put\(transfer\)' transfer.c
search_re 'get_file\(ctx->slot_proof_file\)' lease.c
search_re 'get_file\(ctx->lifetime_proof_file\)' lease.c
search_re 'ctx->transfer_count' lease.c
search_re 'ctx->direct_count' lease.c
search_re 'mx_lease_direct_begin\(ctx\)' fops.c
search_re 'mx_lease_direct_end\(ctx\)' fops.c
search_re 'struct mx_file_ctx \*owner_ctx' mx_dma.h
bar_mmap_block=$(sed -n '/static int mxdma_bar_mmap_v1/,/return ret;/p' core_v1.c)
grep -q 'mx_file_ctx_get(ctx)' <<<"$bar_mmap_block"
grep -q 'bar_vma->owner_ctx = ctx' <<<"$bar_mmap_block"
bar_close_block=$(sed -n '/static void mx_bar_vma_close/,/^}/p' core_v1.c)
grep -q 'mx_file_ctx_put(owner_ctx)' <<<"$bar_close_block"
search_re 'mx_lease_anchor_slot_domain' lease.c
search_re 'file_inode\(slot_file\) != lease->slot_domain_inode' lease.c
search_re 'mx_lease_clone_with_ofd_lock\(source, F_RDLCK' lease.c
search_re 'F_WRLCK, 0, OFFSET_MAX' lease.c
search_re 'ctx->direct_count, ctx->transfer_count' lease.c
search_re 'mx_lease_authorize_no_completion\(file_ctx\)' ioctl.c
search_fixed 'return ctx ? -EOPNOTSUPP : -EINVAL' lease.c
if search_re 'time_after[^;]*zombie_timestamp|zombie_timestamp[^;]*time_after' \
        transfer.c; then
    echo "time alone must never authorize zombie DMA reclamation" >&2
    exit 1
fi
search_fixed 'if (!READ_ONCE(mx_pdev->dma_reclaim_safe))' transfer.c
search_fixed 'atomic_read(&transfer->wait_claimed) == 1' transfer.c
search_fixed 'TRACE_EVENT(mx_dma_xfer_post_submit' trace.h
post_submit_line=$(search_lines 'ops->post_submit\(q\)' core_common.c | cut -d: -f1)
proof_trace_line=$(search_lines 'trace_mx_dma_xfer_post_submit\(q->' core_common.c | cut -d: -f1)
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
publisher_check=$(search_context \
    'mx_lease_profile_is_publisher\(req\.profile\) &&' lease.c | \
    count_re '!capable\(CAP_SYS_RAWIO\)')
if [ "$publisher_check" -ne 1 ]; then
    echo "every publisher acquisition must require host CAP_SYS_RAWIO" >&2
    exit 1
fi

echo "lifecycle source invariants: PASS"
