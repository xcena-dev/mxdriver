// SPDX-License-Identifier: <SPDX License Expression>

#include "mx_dma.h"

static DEFINE_IDR(mx_ids);
static DEFINE_SPINLOCK(id_lock);

int transfer_id_alloc(void *ptr)
{
	int id;

	/* Preload outside the lock so the cyclic alloc under id_lock never sleeps. */
	idr_preload(GFP_KERNEL);
	spin_lock(&id_lock);
	id = idr_alloc_cyclic(&mx_ids, ptr, 0, MX_PING_ID, GFP_NOWAIT);
	spin_unlock(&id_lock);
	idr_preload_end();

	return id;
}

void transfer_id_free(struct mx_transfer *transfer)
{
	unsigned long flags;

	spin_lock_irqsave(&id_lock, flags);
	if (idr_find(&mx_ids, transfer->id) == transfer)
		idr_remove(&mx_ids, transfer->id);
	spin_unlock_irqrestore(&id_lock, flags);
}

struct mx_transfer *transfer_id_claim_completion(unsigned long id,
						 unsigned long *flags)
{
	struct mx_transfer *transfer;

	spin_lock_irqsave(&id_lock, *flags);
	transfer = idr_find(&mx_ids, id);
	if (!transfer || atomic_cmpxchg(&transfer->wait_claimed, 0, 1) != 0) {
		spin_unlock_irqrestore(&id_lock, *flags);
		return NULL;
	}
	idr_remove(&mx_ids, id);
	return transfer;
}

void transfer_id_complete_unlock(unsigned long flags)
{
	spin_unlock_irqrestore(&id_lock, flags);
}
