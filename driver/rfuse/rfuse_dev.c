#include "fuse_i.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/uio.h>
#include <linux/miscdevice.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/bitmap.h>
#include <linux/mutex.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/pipe_fs_i.h>
#include <linux/swap.h>
#include <linux/splice.h>
#include <linux/sched.h>
#include <linux/random.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/vmalloc.h>
#include <asm/atomic.h>

#define RFUSE_INT_REQ_BIT (1ULL << 0)
#define RFUSE_REQ_ID_STEP (1ULL << 1)
#define NUM_NUMA_GROUPS 2
#define NUM_NUMA_NODES 20
/*
   0: Round Robin 
   1: ThreadID moduler
   2: Application CPU ID
   8: dynamic local/spread scheduler
 */

#define RFUSE_SELECTION_ALGO 2
#define PAYLOAD_AWARE 1

atomic_t rr_id = ATOMIC_INIT(0);

static const int numa_group[NUM_NUMA_GROUPS][NUM_NUMA_NODES] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 },
	{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39 },
};

int find_numa_group_and_index(int id, int *start_idx)
{
	int group;
	int idx;

	if (start_idx)
		*start_idx = -1;

	for (group = 0; group < NUM_NUMA_GROUPS; group++) {
		for (idx = 0; idx < NUM_NUMA_NODES; idx++) {
			if (numa_group[group][idx] != id)
				continue;

			if (start_idx)
				*start_idx = idx;

			return group;
		}
	}

	return -1;
}

/*
* Duplicated function from dev.c
*/
static struct fuse_dev *fuse_get_dev(struct file *file)
{
	return READ_ONCE(file->private_data);
}

#define RFUSE_PAYLOAD_UNIT_SIZE (FUSE_MAX_MAX_PAGES * PAGE_SIZE)
#define RFUSE_PAYLOAD_POOL_SIZE (RFUSE_WORKER_PER_RING * RFUSE_PAYLOAD_UNIT_SIZE)
#define RFUSE_PAYLOAD_POOL_PAGES (RFUSE_PAYLOAD_POOL_SIZE / PAGE_SIZE)

enum rfuse_sbuf_alloc_reason {
	RFUSE_PAYLOAD_REASON_NONE = 0,
	RFUSE_PAYLOAD_REASON_OVERSIZE,
	RFUSE_PAYLOAD_REASON_EXHAUSTION,
	RFUSE_PAYLOAD_REASON_FRAGMENTATION,
};

struct rfuse_sbuf_search {
	bool found;
	u32 page_index;
};

struct rfuse_sbuf_failure_state {
	u32 free_pages;
	u32 total_pages;
	u32 search_hint;
	bool has_run;
	enum rfuse_sbuf_alloc_reason reason;
};

void rfuse_init_ring_buffer_1(struct ring_buffer_1 *rb);
void rfuse_init_ring_buffer_2(struct ring_buffer_2 *rb);
void rfuse_init_ring_buffer_3(struct ring_buffer_3 *rb);

typedef void *(*rfuse_vmalloc_node_range_fn_t)(unsigned long size,
					       unsigned long align,
					       unsigned long start,
					       unsigned long end,
					       gfp_t gfp_mask,
					       pgprot_t prot,
					       unsigned long vm_flags,
					       int node,
					       const void *caller);

static DEFINE_MUTEX(rfuse_vmalloc_lookup_lock);
static rfuse_vmalloc_node_range_fn_t rfuse_vmalloc_node_range_fn;
static bool rfuse_vmalloc_lookup_done;

static rfuse_vmalloc_node_range_fn_t rfuse_get_vmalloc_node_range_fn(void)
{
#ifdef CONFIG_KPROBES
	static unsigned long (*kallsyms_lookup_name_fn)(const char *name);
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	rfuse_vmalloc_node_range_fn_t fn;
	int ret;

	fn = READ_ONCE(rfuse_vmalloc_node_range_fn);
	if (fn || READ_ONCE(rfuse_vmalloc_lookup_done))
		return fn;

	mutex_lock(&rfuse_vmalloc_lookup_lock);
	fn = rfuse_vmalloc_node_range_fn;
	if (fn || rfuse_vmalloc_lookup_done)
		goto out_unlock;

	ret = register_kprobe(&kp);
	if (ret)
		goto out_done;

	kallsyms_lookup_name_fn = (void *)kp.addr;
	unregister_kprobe(&kp);
	if (!kallsyms_lookup_name_fn)
		goto out_done;

	fn = (rfuse_vmalloc_node_range_fn_t)
		kallsyms_lookup_name_fn("__vmalloc_node_range");
	if (!fn)
		goto out_done;

	WRITE_ONCE(rfuse_vmalloc_node_range_fn, fn);

out_done:
	WRITE_ONCE(rfuse_vmalloc_lookup_done, true);
out_unlock:
	mutex_unlock(&rfuse_vmalloc_lookup_lock);
	return READ_ONCE(rfuse_vmalloc_node_range_fn);
#else
	return NULL;
#endif
}

static void *rfuse_vmalloc_user_node(unsigned long size, int node)
{
	rfuse_vmalloc_node_range_fn_t fn;

	/*
	 * LDY: sbuf payload는 riq_id에 대응하는 CPU의 NUMA node에만
	 * 배치한다. __GFP_THISNODE를 사용해 다른 node fallback을 막고,
	 * local page 확보에 실패하면 RFUSE init 자체를 실패시킨다.
	 * VM_USERMAP은 기존 RFUSE_PAYLOAD mmap 경로를 유지하기 위해
	 * 필요하다.
	 *
	 * 외부 module에서는 __vmalloc_node_range()를 직접 link할 수
	 * 없어서, 1회 kprobe lookup으로 심볼 주소를 얻은 뒤
	 * vmalloc_user()와 동일한 prot/vm_flags로 호출한다.
	 */
	fn = rfuse_get_vmalloc_node_range_fn();
	if (!fn)
		return NULL;

	return fn(size, SHMLBA, VMALLOC_START, VMALLOC_END,
		  GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE,
		  PAGE_KERNEL, VM_USERMAP, node, __builtin_return_address(0));
}

static int rfuse_target_node(int riq_id, int *node_id)
{
	int node;

	/*
	 * LDY: riq_id를 CPU index처럼 사용하므로, strict NUMA policy를
	 * 위해 가능한 CPU/node 조합만 허용한다. sbuf payload는 물론
	 * 같은 ring의 초기화 경로도 동일한 검증 결과를 사용한다.
	 */
	if (riq_id < 0 || riq_id >= nr_cpu_ids || !cpu_possible(riq_id))
		return -EINVAL;

	node = cpu_to_node(riq_id);
	if (node < 0 || !node_possible(node) || !node_online(node))
		return -EINVAL;

	*node_id = node;
	return 0;
}

static void rfuse_sbuf_publish_req_locked(struct rfuse_req *r_req,
					     u32 page_index, u32 page_count)
{
	r_req->sbuf_page_index = page_index;
	r_req->sbuf_page_count = page_count;
	r_req->sbuf_reserved = true;
	r_req->sbuf_offset = page_index << PAGE_SHIFT;
	r_req->sbuf_capacity = (size_t)page_count << PAGE_SHIFT;
}

static void rfuse_sbuf_clear_req_locked(struct rfuse_req *r_req)
{
	r_req->sbuf_reserved = false;
	r_req->sbuf_page_index = 0;
	r_req->sbuf_page_count = 0;
	r_req->sbuf_offset = 0;
	r_req->sbuf_capacity = 0;
	r_req->sbuf_len = 0;
}

static void rfuse_sbuf_reset_unreserved_req_locked(struct rfuse_req *r_req)
{
	if (WARN_ON(r_req->sbuf_reserved))
		return;

	rfuse_sbuf_clear_req_locked(r_req);
}

static struct rfuse_sbuf_search
rfuse_sbuf_find_run_locked(struct rfuse_iqueue *riq, u32 need_pages)
{
	struct rfuse_sbuf_search search = { 0 };
	unsigned long pos;
	u32 start;

	if (!need_pages || need_pages > riq->sbuf_page_count)
		return search;

	start = riq->sbuf_search_hint;
	if (start >= riq->sbuf_page_count)
		start = 0;

	pos = bitmap_find_next_zero_area(riq->sbuf_bitmap,
					 riq->sbuf_page_count,
					 start, need_pages, 0);
	if (pos < riq->sbuf_page_count) {
		search.found = true;
		search.page_index = pos;
		return search;
	}

	if (!start)
		return search;

	pos = bitmap_find_next_zero_area(riq->sbuf_bitmap,
					 riq->sbuf_page_count,
					 0, need_pages, 0);
	if (pos < riq->sbuf_page_count) {
		search.found = true;
		search.page_index = pos;
	}

	return search;
}

static u32 rfuse_sbuf_find_largest_free_run_locked(struct rfuse_iqueue *riq)
{
	unsigned long pos = 0;
	u32 largest = 0;

	while (pos < riq->sbuf_page_count) {
		unsigned long used;
		u32 run_len;

		pos = find_next_zero_bit(riq->sbuf_bitmap,
					 riq->sbuf_page_count, pos);
		if (pos >= riq->sbuf_page_count)
			break;

		used = find_next_bit(riq->sbuf_bitmap,
				     riq->sbuf_page_count, pos);
		run_len = used >= riq->sbuf_page_count ?
			  riq->sbuf_page_count - pos : used - pos;
		if (run_len > largest)
			largest = run_len;

		pos = used + 1;
	}

	return largest;
}

static u32 rfuse_sbuf_measure_free_run_locked(struct rfuse_iqueue *riq,
						 u32 page_index, u32 page_count)
{
	u32 left = page_index;
	u32 right = page_index + page_count;

	while (left > 0 && !test_bit(left - 1, riq->sbuf_bitmap))
		left--;
	while (right < riq->sbuf_page_count &&
	       !test_bit(right, riq->sbuf_bitmap))
		right++;

	return right - left;
}

static void rfuse_sbuf_refresh_max_free_hint_locked(struct rfuse_iqueue *riq)
{
	WRITE_ONCE(riq->sbuf_max_free_hint,
		   rfuse_sbuf_find_largest_free_run_locked(riq));
}

static bool rfuse_sbuf_can_reserve_locked(struct rfuse_iqueue *riq, u32 need_pages)
{
	if (!need_pages)
		return false;

	return rfuse_sbuf_find_run_locked(riq, need_pages).found;
}

static bool rfuse_sbuf_can_reserve(struct rfuse_iqueue *riq, u32 need_pages)
{
	bool can_reserve;

	spin_lock(&riq->sbuf_lock);
	can_reserve = rfuse_sbuf_can_reserve_locked(riq, need_pages);
	spin_unlock(&riq->sbuf_lock);

	return can_reserve;
}

static enum rfuse_sbuf_alloc_reason
rfuse_sbuf_classify_failure_locked(struct rfuse_iqueue *riq, u32 need_pages,
				      bool has_run)
{
	if (need_pages > riq->sbuf_page_count)
		return RFUSE_PAYLOAD_REASON_OVERSIZE;
	if (has_run)
		return RFUSE_PAYLOAD_REASON_NONE;
	if (riq->sbuf_free_pages < need_pages)
		return RFUSE_PAYLOAD_REASON_EXHAUSTION;
	return RFUSE_PAYLOAD_REASON_FRAGMENTATION;
}

static void rfuse_sbuf_capture_failure_locked(struct rfuse_iqueue *riq,
						 u32 need_pages,
						 struct rfuse_sbuf_failure_state *state)
{
	state->free_pages = riq->sbuf_free_pages;
	state->total_pages = riq->sbuf_page_count;
	state->search_hint = riq->sbuf_search_hint;
	state->has_run = rfuse_sbuf_can_reserve_locked(riq, need_pages);
	state->reason = rfuse_sbuf_classify_failure_locked(riq, need_pages,
							      state->has_run);
}

static const char *rfuse_sbuf_reason_str(enum rfuse_sbuf_alloc_reason reason)
{
	switch (reason) {
	case RFUSE_PAYLOAD_REASON_OVERSIZE:
		return "oversize";
	case RFUSE_PAYLOAD_REASON_EXHAUSTION:
		return "exhaustion";
	case RFUSE_PAYLOAD_REASON_FRAGMENTATION:
		return "fragmentation";
	default:
		return "none";
	}
}

static void rfuse_sbuf_set_transport_flags(struct rfuse_req *r_req,
					      unsigned int sbuf_flags)
{
	r_req->sbuf_flags &= ~(RFUSE_PAYLOAD_IN | RFUSE_PAYLOAD_OUT);
	r_req->sbuf_flags |= sbuf_flags &
				(RFUSE_PAYLOAD_IN | RFUSE_PAYLOAD_OUT);
}

static void rfuse_sbuf_clear_transport_flags(struct rfuse_req *r_req)
{
	r_req->sbuf_flags &= ~(RFUSE_PAYLOAD_IN | RFUSE_PAYLOAD_OUT);
}

static const char *rfuse_sbuf_req_type_str(const struct rfuse_req *r_req)
{
	if (r_req->in.opcode == FUSE_WRITE ||
	    ((r_req->sbuf_flags & RFUSE_PAYLOAD_IN) &&
	     r_req->in.opcode != FUSE_READ))
		return "WRITE";
	if (r_req->in.opcode == FUSE_READ ||
	    (r_req->sbuf_flags & RFUSE_PAYLOAD_OUT))
		return "READ";
	return "UNKNOWN";
}

static void rfuse_sbuf_log_failure(struct rfuse_req *r_req, u32 need_pages,
				      const struct rfuse_sbuf_failure_state *state,
				      const char *action)
{
	pr_debug_ratelimited("RFUSE: sbuf alloc type=%s riq=%d req=%u opcode=%u need_pages=%u free_pages=%u total_pages=%u search_hint=%u has_run=%u reason=%s action=%s pid=%d\n",
			     rfuse_sbuf_req_type_str(r_req), r_req->riq_id,
			     r_req->index, r_req->in.opcode,
			     need_pages, state->free_pages, state->total_pages,
			     state->search_hint, state->has_run,
			     rfuse_sbuf_reason_str(state->reason), action,
			     current->pid);
}

static int rfuse_sbuf_reserve_locked(struct rfuse_req *r_req, u32 need_pages);

static int rfuse_sbuf_try_alloc(struct rfuse_req *r_req, size_t need,
				   struct rfuse_sbuf_failure_state *state)
{
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc,
							     r_req->riq_id);
	u32 need_pages;
	int ret;

	if (!need)
		return 0;

	need_pages = DIV_ROUND_UP(need, PAGE_SIZE);
	if (!need_pages)
		return -EINVAL;
	if (need_pages > riq->sbuf_page_count) {
		spin_lock(&riq->sbuf_lock);
		rfuse_sbuf_reset_unreserved_req_locked(r_req);
		rfuse_sbuf_capture_failure_locked(riq, need_pages, state);
		spin_unlock(&riq->sbuf_lock);
		return -E2BIG;
	}

	spin_lock(&riq->sbuf_lock);
	ret = rfuse_sbuf_reserve_locked(r_req, need_pages);
	if (ret) {
		rfuse_sbuf_reset_unreserved_req_locked(r_req);
		rfuse_sbuf_capture_failure_locked(riq, need_pages, state);
	}
	spin_unlock(&riq->sbuf_lock);

	return ret;
}

static int rfuse_sbuf_reserve_locked(struct rfuse_req *r_req, u32 need_pages)
{
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	struct rfuse_sbuf_search search;

	if (WARN_ON(!need_pages))
		return -EINVAL;
	if (WARN_ON(r_req->sbuf_reserved))
		return -EEXIST;
	if (need_pages > riq->sbuf_page_count)
		return -E2BIG;

	search = rfuse_sbuf_find_run_locked(riq, need_pages);
	if (!search.found) {
		rfuse_sbuf_refresh_max_free_hint_locked(riq);
		return -EAGAIN;
	}

	bitmap_set(riq->sbuf_bitmap, search.page_index, need_pages);
	if (WARN_ON(riq->sbuf_free_pages < need_pages)) {
		bitmap_clear(riq->sbuf_bitmap, search.page_index, need_pages);
		return -ENOSPC;
	}
	riq->sbuf_free_pages -= need_pages;
	riq->sbuf.used = riq->sbuf.size -
			    ((size_t)riq->sbuf_free_pages << PAGE_SHIFT);
	rfuse_sbuf_publish_req_locked(r_req, search.page_index, need_pages);
	riq->sbuf_search_hint = search.page_index + need_pages;
	if (riq->sbuf_search_hint >= riq->sbuf_page_count)
		riq->sbuf_search_hint = 0;

	return 0;
}

static bool rfuse_sbuf_range_allocated_locked(struct rfuse_iqueue *riq,
						 u32 page_index, u32 page_count)
{
	u32 i;

	for (i = 0; i < page_count; i++) {
		if (!test_bit(page_index + i, riq->sbuf_bitmap))
			return false;
	}

	return true;
}

static bool rfuse_sbuf_release_locked(struct rfuse_req *r_req)
{
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	u32 page_index = r_req->sbuf_page_index;
	u32 page_count = r_req->sbuf_page_count;
	u32 merged_len;

	if (WARN_ON(!r_req->sbuf_reserved))
		return false;
	if (WARN_ON(!page_count))
		return false;
	if (WARN_ON(page_index + page_count > riq->sbuf_page_count))
		return false;
	if (WARN_ON(!rfuse_sbuf_range_allocated_locked(riq, page_index, page_count)))
		return false;

	bitmap_clear(riq->sbuf_bitmap, page_index, page_count);
	riq->sbuf_free_pages += page_count;
	if (WARN_ON(riq->sbuf_free_pages > riq->sbuf_page_count)) {
		bitmap_set(riq->sbuf_bitmap, page_index, page_count);
		riq->sbuf_free_pages -= page_count;
		return false;
	}
	riq->sbuf.used = riq->sbuf.size -
			    ((size_t)riq->sbuf_free_pages << PAGE_SHIFT);
	merged_len = rfuse_sbuf_measure_free_run_locked(riq, page_index,
							  page_count);
	if (merged_len > READ_ONCE(riq->sbuf_max_free_hint))
		WRITE_ONCE(riq->sbuf_max_free_hint, merged_len);
	rfuse_sbuf_clear_req_locked(r_req);
	return true;
}

static int rfuse_sbuf_alloc(struct rfuse_req *r_req, size_t need, bool may_wait)
{
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(r_req->fm->fc,
							     r_req->riq_id);
	struct rfuse_sbuf_failure_state state = { 0 };
	u32 need_pages;
	int ret;

	if (!need)
		return 0;

	need_pages = DIV_ROUND_UP(need, PAGE_SIZE);
	if (!need_pages)
		return -EINVAL;

	for (;;) {
		ret = rfuse_sbuf_try_alloc(r_req, need, &state);
		if (!ret)
			return 0;
		if (ret != -EAGAIN || !may_wait)
			break;

		rfuse_sbuf_log_failure(r_req, need_pages, &state, "wait");
		ret = wait_event_interruptible(riq->sbuf_waitq,
				!r_req->fm->fc->connected ||
				rfuse_sbuf_can_reserve(riq, need_pages));
		if (ret)
			break;
		if (!r_req->fm->fc->connected) {
			ret = -ENOTCONN;
			break;
		}
	}

	if (ret == -EAGAIN || ret == -E2BIG || ret == -ENOMEM)
		rfuse_sbuf_log_failure(r_req, need_pages, &state, "fail");

	return ret;
}

void rfuse_release_sbuf(struct rfuse_req *r_req)
{
	struct rfuse_iqueue *riq;
	bool released;

	if (!r_req || !r_req->sbuf_reserved)
		return;

	riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	spin_lock(&riq->sbuf_lock);
	released = rfuse_sbuf_release_locked(r_req);
	spin_unlock(&riq->sbuf_lock);
	if (!released)
		return;

	/*
	 * Release validation is best-effort: sbuf_reserved plus bitmap range
	 * checks catch obvious misuse, but they do not prove absence of a late
	 * release after range reuse without an additional reservation cookie.
	 */
	wake_up(&riq->sbuf_waitq);
	r_req->sbuf_flags = 0;
}

static int rfuse_sbuf_pages_capacity(struct rfuse_pages *rp, size_t *capacity)
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

static int rfuse_sbuf_copy_from_pages(struct rfuse_req *r_req, size_t len)
{
	struct rfuse_pages *rp = r_req->rp;
	char *dst;
	unsigned int i;
	size_t capacity;
	size_t remaining = len;
	int err;

	if (!rp || !len)
		return 0;

	err = rfuse_sbuf_pages_capacity(rp, &capacity);
	if (err)
		return err;
	if (len > capacity)
		return -EIO;

	dst = (char *)r_req->fm->fc->riq[r_req->riq_id]->sbuf.kaddr + r_req->sbuf_offset;
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

static size_t rfuse_requested_out_sbuf_len(struct rfuse_req *r_req)
{
	if (r_req->in.opcode == FUSE_READ) {
		struct fuse_read_in *in = (struct fuse_read_in *)&r_req->args;

		return in->size;
	}

	return r_req->sbuf_capacity;
}

static void rfuse_zero_sbuf_tail(struct rfuse_pages *rp, size_t copied,
				    size_t requested)
{
	size_t sbuf_pos = 0;
	unsigned int i;

	for (i = 0; i < rp->num_pages && sbuf_pos < requested; i++) {
		struct fuse_page_desc *desc = &rp->descs[i];
		size_t desc_len = min_t(size_t, desc->length,
					requested - sbuf_pos);

		if (copied < sbuf_pos + desc_len) {
			size_t zero_start = 0;
			size_t zero_len;
			void *addr;

			if (copied > sbuf_pos)
				zero_start = copied - sbuf_pos;
			zero_len = desc_len - zero_start;

			addr = kmap_atomic(rp->pages[i]);
			memset(addr + desc->offset + zero_start, 0, zero_len);
			kunmap_atomic(addr);
			flush_dcache_page(rp->pages[i]);
		}

		sbuf_pos += desc_len;
	}
}

int rfuse_import_sbuf(struct rfuse_req *r_req)
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

	if (!rp || !r_req->sbuf_capacity)
		return 0;
	if (!(r_req->sbuf_flags & RFUSE_PAYLOAD_OUT))
		return 0;
	if (r_req->out.error)
		return 0;

	actual = r_req->out.arglen;
	requested = rfuse_requested_out_sbuf_len(r_req);

	err = rfuse_sbuf_pages_capacity(rp, &page_capacity);
	if (err)
		return err;
	if (requested > r_req->sbuf_capacity ||
	    requested > page_capacity ||
	    actual > r_req->sbuf_capacity ||
	    actual > requested ||
	    actual > page_capacity)
		return -EIO;

	remaining = actual;
	riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	src = (char *)riq->sbuf.kaddr + r_req->sbuf_offset;
	r_req->sbuf_len = remaining;

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
	if (r_req->page_zeroing && r_req->sbuf_len < requested)
		rfuse_zero_sbuf_tail(rp, r_req->sbuf_len, requested);

	return 0;
}

int rfuse_reserve_sbuf(struct rfuse_req *r_req, size_t len,
			  unsigned int sbuf_flags, bool may_wait)
{
	int ret;

	if (!r_req)
		return -EINVAL;
	if (!len)
		return 0;
	if ((sbuf_flags & (RFUSE_PAYLOAD_IN | RFUSE_PAYLOAD_OUT)) == 0)
		return -EINVAL;
	if ((sbuf_flags & RFUSE_PAYLOAD_IN) &&
	    (sbuf_flags & RFUSE_PAYLOAD_OUT))
		return -EINVAL;
	if (r_req->sbuf_capacity)
		return len <= r_req->sbuf_capacity ? 0 : -EINVAL;

	rfuse_sbuf_set_transport_flags(r_req, sbuf_flags);

	/* LDY: READ/WRITE sbuf 부족은 legacy fallback으로 우회하지 않고,
	 * sbuf 반환을 기다린 뒤 동일한 shared-buffer data path로 재시도한다.
	 * ABI나 request semantics는 변경하지 않는다.
	 */
	ret = rfuse_sbuf_alloc(r_req, len, may_wait);
	if (ret) {
		rfuse_sbuf_clear_transport_flags(r_req);
		return ret;
	}

	return 0;
}

ssize_t rfuse_sbuf_copy_from_iter(struct rfuse_req *r_req,
				     struct iov_iter *ii, size_t len)
{
	struct rfuse_iqueue *riq;
	char *dst;
	size_t copied;

	if (!r_req || !ii)
		return -EINVAL;
	if (!len)
		return 0;
	if (!r_req->sbuf_capacity ||
	    !(r_req->sbuf_flags & RFUSE_PAYLOAD_IN))
		return -EINVAL;
	if (len > r_req->sbuf_capacity)
		return -EINVAL;

	riq = rfuse_get_specific_iqueue(r_req->fm->fc, r_req->riq_id);
	dst = (char *)riq->sbuf.kaddr + r_req->sbuf_offset;
	copied = copy_from_iter(dst, len, ii);
	if (!copied)
		return -EFAULT;

	r_req->sbuf_len = copied;
	return copied;
}

int rfuse_prepare_sbuf(struct rfuse_req *r_req, bool may_wait)
{
	size_t need = 0;
	int ret;

	if (r_req->sbuf_capacity)
		return 0;

	if (r_req->in.opcode == FUSE_WRITE && r_req->in_pages && r_req->in.arglen[0]) {
		need = r_req->in.arglen[0];
		rfuse_sbuf_set_transport_flags(r_req, RFUSE_PAYLOAD_IN);
	} else if (r_req->in.opcode == FUSE_READ && r_req->out_pages && r_req->out.arglen) {
		need = r_req->out.arglen;
		rfuse_sbuf_set_transport_flags(r_req, RFUSE_PAYLOAD_OUT);
	} else {
		return 0;
	}

	ret = rfuse_sbuf_alloc(r_req, need, may_wait);
	if (ret)
		return ret;

	if (r_req->sbuf_flags & RFUSE_PAYLOAD_IN) {
		ret = rfuse_sbuf_copy_from_pages(r_req, need);
		if (ret) {
			rfuse_release_sbuf(r_req);
			return ret;
		}
		r_req->sbuf_len = need;
	}

	return 0;
}

static int rfuse_sbuf_pool_init(struct rfuse_iqueue *riq)
{
	int err;
	int node;

	BUILD_BUG_ON(RFUSE_PAYLOAD_POOL_SIZE % PAGE_SIZE);

	err = rfuse_target_node(riq->riq_id, &node);
	if (err)
		return err;

	riq->sbuf.kaddr = rfuse_vmalloc_user_node(RFUSE_PAYLOAD_POOL_SIZE, node);
	if (!riq->sbuf.kaddr)
		return -ENOMEM;

	riq->sbuf.uaddr = NULL;
	riq->sbuf.size = RFUSE_PAYLOAD_POOL_SIZE;
	riq->sbuf.used = 0;
	spin_lock_init(&riq->sbuf_lock);
	init_waitqueue_head(&riq->sbuf_waitq);
	riq->sbuf_page_count = RFUSE_PAYLOAD_POOL_PAGES;
	riq->sbuf_free_pages = riq->sbuf_page_count;
	riq->sbuf_search_hint = 0;
	WRITE_ONCE(riq->sbuf_max_free_hint,
		   riq->sbuf_page_count);
	riq->sbuf_bitmap = bitmap_zalloc(riq->sbuf_page_count, GFP_KERNEL);
	if (!riq->sbuf_bitmap) {
		vfree(riq->sbuf.kaddr);
		riq->sbuf.kaddr = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void rfuse_sbuf_pool_destroy(struct rfuse_iqueue *riq)
{
	if (riq->sbuf_bitmap) {
		bitmap_free(riq->sbuf_bitmap);
		riq->sbuf_bitmap = NULL;
	}
	if (riq->sbuf.kaddr)
		vfree(riq->sbuf.kaddr);
}

static void rfuse_iqueue_destroy_one(struct rfuse_iqueue *riq)
{
	if (!riq)
		return;

	kfree(riq->pending.kaddr);
	kfree(riq->interrupts.kaddr);
	kfree(riq->forgets.kaddr);
	kfree(riq->completes.kaddr);
	kfree(riq->karg);
	kfree(riq->kreq);
	rfuse_sbuf_pool_destroy(riq);

	kfree(riq->argbm.bitmap);
	kfree(riq->reqbm.bitmap);
	kfree(riq);
}

static int rfuse_iqueue_alloc_one(struct rfuse_iqueue **riq_out, int riq_id,
				       void *priv)
{
	struct rfuse_iqueue *riq;
	int node_id;
	int err;

	err = rfuse_target_node(riq_id, &node_id);
	if (err)
		return err;

	riq = kzalloc_node(4096, GFP_KERNEL, node_id);
	if (!riq)
		return -ENOMEM;

	spin_lock_init(&riq->lock);
	riq->riq_id = riq_id;
	init_waitqueue_head(&riq->waitq);
	init_waitqueue_head(&riq->idle_user_waitq);
	riq->connected = 1;
	riq->priv = priv;
	WRITE_ONCE(riq->stream_hint, 0);

	rfuse_init_ring_buffer_1(&riq->pending);
	rfuse_init_ring_buffer_2(&riq->interrupts);
	rfuse_init_ring_buffer_3(&riq->forgets);
	rfuse_init_ring_buffer_1(&riq->completes);

	riq->pending.kaddr = kmalloc_node(sizeof(struct rfuse_address_entry) *
					  RFUSE_MAX_QUEUE_SIZE,
					  GFP_KERNEL, node_id);
	if (!riq->pending.kaddr) {
		err = -ENOMEM;
		goto err_destroy;
	}

	riq->interrupts.kaddr = kmalloc_node(sizeof(struct rfuse_interrupt_entry) *
					     RFUSE_MAX_QUEUE_SIZE,
					     GFP_KERNEL, node_id);
	if (!riq->interrupts.kaddr) {
		err = -ENOMEM;
		goto err_destroy;
	}

	riq->forgets.kaddr = kmalloc_node(sizeof(struct rfuse_forget_entry) *
					  RFUSE_MAX_QUEUE_SIZE,
					  GFP_KERNEL, node_id);
	if (!riq->forgets.kaddr) {
		err = -ENOMEM;
		goto err_destroy;
	}

	riq->completes.kaddr = kmalloc_node(sizeof(struct rfuse_address_entry) *
					    RFUSE_MAX_QUEUE_SIZE,
					    GFP_KERNEL, node_id);
	if (!riq->completes.kaddr) {
		err = -ENOMEM;
		goto err_destroy;
	}

	riq->karg = kmalloc_node(sizeof(struct rfuse_arg) *
				 RFUSE_MAX_QUEUE_SIZE * 2,
				 GFP_KERNEL, node_id);
	if (!riq->karg) {
		err = -ENOMEM;
		goto err_destroy;
	}

	riq->kreq = kmalloc_node(sizeof(struct rfuse_req) *
				 RFUSE_MAX_QUEUE_SIZE * 2,
				 GFP_KERNEL, node_id);
	if (!riq->kreq) {
		err = -ENOMEM;
		goto err_destroy;
	}

	err = rfuse_sbuf_pool_init(riq);
	if (err)
		goto err_destroy;

	riq->argbm.bitmap_size = RFUSE_MAX_QUEUE_SIZE * 2;
	riq->reqbm.bitmap_size = RFUSE_MAX_QUEUE_SIZE * 2;
	riq->reqbm.full = 0;
	riq->argbm.full = 0;
	riq->argbm.bitmap = kzalloc_node((RFUSE_MAX_QUEUE_SIZE * 2) >> 3,
					 GFP_KERNEL, node_id);
	if (!riq->argbm.bitmap) {
		err = -ENOMEM;
		goto err_destroy;
	}

	riq->reqbm.bitmap = kzalloc_node((RFUSE_MAX_QUEUE_SIZE * 2) >> 3,
					 GFP_KERNEL, node_id);
	if (!riq->reqbm.bitmap) {
		err = -ENOMEM;
		goto err_destroy;
	}

	INIT_LIST_HEAD(&riq->bg_queue);
	spin_lock_init(&riq->bg_lock);
	init_waitqueue_head(&riq->blocked_waitq);

	riq->max_background = FUSE_DEFAULT_MAX_BACKGROUND;
	riq->congestion_threshold = FUSE_DEFAULT_CONGESTION_THRESHOLD;
	riq->num_background = 0;
	riq->active_background = 0;
	riq->blocked = 0;
	riq->num_sync_sleeping = 0;

	*riq_out = riq;
	return 0;

err_destroy:
	rfuse_iqueue_destroy_one(riq);
	return err;
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
	if (!fc->initialized)
		return true;
	if (!for_background)
		return false;

#if PAYLOAD_AWARE
	return false;
#else
	struct rfuse_iqueue *riq;

	riq = rfuse_get_specific_iqueue(fc, riq_id);
	return riq->blocked;
#endif
}

static bool rfuse_background_submission_available(struct rfuse_iqueue *riq)
{
  return true;	
  // return riq->active_background < riq->max_background;
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
		int node;
		unsigned int count;
		int err;

		err = rfuse_target_node(i, &node);
		if (err)
			continue;

		if (node < 0 || node >= MAX_NUMNODES)
			continue;

		count = fc->riq_node_count[node];
		if (count >= RFUSE_NUM_IQUEUE)
			continue;

		fc->riq_node_ids[node][count] = i;
		fc->riq_node_count[node] = count + 1;
	}
}

int rfuse_iqueue_init(struct fuse_conn *fc, void *priv)
{
	int i = 0;
	int err;
	struct rfuse_iqueue **riq;

	/* LDY: 실패 시 부분 초기화된 riq 배열을 남기지 않도록 포인터 배열도
	 * zero-init 해두고, 각 ring은 개별 unwind helper로 정리한다.
	 */
	riq = kcalloc(RFUSE_NUM_IQUEUE, sizeof(*riq), GFP_KERNEL);
	if (!riq)
		return -ENOMEM;

	for (i = 0; i < RFUSE_NUM_IQUEUE; i++) {
		err = rfuse_iqueue_alloc_one(&riq[i], i, priv);
		if (err)
			goto err_destroy;
	}

	fc->riq = riq;
	rfuse_init_iqueue_numa_candidates(fc);
	return 0;

err_destroy:
	while (--i >= 0)
		rfuse_iqueue_destroy_one(riq[i]);
	kfree(riq);
	fc->riq = NULL;
	return err;
}

void rfuse_iqueue_release(struct fuse_conn *fc)
{
	int i = 0;
	struct rfuse_iqueue **riq = fc->riq;

	if (!riq)
		return;

	pr_info("RFUSE_SCHED mode=%d local=%llu spread=%llu switches=%llu mask=0x%llx low_windows=%d\n",
		atomic_read(&fc->rfuse_sched_mode),
		atomic64_read(&fc->rfuse_sched_local_count),
		atomic64_read(&fc->rfuse_sched_spread_count),
		atomic64_read(&fc->rfuse_sched_switch_count),
		atomic64_read(&fc->rfuse_sched_mask),
		atomic_read(&fc->rfuse_sched_low_windows));

	for(i = 0; i < RFUSE_NUM_IQUEUE; i++) {
		rfuse_iqueue_destroy_one(riq[i]);
	}

	kfree(riq);
	fc->riq = NULL;
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
			ptr = fud->fc->riq[riq_id]->sbuf.kaddr;
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

static int select_numa_aware(struct fuse_conn *fc)
{
	int cpu_id = task_cpu(current);
	int start_idx;
	int numa_id;
	int offset;

	numa_id = find_numa_group_and_index(cpu_id, &start_idx);
	if (numa_id < 0)
		return cpu_id;

  for (;;) {
  	for (offset = 1; offset < NUM_NUMA_NODES; offset++) {
	  	int idx = (start_idx + offset) % NUM_NUMA_NODES;
		  int riq_id = numa_group[numa_id][idx];
  		struct rfuse_iqueue *riq = fc->riq[riq_id];

	  	if (READ_ONCE(riq->num_background) <= READ_ONCE(riq->congestion_threshold))
		  	return riq_id;
  	}
    cond_resched();
  }
}

static int select_numa_aware_v2(struct fuse_conn *fc,
				struct fuse_inode *fi, size_t len)
{
	int cpu_id = task_cpu(current);
	int start_idx;
	int numa_id;
	int offset;
  int i;	
  unsigned int requested_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	u64 nodeid = 0;

	numa_id = find_numa_group_and_index(cpu_id, &start_idx);
	if (numa_id < 0)
		return cpu_id;

	if (fi)
		nodeid = READ_ONCE(fi->nodeid);

	// for (i = 0; i < 3; i++) {
  for (;;) {
		int idx = start_idx;
		int riq_id = numa_group[numa_id][idx];
		struct rfuse_iqueue *riq = NULL;

		if (riq_id >= 0 && riq_id < RFUSE_NUM_IQUEUE)
			riq = fc->riq[riq_id];

		if (nodeid && riq) {
			u64 old = READ_ONCE(riq->stream_hint);

			/* LDY: riq별 stream_hint는 최근 file stream의 nodeid를 기억한다.
			 * 현재 CPU riq의 hint와 현재 요청 fi->nodeid가 다르면 short-flow의
			 * 첫 요청으로 보고 local wake latency를 줄이기 위해 current riq를
			 * 선택한다. 같으면 streaming 요청으로 보고 app/daemon runqueue 경쟁을
			 * 피하기 위해 current riq를 제외한 기존 remote 선택 경로로 보낸다.
			 */
			if (old != nodeid) {
				WRITE_ONCE(riq->stream_hint, nodeid);
				return riq_id;
			}
		}

		for (offset = 1; offset < NUM_NUMA_NODES; offset++) {
			idx = (start_idx + offset) % NUM_NUMA_NODES;
			riq_id = numa_group[numa_id][idx];
			if (riq_id < 0 || riq_id >= RFUSE_NUM_IQUEUE)
				continue;

			riq = fc->riq[riq_id];
			if (!riq)
				continue;

			if ((READ_ONCE(riq->num_background) <= READ_ONCE(riq->congestion_threshold)) ||
			    (READ_ONCE(riq->sbuf_max_free_hint) >= requested_pages))
				return riq_id;
		}
		cond_resched();
	}

	return cpu_id;
}

struct rfuse_iqueue *rfuse_get_iqueue_for_async(struct fuse_conn *fc,
						struct fuse_inode *fi,
						size_t sbuf_len)
{
	int id;

	id = select_numa_aware_v2(fc, fi, sbuf_len);

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
			spin_unlock(&riq->lock);
			wait_event_interruptible(riq->waitq, !READ_ONCE(riq->reqbm.full));
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
		wake_up_all(&riq->waitq);
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
			spin_unlock(&riq->lock);
			wait_event_interruptible(riq->waitq,!READ_ONCE(riq->argbm.full));
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
		wake_up_all(&riq->waitq);
	}
	spin_unlock(&riq->lock);
}

static void rfuse_request_free(struct rfuse_req *req){
	bool in_sbuf = req->sbuf_capacity &&
			  (req->sbuf_flags & RFUSE_PAYLOAD_IN);
	bool out_sbuf = req->sbuf_capacity &&
			   (req->sbuf_flags & RFUSE_PAYLOAD_OUT);

	rfuse_release_sbuf(req);

	if(req->in.arglen[0] != 0 && !req->in_pages && !in_sbuf)
		rfuse_put_argument_buffer(req->fm, req->in.arg[0], req->riq_id);
	if(req->in.arglen[1] != 0 && !req->in_pages && !in_sbuf)
		rfuse_put_argument_buffer(req->fm, req->in.arg[1], req->riq_id);
	if(req->out.arglen != 0 && !req->out_pages && !out_sbuf)
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

static struct rfuse_req *rfuse_request_alloc(struct fuse_mount *fm,
					     size_t sbuf_len,
					     struct fuse_inode *fi){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_iqueue(fc);
	int riq_id = riq->riq_id;
	struct rfuse_req *r_req = NULL;
	uint32_t req_index;

	(void)sbuf_len;
	(void)fi;

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
				bool force, size_t sbuf_len,
				struct fuse_inode *fi){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_req *r_req;
	struct rfuse_iqueue *riq;
	int err;

	if(force) {
		atomic_inc(&fc->num_waiting);

		r_req = rfuse_request_alloc(fm, sbuf_len, fi);
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

		r_req = rfuse_request_alloc(fm, sbuf_len, fi);
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
			if (wait_event_killable_exclusive(riq->blocked_waitq, !rfuse_block_alloc(fc, for_background, r_req->riq_id))) {
				goto out;
			}
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

static struct rfuse_req *try_rfuse_request_alloc_on_riq(struct fuse_mount *fm,
							int riq_id,
							spinlock_t *file_lock){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, riq_id);
	struct rfuse_req *r_req = NULL;
	uint32_t req_index;

	do {
		req_index = try_rfuse_get_request_buffer(fm, riq_id); // Get a new index
		if(req_index == -1) {
			if(file_lock)
				spin_unlock(file_lock);
			wait_event_interruptible(riq->waitq, !READ_ONCE(riq->reqbm.full));
			if(file_lock)
				spin_lock(file_lock);
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

static struct rfuse_req *try_rfuse_request_alloc(struct fuse_mount *fm,
						 size_t sbuf_len,
						 struct fuse_inode *fi,
						 spinlock_t *file_lock){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_iqueue_for_async(fc, fi, sbuf_len);

	return try_rfuse_request_alloc_on_riq(fm, riq->riq_id, file_lock);
}

struct rfuse_req *try_rfuse_get_req(struct fuse_mount *fm,
				    bool for_background, bool force,
				    size_t sbuf_len, struct fuse_inode *fi,
				    spinlock_t *file_lock){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_req *r_req;
	struct rfuse_iqueue *riq;
	int err;

	if(force) {
		atomic_inc(&fc->num_waiting);

		r_req = try_rfuse_request_alloc(fm, sbuf_len, fi, file_lock);
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

		r_req = try_rfuse_request_alloc(fm, sbuf_len, fi, file_lock);
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
			if (wait_event_killable_exclusive(riq->blocked_waitq, !rfuse_block_alloc(fc, for_background, r_req->riq_id))) {
				goto out;
			}
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

struct rfuse_req *try_rfuse_get_req_on_riq(struct fuse_mount *fm,
					   bool for_background, bool force,
					   int riq_id, spinlock_t *file_lock){
	struct fuse_conn *fc = fm->fc;
	struct rfuse_req *r_req;
	struct rfuse_iqueue *riq;
	int err;

	if(force) {
		atomic_inc(&fc->num_waiting);

		r_req = try_rfuse_request_alloc_on_riq(fm, riq_id, file_lock);
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

		r_req = try_rfuse_request_alloc_on_riq(fm, riq_id, file_lock);
		err = -ENOMEM;
		if (!r_req) {
			if (for_background)
				wake_up(&fc->blocked_waitq);
			goto out;
		}

		if(rfuse_block_alloc(fc, for_background, r_req->riq_id)){
			err = -EINTR;
			riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
			if (wait_event_killable_exclusive(riq->blocked_waitq,
					!rfuse_block_alloc(fc, for_background,
							    r_req->riq_id))) {
				goto out;
			}
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

static int rfuse_queue_request(struct rfuse_req *r_req)
{
	struct fuse_mount *fm = r_req->fm;
	struct fuse_conn *fc = fm->fc;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);
	struct rfuse_address_entry *entry;

	if(test_bit(FR_BACKGROUND, &r_req->flags)){
		__set_bit(FR_ASYNC,&r_req->flags);
	}

	spin_lock(&riq->lock);						// set lock
	entry = rfuse_read_pending_tail(riq);		// Get an entry
	if (unlikely(!entry)) {
		spin_unlock(&riq->lock);
		return -EAGAIN;
	}
	r_req->in.unique = rfuse_get_unique(riq); 
	entry->request = r_req->index;				// fill entry
	if(!test_bit(FR_BACKGROUND, &r_req->flags)) // only increase the reference count for synchronous requests
		__rfuse_get_request(r_req);
	rfuse_submit_pending_tail(riq);				// Commit entry
	spin_unlock(&riq->lock);					// unlock
		if(waitqueue_active(&riq->idle_user_waitq)){
			wake_up(&riq->idle_user_waitq);		// Wake up idle user thread
	}
	return 0;
}

// PENDING QUEUE INSERT
ssize_t rfuse_simple_request(struct rfuse_req *r_req){
	ssize_t ret=0;
	int err;

	err = rfuse_queue_request(r_req);
	if (err)
		return err;
	rfuse_request_wait_answer(r_req);
	smp_rmb();

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
		int err;

		bg_entry = list_first_entry(queue, struct rfuse_bg_entry, list);
		list_del(&bg_entry->list);
		riq = rfuse_get_specific_iqueue(fc, bg_entry->riq_id);
		r_req = (struct rfuse_req *)&riq->kreq[bg_entry->request];
		err = rfuse_queue_request(r_req);
		kfree(bg_entry);
		if (unlikely(err)) {
			r_req->out.error = err;
			rfuse_request_end(r_req);
		}
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
	struct rfuse_bg_entry *bg_entry;
	LIST_HEAD(to_queue);
	bool queued = false;
	int err = -ENOTCONN;
	struct rfuse_iqueue *riq = rfuse_get_specific_iqueue(fc, r_req->riq_id);

	bg_entry = kmalloc_node(sizeof(struct rfuse_bg_entry),
				GFP_KERNEL,
				cpu_to_node(r_req->riq_id));

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
		if (riq->num_background == riq->max_background)
			riq->blocked = 1;
		if (riq->num_background == riq->congestion_threshold && fm->sb) {
			set_bdi_congested(fm->sb->s_bdi, BLK_RW_SYNC);
			set_bdi_congested(fm->sb->s_bdi, BLK_RW_ASYNC);
		}

		list_add_tail(&bg_entry->list, &riq->bg_queue); // Add it to background queue
		rfuse_flush_bg_queue_locked(riq, &to_queue);
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
		if (riq->num_background == riq->max_background) {
			riq->blocked = 0;
			wake_up(&riq->blocked_waitq);
		} else if (!riq->blocked) {
			/*
			 * Wake the next blocked submitter, if any.  The wake
			 * above already synchronizes riq->blocked updates with
			 * sleepers, so waitqueue_active() is sufficient here.
			 */
			if (waitqueue_active(&riq->blocked_waitq))
				wake_up(&riq->blocked_waitq);
		}
		if (riq->num_background == riq->congestion_threshold && fm->sb) {
			clear_bdi_congested(fm->sb->s_bdi, BLK_RW_SYNC);
			clear_bdi_congested(fm->sb->s_bdi, BLK_RW_ASYNC);
		}
		riq->num_background--;
		riq->active_background--;
		rfuse_flush_bg_queue_locked(riq, &to_queue);
		spin_unlock(&riq->bg_lock);

		rfuse_queue_bg_list(fc, &to_queue);
	}

	{
		int err;

		err = rfuse_import_sbuf(r_req);
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
	if (unlikely(!rp || !rp->pages || !rp->descs))
		return -EIO;
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
	if (unlikely(!r_req->rp || !r_req->rp->pages || !r_req->rp->descs))
		return -EIO;

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
		WRITE_ONCE(riq[i]->stream_hint, 0);

		spin_lock(&riq[i]->bg_lock);
		rfuse_flush_bg_queue_locked(riq[i], &to_queue);
		spin_unlock(&riq[i]->bg_lock);

		spin_unlock(&riq[i]->lock);
		rfuse_queue_bg_list(fc, &to_queue);

		wake_up_all(&riq[i]->waitq);
		wake_up_all(&riq[i]->idle_user_waitq);
		wake_up_all(&riq[i]->blocked_waitq);
	}
}
EXPORT_SYMBOL_GPL(rfuse_abort_conn);
