/* SPDX-License-Identifier: <SPDX License Expression> */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mx_dma

#if !defined(_MX_DMA_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _MX_DMA_TRACE_H

#include <linux/tracepoint.h>

/* IO_OPCODE_* values from mx_dma.h (anonymous enum). Hardcoded to avoid
 * pulling the full driver header into the multi-include trace context. */
#define mx_dma_opcode_names		\
	{ 0,  "R_DATA"   },		\
	{ 1,  "W_DATA"   },		\
	{ 2,  "R_CTX"    },		\
	{ 3,  "W_CTX"    },		\
	{ 4,  "R_SQ"     },		\
	{ 5,  "W_SQ"     },		\
	{ 6,  "R_CQ"     },		\
	{ 7,  "W_CQ"     },		\
	{ 8,  "PASSTHRU" },		\
	{ 9,  "SEND"     },		\
	{ 10, "RECV"     }

/* enum dma_data_direction values (kernel <linux/dma-direction.h>). */
#define mx_dma_dir_names		\
	{ 0, "BIDIR"       },		\
	{ 1, "TO_DEVICE"   },		\
	{ 2, "FROM_DEVICE" },		\
	{ 3, "NONE"        }

/* Wait-exit terminal state. See enum mx_dma_wait_state in transfer.c. */
#define mx_dma_wait_state_names		\
	{ 0, "COMPLETED"      },	\
	{ 1, "RECOVERED"      },	\
	{ 2, "LATE_COMPLETED" },	\
	{ 3, "ZOMBIE"         }

/* _IOC_NR() of MX_IOCTL_* macros from ioctl.c */
#define mx_dma_ioctl_nr_names		\
	{ 1,  "REGISTER_MBOX"      },	\
	{ 2,  "INIT_MBOX"          },	\
	{ 3,  "SEND_CMD_WITH_DATA" },	\
	{ 4,  "RECV_CMDS"          },	\
	{ 5,  "SEND_CMDS"          },	\
	{ 6,  "READ_DATA"          },	\
	{ 7,  "WRITE_DATA"         },	\
	{ 8,  "PASSTHRU_CMD"       },	\
	{ 9,  "HIO_SEND"           },	\
	{ 10, "HIO_RECV"           },	\
	{ 11, "GET_LIVENESS"       }

/* --- ioctl entry / exit --- */

TRACE_EVENT(mx_dma_ioctl_enter,
	TP_PROTO(int dev_id, unsigned int cmd),
	TP_ARGS(dev_id, cmd),
	TP_STRUCT__entry(
		__field(int, dev_id)
		__field(unsigned int, cmd)
		__field(u8, nr)
	),
	TP_fast_assign(
		__entry->dev_id = dev_id;
		__entry->cmd = cmd;
		__entry->nr = _IOC_NR(cmd);
	),
	TP_printk("dev=%d cmd=%s(0x%x)",
		__entry->dev_id,
		__print_symbolic(__entry->nr, mx_dma_ioctl_nr_names),
		__entry->cmd)
);

TRACE_EVENT(mx_dma_ioctl_exit,
	TP_PROTO(int dev_id, unsigned int cmd, long ret),
	TP_ARGS(dev_id, cmd, ret),
	TP_STRUCT__entry(
		__field(int, dev_id)
		__field(unsigned int, cmd)
		__field(u8, nr)
		__field(long, ret)
	),
	TP_fast_assign(
		__entry->dev_id = dev_id;
		__entry->cmd = cmd;
		__entry->nr = _IOC_NR(cmd);
		__entry->ret = ret;
	),
	TP_printk("dev=%d cmd=%s ret=%ld",
		__entry->dev_id,
		__print_symbolic(__entry->nr, mx_dma_ioctl_nr_names),
		__entry->ret)
);

/* --- ctrl/data transfer enter/exit pairs ---
 *
 * One unified enter class (size, dev_addr, nowait) is used for both read and
 * write; for reads the call site passes nowait=false.
 */

DECLARE_EVENT_CLASS(mx_dma_xfer_enter_class,
	TP_PROTO(int dev_id, u32 qid, int opcode, size_t size, u64 dev_addr, bool nowait),
	TP_ARGS(dev_id, qid, opcode, size, dev_addr, nowait),
	TP_STRUCT__entry(
		__field(int, dev_id)
		__field(u32, qid)
		__field(int, opcode)
		__field(size_t, size)
		__field(u64, dev_addr)
		__field(bool, nowait)
	),
	TP_fast_assign(
		__entry->dev_id = dev_id;
		__entry->qid = qid;
		__entry->opcode = opcode;
		__entry->size = size;
		__entry->dev_addr = dev_addr;
		__entry->nowait = nowait;
	),
	TP_printk("dev=%d qid=%u op=%s size=%zu dev_addr=0x%llx nowait=%d",
		__entry->dev_id,
		__entry->qid,
		__print_symbolic(__entry->opcode, mx_dma_opcode_names),
		__entry->size,
		__entry->dev_addr,
		__entry->nowait)
);

DEFINE_EVENT(mx_dma_xfer_enter_class, mx_dma_ctrl_read_enter,
	TP_PROTO(int dev_id, u32 qid, int opcode, size_t size, u64 dev_addr, bool nowait),
	TP_ARGS(dev_id, qid, opcode, size, dev_addr, nowait));

DEFINE_EVENT(mx_dma_xfer_enter_class, mx_dma_ctrl_write_enter,
	TP_PROTO(int dev_id, u32 qid, int opcode, size_t size, u64 dev_addr, bool nowait),
	TP_ARGS(dev_id, qid, opcode, size, dev_addr, nowait));

DEFINE_EVENT(mx_dma_xfer_enter_class, mx_dma_data_read_enter,
	TP_PROTO(int dev_id, u32 qid, int opcode, size_t size, u64 dev_addr, bool nowait),
	TP_ARGS(dev_id, qid, opcode, size, dev_addr, nowait));

DEFINE_EVENT(mx_dma_xfer_enter_class, mx_dma_data_write_enter,
	TP_PROTO(int dev_id, u32 qid, int opcode, size_t size, u64 dev_addr, bool nowait),
	TP_ARGS(dev_id, qid, opcode, size, dev_addr, nowait));

DECLARE_EVENT_CLASS(mx_dma_xfer_exit_class,
	TP_PROTO(int dev_id, u32 qid, int opcode, ssize_t ret),
	TP_ARGS(dev_id, qid, opcode, ret),
	TP_STRUCT__entry(
		__field(int, dev_id)
		__field(u32, qid)
		__field(int, opcode)
		__field(ssize_t, ret)
	),
	TP_fast_assign(
		__entry->dev_id = dev_id;
		__entry->qid = qid;
		__entry->opcode = opcode;
		__entry->ret = ret;
	),
	TP_printk("dev=%d qid=%u op=%s ret=%zd",
		__entry->dev_id,
		__entry->qid,
		__print_symbolic(__entry->opcode, mx_dma_opcode_names),
		__entry->ret)
);

DEFINE_EVENT(mx_dma_xfer_exit_class, mx_dma_ctrl_read_exit,
	TP_PROTO(int dev_id, u32 qid, int opcode, ssize_t ret),
	TP_ARGS(dev_id, qid, opcode, ret));

DEFINE_EVENT(mx_dma_xfer_exit_class, mx_dma_ctrl_write_exit,
	TP_PROTO(int dev_id, u32 qid, int opcode, ssize_t ret),
	TP_ARGS(dev_id, qid, opcode, ret));

DEFINE_EVENT(mx_dma_xfer_exit_class, mx_dma_data_read_exit,
	TP_PROTO(int dev_id, u32 qid, int opcode, ssize_t ret),
	TP_ARGS(dev_id, qid, opcode, ret));

DEFINE_EVENT(mx_dma_xfer_exit_class, mx_dma_data_write_exit,
	TP_PROTO(int dev_id, u32 qid, int opcode, ssize_t ret),
	TP_ARGS(dev_id, qid, opcode, ret));

/* --- transfer lifecycle (enqueue / submit / complete / wait_exit) ---
 *
 * xfer_id is the IDR-allocated transfer->id and is globally unique across
 * devices, so submit/complete events omit dev_id — analysis scripts join on
 * xfer_id back to the enqueue event when per-device filtering is needed.
 */

TRACE_EVENT(mx_dma_xfer_enqueue,
	TP_PROTO(int dev_id, u32 xfer_id, int opcode, int dir, size_t size,
		 bool is_sg, int parallel_idx, int parallel_count),
	TP_ARGS(dev_id, xfer_id, opcode, dir, size, is_sg, parallel_idx, parallel_count),
	TP_STRUCT__entry(
		__field(int, dev_id)
		__field(u32, xfer_id)
		__field(int, opcode)
		__field(int, dir)
		__field(size_t, size)
		__field(bool, is_sg)
		__field(int, parallel_idx)
		__field(int, parallel_count)
	),
	TP_fast_assign(
		__entry->dev_id = dev_id;
		__entry->xfer_id = xfer_id;
		__entry->opcode = opcode;
		__entry->dir = dir;
		__entry->size = size;
		__entry->is_sg = is_sg;
		__entry->parallel_idx = parallel_idx;
		__entry->parallel_count = parallel_count;
	),
	TP_printk("dev=%d xfer=%u op=%s dir=%s size=%zu is_sg=%d (%d/%d)",
		__entry->dev_id,
		__entry->xfer_id,
		__print_symbolic(__entry->opcode, mx_dma_opcode_names),
		__print_symbolic(__entry->dir, mx_dma_dir_names),
		__entry->size,
		__entry->is_sg,
		__entry->parallel_idx,
		__entry->parallel_count)
);

TRACE_EVENT(mx_dma_xfer_submit,
	TP_PROTO(u32 xfer_id, bool no_completion),
	TP_ARGS(xfer_id, no_completion),
	TP_STRUCT__entry(
		__field(u32, xfer_id)
		__field(bool, no_completion)
	),
	TP_fast_assign(
		__entry->xfer_id = xfer_id;
		__entry->no_completion = no_completion;
	),
	TP_printk("xfer=%u no_completion=%d",
		__entry->xfer_id,
		__entry->no_completion)
);

TRACE_EVENT(mx_dma_xfer_complete,
	TP_PROTO(u32 xfer_id, u8 status, u64 result, bool is_zombie),
	TP_ARGS(xfer_id, status, result, is_zombie),
	TP_STRUCT__entry(
		__field(u32, xfer_id)
		__field(u8, status)
		__field(u64, result)
		__field(bool, is_zombie)
	),
	TP_fast_assign(
		__entry->xfer_id = xfer_id;
		__entry->status = status;
		__entry->result = result;
		__entry->is_zombie = is_zombie;
	),
	TP_printk("xfer=%u status=0x%x result=0x%llx is_zombie=%d",
		__entry->xfer_id,
		__entry->status,
		__entry->result,
		__entry->is_zombie)
);

/* Completion arrived for a transfer that find_transfer_by_id could not match
 * (already freed, late after zombie cleanup, or HW corruption).  Separate
 * event so analysis can distinguish alive vs orphan without an is_zombie
 * sentinel. */
TRACE_EVENT(mx_dma_xfer_complete_orphan,
	TP_PROTO(u32 xfer_id, u8 status, u64 result),
	TP_ARGS(xfer_id, status, result),
	TP_STRUCT__entry(
		__field(u32, xfer_id)
		__field(u8, status)
		__field(u64, result)
	),
	TP_fast_assign(
		__entry->xfer_id = xfer_id;
		__entry->status = status;
		__entry->result = result;
	),
	TP_printk("xfer=%u status=0x%x result=0x%llx",
		__entry->xfer_id,
		__entry->status,
		__entry->result)
);

TRACE_EVENT(mx_dma_xfer_wait_exit,
	TP_PROTO(int dev_id, u32 xfer_id, ssize_t ret, int state),
	TP_ARGS(dev_id, xfer_id, ret, state),
	TP_STRUCT__entry(
		__field(int, dev_id)
		__field(u32, xfer_id)
		__field(ssize_t, ret)
		__field(int, state)
	),
	TP_fast_assign(
		__entry->dev_id = dev_id;
		__entry->xfer_id = xfer_id;
		__entry->ret = ret;
		__entry->state = state;
	),
	TP_printk("dev=%d xfer=%u ret=%zd state=%s",
		__entry->dev_id,
		__entry->xfer_id,
		__entry->ret,
		__print_symbolic(__entry->state, mx_dma_wait_state_names))
);

#endif /* _MX_DMA_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>
