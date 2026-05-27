#include "fuse_i.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/uio.h>
#include <linux/miscdevice.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/pipe_fs_i.h>
#include <linux/swap.h>
#include <linux/splice.h>
#include <linux/sched.h>
#include <linux/random.h>
#include <linux/vmalloc.h>
#include <asm/atomic.h>

#define RFUSE_INT_REQ_BIT (1ULL << 0)
#define RFUSE_REQ_ID_STEP (1ULL << 1)
/*
   0: Round Robin 
   1: ThreadID moduler
   2: Application CPU ID
   8: dynamic local/spread scheduler
 */

#define RFUSE_SELECTION_ALGO 2
#ifndef LDY_PAYLOAD_AWARE
#define LDY_PAYLOAD_AWARE 1
#endif

atomic_t rr_id = ATOMIC_INIT(0);

static const int numa_group[][20] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
	  20, 21, 22, 23, 24, 25, 26, 27, 28, 29 },
	{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	  30, 31, 32, 33, 34, 35, 36, 37, 38, 39 },
};
static const unsigned int nr_numa_groups = ARRAY_SIZE(numa_group);
static const unsigned int nr_numa_nodes = ARRAY_SIZE(numa_group[0]);
static atomic_t numa_group_rr_cursor[ARRAY_SIZE(numa_group)];

/*
* Duplicated function from dev.c
*/
static struct fuse_dev *fuse_get_dev(struct file *file)
{
	return READ_ONCE(file->private_data);
}

#define RFUSE_PAYLOAD_UNIT_SIZE (FUSE_MAX_MAX_PAGES * PAGE_SIZE)
#define RFUSE_PAYLOAD_POOL_SIZE (RFUSE_WORKER_PER_RING * RFUSE_PAYLOAD_UNIT_SIZE)

struct rfuse_payload_extent {
	struct list_head list;
	u32 offset;
	u32 len;
	u32 req_index;
	u32 generation;
};

static void rfuse_payload_refresh_largest_free(struct rfuse_iqueue *riq)
{
	struct rfuse_payload_extent *extent;
	u32 largest = 0;

	list_for_each_entry(extent, &riq->payload_free, list) {
		if (extent->len > largest)
			largest = extent->len;
	}

	WRITE_ONCE(riq->payload_largest_free, largest);
}

static bool rfuse_payload_find_extent(struct rfuse_iqueue *riq, size_t need)
{
	struct rfuse_payload_extent *extent;

	spin_lock(&riq->payload_lock);
	list_for_each_entry(extent, &riq->payload_free, list) {
		if (extent->len >= need) {
			spin_unlock(&riq->payload_lock);
			return true;
		}
	}
	spin_unlock(&riq->payload_lock);

	return false;
}

static void rfuse_payload_merge_free(struct rfuse_iqueue *riq)
{
	struct rfuse_payload_extent *cur;
	struct rfuse_payload_extent *next;

	if (list_empty(&riq->payload_free))
		return;

	cur = list_first_entry(&riq->payload_free, struct rfuse_payload_extent, list);
	while (cur->list.next != &riq->payload_free) {
		next = list_next_entry(cur, list);
		if (cur->offset + cur->len != next->offset)
			goto next_extent;

		cur->len += next->len;
		list_del(&next->list);
		kfree(next);
		continue;

next_extent:
		cur = next;
	}
}

static void rfuse_payload_insert_free_extent(struct rfuse_iqueue *riq,
					     struct rfuse_payload_extent *extent)
{
	struct rfuse_payload_extent *iter;

	list_for_each_entry(iter, &riq->payload_free, list) {
		if (extent->offset < iter->offset) {
			list_add_tail(&extent->list, &iter->list);
			rfuse_payload_merge_free(riq);
			rfuse_payload_refresh_largest_free(riq);
			return;
		}
	}

	list_add_tail(&extent->list, &riq->payload_free);
	rfuse_payload_merge_free(riq);
	rfuse_payload_refresh_largest_free(riq);
}

static int rfuse_payload_alloc_locked(struct rfuse_req *r_req, size_t need,
				      struct rfuse_payload_extent *active)
{
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	struct rfuse_payload_extent *extent;

	list_for_each_entry(extent, &riq->payload_free, list) {
		if (extent->len < need)
			continue;

		active->offset = extent->offset;
		active->len = need;
		active->req_index = r_req->index;
		active->generation = ++riq->payload_next_generation;
		list_add_tail(&active->list, &riq->payload_active);

		extent->offset += need;
		extent->len -= need;
		if (!extent->len) {
			list_del(&extent->list);
			kfree(extent);
		}

		riq->payload.used += need;
		rfuse_payload_refresh_largest_free(riq);
		r_req->payload_offset = active->offset;
		r_req->payload_capacity = need;
		r_req->payload_generation = active->generation;
		return 0;
	}

	return -EAGAIN;
}

static int rfuse_payload_alloc(struct rfuse_req *r_req, size_t need, bool may_wait)
{
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	struct rfuse_payload_extent *active;
	size_t aligned_need;
	u32 attempts = 0;
	u32 waits = 0;
	int ret;

	if (!need)
		return 0;

	aligned_need = PAGE_ALIGN(need);
	if (aligned_need > riq->payload.size)
		return -E2BIG;

	active = kmalloc(sizeof(*active), GFP_KERNEL);
	if (!active)
		return -ENOMEM;

	for (;;) {
		spin_lock(&riq->payload_lock);
		ret = rfuse_payload_alloc_locked(r_req, aligned_need, active);
		spin_unlock(&riq->payload_lock);
		attempts++;
		if (!ret)
			return 0;
		if (ret != -EAGAIN || !may_wait)
			break;

		pr_info("RFUSE: payload alloc throttle riq=%d req=%u opcode=%u need=%zu used=%u size=%u largest_free=%u attempts=%u waits=%u pid=%d\n",
			 r_req->riq_id, r_req->index, r_req->in.opcode,
			 aligned_need, riq->payload.used, riq->payload.size,
			 riq->payload_largest_free, attempts, waits,
			 current->pid);
		waits++;
		ret = wait_event_interruptible(riq->payload_waitq,
				!r_req->fm->fc->connected ||
				rfuse_payload_find_extent(riq, aligned_need));
    /*
		pr_info("RFUSE: payload alloc throttle end riq=%d req=%u opcode=%u need=%zu ret=%d used=%u size=%u largest_free=%u connected=%d pid=%d\n",
			 r_req->riq_id, r_req->index, r_req->in.opcode,
			 aligned_need, ret,
			 riq->payload.used, riq->payload.size,
			 riq->payload_largest_free, r_req->fm->fc->connected,
			 current->pid);
    */
		if (ret)
			break;
		if (!r_req->fm->fc->connected) {
			ret = -ENOTCONN;
			break;
		}
	}

	kfree(active);
	return ret;
}

void rfuse_release_payload(struct rfuse_req *r_req)
{
	struct rfuse_iqueue *riq;
	struct rfuse_payload_extent *extent;

	if (!r_req || !r_req->payload_capacity || !r_req->payload_generation)
		return;

	riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	spin_lock(&riq->payload_lock);
	list_for_each_entry(extent, &riq->payload_active, list) {
		if (extent->req_index != r_req->index ||
		    extent->generation != r_req->payload_generation)
			continue;

		extent->req_index = 0;
		extent->generation = 0;
		riq->payload.used -= extent->len;
		list_del(&extent->list);
		rfuse_payload_insert_free_extent(riq, extent);
		break;
	}
	spin_unlock(&riq->payload_lock);
	wake_up_all(&riq->payload_waitq);

	r_req->payload_offset = 0;
	r_req->payload_len = 0;
	r_req->payload_capacity = 0;
	r_req->payload_generation = 0;
	r_req->payload_flags = 0;
}

static int rfuse_payload_pages_capacity(struct rfuse_pages *rp, size_t *capacity)
{
	size_t total = 0;
	unsigned int i;

	if (!rp || !rp->pages || !rp->descs || !capacity)
		return -EINVAL;

	for (i = 0; i < rp->num_pages; i++) {
		struct fuse_page_desc *desc = &rp->descs[i];

		if (!rp->pages[i])
			return -EIO;
		if (desc->offset > PAGE_SIZE ||
		    desc->length > PAGE_SIZE - desc->offset)
			return -EIO;
		if ((size_t)-1 - total < desc->length)
			return -EIO;

		total += desc->length;
	}

	*capacity = total;
	return 0;
}

static int rfuse_payload_copy_from_pages(struct rfuse_req *r_req, size_t len)
{
	struct rfuse_pages *rp = r_req->rp;
	char *dst;
	unsigned int i;
	size_t capacity;
	size_t remaining = len;
	int err;

	if (!rp || !len)
		return 0;

	err = rfuse_payload_pages_capacity(rp, &capacity);
	if (err)
		return err;
	if (len > capacity)
		return -EIO;

	dst = (char *)r_req->fm->fc->riq[r_req->riq_id]->payload.kaddr + r_req->payload_offset;
	for (i = 0; i < rp->num_pages && remaining; i++) {
		unsigned int offset = rp->descs[i].offset;
		unsigned int count = min_t(size_t, remaining, rp->descs[i].length);
		void *src = kmap_atomic(rp->pages[i]);

		memcpy(dst, src + offset, count);
		kunmap_atomic(src);
		dst += count;
		remaining -= count;
	}

	return remaining ? -EIO : 0;
}

static size_t rfuse_requested_out_payload_len(struct rfuse_req *r_req)
{
	if (r_req->in.opcode == FUSE_READ) {
		struct fuse_read_in *in = (struct fuse_read_in *)&r_req->args;

		return in->size;
	}

	return r_req->payload_capacity;
}

static void rfuse_zero_payload_tail(struct rfuse_pages *rp, size_t copied,
				    size_t requested)
{
	size_t payload_pos = 0;
	unsigned int i;

	for (i = 0; i < rp->num_pages && payload_pos < requested; i++) {
		struct fuse_page_desc *desc = &rp->descs[i];
		size_t desc_len = min_t(size_t, desc->length,
					requested - payload_pos);

		if (copied < payload_pos + desc_len) {
			size_t zero_start = 0;
			size_t zero_len;
			void *addr;

			if (copied > payload_pos)
				zero_start = copied - payload_pos;
			zero_len = desc_len - zero_start;

			addr = kmap_atomic(rp->pages[i]);
			memset(addr + desc->offset + zero_start, 0, zero_len);
			kunmap_atomic(addr);
			flush_dcache_page(rp->pages[i]);
		}

		payload_pos += desc_len;
	}
}

int rfuse_import_payload(struct rfuse_req *r_req)
{
	struct rfuse_pages *rp = r_req->rp;
	struct rfuse_iqueue *riq;
	char *src;
	unsigned int i;
	size_t actual;
	size_t requested;
	size_t page_capacity;
	size_t remaining;
	int err;

	if (!rp || !r_req->payload_capacity || (r_req->payload_flags & RFUSE_PAYLOAD_FALLBACK))
		return 0;
	if (!(r_req->payload_flags & RFUSE_PAYLOAD_OUT))
		return 0;
	if (r_req->out.error)
		return 0;

	actual = r_req->out.arglen;
	requested = rfuse_requested_out_payload_len(r_req);

	err = rfuse_payload_pages_capacity(rp, &page_capacity);
	if (err)
		return err;
	if (requested > r_req->payload_capacity ||
	    requested > page_capacity ||
	    actual > r_req->payload_capacity ||
	    actual > requested ||
	    actual > page_capacity)
		return -EIO;

	remaining = actual;
	riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	src = (char *)riq->payload.kaddr + r_req->payload_offset;
	r_req->payload_len = remaining;

	for (i = 0; i < rp->num_pages && remaining; i++) {
		unsigned int offset = rp->descs[i].offset;
		unsigned int count = min_t(size_t, remaining, rp->descs[i].length);
		void *dst = kmap_atomic(rp->pages[i]);

		memcpy(dst + offset, src, count);
		kunmap_atomic(dst);
		flush_dcache_page(rp->pages[i]);
		src += count;
		remaining -= count;
	}

	if (remaining)
		return -EIO;
	if (r_req->page_zeroing && r_req->payload_len < requested)
		rfuse_zero_payload_tail(rp, r_req->payload_len, requested);

	return 0;
}

int rfuse_reserve_payload(struct rfuse_req *r_req, size_t len,
			  unsigned int payload_flags, bool may_wait)
{
	int ret;

	if (!r_req)
		return -EINVAL;
	if (!len)
		return 0;
	if ((payload_flags & (RFUSE_PAYLOAD_IN | RFUSE_PAYLOAD_OUT)) == 0)
		return -EINVAL;
	if ((payload_flags & RFUSE_PAYLOAD_IN) &&
	    (payload_flags & RFUSE_PAYLOAD_OUT))
		return -EINVAL;
	if (r_req->payload_capacity)
		return len <= r_req->payload_capacity ? 0 : -EINVAL;
	if (r_req->payload_flags & RFUSE_PAYLOAD_FALLBACK)
		return -EINVAL;

	r_req->payload_flags = payload_flags;
	ret = rfuse_payload_alloc(r_req, len, may_wait);
	if (ret) {
		r_req->payload_flags = 0;
		return ret;
	}

	return 0;
}

ssize_t rfuse_payload_copy_from_iter(struct rfuse_req *r_req,
				     struct iov_iter *ii, size_t len)
{
	struct rfuse_iqueue *riq;
	char *dst;
	size_t copied;

	if (!r_req || !ii)
		return -EINVAL;
	if (!len)
		return 0;
	if (!r_req->payload_capacity ||
	    !(r_req->payload_flags & RFUSE_PAYLOAD_IN) ||
	    (r_req->payload_flags & RFUSE_PAYLOAD_FALLBACK))
		return -EINVAL;
	if (len > r_req->payload_capacity)
		return -EINVAL;

	riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	dst = (char *)riq->payload.kaddr + r_req->payload_offset;
	copied = copy_from_iter(dst, len, ii);
	if (!copied)
		return -EFAULT;

	r_req->payload_len = copied;
	return copied;
}

int rfuse_prepare_payload(struct rfuse_req *r_req, bool may_wait)
{
	size_t need = 0;
	int ret;

	if (r_req->payload_capacity || (r_req->payload_flags & RFUSE_PAYLOAD_FALLBACK))
		return 0;

	if (r_req->in.opcode == FUSE_WRITE && r_req->in_pages && r_req->in.arglen[0]) {
		need = r_req->in.arglen[0];
		r_req->payload_flags = RFUSE_PAYLOAD_IN;
	} else if (r_req->in.opcode == FUSE_READ && r_req->out_pages && r_req->out.arglen) {
		need = r_req->out.arglen;
		r_req->payload_flags = RFUSE_PAYLOAD_OUT;
	} else {
		return 0;
	}

	ret = rfuse_payload_alloc(r_req, need, may_wait);
	if (ret == -EAGAIN || ret == -ENOMEM || ret == -E2BIG) {
		r_req->payload_flags |= RFUSE_PAYLOAD_FALLBACK;
		return 0;
	}
	if (ret)
		return ret;

	if (r_req->payload_flags & RFUSE_PAYLOAD_IN) {
		ret = rfuse_payload_copy_from_pages(r_req, need);
		if (ret) {
			rfuse_release_payload(r_req);
			return ret;
		}
		r_req->payload_len = need;
	}

	return 0;
}

static int rfuse_payload_pool_init(struct rfuse_iqueue *riq)
{
	struct rfuse_payload_extent *extent;

	riq->payload.kaddr = vmalloc_user(RFUSE_PAYLOAD_POOL_SIZE);
	if (!riq->payload.kaddr)
		return -ENOMEM;

	riq->payload.uaddr = NULL;
	riq->payload.size = RFUSE_PAYLOAD_POOL_SIZE;
	riq->payload.used = 0;
	spin_lock_init(&riq->payload_lock);
	init_waitqueue_head(&riq->payload_waitq);
	INIT_LIST_HEAD(&riq->payload_free);
	INIT_LIST_HEAD(&riq->payload_active);
	riq->payload_next_generation = 0;
	riq->payload_largest_free = 0;

	extent = kmalloc(sizeof(*extent), GFP_KERNEL);
	if (!extent) {
		vfree(riq->payload.kaddr);
		riq->payload.kaddr = NULL;
		return -ENOMEM;
	}

	extent->offset = 0;
	extent->len = RFUSE_PAYLOAD_POOL_SIZE;
	extent->req_index = 0;
	extent->generation = 0;
	INIT_LIST_HEAD(&extent->list);
	list_add_tail(&extent->list, &riq->payload_free);
	riq->payload_largest_free = RFUSE_PAYLOAD_POOL_SIZE;

	return 0;
}

static void rfuse_payload_pool_destroy(struct rfuse_iqueue *riq)
{
	struct rfuse_payload_extent *extent;
	struct rfuse_payload_extent *next;

	list_for_each_entry_safe(extent, next, &riq->payload_free, list) {
		list_del(&extent->list);
		kfree(extent);
	}
	list_for_each_entry_safe(extent, next, &riq->payload_active, list) {
		list_del(&extent->list);
		kfree(extent);
	}
	if (riq->payload.kaddr)
		vfree(riq->payload.kaddr);
}


/************ 0. Copy of original fuse functions ************/
static void rfuse_drop_waiting(struct fuse_conn *fc)
{
	/*
	 * lockess check of fc->connected is okay, because atomic_dec_and_test()
	 * provides a memory barrier matched with the one in fuse_wait_aborted()
	 * to ensure no wake-up is missed.
	 */
	if (atomic_dec_and_test(&fc->num_waiting) &&
			!READ_ONCE(fc->connected)) {
		/* wake up aborters */
		wake_up_all(&fc->blocked_waitq);
	}
}

static bool rfuse_block_alloc(struct fuse_conn *fc, bool for_background, int riq_id)
{
#if LDY_PAYLOAD_AWARE
	(void)for_background;
	(void)riq_id;

	return !fc->initialized;
#else
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);

	return !fc->initialized || (for_background && riq->blocked);
#endif
}

static bool rfuse_background_submission_available(struct rfuse_iqueue *riq)
{
#if LDY_PAYLOAD_AWARE
	(void)riq;

	return true;
#else
	return riq->active_background < riq->max_background;
#endif
}

// Get a unique request number 
// This should be called with the riq lock aquired
u64 rfuse_get_unique(struct rfuse_iqueue *riq){ 
	riq->reqctr += RFUSE_REQ_ID_STEP;
	return riq->reqctr;
}

static void rfuse_force_creds(struct rfuse_req *r_req){
	struct fuse_conn *fc = r_req->fm->fc;

	r_req->in.uid = from_kuid(fc->user_ns, current_fsuid());
    r_req->in.gid = from_kgid(fc->user_ns, current_fsgid());
    r_req->in.pid = pid_nr_ns(task_pid(current), fc->pid_ns);

}

static int rfuse_check_page(struct page *page)
{
	if (page_mapcount(page) ||
			page->mapping != NULL ||
			(page->flags & PAGE_FLAGS_CHECK_AT_PREP &
			 ~(1 << PG_locked |
				 1 << PG_referenced |
				 1 << PG_uptodate |
				 1 << PG_lru |
				 1 << PG_active |
				 1 << PG_workingset |
				 1 << PG_reclaim |
				 1 << PG_waiters))) {
		return 1;
	}
	return 0;
}



/************ 1. Mmap ************/

void rfuse_init_ring_buffer_1(struct ring_buffer_1 *rb){
	rb->tail=0;
	rb->head=0;
	rb->mask = RFUSE_MAX_QUEUE_SIZE-1;
	rb->entries= RFUSE_MAX_QUEUE_SIZE;
}
void rfuse_init_ring_buffer_2(struct ring_buffer_2 *rb){
	rb->head=0;
	rb->tail=0;
	rb->mask = RFUSE_MAX_QUEUE_SIZE-1;
	rb->entries= RFUSE_MAX_QUEUE_SIZE;
}
void rfuse_init_ring_buffer_3(struct ring_buffer_3 *rb){
	rb->head=0;
    rb->tail=0;
    rb->mask = RFUSE_MAX_QUEUE_SIZE-1;
    rb->entries= RFUSE_MAX_QUEUE_SIZE;
}	

static void rfuse_init_iqueue_numa_candidates(struct fuse_conn *fc)
{
	int i;

	memset(fc->riq_node_count, 0, sizeof(fc->riq_node_count));

	for (i = 0; i < RFUSE_NUM_IQUEUE; i++) {
		int node = cpu_to_node(i);
		unsigned int count;

		if (node < 0 || node >= MAX_NUMNODES)
			continue;

		count = fc->riq_node_count[node];
		if (count >= RFUSE_NUM_IQUEUE)
			continue;

		fc->riq_node_ids[node][count] = i;
		fc->riq_node_count[node] = count + 1;
	}
}

void rfuse_iqueue_init(struct fuse_conn *fc, void *priv){
	int i = 0;	
	struct rfuse_iqueue **riq;
	int node_id = 0;

	// init rfuse iqueue
	riq = kzalloc(sizeof(struct rfuse_iqueue *) * RFUSE_NUM_IQUEUE, GFP_KERNEL);
	for(i = 0; i < RFUSE_NUM_IQUEUE; i++){
		node_id = cpu_to_node(i);

		riq[i] = kzalloc_node(4096, GFP_KERNEL, node_id);

		spin_lock_init(&riq[i]->lock);
		riq[i]->riq_id = i;
		init_waitqueue_head(&riq[i]->waitq);
		init_waitqueue_head(&riq[i]->idle_user_waitq);
		riq[i]->connected=1;
		riq[i]->priv=priv;
	
		// init ring buffer
		rfuse_init_ring_buffer_1(&riq[i]->pending);
		rfuse_init_ring_buffer_2(&riq[i]->interrupts);
		rfuse_init_ring_buffer_3(&riq[i]->forgets);
		rfuse_init_ring_buffer_1(&riq[i]->completes);
	
		// allocate space for mmap
		riq[i]->pending.kaddr = kmalloc_node(sizeof(struct rfuse_address_entry) * RFUSE_MAX_QUEUE_SIZE, GFP_KERNEL, node_id);
		riq[i]->interrupts.kaddr = kmalloc_node(sizeof(struct rfuse_interrupt_entry) * RFUSE_MAX_QUEUE_SIZE, GFP_KERNEL, node_id);
		riq[i]->forgets.kaddr = kmalloc_node(sizeof(struct rfuse_forget_entry) * RFUSE_MAX_QUEUE_SIZE, GFP_KERNEL, node_id);
		riq[i]->completes.kaddr = kmalloc_node(sizeof(struct rfuse_address_entry) * RFUSE_MAX_QUEUE_SIZE, GFP_KERNEL, node_id);
		riq[i]->karg = kmalloc_node(sizeof(struct rfuse_arg)*RFUSE_MAX_QUEUE_SIZE * 2, GFP_KERNEL, node_id);
		riq[i]->kreq = kmalloc_node(sizeof(struct rfuse_req)*RFUSE_MAX_QUEUE_SIZE * 2, GFP_KERNEL, node_id);
		rfuse_payload_pool_init(riq[i]);
	
		// init bitmaps
		riq[i]->argbm.bitmap_size = RFUSE_MAX_QUEUE_SIZE * 2;
		riq[i]->reqbm.bitmap_size = RFUSE_MAX_QUEUE_SIZE * 2;
		riq[i]->reqbm.full=0;
		riq[i]->argbm.full=0;
		riq[i]->argbm.bitmap = kzalloc_node((RFUSE_MAX_QUEUE_SIZE*2)>>3, GFP_KERNEL, node_id);
		riq[i]->reqbm.bitmap = kzalloc_node((RFUSE_MAX_QUEUE_SIZE*2)>>3, GFP_KERNEL, node_id);

		// init background queue
		INIT_LIST_HEAD(&riq[i]->bg_queue);
		spin_lock_init(&riq[i]->bg_lock);
		init_waitqueue_head(&riq[i]->blocked_waitq);

		riq[i]->max_background = FUSE_DEFAULT_MAX_BACKGROUND;
		riq[i]->congestion_threshold = FUSE_DEFAULT_CONGESTION_THRESHOLD;
		riq[i]->num_background = 0;
		riq[i]->active_background = 0;

		riq[i]->num_sync_sleeping = 0;
	}

	fc->riq = riq;
	rfuse_init_iqueue_numa_candidates(fc);
}

void rfuse_iqueue_release(struct fuse_conn *fc){
	int i = 0;
	struct rfuse_iqueue **riq = fc->riq;

	pr_info("RFUSE_SCHED mode=%d local=%llu spread=%llu switches=%llu mask=0x%llx low_windows=%d\n",
		atomic_read(&fc->rfuse_sched_mode),
		atomic64_read(&fc->rfuse_sched_local_count),
		atomic64_read(&fc->rfuse_sched_spread_count),
		atomic64_read(&fc->rfuse_sched_switch_count),
		atomic64_read(&fc->rfuse_sched_mask),
		atomic_read(&fc->rfuse_sched_low_windows));

	for(i = 0; i < RFUSE_NUM_IQUEUE; i++) {
		kfree(riq[i]->pending.kaddr);
		kfree(riq[i]->interrupts.kaddr);
		kfree(riq[i]->forgets.kaddr);
		kfree(riq[i]->completes.kaddr);
		kfree(riq[i]->karg);
		kfree(riq[i]->kreq);
		rfuse_payload_pool_destroy(riq[i]);
	
		kfree(riq[i]->argbm.bitmap);
		kfree(riq[i]->reqbm.bitmap);
		kfree(riq[i]);
	}

	kfree(riq);
}
/* for I/O mmap pages*/
int rfuse_io_mmap(struct vm_area_struct *vma, struct fuse_dev *fud, int req_index, int riq_id, unsigned nbytes){
	struct rfuse_pages *rp;
	struct rfuse_req *r_req = &fud->fc->riq[riq_id]->kreq[req_index];
	unsigned long addr = vma->vm_start;
	unsigned int i;
	
	rp = r_req->rp;
	for(i = 0;i < rp->num_pages && nbytes; i++){
		struct page *page = rp->pages[i];
		unsigned long size = (unsigned long)min(nbytes, rp->descs[i].length);
		unsigned int offset = rp->descs[i].offset;
		
		void *mapaddr = kmap_atomic(page);
		void *buf = mapaddr + offset;
		unsigned long pfn = virt_to_phys(buf) >> PAGE_SHIFT;
		
		if(remap_pfn_range(vma, addr, pfn, size, vma->vm_page_prot)) {
			return 1;
		}

		kunmap_atomic(mapaddr);
		addr += size;
	}

	return 0;
}
/* do this across all riqs */
void *rfuse_validate_mmap_request(struct fuse_dev *fud, loff_t pgoff, size_t size){
	struct page *page;
	void *ptr;

	/* riq_id should be passed by the mmap offset argument */
	unsigned long long map_args = pgoff << PAGE_SHIFT;
	loff_t map_queue = map_args & RFUSE_QUEUE_MAP_MASK;
	int riq_id = (int)((map_args & RFUSE_RIQ_ID_MASK) >> 16);

	switch(map_queue){
		case RFUSE_IQUEUE:
			ptr = (fud->fc->riq[riq_id]);
			break;
		case RFUSE_PENDING:
			ptr = fud->fc->riq[riq_id]->pending.kaddr;
			break;
		case RFUSE_INTERRUPT:
			ptr = fud->fc->riq[riq_id]->interrupts.kaddr;
			break;
		case RFUSE_FORGET:
			ptr = fud->fc->riq[riq_id]->forgets.kaddr;
			break;
		case RFUSE_COMPLETE:
			ptr = fud->fc->riq[riq_id]->completes.kaddr;
			break;
		case RFUSE_ARG:
			ptr = fud->fc->riq[riq_id]->karg;
			break;
		case RFUSE_REQ:
			ptr = fud->fc->riq[riq_id]->kreq;
			break;
		case RFUSE_PAYLOAD:
			ptr = fud->fc->riq[riq_id]->payload.kaddr;
			break;
		default:
			return ERR_PTR(-EINVAL);
	}

	// Check if the virtual memory space is big enough
	page = virt_to_head_page(ptr);
	if(size > page_size(page))
		return ERR_PTR(-EINVAL);

	return ptr;
}

/************ 2. Ring buffer ************/

static int select_round_robin(struct fuse_conn *fc){
	int ret;

	spin_lock(&fc->lock);

	if(atomic_read(&rr_id) == RFUSE_NUM_IQUEUE) 
		atomic_set(&rr_id, 0);

	ret = atomic_read(&rr_id);
	atomic_add(1, &rr_id);
	spin_unlock(&fc->lock);

	return ret;
}

static int select_thread_id(void){
	int ret = current->pid;
	
	return (ret % RFUSE_NUM_IQUEUE);
}

static int select_cpu_id(void){
	int ret = task_cpu(current);
	
	return (ret % RFUSE_NUM_IQUEUE);
}

static int __maybe_unused select_payload_aware(struct fuse_conn *fc, size_t len)
{
	size_t need = PAGE_ALIGN(len);
	int start = select_cpu_id();
	int i;

	if (!need || need > fc->riq[start]->payload.size)
		return start;

	for (;;) {
		for (i = 0; i < RFUSE_NUM_IQUEUE; i++) {
			int id = (start + i + 1) % RFUSE_NUM_IQUEUE;
			struct rfuse_iqueue *riq = fc->riq[id];
			u32 free_payload;

			if (id < 0 || id >= RFUSE_NUM_IQUEUE || id == start)
				continue;

			spin_lock(&riq->payload_lock);
			free_payload = riq->payload.size - riq->payload.used;
			spin_unlock(&riq->payload_lock);

			if (free_payload >= need)
				return id;
		}

		cond_resched();
	}
}

static unsigned int numa_group_next_rr_index(unsigned int group_id)
{
	unsigned int cursor;

	if (WARN_ON_ONCE(group_id >= nr_numa_groups))
		return 0;

	cursor = (unsigned int)atomic_inc_return(
			&numa_group_rr_cursor[group_id]) - 1;
	return cursor % nr_numa_nodes;
}

static int check_numa_group(int cpu, const int **group)
{
	if (cpu >= 0 && cpu <= 9) {
		*group = numa_group[0];
		return 0;
	}

	if (cpu >= 20 && cpu <= 29) {
		*group = numa_group[0];
		return 0;
	}

	if (cpu >= 10 && cpu <= 19) {
		*group = numa_group[1];
		return 0;
	}

	if (cpu >= 30 && cpu <= 39) {
		*group = numa_group[1];
		return 0;
	}

	*group = NULL;
	return 1;
}

static int check_numa_group_rr(int cpu, const int **group, unsigned int *group_id)
{
	unsigned int i;

	if (check_numa_group(cpu, group))
		return 1;

	for (i = 0; i < nr_numa_groups; i++) {
		if (*group == numa_group[i]) {
			*group_id = i;
			return 0;
		}
	}

	return 1;
}

static int __maybe_unused select_payload_aware_v2(struct fuse_conn *fc, size_t len)
{
	size_t need = PAGE_ALIGN(len);
	const int *group;
	int start = select_cpu_id();
	unsigned int start_idx;
	unsigned int i;

	if (!need || need > fc->riq[start]->payload.size)
		return start;

	if (check_numa_group(start, &group))
		return start;
  
	for (start_idx = 0; start_idx < nr_numa_nodes; start_idx++) {
		if (group[start_idx] == start)
			break;
	}
  
	if (start_idx == nr_numa_nodes)
		return start;

	for (;;) {
		for (i = 0; i < nr_numa_nodes; i++) {
			int id = group[(start_idx + i + 1) % nr_numa_nodes];
      // int id = group[i];
			struct rfuse_iqueue *riq;
			u32 free_payload;

			if (id < 0 || id >= RFUSE_NUM_IQUEUE || id == start)
				continue;

			riq = fc->riq[id];
			if (READ_ONCE(riq->num_sync_sleeping))
				continue;

			spin_lock(&riq->payload_lock);
			free_payload = riq->payload_largest_free;
			spin_unlock(&riq->payload_lock);

			if (free_payload >= need)
				return id;
		}

		cond_resched();
	}
}

static int select_payload_aware_v3(struct fuse_conn *fc, size_t len)
{
	size_t need = PAGE_ALIGN(len);
	const int *group;
	int start = select_cpu_id();
	unsigned int group_id;
	unsigned int start_idx;
	unsigned int i;

	if (!need || need > fc->riq[start]->payload.size)
		return start;

	if (check_numa_group_rr(start, &group, &group_id))
		return start;

	for (;;) {
		start_idx = numa_group_next_rr_index(group_id);

		for (i = 0; i < nr_numa_nodes; i++) {
			int id = group[(start_idx + i) % nr_numa_nodes];
			struct rfuse_iqueue *riq;
			u32 free_payload;

			if (id < 0 || id >= RFUSE_NUM_IQUEUE || id == start)
				continue;

			riq = fc->riq[id];
			if (READ_ONCE(riq->num_sync_sleeping))
				continue;

			spin_lock(&riq->payload_lock);
			free_payload = riq->payload_largest_free;
			spin_unlock(&riq->payload_lock);

			if (free_payload >= need)
				return id;
		}

		cond_resched();
	}
}

static int select_bg_aware(struct fuse_conn *fc) {
  const int *group;
  int start = select_cpu_id();
  unsigned int i;

  if (check_numa_group(start, &group))
    return start;

  for (;;) {
    for (i = 0; i < nr_numa_nodes; i++) {
      int id = group[i];
      struct rfuse_iqueue *riq;
      bool available;

      if (id < 0 || id >= RFUSE_NUM_IQUEUE || id == start)
        continue;

      riq = fc->riq[id];
      if (READ_ONCE(riq->num_sync_sleeping))
        continue;

      spin_lock(&riq->bg_lock);
      available = riq->num_background < riq->max_background;
      spin_unlock(&riq->bg_lock);

      if (available)
        return id;
    }

    cond_resched();
  }
}

static int select_bg_aware_cpu(struct fuse_conn *fc)
{
	const int *group;
	int start = select_cpu_id();
	unsigned int start_idx;
	unsigned int i;

	if (check_numa_group(start, &group))
		return start;

	for (start_idx = 0; start_idx < nr_numa_nodes; start_idx++) {
		if (group[start_idx] == start)
			break;
	}

	if (start_idx == nr_numa_nodes)
		return start;

	for (;;) {
		for (i = 0; i < nr_numa_nodes; i++) {
			int id = group[(start_idx + i) % nr_numa_nodes];
			struct rfuse_iqueue *riq;
			bool available;

			if (id < 0 || id >= RFUSE_NUM_IQUEUE || id == start)
				continue;

			riq = fc->riq[id];
			if (READ_ONCE(riq->num_sync_sleeping))
				continue;

			spin_lock(&riq->bg_lock);
			available = riq->num_background < riq->max_background;
			spin_unlock(&riq->bg_lock);

			if (available)
				return id;
		}

		cond_resched();
	}
}

static int select_bg_aware_rr(struct fuse_conn *fc)
{
	const int *group;
	int start = select_cpu_id();
	unsigned int group_id;
	unsigned int start_idx;
	unsigned int i;

	if (check_numa_group_rr(start, &group, &group_id))
		return start;

	for (;;) {
		start_idx = numa_group_next_rr_index(group_id);

		for (i = 0; i < nr_numa_nodes; i++) {
			int id = group[(start_idx + i) % nr_numa_nodes];
			struct rfuse_iqueue *riq;
      bool available;

			//if (id < 0 || id >= RFUSE_NUM_IQUEUE || id == start)
				//continue;

			riq = fc->riq[id];
			if (READ_ONCE(riq->num_sync_sleeping))
				continue;

			spin_lock(&riq->payload_lock);
      available = riq->num_background < riq->max_background;
			spin_unlock(&riq->payload_lock);

			if (available)
				return id;
		}

		cond_resched();
	}
}

struct rfuse_iqueue *rfuse_get_iqueue_for_async(struct fuse_conn *fc,
						size_t payload_len)
{
	int id;

  
	id = payload_len ? select_payload_aware_v2(fc, payload_len) :
		select_bg_aware(fc);
  
  // id = select_bg_aware_cpu(fc);
  // id = select_cpu_id();
  // id = select_round_robin(fc);
	return fc->riq[id];
}

struct rfuse_iqueue *rfuse_get_iqueue_for_wt_async(struct fuse_conn *fc,
						   size_t payload_len)
{
	int id;

	id = payload_len ? select_payload_aware_v2(fc, payload_len) :
		select_bg_aware(fc);

	return fc->riq[id];
}

struct rfuse_iqueue *rfuse_get_iqueue(struct fuse_conn *fc){
	int id = 0;
	int i = 0, tmp = 0;

	switch(RFUSE_SELECTION_ALGO) {
		case 0:
			id = select_round_robin(fc);
			break;
		case 1:
			id = select_thread_id();
			break;
		case 2:
			id = select_cpu_id();
			break;
		default:
			/* default is use only first rfuse_iqueue */
			id = 0;
	}

	/* Selected rfuse iqueue is congested, try to pick idle rfuse iqueue*/
	if(fc->riq[id]->num_sync_sleeping) {
		for(i = 0; i < 10; i++) {
			get_random_bytes(&tmp, sizeof(tmp) - 1);
			id = tmp % RFUSE_NUM_IQUEUE;
			if(!fc->riq[id]->num_sync_sleeping)
				break;
		}
	}
	
	return fc->riq[id];
}

struct rfuse_iqueue *rfuse_get_specific_iqueue(struct fuse_conn *fc, int id){
	return fc->riq[id];
}

struct rfuse_address_entry *rfuse_read_pending_tail(struct rfuse_iqueue *riq){
	struct ring_buffer_1 *pending = &riq->pending;
	struct rfuse_address_entry *ret = NULL;
	unsigned int head; 
	unsigned int next;

	head = smp_load_acquire(&pending->head); // Cannot touch in kernel
	next = pending->tail + 1;

	if (next - head <= pending->entries) {
		ret = &pending->kaddr[pending->tail & pending->mask];
    }
    return ret;
}

void rfuse_submit_pending_tail(struct rfuse_iqueue *riq){
	struct ring_buffer_1 *pending = &riq->pending;
	unsigned int next =  pending->tail + 1;
	smp_store_release(&pending->tail,next);
}

struct rfuse_forget_entry *rfuse_read_forgets_tail(struct rfuse_iqueue *riq){
	struct ring_buffer_3 *forgets = &riq->forgets;
	struct rfuse_forget_entry *ret = NULL;
	unsigned int head; 
	unsigned int next;

	head = smp_load_acquire(&forgets->head); // Cannot touch in kernel
	next = forgets->tail + 1;

	if (next - head <= forgets->entries) {
		ret = &forgets->kaddr[forgets->tail & forgets->mask];
    	}

    	return ret;
}

void rfuse_submit_forgets_tail(struct rfuse_iqueue *riq){
	struct ring_buffer_3 *forgets = &riq->forgets;
	unsigned int next =  forgets->tail + 1;
	smp_store_release(&forgets->tail,next);
}


struct rfuse_interrupt_entry *rfuse_read_interrupts_tail(struct rfuse_iqueue *riq){
	struct ring_buffer_2 *interrupts = &riq->interrupts;
	struct rfuse_interrupt_entry *ret = NULL;
	unsigned int head; 
	unsigned int next;

	head = smp_load_acquire(&interrupts->head); // Cannot touch in kernel
	next = interrupts->tail + 1;

	if (next - head <= interrupts->entries) {
		ret = &interrupts->kaddr[interrupts->tail & interrupts->mask];
    }
    return ret;
}

void rfuse_submit_interrupt_tail(struct rfuse_iqueue *riq){
	struct ring_buffer_2 *interrupts = &riq->interrupts;
	unsigned int next =  interrupts->tail + 1;
	smp_store_release(&interrupts->tail,next);
}


struct rfuse_address_entry *rfuse_read_complete_head(struct rfuse_iqueue *riq){
	struct ring_buffer_1 *completes = &riq->completes;
	unsigned int head = completes->head;
	unsigned int tail = smp_load_acquire(&completes->tail);
	struct rfuse_address_entry *ret = NULL;
	if(head < tail){
		ret = &completes->kaddr[head & completes->mask];
	}

    return ret;
}

void rfuse_extract_complete_head(struct rfuse_iqueue *riq){
	struct ring_buffer_1 *completes = &riq->completes;
	unsigned int next = completes->head + 1;
	smp_store_release(&completes->head,next);
}

/************ 3. Request & Argument ************/

uint32_t rfuse_get_request_buffer(struct fuse_mount *fm, int riq_id){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	uint32_t request_index;

	for(;;){
		spin_lock(&riq->lock);
		request_index = find_next_zero_bit(riq->reqbm.bitmap,riq->reqbm.bitmap_size,0);

		if(request_index == riq->reqbm.bitmap_size){
			// There are no empty request buffer
			riq->reqbm.full = 1; // set to full
			pr_info("RFUSE: request buffer throttle start riq=%d full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->reqbm.full),
				 current->pid);
			spin_unlock(&riq->lock);
			wait_event_interruptible(riq->waitq, !READ_ONCE(riq->reqbm.full));
      /*
			pr_info("RFUSE: request buffer throttle end riq=%d full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->reqbm.full),
				 current->pid);
      */
		}
		else{
			__set_bit(request_index,riq->reqbm.bitmap);
			spin_unlock(&riq->lock);
			break;
		}	
	}
	return request_index;
}

void rfuse_put_request_buffer(struct fuse_mount *fm, uint32_t request_index, int riq_id){
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fm->fc, riq_id);
	spin_lock(&riq->lock);
	__clear_bit(request_index,riq->reqbm.bitmap);

	if(riq->reqbm.full == 1){
		riq->reqbm.full = 0;
		wake_up(&riq->waitq);
	}
	spin_unlock(&riq->lock);
}

uint32_t rfuse_get_argument_buffer(struct fuse_mount *fm, int riq_id){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	uint32_t arg_index;

	for(;;){
		spin_lock(&riq->lock);
		arg_index = find_next_zero_bit(riq->argbm.bitmap,riq->argbm.bitmap_size,0);

		if(arg_index == riq->argbm.bitmap_size){
			// There are no empty request buffer
			riq->argbm.full = 1; // set to full
			pr_info("RFUSE: argument buffer throttle start riq=%d full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->argbm.full),
				 current->pid);
			spin_unlock(&riq->lock);
			wait_event_interruptible(riq->waitq,!READ_ONCE(riq->argbm.full));
      /*
			pr_info("RFUSE: argument buffer throttle end riq=%d full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->argbm.full),
				 current->pid);
      */
		}
		else{
			__set_bit(arg_index,riq->argbm.bitmap);
			spin_unlock(&riq->lock);
			break;
		}	
	}
	return arg_index;
}

void rfuse_put_argument_buffer(struct fuse_mount *fm, uint32_t arg_index, int riq_id){
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fm->fc, riq_id);
	spin_lock(&riq->lock);

	__clear_bit(arg_index,riq->argbm.bitmap);
	
	if(riq->argbm.full == 1){
		riq->argbm.full = 0;
		wake_up(&riq->waitq);
	}
	spin_unlock(&riq->lock);
}

static void rfuse_request_free(struct rfuse_req *req){
	bool in_payload = req->payload_capacity &&
			  (req->payload_flags & RFUSE_PAYLOAD_IN);
	bool out_payload = req->payload_capacity &&
			   (req->payload_flags & RFUSE_PAYLOAD_OUT);

	rfuse_release_payload(req);

	if(req->in.arglen[0] != 0 && !req->in_pages && !in_payload)
		rfuse_put_argument_buffer(req->fm, req->in.arg[0], req->riq_id);
	if(req->in.arglen[1] != 0 && !req->in_pages && !in_payload)
		rfuse_put_argument_buffer(req->fm, req->in.arg[1], req->riq_id);
	if(req->out.arglen != 0 && !req->out_pages && !out_payload)
		rfuse_put_argument_buffer(req->fm, req->out.arg, req->riq_id);

	rfuse_put_request_buffer(req->fm, req->index, req->riq_id);
	smp_mb();
}

void rfuse_put_request(struct rfuse_req *r_req){
	struct fuse_conn *fc;
	struct rfuse_iqueue *riq;
	
	if(!r_req){
		return;
	}

	fc = r_req->fm->fc;
	riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
	if (refcount_dec_and_test(&r_req->count)) {
        if (test_bit(FR_BACKGROUND, &r_req->flags)) {
            /*
             * We get here in the unlikely case that a background
             * request was allocated but not sent
             */
            spin_lock(&riq->bg_lock);
            if (!riq->blocked)
                wake_up(&riq->blocked_waitq);
            spin_unlock(&riq->bg_lock);
        }

        if (test_bit(FR_WAITING, &r_req->flags)) {
            __clear_bit(FR_WAITING, &r_req->flags);
            rfuse_drop_waiting(fc);
        }
		rfuse_request_free(r_req);
    }  
}

struct rfuse_req *rfuse_request_alloc(struct fuse_mount *fm, size_t payload_len){
	struct fuse_conn *fc = fm->fc;
  
	struct rfuse_iqueue *riq = payload_len ?
		fc->riq[select_payload_aware_v3(fc, payload_len)] :
		rfuse_get_iqueue(fc);
  
  //struct rfuse_iqueue *riq = rfuse_get_iqueue(fc);
	int riq_id = riq->riq_id;
	struct rfuse_req *r_req = NULL;
	uint32_t req_index;

	req_index = rfuse_get_request_buffer(fm, riq_id); // Get a new index
	r_req = (struct rfuse_req*)&riq->kreq[req_index]; // Get a new entry

	if(r_req) {
		// Initialize request
		memset(r_req, 0, sizeof(struct rfuse_req));
		init_waitqueue_head(&r_req->waitq);
		refcount_set(&r_req->count, 1);
		__set_bit(FR_PENDING,&r_req->flags);
		r_req->fm = fm;
		r_req->index = req_index;
		r_req->riq_id = riq_id;
	}

	// INITIALIZE DONE
	return r_req;
}

struct rfuse_req *rfuse_get_req(struct fuse_mount *fm, bool for_background,
				bool force, size_t payload_len){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_req *r_req;
	struct rfuse_iqueue *riq;
	int err;

	if(force) {
		atomic_inc(&fc->num_waiting);

		r_req = rfuse_request_alloc(fm, payload_len);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		__set_bit(FR_WAITING, &r_req->flags);
		if(for_background){
			__set_bit(FR_BACKGROUND, &r_req->flags);
		} else{
			if(!r_req->nocreds)
				rfuse_force_creds(r_req);

			__set_bit(FR_FORCE, &r_req->flags);
		}
	} else {
		atomic_inc(&fc->num_waiting);

		smp_rmb();
		err = -ENOTCONN;
		if(!fc->connected) {
			goto out;
		}

		err = -ECONNREFUSED;
		if (fc->conn_error) {
			goto out; 
		}

		r_req = rfuse_request_alloc(fm, payload_len);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		// Pass riq_id to find riq if it needs to wait
		if(rfuse_block_alloc(fc, for_background, r_req->riq_id)){
			err = -EINTR;
			riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
      
			pr_info("RFUSE: background depth throttle start riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      
			if (wait_event_killable_exclusive(riq->blocked_waitq, !rfuse_block_alloc(fc, for_background, r_req->riq_id))) {
				goto out;
			}
      /*
			pr_info("RFUSE: background depth throttle end riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      */
		}


		r_req->in.uid = from_kuid(fc->user_ns, current_fsuid());
		r_req->in.gid = from_kgid(fc->user_ns, current_fsgid());
		r_req->in.pid = pid_nr_ns(task_pid(current), fc->pid_ns);

		__set_bit(FR_WAITING, &r_req->flags);
		if (for_background) 
			__set_bit(FR_BACKGROUND, &r_req->flags);

		if (unlikely(r_req->in.uid == ((uid_t)-1) || r_req->in.gid == ((gid_t)-1))) {
			rfuse_put_request(r_req);
			return ERR_PTR(-EOVERFLOW);
		}
	}

	smp_mb();

	return r_req;  

out:
	rfuse_drop_waiting(fc);
	return ERR_PTR(err);
}

/* 
	This is special request allocation function for rfuse writeback cache 
	If failed to get request from request buffer, release file lock and wait for request buffer
*/
static uint32_t try_rfuse_get_request_buffer(struct fuse_mount *fm, int riq_id){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	uint32_t request_index;

	for(;;){
		spin_lock(&riq->lock);
		request_index = find_next_zero_bit(riq->reqbm.bitmap,riq->reqbm.bitmap_size,0);

		if(request_index == riq->reqbm.bitmap_size){
			// There are no empty request buffer
			riq->reqbm.full = 1; // set to full
			spin_unlock(&riq->lock);
			return -1;
		}
		else{
			__set_bit(request_index,riq->reqbm.bitmap);
			spin_unlock(&riq->lock);
			break;
		}	
	}
	return request_index;
}

static struct rfuse_req *try_rfuse_request_alloc(struct fuse_mount *fm,
						 size_t payload_len,
						 spinlock_t *file_lock){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_iqueue_for_async(fc, payload_len);
	int riq_id = riq->riq_id;
	struct rfuse_req *r_req = NULL;
	uint32_t req_index;

	do {
		req_index = try_rfuse_get_request_buffer(fm, riq_id); // Get a new index
		if(req_index == -1) {
			pr_info("RFUSE: request buffer throttle start riq=%d async=1 full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->reqbm.full),
				 current->pid);
			if(file_lock)
				spin_unlock(file_lock);
			wait_event_interruptible(riq->waitq, !READ_ONCE(riq->reqbm.full));
			if(file_lock)
				spin_lock(file_lock);
      /*
			pr_info("RFUSE: request buffer throttle end riq=%d async=1 full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->reqbm.full),
				 current->pid);
      */
		}
	} while(req_index == -1);
	
	r_req = (struct rfuse_req*)&riq->kreq[req_index]; // Get a new entry

	if(r_req) {
		// Initialize request
		memset(r_req, 0, sizeof(struct rfuse_req));
		init_waitqueue_head(&r_req->waitq);
		refcount_set(&r_req->count, 1);
		__set_bit(FR_PENDING,&r_req->flags);
		r_req->fm = fm;
		r_req->index = req_index;
		r_req->riq_id = riq_id;
	}

	// INITIALIZE DONE
	return r_req;
}

static struct rfuse_req *try_rfuse_wt_request_alloc(struct fuse_mount *fm,
						    size_t payload_len,
						    spinlock_t *file_lock)
{
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq;
	int riq_id;
	struct rfuse_req *r_req = NULL;
	uint32_t req_index;

	riq = rfuse_get_iqueue_for_wt_async(fc, payload_len);
	riq_id = riq->riq_id;
	do {
		req_index = try_rfuse_get_request_buffer(fm, riq_id);
		if (req_index == -1) {
			pr_info("RFUSE: write request buffer throttle start riq=%d async=1 full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->reqbm.full),
				 current->pid);
			if (file_lock)
				spin_unlock(file_lock);
			wait_event_interruptible(riq->waitq,
						 !READ_ONCE(riq->reqbm.full));
			if (file_lock)
				spin_lock(file_lock);
      /*
			pr_info("RFUSE: write request buffer throttle end riq=%d async=1 full=%d pid=%d\n",
				 riq_id, READ_ONCE(riq->reqbm.full),
				 current->pid);
      */
		}
	} while (req_index == -1);

	r_req = (struct rfuse_req *)&riq->kreq[req_index];
	if (r_req) {
		memset(r_req, 0, sizeof(struct rfuse_req));
		init_waitqueue_head(&r_req->waitq);
		refcount_set(&r_req->count, 1);
		__set_bit(FR_PENDING, &r_req->flags);
		r_req->fm = fm;
		r_req->index = req_index;
		r_req->riq_id = riq_id;
	}

	return r_req;
}

struct rfuse_req *try_rfuse_get_req(struct fuse_mount *fm,
				    bool for_background, bool force,
				    size_t payload_len, spinlock_t *file_lock){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_req *r_req;
	struct rfuse_iqueue *riq;
	int err;

	if(force) {
		atomic_inc(&fc->num_waiting);

		r_req = try_rfuse_request_alloc(fm, payload_len, file_lock);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		__set_bit(FR_WAITING, &r_req->flags);
		if(for_background){
			__set_bit(FR_BACKGROUND, &r_req->flags);
		} else{
			if(!r_req->nocreds)
				rfuse_force_creds(r_req);

			__set_bit(FR_FORCE, &r_req->flags);
		}
	} else {
		atomic_inc(&fc->num_waiting);

		smp_rmb();
		err = -ENOTCONN;
		if(!fc->connected) {
			goto out;
		}

		err = -ECONNREFUSED;
		if (fc->conn_error) {
			goto out; 
		}

		r_req = try_rfuse_request_alloc(fm, payload_len, file_lock);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		// Pass riq_id to find riq if it needs to wait
		if(rfuse_block_alloc(fc, for_background, r_req->riq_id)){
			err = -EINTR;
			riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
      /*
			pr_info("RFUSE: background depth throttle start riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      */
			if (wait_event_killable_exclusive(riq->blocked_waitq, !rfuse_block_alloc(fc, for_background, r_req->riq_id))) {
				goto out;
			}
      /*
			pr_info("RFUSE: background depth throttle end riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      */
		}


		r_req->in.uid = from_kuid(fc->user_ns, current_fsuid());
		r_req->in.gid = from_kgid(fc->user_ns, current_fsgid());
		r_req->in.pid = pid_nr_ns(task_pid(current), fc->pid_ns);

		__set_bit(FR_WAITING, &r_req->flags);
		if (for_background) 
			__set_bit(FR_BACKGROUND, &r_req->flags);

		if (unlikely(r_req->in.uid == ((uid_t)-1) || r_req->in.gid == ((gid_t)-1))) {
			rfuse_put_request(r_req);
			return ERR_PTR(-EOVERFLOW);
		}
	}

	smp_mb();
	
	return r_req;  

out:
	rfuse_drop_waiting(fc);
	return ERR_PTR(err);
}

struct rfuse_req *try_rfuse_wt_get_req(struct fuse_mount *fm,
				       bool for_background, bool force,
				       size_t payload_len,
				       spinlock_t *file_lock)
{
	struct fuse_conn *fc = fm->fc;
	struct rfuse_req *r_req;
	struct rfuse_iqueue *riq;
	int err;

	if(force) {
		atomic_inc(&fc->num_waiting);

		r_req = try_rfuse_wt_request_alloc(fm, payload_len, file_lock);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		__set_bit(FR_WAITING, &r_req->flags);
		if(for_background){
			__set_bit(FR_BACKGROUND, &r_req->flags);
		} else{
			if(!r_req->nocreds)
				rfuse_force_creds(r_req);

			__set_bit(FR_FORCE, &r_req->flags);
		}
	} else {
		atomic_inc(&fc->num_waiting);

		smp_rmb();
		err = -ENOTCONN;
		if(!fc->connected) {
			goto out;
		}

		err = -ECONNREFUSED;
		if (fc->conn_error) {
			goto out;
		}

		r_req = try_rfuse_wt_request_alloc(fm, payload_len, file_lock);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		if(rfuse_block_alloc(fc, for_background, r_req->riq_id)){
			err = -EINTR;
			riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
      /*
			pr_info("RFUSE: write background depth throttle start riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      */
			if (wait_event_killable_exclusive(riq->blocked_waitq,
					!rfuse_block_alloc(fc, for_background,
							    r_req->riq_id))) {
				goto out;
			}
      /*
			pr_info("RFUSE: write background depth throttle end riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      */
		}

		r_req->in.uid = from_kuid(fc->user_ns, current_fsuid());
		r_req->in.gid = from_kgid(fc->user_ns, current_fsgid());
		r_req->in.pid = pid_nr_ns(task_pid(current), fc->pid_ns);

		__set_bit(FR_WAITING, &r_req->flags);
		if (for_background)
			__set_bit(FR_BACKGROUND, &r_req->flags);

		if (unlikely(r_req->in.uid == ((uid_t)-1) ||
			     r_req->in.gid == ((gid_t)-1))) {
			rfuse_put_request(r_req);
			return ERR_PTR(-EOVERFLOW);
		}
	}

	smp_mb();

	return r_req;

out:
	rfuse_drop_waiting(fc);
	return ERR_PTR(err);
}

/************ 4. Insert to Queue ************/

void __rfuse_get_request(struct rfuse_req *r_req){
	refcount_inc(&r_req->count);
}
void __rfuse_put_request(struct rfuse_req *r_req){
	refcount_dec(&r_req->count);
}

static int rfuse_queue_interrupt(struct rfuse_req *r_req){
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	struct rfuse_interrupt_entry *target_entry;

	spin_lock(&riq->lock);
	/* Check for we've sent request to interrupt this req */
	if (unlikely(!test_bit(FR_INTERRUPTED, &r_req->flags))) {
		spin_unlock(&riq->lock);
		return -EINVAL;
	}
	
	// Get a new entry from the interrupt queue
	target_entry = rfuse_read_interrupts_tail(riq);
	if(!target_entry){
		spin_unlock(&riq->lock);
		return 1;
	}
	// Inset a new Interrupt Queue Entry
	target_entry->unique = r_req->in.unique;
	rfuse_submit_interrupt_tail(riq);
	spin_unlock(&riq->lock);

	return 0;
}

// FORGET QUEUE INSERT
void rfuse_queue_forget(struct fuse_conn *fc, u64 nodeid, u64 nlookup){
	struct rfuse_iqueue *riq = rfuse_get_iqueue(fc);
	struct rfuse_forget_entry *target_entry = NULL;

	spin_lock(&riq->lock);
	if (riq->connected) {
		// Allocate a new forget_entry
		target_entry = rfuse_read_forgets_tail(riq);
		if(!target_entry){
			spin_unlock(&riq->lock);
			return;
		}
		
		// Fill inside the forget entry
		target_entry->unique = rfuse_get_unique(riq); 
		target_entry->nodeid = nodeid;
		target_entry->nlookup = nlookup;

		// Commit the forget entry
		rfuse_submit_forgets_tail(riq);
	}
	spin_unlock(&riq->lock);
}


/*
	Wait completion by busy waiting, do not sleep
	We can use r_req->flags for checking whether request is finished or not
*/
static void rfuse_request_wait_answer(struct rfuse_req *r_req){
	struct fuse_conn *fc = r_req->fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
	int err;

	if (!fc->no_interrupt) {
		err = rfuse_completion_poll(fc, riq, r_req);

		if (!err)
			return;

		set_bit(FR_INTERRUPTED, &r_req->flags);
		smp_mb__after_atomic();
		if (test_bit(FR_SENT, &r_req->flags))
			rfuse_queue_interrupt(r_req);
	}

	if (!test_bit(FR_FORCE, &r_req->flags)) {
		/* Only fatal signals may interrupt this */
		err = wait_event_killable(r_req->waitq,
				test_bit(FR_FINISHED, &r_req->flags));
		if (!err)
			return;

		spin_lock(&riq->lock);
		/* Request is not yet in userspace, bail out */
		if (test_bit(FR_PENDING, &r_req->flags)) {
			spin_unlock(&riq->lock);
			__rfuse_put_request(r_req);
			r_req->out.error = -EINTR;
			return;
		}
		spin_unlock(&riq->lock);
	}

	/*
	 * Either request is already in userspace, or it was forced.
	 * Wait it out.
	 */
	wait_event(r_req->waitq, test_bit(FR_FINISHED, &r_req->flags));
}

static void rfuse_queue_request(struct rfuse_req *r_req){
	struct fuse_mount *fm = r_req->fm;
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
	struct rfuse_address_entry *entry;

	if(test_bit(FR_BACKGROUND, &r_req->flags)){
		__set_bit(FR_ASYNC,&r_req->flags);
	}

	spin_lock(&riq->lock);						// set lock
	entry = rfuse_read_pending_tail(riq);		// Get an entry
	r_req->in.unique = rfuse_get_unique(riq); 
	entry->request = r_req->index;				// fill entry
	if(!test_bit(FR_BACKGROUND, &r_req->flags)) // only increase the reference count for synchronous requests
		__rfuse_get_request(r_req);
	rfuse_submit_pending_tail(riq);				// Commit entry
	spin_unlock(&riq->lock);					// unlock
	if(waitqueue_active(&riq->idle_user_waitq)){
		wake_up(&riq->idle_user_waitq);		// Wake up idle user thread
	}
}

// PENDING QUEUE INSERT
ssize_t rfuse_simple_request(struct rfuse_req *r_req){
	ssize_t ret=0;
	int err;

	err = rfuse_prepare_payload(r_req, true);
	if (err)
		return err;

	rfuse_queue_request(r_req);
	rfuse_request_wait_answer(r_req);
	smp_rmb();

	err = rfuse_import_payload(r_req);
	if (err)
		return err;

	ret = r_req->out.error;
	if (!ret && r_req->out_argvar) {
		ret = r_req->out.arglen;
	}

	return ret;
}


static void rfuse_queue_bg_list(struct fuse_conn *fc, struct list_head *queue)
{
	while (!list_empty(queue)) {
		struct rfuse_bg_entry *bg_entry;
		struct rfuse_iqueue *riq;
		struct rfuse_req *r_req;

		bg_entry = list_first_entry(queue, struct rfuse_bg_entry, list);
		list_del(&bg_entry->list);
		riq = rfuse_get_specific_iqueue(fc, bg_entry->riq_id);
		r_req = (struct rfuse_req *)&riq->kreq[bg_entry->request];
		kfree(bg_entry);

		rfuse_queue_request(r_req); // only queue request but not wait for it
	}
}

// MOVE (BG QUEUE --> local submit list)
static void rfuse_flush_bg_queue_locked(struct rfuse_iqueue *riq,
					struct list_head *queue)
{
	while (rfuse_background_submission_available(riq) &&
			!list_empty(&riq->bg_queue)) {
		struct rfuse_bg_entry *bg_entry;

		bg_entry = list_first_entry(&riq->bg_queue, struct rfuse_bg_entry, list);
		list_move_tail(&bg_entry->list, queue);
		riq->active_background++;
	}
}

/* Main: Async request를 할당, 초기화 후에 background list에 삽입 후 pending queue로 push */
static int rfuse_request_queue_background(struct rfuse_req *r_req)
{
	struct fuse_mount *fm = r_req->fm;
	struct fuse_conn *fc = fm->fc;
	struct rfuse_bg_entry *bg_entry = kmalloc_node(sizeof(struct rfuse_bg_entry), GFP_KERNEL, cpu_to_node(r_req->riq_id));
	LIST_HEAD(to_queue);
	bool queued = false;
	int err = -ENOTCONN;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);

	if (!bg_entry)
		return -ENOMEM;

	WARN_ON(!test_bit(FR_BACKGROUND, &r_req->flags));
	if (!test_bit(FR_WAITING, &r_req->flags)) {
		__set_bit(FR_WAITING, &r_req->flags);
		atomic_inc(&fc->num_waiting);
	}
	__set_bit(FR_ISREPLY, &r_req->flags);

	// Initialize background entry
	INIT_LIST_HEAD(&bg_entry->list);
	bg_entry->request = r_req->index;
	bg_entry->riq_id = r_req->riq_id;

	spin_lock(&riq->bg_lock);
	if (likely(riq->connected)) {
		riq->num_background++;
#if !LDY_PAYLOAD_AWARE
		if (riq->num_background == riq->max_background) {
			riq->blocked = 1;
      /*
			pr_info("RFUSE: background depth throttle armed riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u bg_queue_empty=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background,
				 list_empty(&riq->bg_queue), current->pid);
      */
		}
#endif
#if !LDY_PAYLOAD_AWARE
		if (riq->num_background == riq->congestion_threshold && fm->sb) {
			set_bdi_congested(fm->sb->s_bdi, BLK_RW_SYNC);
			set_bdi_congested(fm->sb->s_bdi, BLK_RW_ASYNC);
		}
#endif
    /* LDY: Async request merge */
    /* Sync 요청는 merge 할 수 있는 요청이라고 하더라도, 애초에 이전 요청이 완료되야 다음 요청이 submit 된다.
     * 하지만, async 요청은 이전 요청이 완료되지 않더라도 submit 할 수 있다.
     * 그러므로 background queue에서 write 요청이 들어오면, 이전 tail entry를 꺼내서 offset을 확인
     * 마지막 offset과 1이 차이가 난다면 바로 다음 chunk 이므로 요청을 하나로 합친다.
     * 대신 최대로 늘릴 수 있는 건 1MiB, 왜냐하면 이건 제약이니까.
     * 바로 구현하기 전에 helper 함수를 하나 추가해서, if 문으로 write 요청인지 확인, offset을 확인, 1이 차이가 나는지 확인 후
     * merge가 가능하면, matach log 출력, 불가능하면 mismatch log 출력한다.
     * 이게 가능하려면, 현재 async scheduling 기법인 round robin을 CPU id 기반으로 돌려야 한다.
     */
		list_add_tail(&bg_entry->list, &riq->bg_queue); // Add it to background queue
		rfuse_flush_bg_queue_locked(riq, &to_queue);
		if (list_empty(&to_queue)) {
      /*
			pr_info("RFUSE: background queue throttle hold riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u blocked=%d pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, riq->blocked,
				 current->pid);
      */
		}
		queued = true;
		err = 0;
	}
	spin_unlock(&riq->bg_lock);

	if (queued)
		rfuse_queue_bg_list(fc, &to_queue);
	else
		kfree(bg_entry);

	return err;
}


// BACKGROUND QUEUE INSERT
int rfuse_simple_background(struct fuse_mount *fm, struct rfuse_req *r_req){
	return rfuse_request_queue_background(r_req);
}

void rfuse_request_end(struct rfuse_req *r_req){
	struct fuse_mount *fm = r_req->fm;
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
	LIST_HEAD(to_queue);

	if(test_bit(FR_BACKGROUND, &r_req->flags)){
		spin_lock(&riq->bg_lock);
		clear_bit(FR_BACKGROUND, &r_req->flags);
#if !LDY_PAYLOAD_AWARE
		if (riq->num_background == riq->max_background) {
			riq->blocked = 0;
      /*
			pr_info("RFUSE: background depth throttle disarmed riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, current->pid);
      */
			wake_up(&riq->blocked_waitq);
		} else if (!riq->blocked) {
			/*
			 * Wake up next waiter, if any.  It's okay to use
			 * waitqueue_active(), as we've already synced up
			 * fc->blocked with waiters with the wake_up() call
			 * above.
			 */
			if (waitqueue_active(&riq->blocked_waitq))
				wake_up(&riq->blocked_waitq);
		}
#endif

#if !LDY_PAYLOAD_AWARE
		if (riq->num_background == riq->congestion_threshold && fm->sb) {
			clear_bdi_congested(fm->sb->s_bdi, BLK_RW_SYNC);
			clear_bdi_congested(fm->sb->s_bdi, BLK_RW_ASYNC);
		}
#endif
		riq->num_background--;
		riq->active_background--;
		rfuse_flush_bg_queue_locked(riq, &to_queue);
		if (!list_empty(&to_queue)) {
			pr_info("RFUSE: background queue throttle release riq=%d req=%u opcode=%u num_background=%u active_background=%u max_background=%u pid=%d\n",
				 r_req->riq_id, r_req->index, r_req->in.opcode,
				 riq->num_background, riq->active_background,
				 riq->max_background, current->pid);
		}
		spin_unlock(&riq->bg_lock);

		rfuse_queue_bg_list(fc, &to_queue);
	}

	if (test_bit(FR_ASYNC, &r_req->flags)) {
		int err;

		err = rfuse_import_payload(r_req);
		if (err && !r_req->out.error)
			r_req->out.error = err;
	}

	if (test_bit(FR_ASYNC, &r_req->flags)) {
		r_req->end(r_req->fm, r_req, r_req->out.error);
	}

	rfuse_put_request(r_req);
}


// READ FROM COMPLETE QUEUE for getattr
// static void rfuse_read_complete_queue(){
// 	struct rfuse_address_entry *c_entry; 
// 	spin_lock(&riq->lock);
// 	c_entry = rfuse_read_complete_head(riq);
// 	if(c_entry){
// 		r_req = &riq->kreq[c_entry->request];
// 		outarg_rfuse = (struct fuse_attr_out*)&r_req->args;
// 		rfuse_extract_complete_head(riq);
// 	}
// 	spin_unlock(&riq->lock);
// }


/************ 5. Device Read Write ************/
/*
 * Lock the request.  Up to the next unlock_request() there mustn't be
 * anything that could cause a page-fault.  If the request was already
 * aborted bail out.
 */
static int rfuse_lock_request(struct rfuse_req *r_req)
{
	int err = 0;
	if (r_req) {
		spin_lock(&r_req->waitq.lock);
		if (test_bit(FR_ABORTED, &r_req->flags))
			err = -ENOENT;
		else
			set_bit(FR_LOCKED, &r_req->flags);
		spin_unlock(&r_req->waitq.lock);
	}
	return err;
}

/*
 * Unlock request.  If it was aborted while locked, caller is responsible
 * for unlocking and ending the request.
 */
static int rfuse_unlock_request(struct rfuse_req *r_req)
{
	int err = 0;
	if (r_req) {
		spin_lock(&r_req->waitq.lock);
		if (test_bit(FR_ABORTED, &r_req->flags))
			err = -ENOENT;
		else
			clear_bit(FR_LOCKED, &r_req->flags);
		spin_unlock(&r_req->waitq.lock);
	}
	return err;
}

struct rfuse_copy_state {
	int write;
	struct rfuse_req *r_req;
	struct iov_iter *iter;
	struct pipe_buffer *pipebufs;
	struct pipe_buffer *currbuf;
	struct pipe_inode_info *pipe;
	unsigned long nr_segs;
	struct page *pg;
	unsigned len;
	unsigned offset;
	unsigned move_pages:1;
};

static void rfuse_copy_init(struct rfuse_copy_state *rcs, int write, struct iov_iter *iter){
	memset(rcs, 0, sizeof(*rcs));
	rcs->write = write;
	rcs->iter = iter;
}


/* Unmap and put previous page of userspace buffer */
static void rfuse_copy_finish(struct rfuse_copy_state *rcs)
{
	if (rcs->currbuf) {
		struct pipe_buffer *buf = rcs->currbuf;

		if (rcs->write)
			buf->len = PAGE_SIZE - rcs->len;
		rcs->currbuf = NULL;
	} else if (rcs->pg) {
		if (rcs->write) {
			flush_dcache_page(rcs->pg);
			set_page_dirty_lock(rcs->pg);
		}
		put_page(rcs->pg);
	}
	rcs->pg = NULL;
}

/*
 * Get another pagefull of userspace buffer, and map it to kernel
 * address space, and lock request
 */
static int rfuse_copy_fill(struct rfuse_copy_state *rcs)
{
	struct page *page;
	int err;

	err = rfuse_unlock_request(rcs->r_req);
	if (err)
		return err;

	rfuse_copy_finish(rcs);
	if (rcs->pipebufs) {
		struct pipe_buffer *buf = rcs->pipebufs;

		if (!rcs->write) {
			err = pipe_buf_confirm(rcs->pipe, buf);
			if (err)
				return err;

			BUG_ON(!rcs->nr_segs);
			rcs->currbuf = buf;
			rcs->pg = buf->page;
			rcs->offset = buf->offset;
			rcs->len = buf->len;
			rcs->pipebufs++;
			rcs->nr_segs--;
		} else {
			if (rcs->nr_segs >= rcs->pipe->max_usage)
				return -EIO;

			page = alloc_page(GFP_HIGHUSER);
			if (!page)
				return -ENOMEM;

			buf->page = page;
			buf->offset = 0;
			buf->len = 0;

			rcs->currbuf = buf;
			rcs->pg = page;
			rcs->offset = 0;
			rcs->len = PAGE_SIZE;
			rcs->pipebufs++;
			rcs->nr_segs++;
		}
	} else {
		size_t off;
		err = iov_iter_get_pages(rcs->iter, &page, PAGE_SIZE, 1, &off);
		if (err < 0)
			return err;
		BUG_ON(!err);
		rcs->len = err;
		rcs->offset = off;
		rcs->pg = page;
		iov_iter_advance(rcs->iter, err);
	}

	return rfuse_lock_request(rcs->r_req);
}

/* Do as much copy to/from userspace buffer as we can */
static int rfuse_copy_do(struct rfuse_copy_state *rcs, void **val, unsigned *size)
{
	unsigned ncpy = min(*size, rcs->len);
	if (val) {
		void *pgaddr = kmap_atomic(rcs->pg);
		void *buf = pgaddr + rcs->offset;

		if (rcs->write)
			memcpy(buf, *val, ncpy);
		else
			memcpy(*val, buf, ncpy);

		kunmap_atomic(pgaddr);
		*val += ncpy;
	}
	*size -= ncpy;
	rcs->len -= ncpy;
	rcs->offset += ncpy;
	return ncpy;
}

static int rfuse_try_move_page(struct rfuse_copy_state *rcs, struct page **pagep)
{
	int err;
	struct page *oldpage = *pagep;
	struct page *newpage;
	struct pipe_buffer *buf = rcs->pipebufs;

	get_page(oldpage);
	err = rfuse_unlock_request(rcs->r_req);
	if (err)
		goto out_put_old;

	rfuse_copy_finish(rcs);

	err = pipe_buf_confirm(rcs->pipe, buf);
	if (err)
		goto out_put_old;

	BUG_ON(!rcs->nr_segs);
	rcs->currbuf = buf;
	rcs->len = buf->len;
	rcs->pipebufs++;
	rcs->nr_segs--;

	if (rcs->len != PAGE_SIZE)
		goto out_fallback;

	if (!pipe_buf_try_steal(rcs->pipe, buf))
		goto out_fallback;

	newpage = buf->page;

	if (!PageUptodate(newpage))
		SetPageUptodate(newpage);

	ClearPageMappedToDisk(newpage);

	if (rfuse_check_page(newpage) != 0)
		goto out_fallback_unlock;

	/*
	 * This is a new and locked page, it shouldn't be mapped or
	 * have any special flags on it
	 */
	if (WARN_ON(page_mapped(oldpage)))
		goto out_fallback_unlock;
	if (WARN_ON(page_has_private(oldpage)))
		goto out_fallback_unlock;
	if (WARN_ON(PageDirty(oldpage) || PageWriteback(oldpage)))
		goto out_fallback_unlock;
	if (WARN_ON(PageMlocked(oldpage)))
		goto out_fallback_unlock;

	replace_page_cache_page(oldpage, newpage);

	get_page(newpage);

	if (!(buf->flags & PIPE_BUF_FLAG_LRU))
		lru_cache_add(newpage);

	err = 0;
	spin_lock(&rcs->r_req->waitq.lock);
	if (test_bit(FR_ABORTED, &rcs->r_req->flags))
		err = -ENOENT;
	else
		*pagep = newpage;
	spin_unlock(&rcs->r_req->waitq.lock);

	if (err) {
		unlock_page(newpage);
		put_page(newpage);
		goto out_put_old;
	}

	unlock_page(oldpage);
	/* Drop ref for ap->pages[] array */
	put_page(oldpage);
	rcs->len = 0;

	err = 0;
out_put_old:
	/* Drop ref obtained in this function */
	put_page(oldpage);
	return err;

out_fallback_unlock:
	unlock_page(newpage);
out_fallback:
	rcs->pg = buf->page;
	rcs->offset = buf->offset;

	err = rfuse_lock_request(rcs->r_req);
	if (!err)
		err = 1;

	goto out_put_old;
}

static int rfuse_ref_page(struct rfuse_copy_state *rcs, struct page *page, unsigned offset, unsigned count){
	struct pipe_buffer *buf;
	int err;

	if (rcs->nr_segs >= rcs->pipe->max_usage)
		return -EIO;

	get_page(page);
	err = rfuse_unlock_request(rcs->r_req);
	if (err) {
		put_page(page);
		return err;
	}

	rfuse_copy_finish(rcs);

	buf = rcs->pipebufs;
	buf->page = page;
	buf->offset = offset;
	buf->len = count;

	rcs->pipebufs++;
	rcs->nr_segs++;
	rcs->len = 0;

	return 0;
}

static int rfuse_copy_page(struct rfuse_copy_state *rcs, struct page **pagep,
		unsigned offset, unsigned count, int zeroing){

	int err;
	struct page *page = *pagep;
	
	if(page && zeroing && count < PAGE_SIZE)
		clear_highpage(page);
	
	while(count){
		if (rcs->write && rcs->pipebufs && page) {
			return rfuse_ref_page(rcs, page, offset, count);
		} else if (!rcs->len) {
			if (rcs->move_pages && page && offset == 0 && count == PAGE_SIZE) {
				err = rfuse_try_move_page(rcs, pagep);
				if (err <= 0)
					return err;
			} else {
				err = rfuse_copy_fill(rcs);
				if (err)
					return err;
			}
		}
		if (page) {
			void *mapaddr = kmap_atomic(page);
			void *buf = mapaddr + offset;
			offset += rfuse_copy_do(rcs, &buf, &count);
			kunmap_atomic(mapaddr);
		} else
			offset += rfuse_copy_do(rcs, NULL, &count);
	}
	if(page && !rcs->write)
		flush_dcache_page(page);
	return 0;
}

static int rfuse_copy_pages(struct rfuse_copy_state *rcs, unsigned nbytes,
		int zeroing)
{
	unsigned i;
	struct rfuse_req *r_req = rcs->r_req;
	struct rfuse_pages *rp = r_req->rp;

	if(rp == NULL){
		return 0;
	}
	
	for (i = 0; i < rp->num_pages && (nbytes || zeroing); i++) {
		int err;
		unsigned int offset = rp->descs[i].offset;
		unsigned int count = min(nbytes, rp->descs[i].length);
		err = rfuse_copy_page(rcs, &rp->pages[i], offset, count, zeroing);
		if (err)
			return err;

		nbytes -= count;
	}
	return 0;
}

ssize_t rfuse_dev_do_read(struct fuse_dev *fud, struct file *file, struct iov_iter *to, size_t garbage, loff_t index)
{
	struct fuse_conn *fc = fud->fc;
	struct rfuse_pages *rp;
	unsigned i;
	unsigned nbytes;
	ssize_t res = 0;

	struct rfuse_copy_state rcs;	
	int riq_id = (int)((index & RFUSE_RIQ_ID_MASK) >> 16);
	int req_index = (int)((index & RFUSE_REQ_IDX_MASK) >> 32);
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	struct rfuse_req *r_req = (struct rfuse_req*)&riq->kreq[req_index];


	rp = r_req->rp;
	rfuse_copy_init(&rcs, 1, to);	

	rcs.r_req = r_req;
	nbytes = r_req->in.arglen[0];

	for(i =0; i<rp->num_pages && (nbytes); i++){
		int err;
		unsigned int offset = rp->descs[i].offset;
		unsigned int count = min(nbytes,rp->descs[i].length);

		err = rfuse_copy_page(&rcs, &rp->pages[i], offset, count, 0);
		if(err)
			return err;
		nbytes -= count;
		res += count;
	}
	rfuse_copy_finish(&rcs);

	return res;
}

static int rfuse_dev_prep_splice_read(struct fuse_dev *fud, struct rfuse_req *r_req, struct rfuse_copy_state *rcs)
{
	int err = 0;
	struct fuse_write_in *inarg = (struct fuse_write_in *)&r_req->args;
	err = rfuse_copy_pages(rcs, inarg->size, 0);
	rfuse_copy_finish(rcs);

	return err;
}

ssize_t rfuse_dev_splice_read(struct file *in, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags)
{
	int total, ret;
	int page_nr = 0;
	struct pipe_buffer *bufs;
	struct rfuse_copy_state rcs;	
	struct fuse_dev *fud = fuse_get_dev(in);
	struct fuse_conn *fc = fud->fc;

	int riq_id = (int)((*ppos & RFUSE_RIQ_ID_MASK) >> 16);
	int req_index = (int)((*ppos & RFUSE_REQ_IDX_MASK) >> 32);
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	struct rfuse_req *r_req = (struct rfuse_req*)&riq->kreq[req_index];

	if (!fud)
		return -EPERM;

	bufs = kvmalloc_array(pipe->max_usage, sizeof(struct pipe_buffer),
			GFP_KERNEL);
	if (!bufs)
		return -ENOMEM;

	rfuse_copy_init(&rcs, 1, NULL);
	rcs.pipebufs = bufs;
	rcs.pipe = pipe;
	rcs.r_req = r_req;
	ret = rfuse_dev_prep_splice_read(fud, r_req, &rcs);
	if (ret < 0)
		goto out;

	if (pipe_occupancy(pipe->head, pipe->tail) + rcs.nr_segs > pipe->max_usage) {
		ret = -EIO;
		goto out;
	}

	for (ret = total = 0; page_nr < rcs.nr_segs; total += ret) {
		/*
		 * Need to be careful about this.  Having buf->ops in module
		 * code can Oops if the buffer persists after module unload.
		 */
		bufs[page_nr].ops = &nosteal_pipe_buf_ops;
		bufs[page_nr].flags = 0;
		ret = add_to_pipe(pipe, &bufs[page_nr++]);
		if (unlikely(ret < 0))
			break;
	}
	if (total)
		ret = total;
out:
	for (; page_nr < rcs.nr_segs; page_nr++)
		put_page(bufs[page_nr].page);

	kvfree(bufs);
	return ret;
}

ssize_t rfuse_dev_do_write(struct fuse_dev *fud, struct iov_iter *from, unsigned nbytes, size_t index){
	struct fuse_conn *fc = fud->fc;
	struct rfuse_pages *rp;
	unsigned i;
	ssize_t res = 0;

	struct rfuse_copy_state rcs;	
	int riq_id = (int)((index & RFUSE_RIQ_ID_MASK) >> 16);
	int req_index = (int)((index & RFUSE_REQ_IDX_MASK) >> 32);
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	struct rfuse_req *r_req = (struct rfuse_req*)&riq->kreq[req_index];

	rp = r_req->rp;
	rfuse_copy_init(&rcs, 0 ,from);	

	rcs.r_req = r_req;
	
	if(r_req->out.arglen > nbytes)
		r_req->out.arglen = nbytes;

	for(i =0; i < rp->num_pages && (nbytes); i++){
		int err;
		unsigned int offset = rp->descs[i].offset;
		unsigned int count = min(nbytes,rp->descs[i].length);
	
		err = rfuse_copy_page(&rcs, &rp->pages[i],offset, count,0);
		if(err)
			return err;
		nbytes -= count;
		res += count;
	}
	rfuse_copy_finish(&rcs);

	return res;
}

static int rfuse_dev_do_splice_write(struct fuse_dev *fud, struct rfuse_req *r_req, struct rfuse_copy_state *rcs, size_t len)
{
	int err = 0;
	
	if (!r_req->page_replace)
		rcs->move_pages = 0;

	err = rfuse_copy_pages(rcs, len, r_req->page_zeroing);
	rfuse_copy_finish(rcs);

	if(test_bit(FR_BACKGROUND, &r_req->flags)) {
		rfuse_request_end(r_req);
	} else {
		set_bit(FR_FINISHED, &r_req->flags);
		if(waitqueue_active(&r_req->waitq)) {
			wake_up(&r_req->waitq);
		}
	}

	return err ? err : len;
}

ssize_t rfuse_dev_splice_write(struct pipe_inode_info *pipe, struct file *out, loff_t *ppos, size_t len, unsigned int flags)
{
	unsigned int head, tail, mask, count;
	unsigned nbuf;
	unsigned idx;
	struct pipe_buffer *bufs;
	struct rfuse_copy_state rcs;
	struct fuse_dev *fud;
	struct fuse_conn *fc;
	size_t rem;
	ssize_t ret;
	int riq_id;
	int req_index;
	struct rfuse_iqueue *riq;
	struct rfuse_req *r_req;

	fud = fuse_get_dev(out);
	if (!fud)
		return -EPERM;
	fc = fud->fc;
	riq_id = (int)((*ppos & RFUSE_RIQ_ID_MASK) >> 16);
	req_index = (int)((*ppos & RFUSE_REQ_IDX_MASK) >> 32);
	riq = rfuse_get_specific_iqueue(fc, riq_id);
	r_req = (struct rfuse_req*)&riq->kreq[req_index];

	pipe_lock(pipe);

	head = pipe->head;
	tail = pipe->tail;
	mask = pipe->ring_size - 1;
	count = head - tail;

	bufs = kvmalloc_array(count, sizeof(struct pipe_buffer), GFP_KERNEL);
	if (!bufs) {
		pipe_unlock(pipe);
		return -ENOMEM;
	}

	nbuf = 0;
	rem = 0;
	for (idx = tail; idx != head && rem < len; idx++)
		rem += pipe->bufs[idx & mask].len;

	ret = -EINVAL;
	if (rem < len)
		goto out_free;

	rem = len;
	while (rem) {
		struct pipe_buffer *ibuf;
		struct pipe_buffer *obuf;

		if (WARN_ON(nbuf >= count || tail == head))
			goto out_free;

		ibuf = &pipe->bufs[tail & mask];
		obuf = &bufs[nbuf];

		if (rem >= ibuf->len) {
			*obuf = *ibuf;
			ibuf->ops = NULL;
			tail++;
			pipe->tail = tail;
		} else {
			if (!pipe_buf_get(pipe, ibuf))
				goto out_free;

			*obuf = *ibuf;
			obuf->flags &= ~PIPE_BUF_FLAG_GIFT;
			obuf->len = rem;
			ibuf->offset += obuf->len;
			ibuf->len -= obuf->len;
		}
		nbuf++;
		rem -= obuf->len;
	}
	pipe_unlock(pipe);

	rfuse_copy_init(&rcs, 0, NULL);
	rcs.pipebufs = bufs;
	rcs.nr_segs = nbuf;
	rcs.pipe = pipe;
	rcs.r_req = r_req;

	if (flags & SPLICE_F_MOVE)
		rcs.move_pages = 1;

	ret = rfuse_dev_do_splice_write(fud, r_req, &rcs, len);
	pipe_lock(pipe);
out_free:
	for (idx = 0; idx < nbuf; idx++)
		pipe_buf_release(pipe, &bufs[idx]);
	pipe_unlock(pipe);
	kvfree(bufs);
	return ret;
}

/************ 6. Unmount ************/
void rfuse_abort_conn(struct fuse_conn *fc){
	struct rfuse_iqueue **riq = fc->riq;
	unsigned int i;

	for(i = 0; i < RFUSE_NUM_IQUEUE; i++) {
		LIST_HEAD(to_queue);

		spin_lock(&riq[i]->lock);
		riq[i]->connected = 0;

		spin_lock(&riq[i]->bg_lock);
		rfuse_flush_bg_queue_locked(riq[i], &to_queue);
		spin_unlock(&riq[i]->bg_lock);

		spin_unlock(&riq[i]->lock);
		rfuse_queue_bg_list(fc, &to_queue);

		wake_up_all(&riq[i]->waitq);
		wake_up_all(&riq[i]->idle_user_waitq);
	}
}
EXPORT_SYMBOL_GPL(rfuse_abort_conn);
