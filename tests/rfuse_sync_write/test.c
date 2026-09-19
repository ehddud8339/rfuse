/* 커널 함수를 복제하지 않고 실제 소스의 sync 구간을 실행한다.
 * 모의 page/filemap은 각 단계의 잠금 및 공개 상태를 검증한다.
 * 실제 커널의 VM 경합 검증은 별도 mount 테스트가 필요하다.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define PAGE_SHIFT 12
#define PAGE_SIZE (1U << PAGE_SHIFT)
#define GFP_KERNEL 0
#define FUSE_WRITE 16
#define RFUSE_PAYLOAD_IN 1
#define FUSE_WRITE_KILL_SUIDGID 1
#define CAP_FSETID 1
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
typedef unsigned long pgoff_t;
struct inode { int unused; };
struct address_space { struct inode *host; bool locked; };
struct page {
	struct address_space *mapping;
	bool uptodate, locked, mapped, dirty, present;
	int refs;
	loff_t offset;
	char data[PAGE_SIZE];
};
struct rfuse_iqueue { struct { void *kaddr; } sbuf; };
struct fuse_conn { bool handle_killpriv_v2; struct rfuse_iqueue *riq[1]; };
struct fuse_mount { struct fuse_conn *fc; };
struct fuse_file { struct fuse_mount *fm; int fh, nodeid; };
struct file { struct address_space *f_mapping; struct fuse_file *private_data; };
struct kiocb { struct file *ki_filp; };
struct iov_iter { size_t consumed; };
struct fuse_write_in { size_t fh, offset, flags, write_flags, size; };
struct fuse_write_out { size_t size; };
struct rfuse_req {
	union { struct fuse_write_in in; struct fuse_write_out out; } args;
	struct { int opcode, nodeid; size_t arglen[1]; } in;
	int riq_id;
	size_t sbuf_offset;
};
static struct inode inode;
static struct address_space mapping = { .host = &inode };
static struct page pages[3];
static char payload[3 * PAGE_SIZE];
static struct rfuse_iqueue queue = { .sbuf.kaddr = payload };
static struct fuse_conn conn = { .riq = { &queue } };
static struct fuse_mount mount = { .fc = &conn };
static struct fuse_file ff = { .fm = &mount };
static struct file file = { .f_mapping = &mapping, .private_data = &ff };
static struct kiocb iocb = { .ki_filp = &file };
static int alloc_error, reserve_error, copy_error, flush_error, reply_error;
static size_t reply_size, copy_limit;
static int requests, request_puts;
static bool cleared_before_unmap;
static void *kvcalloc(size_t n, size_t size, int flags)
{ return alloc_error ? NULL : calloc(n, size); }
static void kvfree(void *p) { free(p); }
static void filemap_invalidate_lock(struct address_space *m)
{ assert(!m->locked); m->locked = true; }
static void filemap_invalidate_unlock(struct address_space *m)
{ assert(m->locked); m->locked = false; }
static struct page *find_lock_page(struct address_space *m, pgoff_t index)
{
	assert(m->locked && index < 3);
	struct page *p = &pages[index];
	if (!p->present) return NULL;
	assert(!p->locked); p->locked = true; p->refs++;
	return p;
}
#define PageUptodate(p) ((p)->uptodate)
#define ClearPageUptodate(p) ((p)->uptodate = false)
#define SetPageUptodate(p) ((p)->uptodate = true)
static void lock_page(struct page *p) { assert(!p->locked); p->locked = true; }
static void unlock_page(struct page *p) { assert(p->locked); p->locked = false; }
static void put_page(struct page *p) { assert(p->refs > 0); p->refs--; }
static loff_t page_offset(struct page *p) { return p->offset; }
static void *kmap_atomic(struct page *p) { assert(p->locked); return p->data; }
static void kunmap_atomic(void *p) {}
static void flush_dcache_page(struct page *p) {}
static void rfuse_wait_on_page_writeback(struct inode *i, pgoff_t index) {}
static void unmap_mapping_range(struct address_space *m, loff_t start,
				loff_t len, int even_cows)
{
	assert(m->locked && !even_cows);
	for (int i = 0; i < 3; i++) {
		struct page *p = &pages[i];
		if (!p->present || p->offset < start || p->offset >= start + len) continue;
		assert(p->locked && !p->uptodate);
		p->mapped = false;
	}
	cleared_before_unmap = true;
}
static int filemap_write_and_wait_range(struct address_space *m, loff_t a, loff_t b)
{
	assert(m->locked && cleared_before_unmap);
	for (int i = a >> PAGE_SHIFT; i <= b >> PAGE_SHIFT; i++) {
		if (!pages[i].present) continue;
		assert(!pages[i].locked && !pages[i].uptodate && !pages[i].mapped);
		pages[i].dirty = false;
	}
	return flush_error;
}
static unsigned int rfuse_write_flags(struct kiocb *i) { return 0; }
static bool capable(int cap) { return true; }
static int rfuse_reserve_sbuf(struct rfuse_req *r, size_t n, int flags, bool wait)
{ return reserve_error; }
static ssize_t rfuse_sbuf_copy_from_iter(struct rfuse_req *r, struct iov_iter *it, size_t n)
{
	assert(!mapping.locked); /* mmap 사용자 버퍼 fault가 차단 잠금보다 먼저다. */
	if (copy_error) return copy_error;
	n = min_t(size_t, n, copy_limit);
	it->consumed += n;
	return n;
}
static int rfuse_simple_request(struct rfuse_req *r)
{
	assert(mapping.locked);
	loff_t pos = r->args.in.offset;
	size_t n = r->args.in.size;
	for (int i = pos >> PAGE_SHIFT; i <= (pos + n - 1) >> PAGE_SHIFT; i++) {
		if (!pages[i].present) continue;
		assert(pages[i].locked && !pages[i].uptodate && !pages[i].mapped);
		assert(!pages[i].dirty);
		for (size_t j = 0; j < PAGE_SIZE; j++) assert(pages[i].data[j] == 'A');
	}
	requests++;
	r->args.out.size = reply_size;
	return reply_error;
}
static void iov_iter_revert(struct iov_iter *it, size_t n)
{ assert(it->consumed >= n); it->consumed -= n; }
static void rfuse_put_request(struct rfuse_req *r)
{
	assert(!mapping.locked);
	for (int i = 0; i < 3; i++) assert(!pages[i].locked && !pages[i].refs);
	request_puts++;
}
#include "sync_impl.h"
static void reset(void)
{
	memset(pages, 0, sizeof(pages));
	for (int i = 0; i < 3; i++) {
		pages[i].mapping = &mapping;
		pages[i].offset = i * PAGE_SIZE;
		pages[i].present = pages[i].uptodate = pages[i].mapped = true;
		memset(pages[i].data, 'A', PAGE_SIZE);
	}
	memset(payload, 'B', sizeof(payload));
	alloc_error = reserve_error = copy_error = flush_error = reply_error = 0;
	copy_limit = sizeof(payload);
	requests = request_puts = 0;
	cleared_before_unmap = false;
}
static void run(loff_t pos, size_t count, size_t expected, int expected_error)
{
	struct rfuse_req req = {};
	struct iov_iter it = {};
	size_t written = 999;
	int err = rfuse_send_write_sync(&iocb, &req, &it, pos, count, &written);
	assert(err == expected_error && written == expected);
	assert(it.consumed == expected && request_puts == 1 && !mapping.locked);
}
int main(void)
{
	reset(); reply_size = 2 * PAGE_SIZE; pages[0].dirty = true;
	run(1024, reply_size, reply_size, 0);
	assert(requests == 1);
	for (size_t i = 0; i < sizeof(payload); i++)
		assert(pages[i / PAGE_SIZE].data[i % PAGE_SIZE] ==
		       (i >= 1024 && i < 1024 + reply_size ? 'B' : 'A'));
	reset(); reply_size = 3000;
	run(1024, 7000, 3000, 0);
	assert(pages[0].data[4023] == 'B' && pages[0].data[4024] == 'A');
	assert(pages[1].uptodate && pages[1].data[0] == 'A');
	reset(); reply_size = 0; run(1024, 7000, 0, 0);
	assert(pages[0].uptodate && pages[1].uptodate);
	reset(); reply_error = -EIO; reply_size = 9999;
	run(1024, 7000, 0, -EIO);
	assert(!pages[0].uptodate && !pages[1].uptodate);
	reset(); reply_size = 7001; run(1024, 7000, 0, -EIO);
	assert(!pages[0].uptodate);
	reset(); flush_error = -EIO; run(1024, 7000, 0, -EIO);
	assert(!requests && pages[0].uptodate);
	reset(); alloc_error = 1; run(1024, 7000, 0, -ENOMEM);
	assert(!requests && pages[0].uptodate && pages[0].mapped);
	reset(); reserve_error = -E2BIG; run(1024, 7000, 0, -E2BIG);
	assert(!requests);
	reset(); copy_error = -EFAULT; run(1024, 7000, 0, -EFAULT);
	assert(!requests);
	reset(); copy_limit = reply_size = 1000; run(1024, 7000, 1000, 0);
	reset(); pages[0].uptodate = false; reply_size = 1000;
	run(1024, 1000, 1000, 0); assert(!pages[0].uptodate);
	reset(); pages[0].uptodate = false; reply_size = PAGE_SIZE;
	run(0, PAGE_SIZE, PAGE_SIZE, 0); assert(pages[0].uptodate);
	reset(); pages[0].present = pages[1].present = false; reply_size = 7000;
	run(1024, 7000, 7000, 0); assert(requests == 1);
	printf("PASS: 13 sync sbuf scenarios (blocking state, commit, short/error, lifetime)\n");
	return 0;
}
