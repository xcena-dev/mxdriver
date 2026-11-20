/* SPDX-License-Identifier: <SPDX License Expression> */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/version.h>

#ifndef RHEL_RELEASE_CODE
#define RHEL_RELEASE_CODE 0
#endif
#ifndef RHEL_RELEASE_VERSION
#define RHEL_RELEASE_VERSION(a, b) 1
#endif
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/kref.h>
#include <linux/numa.h>
#include <linux/percpu-refcount.h>
#include <linux/pm_qos.h>
#include <linux/poll.h>
#include <linux/path.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/scatterlist.h>
#include <linux/swait.h>
#include <linux/timekeeping.h>
#include <linux/topology.h>
#include <linux/percpu-refcount.h>
#include <linux/uaccess.h>

#include <asm/current.h>
#include <asm/cacheflush.h>

#include "include/uapi/mx_dma_lease.h"
#include "lease_sm.h"

#ifndef CONFIG_WO_CXL
#define MEM_NAME_LEN		(3)
#define MXDMA_MEM_NAME		"mem"
#endif

#define MXDMA_NODE_NAME		"mx_dma"
#define XCENA_PCI_VENDOR_ID	0x20A6

#define MAGIC_COMMAND		0x1234
#define MAGIC_CHAR		0xCCCCCCCCUL
#define MAGIC_DEVICE		0xDDDDDDDDUL

#define MXDMA_BAR_INDEX		2

#define MAX_NUM_OF_MBOX		80
#define HMBOX_UPDATE_BITMASK	(1ull << 18)

#define POLLING_INTERVAL_MSEC	4
#define ZOMBIE_POLL_INTERVAL_MSEC	1000

#define LIVENESS_WAIT_CHUNK_MIN_MSEC	50u
#define LIVENESS_MAX_MULT_CEIL		1000u
#define LIVENESS_STALL_MS_DEFAULT	1000u
#define LIVENESS_DEAD_MS_DEFAULT	5000u
#define LIVENESS_MAX_MULT_DEFAULT	10u

#define MX_PAGES_INLINE_NR	1

#define MX_CMD_INLINE_SIZE	64

enum {
	MX_CDEV_IOCTL = 0,
	MX_CDEV_EVENT,
	MX_CDEV_BDF,
	NUM_OF_MX_CDEV,
};

static const char * const node_name[] __maybe_unused = {
	MXDMA_NODE_NAME "%d_ioctl",
	MXDMA_NODE_NAME "%d_event",
	MXDMA_NODE_NAME "%d_bdf",
};

enum {
	ADMIN_OPCODE_CREATE_IO_CQ = 0,
	ADMIN_OPCODE_DELETE_IO_CQ,
	ADMIN_OPCODE_CREATE_IO_SQ,
	ADMIN_OPCODE_DELETE_IO_SQ,
};

enum {
	IO_OPCODE_DATA_READ = 0,
	IO_OPCODE_DATA_WRITE,
	IO_OPCODE_CONTEXT_READ,
	IO_OPCODE_CONTEXT_WRITE,
	IO_OPCODE_SQ_READ,
	IO_OPCODE_SQ_WRITE,
	IO_OPCODE_CQ_READ,
	IO_OPCODE_CQ_WRITE,
	IO_OPCODE_PASSTHRU,
	IO_OPCODE_SEND,
	IO_OPCODE_RECV,
	IO_OPCODE_PING,
};

static const char * const mxdma_op_name[] __maybe_unused = {
	"R_DATA(0)",
	"W_DATA(1)",
	"R_CTX(2)",
	"W_CTX(3)",
	"R_SQ(4)",
	"W_SQ(5)",
	"R_CQ(6)",
	"W_CQ(7)",
	"PASSTHRU(8)",
	"SEND(9)",
	"RECV(10)",
	"PING(11)",
};

#define MX_PING_ID		((1 << 16) - 1)

enum mx_liveness_health {
	MX_LIVENESS_UNKNOWN	= 0,
	MX_LIVENESS_ALIVE	= 1,
	MX_LIVENESS_SUSPECT	= 2,
	MX_LIVENESS_DEAD	= 3,
};

typedef union {
	struct {
		uint8_t index :7;
		uint8_t phase :1;
	};
	uint8_t full;
} mbox_index_t;

typedef union {
	struct {
		uint64_t mid : 8;
		uint64_t ctx_base : 16;
		uint64_t data_base : 16;
		uint64_t q_size : 4;
		uint64_t data_size : 4;
		uint64_t tail : 8;
		uint64_t head : 8;
	};
	uint64_t u64;
} mbox_context_t;

struct mx_mbox {
	uint64_t r_ctx_addr;
	uint64_t w_ctx_addr;
	uint64_t data_addr;
	mbox_context_t ctx;
	uint32_t depth;
	struct mutex lock;
};

struct mx_sg_context {
	refcount_t refcount;
	struct mx_pci_dev *mx_pdev;
	enum dma_data_direction dir;
	void __user *user_addr;
	size_t total_size;

	struct sg_table sgt;
	struct page **pages;
	int pages_nr;

	struct page		*pages_inline[MX_PAGES_INLINE_NR];
	struct scatterlist	 sg_inline[MX_PAGES_INLINE_NR];
};

struct mx_file_ctx;

struct mx_transfer {
	int id;
	void __user *user_addr;
	size_t size;
	uint64_t device_addr;
	enum dma_data_direction dir;

	struct mx_pci_dev *mx_pdev;
	struct mx_file_ctx *owner_ctx;
	struct file *slot_proof_file;
	struct file *lifetime_proof_file;
	struct work_struct work;

	void *command;
	struct list_head entry;
	struct completion done;
	uint64_t result;
	bool is_sg;

	bool     no_completion;
	uint8_t  status;

	bool is_zombie;
	atomic_t wait_claimed;
	unsigned long zombie_timestamp;
	struct list_head zombie_entry;

	struct mx_sg_context *sg_ctx;
	size_t sg_byte_offset;

	int desc_list_cnt;
	void **desc_list_va;
	dma_addr_t *desc_list_ba;

	uint8_t			 cmd_inline[MX_CMD_INLINE_SIZE] __aligned(8);
};

struct mx_event {
	atomic_t count;
	wait_queue_head_t wq;
};

struct mx_char_dev {
	unsigned long magic;
	struct mx_pci_dev *mx_pdev;
	struct cdev cdev;
	struct device dev;
	dev_t cdev_no;
	int type;

	bool nowait;
	bool enabled;
};

#define MAGIC_FILE_CTX		0x46494c454354584dUL

struct mx_device_lease {
	struct mutex lock;
	struct mx_lease_sm state;
	struct path state_path;
	bool state_path_valid;
	struct inode *state_inode;
	struct path slot_domain_path;
	bool slot_domain_path_valid;
	struct inode *slot_domain_inode;
	u64 state_identity[2];
	u32 state_family;
	u64 device_incarnation;
	u64 generation_counter;
	u64 generation;
	bool removed;
};

struct mx_file_ctx {
	unsigned long magic;
	refcount_t refs;
	struct mx_char_dev *mx_cdev;
	struct mx_pci_dev *mx_pdev;
	struct mx_lease_sm_holder lease;
	struct file *slot_source_file;
	struct file *slot_proof_file;
	struct file *lifetime_source_file;
	struct file *lifetime_proof_file;
	u64 proof_lifetime_tag;
	u32 proof_slot;
	u32 transfer_count;
	u32 direct_count;
	u32 bar_mapping_count;
	bool proofs_ever_bound;
};

struct mx_bar_map;

struct mx_bar_vma {
	struct mx_bar_map *bar_map;
	struct mx_pci_dev *mx_pdev;
	struct mx_file_ctx *owner_ctx;
	refcount_t refs;
};

struct mx_queue;

struct mx_completion_info {
	int id;
	uint64_t result;
	uint8_t status;
};

struct mx_queue_ops {
	bool (*is_pushable)(struct mx_queue *q);
	void (*push_command)(struct mx_queue *q, void *command);
	void (*post_submit)(struct mx_queue *q);

	bool (*is_popable)(struct mx_queue *q);
	void (*pop_completion)(struct mx_queue *q, struct mx_completion_info *info);
	void (*post_complete)(struct mx_queue *q);
	void (*build_ping_command)(void *cmd);
};

struct mx_queue {
	struct device *dev;
	struct mx_pci_dev *mx_pdev;
	struct list_head sq_list;
	spinlock_t sq_lock;
	atomic_t wait_count;
	atomic_t zombie_wait_count;
	struct swait_queue_head sq_wait;
	struct swait_queue_head cq_wait;
	const struct mx_queue_ops *ops;

	atomic_t lv_health;
	atomic_t lv_inflight;
	u64 lv_sent_ns;
	unsigned long lv_progress_jiffies;
	u64 lv_rtt_ns;
	uint8_t lv_ping_cmd[MX_CMD_INLINE_SIZE] __aligned(8);
};

struct mx_operations {
	int (*init_queue) (struct mx_pci_dev *);
	int (*release_queue) (struct mx_pci_dev *);
	int (*recover_queue) (struct mx_pci_dev *);
	void (*free_queue) (struct mx_pci_dev *);
	void * (*create_command_sg) (struct mx_pci_dev *, struct mx_transfer *, int);
	void * (*create_command_ctrl) (struct mx_transfer *, int);
	void * (*create_command_passthru) (struct mx_transfer *, int subopcode);
} __randomize_layout;

struct mx_bar_map {
	struct kref kref;
	struct mutex lock;
	unsigned int count;
	struct address_space *mapping;
};

struct mx_pci_dev {
	unsigned long magic;
	struct kref ref;
	struct percpu_ref io_ref;
	wait_queue_head_t io_quiesce_wq;
	int dev_id;
	dev_t dev_no;

	struct pci_dev *pdev;
	struct list_head registry_entry;
	bool enabled;
	struct mx_device_lease lease;
	char bdf[32];
	bool chrdev_region_allocated;
	bool bar_region_requested;
	bool queues_initialized;
	bool msi_enabled_by_driver;
	bool dma_reclaim_safe;
	bool queue_dma_programmed;
	bool protocol_poisoned;
	bool teardown_started;
	bool module_ref_held;
	bool pci_ref_held;
	bool pci_enabled_by_driver;
	bool bus_master_enabled_by_driver;
	bool readrq_changed;
	bool dma_mask_changed;
	bool coherent_dma_mask_changed;
	bool max_seg_size_changed;
	bool min_align_mask_changed;
	int saved_readrq;
	u64 saved_dma_mask;
	u64 saved_coherent_dma_mask;
	unsigned int saved_max_seg_size;
	unsigned int saved_min_align_mask;
	int attach_error;

	bool liveness_enable;
	unsigned int liveness_stall_ms;
	unsigned int liveness_dead_ms;
	unsigned int liveness_max_mult;

	void __iomem *bar;
	resource_size_t bar_mapped_size;

	bool irq_requested;

	struct mx_bar_map *bar_map;

	struct mx_operations ops;

	struct mx_event event;

	struct mx_queue *admin_queue;
	struct mx_queue *io_queue;

	int reserved_hio_qid;

	struct mx_mbox *sq_mbox_list[MAX_NUM_OF_MBOX];
	struct mx_mbox *cq_mbox_list[MAX_NUM_OF_MBOX];

	struct task_struct *submit_thread;
	struct task_struct *complete_thread;
	struct workqueue_struct *async_wq;

	int num_of_cdev;
	struct mx_char_dev mx_cdev[NUM_OF_MX_CDEV];

	size_t page_size;
	struct dma_pool *page_pool;

	struct list_head zombie_list;
	spinlock_t zombie_lock;
	struct task_struct *zombie_cleanup_thread;

	struct pm_qos_request cpu_latency_req;
};

extern const struct file_operations *mxdma_fops_array[];

extern struct kmem_cache *mx_transfer_cache;

int transfer_id_alloc(void *ptr);
void transfer_id_free(struct mx_transfer *transfer);
struct mx_transfer *transfer_id_claim_completion(unsigned long id,
						 unsigned long *flags);
void transfer_id_complete_unlock(unsigned long flags);
int zombie_cleanup_handler(void *data);

ssize_t read_data_from_device_parallel(struct mx_file_ctx *ctx, char __user *buf, size_t size, loff_t *fpos, int opcode);
ssize_t write_data_to_device_parallel(struct mx_file_ctx *ctx, const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait);

ssize_t read_data_from_device(struct mx_file_ctx *ctx, char __user *buf, size_t size, loff_t *fpos, int opcode);
ssize_t write_data_to_device(struct mx_file_ctx *ctx, const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait);

ssize_t read_ctrl_from_device(struct mx_file_ctx *ctx, char __user *buf, size_t size, loff_t *fpos, int opcode);
ssize_t write_ctrl_to_device(struct mx_file_ctx *ctx, const char __user *buf, size_t size, loff_t *fpos, int opcode, bool nowait);

long ioctl_to_device(struct mx_file_ctx *ctx, unsigned int cmd, unsigned long arg);
bool mx_ioctl_is_direct_cmd(unsigned int cmd);
void mx_free_registered_mboxes(struct mx_pci_dev *mx_pdev);

bool mx_pdev_get_live(struct mx_pci_dev *mx_pdev);
void mx_pdev_put(struct mx_pci_dev *mx_pdev);
void mx_lease_init(struct mx_pci_dev *mx_pdev);
void mx_lease_mark_removed(struct mx_pci_dev *mx_pdev);
void mx_file_ctx_get(struct mx_file_ctx *ctx);
void mx_file_ctx_put(struct mx_file_ctx *ctx);
int mx_lease_transfer_get(struct mx_file_ctx *ctx, struct mx_transfer *transfer);
void mx_lease_transfer_put(struct mx_transfer *transfer);
bool mx_lease_ioctl_cmd(unsigned int cmd);
long mx_lease_ioctl(struct mx_file_ctx *ctx, unsigned int cmd, unsigned long arg);
int mx_lease_direct_begin(struct mx_file_ctx *ctx);
void mx_lease_direct_end(struct mx_file_ctx *ctx);
int mx_lease_authorize_no_completion(struct mx_file_ctx *ctx);
int mx_lease_authorize_memory_cmd(struct mx_file_ctx *ctx, u16 subopcode);

long submit_passthru_command(struct mx_file_ctx *ctx, int subopcode,
				    uint64_t device_addr, uint64_t size, bool no_completion,
				    uint8_t *out_status, uint64_t *out_host_addr);

ssize_t submit_protocol_transfer(struct mx_file_ctx *ctx, char __user *buf, size_t size, int opcode);

int desc_list_alloc(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer, int list_cnt);
void desc_list_free(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer);

int mx_get_list_count(size_t total_desc_cnt, int descs_per_list);
int mx_get_total_desc_count(struct scatterlist *sg, size_t intra_off, size_t byte_size,
			    size_t dma_size, size_t *out_cnt);
int mx_desc_list_init(struct mx_pci_dev *mx_pdev, struct mx_transfer *transfer,
		      size_t dma_size, int descs_per_list, bool skip_first_entry,
		      size_t desc_cnt, uint64_t *out_ba);

int mx_sg_locate(struct sg_table *sgt, size_t byte_offset,
		 struct scatterlist **out_sg, size_t *out_intra);

size_t mx_prp_first_chunk_len(struct scatterlist *sg, size_t intra_off, size_t dma_size);

void mx_queue_common_init(struct mx_queue *q, struct mx_pci_dev *mx_pdev, const struct mx_queue_ops *ops);
int mx_start_io_queue(struct mx_pci_dev *mx_pdev, struct mx_queue *q);
void mx_stop_queue_threads(struct mx_pci_dev *mx_pdev);
void mx_queue_submit(struct mx_queue *q, struct mx_transfer **transfers, int count);

static inline int mx_fill_ctrl_doorbell(struct mx_transfer *transfer, uint64_t *doorbell)
{
	if (transfer->dir != DMA_TO_DEVICE)
		return 0;

	if (access_ok(transfer->user_addr, transfer->size)) {
		if (copy_from_user(doorbell, transfer->user_addr, sizeof(*doorbell))) {
			pr_warn("Failed to copy_from_user (%llx <- %llx)\n",
					(uint64_t)doorbell, (uint64_t)transfer->user_addr);
			return -EFAULT;
		}
	} else {
		*doorbell = *(uint64_t *)transfer->user_addr;
	}

	return 0;
}

int mx_submit_handler(void *arg);
int mx_complete_handler(void *arg);

static inline void mx_prewake_handlers(struct mx_pci_dev *mx_pdev)
{
	struct mx_queue *q = mx_pdev ? mx_pdev->io_queue : NULL;

	if (!q)
		return;
	swake_up_one(&q->cq_wait);
}

static inline void mx_bind_handlers_to_numa(struct mx_pci_dev *mx_pdev)
{
	const struct cpumask *mask;
	int node;

	if (!mx_pdev || !mx_pdev->pdev)
		return;

	node = dev_to_node(&mx_pdev->pdev->dev);
	if (node == NUMA_NO_NODE)
		return;

	mask = cpumask_of_node(node);
	if (cpumask_empty(mask))
		return;

	if (!IS_ERR_OR_NULL(mx_pdev->submit_thread))
		WARN_ON_ONCE(set_cpus_allowed_ptr(mx_pdev->submit_thread, mask));
	if (!IS_ERR_OR_NULL(mx_pdev->complete_thread))
		WARN_ON_ONCE(set_cpus_allowed_ptr(mx_pdev->complete_thread, mask));
}

struct mx_bar_map *mx_bar_map_alloc(void);
void mx_bar_map_put(struct mx_bar_map *bar_map);
int mx_bar_mmap(struct mx_pci_dev *mx_pdev, struct mx_file_ctx *ctx,
		struct vm_area_struct *vma);

void register_mx_ops_v1(struct mx_operations *ops);
void register_mx_ops_v2(struct mx_operations *ops);


bool is_empty(struct mx_mbox *mbox);
bool is_full(struct mx_mbox *mbox);
uint32_t get_free_space(struct mx_mbox *mbox);
uint32_t get_pending_count(struct mx_mbox *mbox);
uint8_t get_next_index(uint8_t _index, uint32_t count, uint32_t depth);
uint32_t get_data_offset(uint8_t _db);
void mx_mbox_init(struct mx_mbox *mbox, uint64_t ctx_addr, uint64_t data_addr, uint64_t ctx);
