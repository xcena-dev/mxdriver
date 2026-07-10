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

void transfer_id_free(unsigned long id)
{
	spin_lock(&id_lock);
	idr_remove(&mx_ids, id);
	spin_unlock(&id_lock);
}

void *find_transfer_by_id(unsigned long id)
{
	void *ptr;

	spin_lock(&id_lock);
	ptr = idr_find(&mx_ids, id);
	spin_unlock(&id_lock);

	return ptr;
}

