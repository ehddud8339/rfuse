#include "fuse_i.h"
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>

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
			if (r_req->in.opcode == FUSE_READ || r_req->in.opcode == FUSE_WRITE) {
				pr_info("rfuse-lat stage=reply_signal_recv ts=%llu riq=%d req=%u unique=%llu opcode=%u src=comp_poll\n",
					ktime_get_ns(), r_req->riq_id, r_req->index,
					r_req->in.unique, r_req->in.opcode);
			}
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
