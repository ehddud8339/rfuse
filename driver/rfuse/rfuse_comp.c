#include "fuse_i.h"
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#define RFUSE_READ_COMP_POLL_NS (50 * 1000)

static bool rfuse_read_comp_available(struct rfuse_iqueue *riq)
{
	struct ring_buffer_1 *completes = &riq->completes;

	return completes->head < smp_load_acquire(&completes->tail);
}

static void rfuse_read_comp_process_entry(struct rfuse_iqueue *riq,
					  struct rfuse_address_entry *entry)
{
	struct rfuse_req *r_req;
	size_t payload_len;

	r_req = &riq->kreq[entry->request];
	payload_len = r_req->payload_len;

	if (payload_len && payload_len <= RFUSE_SLOT_SIZE &&
	    r_req->payload_index < RFUSE_SLOT_COUNT) {
		rfuse_copy_slot_to_pages(riq, r_req, payload_len);
		rfuse_payload_slot_release(riq, r_req->payload_index);
	}

	rfuse_request_end(r_req);
	return;
}

int rfuse_read_comp_kthread(void *data)
{
	struct rfuse_iqueue *riq = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(riq->read_comp_waitq,
			kthread_should_stop() ||
			atomic_read(&riq->read_comp_pending) ||
			rfuse_read_comp_available(riq));

		if (kthread_should_stop())
			break;

		atomic_set(&riq->read_comp_pending, 0);

		if (rfuse_read_comp_available(riq)) {
			u64 deadline = ktime_get_ns() + RFUSE_READ_COMP_POLL_NS;

			while (ktime_get_ns() < deadline) {
				struct rfuse_address_entry *entry;

				while ((entry = rfuse_read_complete_head(riq))) {
					rfuse_extract_complete_head(riq);
					rfuse_read_comp_process_entry(riq, entry);
				}

				if (!rfuse_read_comp_available(riq))
					cpu_relax();
			}
		}
	}

	return 0;
}

void rfuse_read_comp_signal(struct rfuse_iqueue *riq)
{
	atomic_set(&riq->read_comp_pending, 1);
	wake_up_interruptible(&riq->read_comp_waitq);
}

void rfuse_sleep_comp(struct fuse_conn *fc, struct rfuse_iqueue *riq, struct rfuse_req *r_req) {
	spin_lock(&r_req->waitq.lock);
	set_bit(FR_NEEDWAKEUP, &r_req->flags);
	spin_unlock(&r_req->waitq.lock);

	spin_lock(&riq->lock);
	riq->num_sync_sleeping++;
	spin_unlock(&riq->lock);

	wait_event_interruptible(r_req->waitq, !fc->connected || test_bit(FR_FINISHED, &r_req->flags));

	spin_lock(&riq->lock);
	riq->num_sync_sleeping--;
	spin_unlock(&riq->lock);
}

int rfuse_completion_poll(struct fuse_conn *fc, struct rfuse_iqueue *riq, struct rfuse_req *r_req)
{   
	unsigned long max_idle_due = jiffies + usecs_to_jiffies(RFUSE_COMP_MAX_IDLE);
	
	while(fc->connected) {
		if(test_bit(FR_FINISHED, &r_req->flags)){
			rfuse_request_end(r_req);
			return 0;
		}

		 if(time_after(jiffies, max_idle_due)){
	 		rfuse_sleep_comp(fc, riq, r_req);
		 }

		schedule();
	}

	return -ENOTCONN;
}
