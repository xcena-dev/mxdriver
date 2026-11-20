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
bar_mmap_block=$(sed -n '/^int mx_bar_mmap(/,/^}/p' core_common.c)
grep -q 'mx_file_ctx_get(ctx)' <<<"$bar_mmap_block"
grep -q 'bar_vma->owner_ctx = ctx' <<<"$bar_mmap_block"
grep -q 'kref_get(&mx_pdev->ref)' <<<"$bar_mmap_block"
grep -q 'ctx->bar_mapping_count++' <<<"$bar_mmap_block"
bar_close_block=$(sed -n '/static void mx_bar_vma_close/,/^}/p' core_common.c)
grep -q 'mx_file_ctx_put(owner_ctx)' <<<"$bar_close_block"
grep -q 'mx_pdev_put(mx_pdev)' <<<"$bar_close_block"
grep -q 'owner_ctx->bar_mapping_count--' <<<"$bar_close_block"
if grep -Eq 'mxdma_bar_mmap_v[0-9]|bar_mmap_lock|bar_active_vmas' core_v1.c core_v2.c mx_dma.h; then
    echo "the BAR mapping must not be duplicated per revision" >&2
    exit 1
fi
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
doorbell_sites=$(count_re 'ops->post_submit\(q\)' < core_common.c || true)
if [ "${doorbell_sites:-0}" -ne 2 ]; then
    echo "every post-submit doorbell must be one of the checked SQ advancers" >&2
    exit 1
fi
for submit_fn in mx_queue_submit mx_submit_handler; do
    submit_block=$(sed -n "/^[a-z].* ${submit_fn}(/,/^}/p" core_common.c)
    lock_line=$(grep -n 'spin_lock_irqsave(&q->sq_lock' <<<"$submit_block" | head -1 | cut -d: -f1)
    post_submit_line=$(grep -n 'ops->post_submit(q)' <<<"$submit_block" | head -1 | cut -d: -f1)
    unlock_line=$(grep -n 'spin_unlock_irqrestore(&q->sq_lock' <<<"$submit_block" | head -1 | cut -d: -f1)
    proof_trace_line=$(grep -n 'trace_mx_dma_xfer_post_submit(q->' <<<"$submit_block" | head -1 | cut -d: -f1)
    if [ -z "$lock_line" ] || [ -z "$post_submit_line" ] || [ -z "$unlock_line" ] ||
       [ -z "$proof_trace_line" ]; then
        echo "${submit_fn} must lock, ring the doorbell, unlock and trace" >&2
        exit 1
    fi
    if [ "$post_submit_line" -le "$lock_line" ] || [ "$unlock_line" -le "$post_submit_line" ]; then
        echo "${submit_fn} must ring the SQ doorbell under sq_lock" >&2
        exit 1
    fi
    if [ "$proof_trace_line" -le "$post_submit_line" ]; then
        echo "hardware-visible proof trace must follow the post-submit doorbell" >&2
        exit 1
    fi
    if [ "$unlock_line" -le "$proof_trace_line" ]; then
        echo "${submit_fn} must emit the proof trace before dropping sq_lock" >&2
        exit 1
    fi
done
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
if search_re 'cmpl(\.|->)status' core_v2.c; then
    echo "a v2 completion carries no status; its opcode echo is not a failure" >&2
    exit 1
fi
publisher_check=$(search_context \
    'mx_lease_profile_is_publisher\(req\.profile\) &&' lease.c | \
    count_re '!capable\(CAP_SYS_RAWIO\)')
if [ "$publisher_check" -ne 1 ]; then
    echo "every publisher acquisition must require host CAP_SYS_RAWIO" >&2
    exit 1
fi
type_devnode_block=$(sed -n '/^static char \*mxdma_type_devnode(/,/^}/p' init.c)
class_devnode_block=$(sed -n '/^static char \*mxdma_devnode(/,/^}/p' init.c)
[[ -n $type_devnode_block && -n $class_devnode_block ]]
search_fixed 'mx_cdev->dev.type = &mxdma_cdev_type;' init.c
if ! grep -Fq '*mode = dev_mode;' <<<"$type_devnode_block" ||
   ! grep -Fq '*gid = KGIDT_INIT(dev_gid);' <<<"$type_devnode_block" ||
   grep -Eq '\*mode[[:space:]]*=' <<<"$class_devnode_block"; then
    echo "only the type devnode may set the node mode and gid" >&2
    exit 1
fi
if ! grep -Fq 'if (mode)' <<<"$type_devnode_block" || ! grep -Fq 'if (gid)' <<<"$type_devnode_block"; then
    echo "the type devnode must check mode and gid for NULL" >&2
    exit 1
fi
search_fixed 'if (dev_mode == 0 || dev_mode > 0777 || !gid_valid(KGIDT_INIT(dev_gid))) {' init.c
swait_calls=$(count_re '__swait_event_interruptible_timeout\(' < core_common.c)
stop_conds=$(count_re '\|\| kthread_should_stop\(\),' < core_common.c)
if [ "$swait_calls" -lt 2 ] || [ "$swait_calls" -ne "$stop_conds" ]; then
    echo "every io handler wait must also wake for kthread_stop()" >&2
    exit 1
fi
search_fixed 'if (pushed_count || list_empty(&q->sq_list))' core_common.c
search_fixed 'if (popped_any || mx_queue_live_count(q) <= 0)' core_common.c
cq_timeout_block=$(sed -n '/^static long mx_cq_idle_timeout(/,/^}/p' core_common.c)
[[ -n $cq_timeout_block ]]
if grep -Fq 'MAX_SCHEDULE_TIMEOUT' <<<"$cq_timeout_block"; then
    echo "the CQ handler must keep a bounded wait" >&2
    exit 1
fi
search_fixed 'mx_cq_idle_timeout(q));' core_common.c
search_fixed 'static unsigned int cpu_latency_qos_us = PM_QOS_CPU_LATENCY_DEFAULT_VALUE;' init.c
if scan_c_or_h_sources 'MX_CPU_LATENCY_QOS_US'; then
    echo "the CPU latency QoS must not have a built-in always-on value" >&2
    exit 1
fi
if scan_c_sources 'cpu_latency_qos_(add|update|remove)_request' | grep -Ev '^(\./)?init\.c:' | grep -q .; then
    echo "only init.c may change the CPU latency QoS request" >&2
    exit 1
fi
add_line=$(grep -n -A1 'cpu_latency_qos_add_request(&mx_pdev->cpu_latency_req,' init.c | tail -1)
grep -Fq 'PM_QOS_CPU_LATENCY_DEFAULT_VALUE' <<<"$add_line"
registry_adds=$(count_re 'list_add_tail\(&mx_pdev->registry_entry, &mx_device_list_head\);' < init.c)
applied_adds=$(grep -A1 'list_add_tail(&mx_pdev->registry_entry, &mx_device_list_head);' init.c | \
    count_re 'mx_apply_cpu_latency_qos_locked\(mx_pdev\);')
if [ "$registry_adds" -lt 3 ] || [ "$registry_adds" -ne "$applied_adds" ]; then
    echo "every registry insertion must apply the CPU latency QoS parameter" >&2
    exit 1
fi
qos_set_block=$(sed -n '/^static int cpu_latency_qos_us_set(/,/^}/p' init.c)
grep -Fq 'mutex_lock(&mx_device_list_lock);' <<<"$qos_set_block"
grep -Fq 'list_for_each_entry(mx_pdev, &mx_device_list_head, registry_entry)' <<<"$qos_set_block"
grep -Fq 'mx_apply_cpu_latency_qos_locked(mx_pdev);' <<<"$qos_set_block"
apply_block=$(sed -n '/^static void mx_apply_cpu_latency_qos_locked(/,/^}/p' init.c)
grep -Fq 'lockdep_assert_held(&mx_device_list_lock);' <<<"$apply_block"
grep -Fq 'cpu_latency_qos_request_active(&mx_pdev->cpu_latency_req)' <<<"$apply_block"
grep -Fq 'cpu_latency_qos_update_request(&mx_pdev->cpu_latency_req, cpu_latency_qos_us);' <<<"$apply_block"
if grep -Fq 'cpu_latency_qos_us ?' <<<"$apply_block"; then
    echo "cpu_latency_qos_us must not remap 0 away from the cpu_dma_latency meaning" >&2
    exit 1
fi
grep -Fq 'if (us > PM_QOS_CPU_LATENCY_DEFAULT_VALUE)' <<<"$qos_set_block"
if grep -n '_INTERVAL_MSEC' core_common.c | grep -v 'msecs_to_jiffies(' | grep -q .; then
    echo "poll intervals in ms must be converted with msecs_to_jiffies()" >&2
    exit 1
fi
backoff_base=$(sed -n 's/^#define BACKOFF_BASE_SLEEP_US[[:space:]]*\([0-9][0-9]*\).*/\1/p' core_common.c)
if [ -z "$backoff_base" ] || [ "$backoff_base" -gt 16 ]; then
    echo "the first poll backoff sleep must stay short" >&2
    exit 1
fi
search_fixed 'BUILD_BUG_ON((BACKOFF_BASE_SLEEP_US << BACKOFF_MAX_SHIFT) < BACKOFF_MAX_SLEEP_US);' core_common.c
if search_fixed 'set_page_dirty_lock' transfer.c; then
    echo "DMA-written pages must be dirtied by unpin_user_pages_dirty_lock()" >&2
    exit 1
fi
unpin_call=$(grep -A1 'unpin_user_pages_dirty_lock(ctx->pages, ctx->pages_nr,' transfer.c)
grep -Fq 'ctx->dir != DMA_TO_DEVICE);' <<<"$unpin_call"
sq_list_add_re='list_add(_tail)?\([^;]*sq_list\)'
if scan_c_or_h_sources "$sq_list_add_re" | grep -Ev '^(\./)?core_common\.c:' | grep -q .; then
    echo "only mx_queue_submit() may add to the SQ list" >&2
    exit 1
fi
queue_submit_block=$(sed -n '/^void mx_queue_submit(/,/^}/p' core_common.c)
sq_list_adds=$(count_re "$sq_list_add_re" < core_common.c || true)
sq_list_adds_in_submit=$(count_re "$sq_list_add_re" <<<"$queue_submit_block" || true)
if [ "${sq_list_adds:-0}" -ne 1 ] || [ "${sq_list_adds_in_submit:-0}" -ne 1 ]; then
    echo "only mx_queue_submit() may add to the SQ list" >&2
    exit 1
fi
push_block=$(sed -n '/^static unsigned int mx_sq_push_locked(/,/^}/p' core_common.c)
grep -Fq 'lockdep_assert_held(&q->sq_lock);' <<<"$push_block"
grep -A1 -F 'WRITE_ONCE(q->lv_progress_jiffies, jiffies);' <<<"$push_block" | \
    grep -Fq '*went_live = true;'
grep -Fq 'pushed_count = mx_sq_push_locked(q, lv_on, &went_live);' <<<"$queue_submit_block"
grep -A1 -F 'if (backlog || (lv_on && went_live))' <<<"$queue_submit_block" | \
    grep -Fq 'swake_up_one(&q->sq_wait);'
sq_timeout_block=$(sed -n '/^static long mx_sq_idle_timeout(/,/^}/p' core_common.c)
grep -Fq 'if (lv_on && (' <<<"$sq_timeout_block"
submit_handler_block=$(sed -n '/^int mx_submit_handler(/,/^}/p' core_common.c)
grep -Fq 'READ_ONCE(q->mx_pdev->liveness_enable) != lv_on ||' <<<"$submit_handler_block"
grep -Fq 'rearm = lv_on && timeout == MAX_SCHEDULE_TIMEOUT;' <<<"$submit_handler_block"
grep -Fq '(rearm && mx_queue_live_count(q) > 0) || kthread_should_stop(),' <<<"$submit_handler_block"
grep -Fq 'timeout = mx_sq_idle_timeout(q, lv_on);' <<<"$submit_handler_block"
zombie_lower_re='atomic_(dec|sub)[a-z_]*\([^;]*zombie_wait_count'
zombie_lower_sites=$(scan_c_or_h_sources "$zombie_lower_re" || true)
drain_block=$(sed -n '/^static void drain_zombie_list(/,/^}/p' transfer.c)
if [ "$(grep -c . <<<"$zombie_lower_sites")" -ne 1 ] ||
   [ "$(count_re "$zombie_lower_re" <<<"$drain_block" || true)" != 1 ]; then
    echo "only drain_zombie_list() may lower zombie_wait_count" >&2
    exit 1
fi
zombie_dec_line=$(grep -nE "$zombie_lower_re" <<<"$drain_block" | cut -d: -f1)
zombie_wake_line=$(grep -nF 'swake_up_one(&mx_pdev->io_queue->sq_wait);' <<<"$drain_block" | cut -d: -f1 || true)
if [ -z "$zombie_wake_line" ] || [ "$zombie_wake_line" -le "$zombie_dec_line" ]; then
    echo "draining a zombie must wake the submit thread after lowering zombie_wait_count" >&2
    exit 1
fi
liveness_store_block=$(sed -n '/^static ssize_t liveness_enable_store(/,/^}/p' init.c)
grep -A1 -F 'spin_unlock_irqrestore(&q->sq_lock, flags);' <<<"$liveness_store_block" | \
    grep -Fq 'swake_up_one(&q->sq_wait);'
prewake_block=$(sed -n '/^static inline void mx_prewake_handlers(/,/^}/p' mx_dma.h)
grep -Fq 'swake_up_one(&q->cq_wait);' <<<"$prewake_block"
if grep -Fq 'sq_wait' <<<"$prewake_block"; then
    echo "submitters push themselves, so the submit thread is not prewoken" >&2
    exit 1
fi

echo "lifecycle source invariants: PASS"
