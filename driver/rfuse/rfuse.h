#ifndef _FS_RFUSE_H
#define _FS_RFUSE_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/fuse.h>
#include <linux/mount.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/workqueue.h>
 
#include "rfuse_comp.h"

#define RFUSE_NUM_IQUEUE     40           // Number of rfuse iqueue
#define RFUSE_MAX_QUEUE_SIZE 1024*4      // Maximum number of requests in a queue
#define RFUSE_WORKER_PER_RING 2

#define RFUSE_SCHED_WINDOW_NS      1000000ULL
#define RFUSE_SCHED_SPREAD_ON      4
#define RFUSE_SCHED_LOCAL_ON       1
#define RFUSE_SCHED_LOCAL_WINDOWS  3

enum rfuse_sched_mode {
	RFUSE_SCHED_LOCAL = 0,
	RFUSE_SCHED_SPREAD = 1,
};

#define RFUSE_RIQ_ID_MASK    0x00ff0000ULL
#define RFUSE_QUEUE_MAP_MASK 0xff000000ULL
#define RFUSE_REQ_IDX_MASK   0x0000ffff00000000ULL

#define RFUSE_IQUEUE         0ULL
#define RFUSE_PENDING        0x08000000ULL
#define RFUSE_INTERRUPT      0x18000000ULL
#define RFUSE_FORGET         0x20000000ULL
#define RFUSE_COMPLETE       0x28000000ULL
#define RFUSE_ARG	     0x30000000ULL
#define RFUSE_REQ	     0x38000000ULL
#define RFUSE_READ	     0x40000000ULL
#define RFUSE_WRITE	     0x48000000ULL
#define RFUSE_PAYLOAD       0x50000000ULL

#define RFUSE_PAYLOAD_IN        (1U << 0)
#define RFUSE_PAYLOAD_OUT       (1U << 1)

struct fuse_file;
struct rfuse_pages;

struct rfuse_async_write_range {
	struct rb_node node;
	loff_t start;
	loff_t last;
	loff_t subtree_last;
};

struct rfuse_async_wrt_ctx {
	struct rfuse_async_write_range range;
	struct inode *inode;
	struct fuse_file *ff;
	size_t count;
	bool range_registered;
};

struct rfuse_req{
	/** Request input header **/
	struct{
		uint64_t    unique;
		uint64_t    nodeid;
		uint32_t    opcode;
		uint32_t    uid;
		uint32_t    gid;
		uint32_t    pid;
		uint32_t	arg[2];	    // Location of in operation-specific argument
		uint32_t	arglen[2];	// Size of in operation-specific argument
	}in; // 48 

	/** Request output header **/
	struct{
		int32_t     error;
		uint32_t	arg;	// Location of out operation-specific argument
		uint32_t	arglen;	// Size of out operation-specific argument
		uint32_t	padding;	
	}out; // 16

	/** request buffer index **/
	uint32_t index; // 4
	int32_t riq_id;
	/** Request flags, updated with test/set/clear_bit() **/
	unsigned long flags; // 8

	/** fuse_mount this request belongs to **/
	struct fuse_mount *fm; // 8
	/** refcount **/
	refcount_t count; // 4
	/** Used to wake up the task waiting for completion of request **/
	wait_queue_head_t waitq; // 24

	struct{
		uint8_t argument_space[112];
	}args; // 112

	uint32_t sbuf_page_index;
	uint32_t sbuf_page_count;
	uint32_t sbuf_offset;
	uint32_t sbuf_len;
	uint32_t sbuf_capacity;
	uint32_t sbuf_flags;

	bool force:1;
	bool noreply:1;
	bool nocreds:1;
	bool in_pages:1;
	bool out_pages:1;
	bool out_argvar:1;
	bool page_zeroing:1;
	bool page_replace:1;
	bool may_block:1;
	bool sbuf_reserved:1;

	struct rfuse_pages *rp;
	struct rfuse_async_wrt_ctx wrt_ctx;
	bool has_wrt_ctx;
	void (*end)(struct fuse_mount *fm, struct rfuse_req *r_req, int error);

	/* LDY: librfuse write data-copy instrumentation ABI extension.
	 * rfuse_do_write() records fallback temp-buffer path and pread()
	 * latency so the kernel path_lat_dump interface can aggregate it.
	 * These fields are instrumentation-only and do not affect request
	 * semantics.
	 */
	uint64_t usr_write_tempbuf_cnt;
	uint64_t usr_write_tempbuf_ns;
	uint64_t usr_write_fbuf_if_min_ns;
	uint64_t usr_write_fbuf_if_max_ns;
	uint64_t usr_write_pread_cnt;
	uint64_t usr_write_pread_ns;
	uint64_t usr_write_pread_bytes;
	uint64_t usr_write_pread_err_cnt;

	/* LDY: kernel/user 경계를 넘는 READ/WRITE dequeue 및 write path
	 * 단계별 latency 측정을 위한 timestamp/delta 전달용 필드.
	 * request semantics에는 영향을 주지 않는다.
	 */
	uint64_t ldy_ts_prepare_submit_start_ns;
	uint64_t ldy_ts_enqueue_ns;
	uint64_t ldy_ts_backend_write_start_ns;
	uint64_t ldy_ts_reply_comp_start_ns;
	uint64_t ldy_lat_enqueue_to_dequeue_ns;
	uint64_t ldy_lat_backend_write_ns;
};


struct rfuse_interrupt_entry{
	uint64_t    unique;
};

struct rfuse_forget_entry{
	uint64_t	unique;
	uint64_t    nlookup;
	uint64_t    nodeid;
	uint64_t	padding;
};

struct rfuse_address_entry{ 
	uint32_t request;
}; // 4 bytes

struct rfuse_bg_entry{
	struct list_head list;
	uint32_t request;
	int32_t riq_id;
	uint64_t ldy_prepare_submit_start_ns;
};

// Pending queue, Complete Queue
struct ring_buffer_1{
	uint32_t tail;
	uint32_t head;
	uint32_t mask;
	uint32_t entries;

	struct rfuse_address_entry *kaddr; // kernel address
	struct rfuse_address_entry *uaddr; // user address
};

// Interrupt queue
struct ring_buffer_2{
	uint32_t head;
	uint32_t tail;
	uint32_t mask;
	uint32_t entries;

	struct rfuse_interrupt_entry *kaddr; // kernel address
	struct rfuse_interrupt_entry *uaddr; // user address
};

// Forget queue
struct ring_buffer_3{
	uint32_t head;
	uint32_t tail;
	uint32_t mask;
	uint32_t entries;

	struct rfuse_forget_entry *kaddr; // kernel address
	struct rfuse_forget_entry *uaddr; // user address
};

struct rfuse_arg{
	uint8_t garbage[256];
};

struct rfuse_sbuf_map {
	void *uaddr;
	void *kaddr;
	uint32_t size;
	uint32_t used;
};

/**
  mmap the total rfuse_iqueue to fuse daemon
 **/ 
struct rfuse_iqueue{
	int riq_id;
	/** Pending queue **/
	struct ring_buffer_1 pending;
	/** Interrupt queue **/
	struct ring_buffer_2 interrupts;
	/** Forget queue **/
	struct ring_buffer_3 forgets;
	/** Complete queue **/
	struct ring_buffer_1 completes;

	/** Dyanmic argument buffer **/
	struct rfuse_arg *uarg; // user address
	struct rfuse_arg *karg; // kernel address

	/** Dynamic request buffer **/
	struct rfuse_req *ureq;	// user address
	struct rfuse_req *kreq; // kernel address

	/** Shared sbuf buffer **/
	struct rfuse_sbuf_map sbuf;

	/** Connection established **/
	unsigned connected;

	/** wait queue for requests to wait to receive a request buffer **/
	wait_queue_head_t waitq;

	/** Lock protecting accesses to members of this structure **/
	spinlock_t lock;

	/** The next unique request id **/
	u64 reqctr;

	/** Device specific state */
	void *priv;

	struct {
		unsigned long bitmap_size;
		unsigned full;
		unsigned long *bitmap;
	}argbm;

	struct {
		unsigned long bitmap_size;
		unsigned full;
		unsigned long *bitmap;
	}reqbm;

	wait_queue_head_t idle_user_waitq;

	/** synchronous request congestion control */
	int num_sync_sleeping;

	/** background request congestion control */
	struct list_head bg_queue; 
	spinlock_t bg_lock;

	unsigned max_background;
	unsigned congestion_threshold;
	unsigned num_background;
	unsigned active_background;
	int blocked;

	/** waitq for congested asynchronous requests*/
	wait_queue_head_t blocked_waitq;

	/*
	 * Shared sbuf allocator state, protected by sbuf_lock.
	 * sbuf_bitmap is the allocator source of truth.
	 * sbuf_free_pages is a cached aggregate counter for fast
	 * rejection/debugging only, and sbuf_search_hint is advisory only.
	 */
	spinlock_t sbuf_lock;
	wait_queue_head_t sbuf_waitq;
	unsigned long *sbuf_bitmap;
	uint32_t sbuf_page_count;
	uint32_t sbuf_free_pages;
	uint32_t sbuf_search_hint;
	/*
	 * LDY: sbuf allocator의 next-fit search cursor와 별개인 metadata.
	 * 현재 riq sbuf pool에서 가능한 최대 contiguous free run의
	 * advisory upper bound를 "page" 단위로 유지한다.
	 */
	uint32_t sbuf_max_free_hint;
	/* LDY: riq별 최근 file stream을 구분하기 위한 hint.
	 * 같은 fuse_inode에서 나온 request가 반복되면 streaming 요청으로 보고,
	 * 이후 scheduler에서 current CPU 제외 여부를 판단하는 데 사용한다.
	 * shared riq ABI에 노출되므로 kernel pointer 대신 nodeid를 저장하며,
	 * scheduling hint이므로 strict correctness state가 아니다.
	 */
	u64 stream_hint;
};

static_assert(sizeof(struct rfuse_iqueue) == 456);
static_assert(offsetof(struct rfuse_iqueue, connected) == 192);
static_assert(offsetof(struct rfuse_iqueue, sbuf_bitmap) == 424);
static_assert(offsetof(struct rfuse_iqueue, sbuf_page_count) == 432);
static_assert(offsetof(struct rfuse_iqueue, sbuf_max_free_hint) == 444);
static_assert(offsetof(struct rfuse_iqueue, stream_hint) == 448);

#endif
