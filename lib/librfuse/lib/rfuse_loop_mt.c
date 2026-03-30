#define _GNU_SOURCE

#include "config.h"
#include "fuse_lowlevel.h"
#include "fuse_i.h"
#include "fuse_misc.h"
#include "fuse_kernel.h"
#include "rfuse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <stdatomic.h>
#include <sched.h>

/* Environment var controlling the thread stack size */
#define ENVNAME_THREAD_STACK "FUSE_THREAD_STACK"

#if LDY_LOG
static void rfuse_sleep_stat_init(struct rfuse_daemon_sleep_stat *stat)
{
	stat->count = 0;
	stat->total_ns = 0;
	stat->min_ns = UINT64_MAX;
	stat->max_ns = 0;
}

static void rfuse_sleep_ctx_init(struct rfuse_daemon_sleep_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	rfuse_sleep_stat_init(&ctx->sleep_block_ns);
	rfuse_sleep_stat_init(&ctx->wake_to_read_ns);
	rfuse_sleep_stat_init(&ctx->wake_pending_depth);
	rfuse_sleep_stat_init(&ctx->dequeue_burst_len);
}

static uint64_t rfuse_sleep_stat_min(const struct rfuse_daemon_sleep_stat *stat)
{
	if (stat->count == 0 || stat->min_ns == UINT64_MAX)
		return 0;

	return stat->min_ns;
}

static uint64_t rfuse_sleep_stat_avg(const struct rfuse_daemon_sleep_stat *stat)
{
	if (stat->count == 0)
		return 0;

	return stat->total_ns / stat->count;
}

static void rfuse_sleep_ctx_dump(const struct rfuse_mt *mt)
{
	const struct rfuse_daemon_sleep_ctx *ctx = &mt->sleep_ctx;
	uint64_t blocked_pct = 0;
	uint64_t pending_seen_pct = 0;
	uint64_t wake_with_backlog_pct = 0;

	if (ctx->sleep_calls == 0)
		return;

	blocked_pct = (ctx->sleep_blocked_calls * 100) / ctx->sleep_calls;
	pending_seen_pct = (ctx->pending_seen_before_sleep * 100) / ctx->sleep_calls;
	if (ctx->wake_pending_depth.count)
		wake_with_backlog_pct = (ctx->wake_with_backlog_calls * 100) / ctx->wake_pending_depth.count;

	fuse_log(FUSE_LOG_INFO,
		"rfuse_sleep summary riq=%d sleep_calls=%llu sleep_blocked_calls=%llu blocked_pct=%llu pending_seen_before_sleep=%llu pending_seen_pct=%llu forget_miss_before_sleep=%llu sleep_avg_ns=%llu sleep_min_ns=%llu sleep_max_ns=%llu wake_to_read_avg_ns=%llu wake_to_read_min_ns=%llu wake_to_read_max_ns=%llu wake_pending_avg=%llu wake_pending_min=%llu wake_pending_max=%llu wake_with_backlog_calls=%llu wake_with_backlog_pct=%llu burst_avg=%llu burst_min=%llu burst_max=%llu burst_reqs_total=%llu\n",
		mt->riq_id,
		(unsigned long long)ctx->sleep_calls,
		(unsigned long long)ctx->sleep_blocked_calls,
		(unsigned long long)blocked_pct,
		(unsigned long long)ctx->pending_seen_before_sleep,
		(unsigned long long)pending_seen_pct,
		(unsigned long long)ctx->forget_miss_before_sleep,
		(unsigned long long)rfuse_sleep_stat_avg(&ctx->sleep_block_ns),
		(unsigned long long)rfuse_sleep_stat_min(&ctx->sleep_block_ns),
		(unsigned long long)ctx->sleep_block_ns.max_ns,
		(unsigned long long)rfuse_sleep_stat_avg(&ctx->wake_to_read_ns),
		(unsigned long long)rfuse_sleep_stat_min(&ctx->wake_to_read_ns),
		(unsigned long long)ctx->wake_to_read_ns.max_ns,
		(unsigned long long)rfuse_sleep_stat_avg(&ctx->wake_pending_depth),
		(unsigned long long)rfuse_sleep_stat_min(&ctx->wake_pending_depth),
		(unsigned long long)ctx->wake_pending_depth.max_ns,
		(unsigned long long)ctx->wake_with_backlog_calls,
		(unsigned long long)wake_with_backlog_pct,
		(unsigned long long)rfuse_sleep_stat_avg(&ctx->dequeue_burst_len),
		(unsigned long long)rfuse_sleep_stat_min(&ctx->dequeue_burst_len),
		(unsigned long long)ctx->dequeue_burst_len.max_ns,
		(unsigned long long)ctx->burst_reqs_total);
}

static uint64_t rfuse_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void rfuse_sleep_stat_record(struct rfuse_daemon_sleep_stat *stat, uint64_t delta_ns)
{
	stat->count++;
	stat->total_ns += delta_ns;
	if (delta_ns < stat->min_ns)
		stat->min_ns = delta_ns;
	if (delta_ns > stat->max_ns)
		stat->max_ns = delta_ns;
}

static void rfuse_commit_dequeue_burst(struct rfuse_daemon_sleep_ctx *ctx)
{
	if (ctx->current_burst_len == 0)
		return;

	/*
	 * sleep 직전까지 연속으로 처리한 pending dequeue 길이를 기록한다.
	 * wake당 backlog가 있으면 burst 길이가 1보다 크게 누적된다.
	 */
	rfuse_sleep_stat_record(&ctx->dequeue_burst_len, ctx->current_burst_len);
	ctx->current_burst_len = 0;
}

#else
static void rfuse_sleep_ctx_init(struct rfuse_daemon_sleep_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

static void rfuse_sleep_ctx_dump(const struct rfuse_mt *mt)
{
}

static void rfuse_commit_dequeue_burst(struct rfuse_daemon_sleep_ctx *ctx)
{
}
#endif

static struct fuse_chan *rfuse_chan_new(int fd)
{
	struct fuse_chan *ch = (struct fuse_chan *) malloc(sizeof(*ch));
	if (ch == NULL) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to allocate channel\n");
		return NULL;
	}

	memset(ch, 0, sizeof(*ch));
	ch->fd = fd;
	ch->ctr = 1;
	pthread_mutex_init(&ch->lock, NULL);

	return ch;
}

struct fuse_chan *rfuse_chan_get(struct fuse_chan *ch)
{
	assert(ch->ctr > 0);
	pthread_mutex_lock(&ch->lock);
	ch->ctr++;
	pthread_mutex_unlock(&ch->lock);

	return ch;
}

void rfuse_chan_put(struct fuse_chan *ch)
{
	if (ch == NULL)
		return;
	pthread_mutex_lock(&ch->lock);
	ch->ctr--;
	if (!ch->ctr) {
		pthread_mutex_unlock(&ch->lock);
		close(ch->fd);
		pthread_mutex_destroy(&ch->lock);
		free(ch);
	} else
		pthread_mutex_unlock(&ch->lock);
}

static void rfuse_list_add_worker(struct rfuse_worker *w, struct rfuse_worker *next)
{
	struct rfuse_worker *prev = next->prev;
	w->next = next;
	w->prev = prev;
	prev->next = w;
	next->prev = w;
}

static void rfuse_list_del_worker(struct rfuse_worker *w)
{
	struct rfuse_worker *prev = w->prev;
	struct rfuse_worker *next = w->next;
	prev->next = next;
	next->prev = prev;
}

static void *rfuse_do_work(void *data)
{
	struct rfuse_worker *w = (struct rfuse_worker *) data;
	struct rfuse_mt *mt = w->mt;
	struct rfuse_iqueue *riq = mt->se->riq[mt->riq_id];
	_Atomic int isforget = 0;
	bool counted_general;
	bool processed = false;
	bool retry_pending;

	while (!fuse_session_exited(mt->se)) {
		/**
		 * Currently We check the forget queue once and pending queue once.
		 * We do not check the interrupt queue yet.
		 * isforget == 0 : handle general request
		 * isforget == 1 : handle forget request
		**/
		retry_pending = false;
retry_read_queue:
		pthread_mutex_lock(&mt->lock);
		if (mt->exit) {
			pthread_mutex_unlock(&mt->lock);
			return NULL;
		}

		counted_general = !isforget;
		if (counted_general)
			mt->numavail--;
		if (!retry_pending && mt->numavail == 0 && mt->numworker < RFUSE_WORKER_PER_RING)
			rfuse_loop_start_thread(mt);
		pthread_mutex_unlock(&mt->lock);

		processed = rfuse_read_queue(w, mt, NULL, isforget);

		pthread_mutex_lock(&mt->lock);
		if (counted_general)
			mt->numavail++;

		isforget++;
		if (isforget == 2)
			isforget = 0;

		if (mt->numavail > mt->max_idle) {
			if (mt->exit) {
				pthread_mutex_unlock(&mt->lock);
				return NULL;
			}
			rfuse_list_del_worker(w); // if it exceeds 10 workers, free this worker
			mt->numavail--;
			mt->numworker--;
			pthread_mutex_unlock(&mt->lock);

			pthread_detach(w->thread_id);
			free(w->fbuf.mem);
			rfuse_chan_put(w->ch);
			free(w);
			return NULL;
		} else if (isforget == 0 && !processed) {
			uint64_t sleep_start_ns;
			uint64_t sleep_end_ns;
			uint64_t pending_depth;
			struct ioctl_args {
				int riq_id;
				int req_index;
			} args = { .riq_id = mt->riq_id, .req_index = -1 };
			int res;

			pending_depth = rfuse_smp_load_acquire(&riq->pending.tail) - riq->pending.head;
			if (pending_depth) {
				retry_pending = true;
				pthread_mutex_unlock(&mt->lock);
				goto retry_read_queue;
			}

#if LDY_LOG
			rfuse_commit_dequeue_burst(&mt->sleep_ctx);
			mt->sleep_ctx.sleep_calls++;
			mt->sleep_ctx.forget_miss_before_sleep++;
			mt->sleep_ctx.sleep_blocked_calls++;
#endif

			pthread_mutex_unlock(&mt->lock);
#if LDY_LOG
			sleep_start_ns = rfuse_now_ns();
#endif
			res = ioctl(mt->se->fd, RFUSE_DAEMON_SLEEP, &args);
#if LDY_LOG
			sleep_end_ns = rfuse_now_ns();
			mt->sleep_ctx.last_wake_ns = sleep_end_ns;
			rfuse_sleep_stat_record(&mt->sleep_ctx.sleep_block_ns,
					       sleep_end_ns - sleep_start_ns);
			pending_depth = rfuse_smp_load_acquire(&riq->pending.tail) - riq->pending.head;
			rfuse_sleep_stat_record(&mt->sleep_ctx.wake_pending_depth, pending_depth);
			if (pending_depth > 1)
				mt->sleep_ctx.wake_with_backlog_calls++;
#endif
			if (res == -ENOTCONN) {
				printf("rfuse: User-level daemon lost connection, exit\n");
				return NULL;
			}
			continue;
		}

		pthread_mutex_unlock(&mt->lock);
	}

	sem_post(&mt->finish);

	return NULL;
}

int rfuse_start_thread(pthread_t *thread_id, void *(*func)(void *), void *arg)
{
	sigset_t oldset;
	sigset_t newset;
	int res;
	pthread_attr_t attr;
	char *stack_size;

	/* Override default stack size */
	pthread_attr_init(&attr);
	stack_size = getenv(ENVNAME_THREAD_STACK);
	if (stack_size && pthread_attr_setstacksize(&attr, atoi(stack_size)))
		fuse_log(FUSE_LOG_ERR, "fuse: invalid stack size: %s\n", stack_size);

	/* Disallow signal reception in worker threads */
	sigemptyset(&newset);
	sigaddset(&newset, SIGTERM);
	sigaddset(&newset, SIGINT);
	sigaddset(&newset, SIGHUP);
	sigaddset(&newset, SIGQUIT);
	pthread_sigmask(SIG_BLOCK, &newset, &oldset);
	res = pthread_create(thread_id, &attr, func, arg);
	pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	pthread_attr_destroy(&attr);
	if (res != 0) {
		fuse_log(FUSE_LOG_ERR, "fuse: error creating thread: %s\n",
			strerror(res));
		return -1;
	}

	return 0;
}

static struct fuse_chan *rfuse_clone_chan(struct rfuse_mt *mt)
{
	int res;
	int clonefd;
	uint32_t masterfd;
	struct fuse_chan *newch;
	const char *devname = "/dev/fuse";

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
	clonefd = open(devname, O_RDWR | O_CLOEXEC);
	if (clonefd == -1) {
		fuse_log(FUSE_LOG_ERR, "rfuse: failed to open %s: %s\n", devname,
			strerror(errno));
		return NULL;
	}
	fcntl(clonefd, F_SETFD, FD_CLOEXEC);

	masterfd = mt->se->fd;
	res = ioctl(clonefd, FUSE_DEV_IOC_CLONE, &masterfd);
	if (res == -1) {
		fuse_log(FUSE_LOG_ERR, "rfuse: failed to clone device fd: %s\n",
			strerror(errno));
		close(clonefd);
		return NULL;
	}
	newch = rfuse_chan_new(clonefd);
	if (newch == NULL)
		close(clonefd);

	return newch;
}

int rfuse_loop_start_thread(struct rfuse_mt *mt)
{
	int res;

	struct rfuse_worker *w = malloc(sizeof(struct rfuse_worker));
	if (!w) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to allocate worker structure\n");
		return -1;
	}
	memset(w, 0, sizeof(struct rfuse_worker));
	w->fbuf.mem = NULL;
	w->mt = mt;

	w->ch = NULL;

	if (mt->clone_fd) {
		w->ch = rfuse_clone_chan(mt);
		if(!w->ch) {
			fuse_log(FUSE_LOG_ERR, "fuse: trying to continue "
				"without -o clone_fd.\n");
			mt->clone_fd = 0;
		}
	}
	
	res = rfuse_start_thread(&w->thread_id, rfuse_do_work, w);
	if (res == -1) {
		rfuse_chan_put(w->ch);
		free(w);
		return -1;
	}
	rfuse_list_add_worker(w, &mt->main);
	mt->numavail ++;
	mt->numworker ++;

	return 0;
}

static void rfuse_join_worker(struct rfuse_mt *mt, struct rfuse_worker *w)
{
	pthread_join(w->thread_id, NULL);
	pthread_mutex_lock(&mt->lock);
	rfuse_list_del_worker(w);
	pthread_mutex_unlock(&mt->lock);
	free(w->fbuf.mem);
	rfuse_chan_put(w->ch);
	free(w);
}

void *rfuse_session_loop_mt_mriq(void *data)
{
	int err;
	struct rfuse_mt mt;
	struct rfuse_worker *w;

	struct rfuse_loop_args *args = (struct rfuse_loop_args *)data; 
	int riq_id = args->riq_id;
	struct fuse_session *se = args->se;
	struct fuse_loop_config *config = args->config;
	cpu_set_t cpuset;

	printf("Main thread riq_id: %d\n", riq_id);
	memset(&mt, 0, sizeof(struct rfuse_mt));
	mt.se = se;
	mt.clone_fd = config->clone_fd;
	mt.error = 0;
	mt.numworker = 0;
	mt.numavail = 0;
	mt.max_idle = config->max_idle_threads;
	rfuse_sleep_ctx_init(&mt.sleep_ctx);
	mt.main.thread_id = pthread_self();
	mt.main.prev = mt.main.next = &mt.main;
	mt.riq_id = riq_id;
	sem_init(&mt.finish, 0, 0);
	pthread_mutex_init(&mt.lock, NULL);

	// Set CPU affinity based on riq_id (currently 2 cores per riq)
	// pthread_create() inherits a copy of its creator's CPU affinity mask.
	CPU_ZERO(&cpuset);
	CPU_SET(mt.riq_id, &cpuset);
	pthread_setaffinity_np(mt.main.thread_id, sizeof(cpu_set_t), &cpuset);

	pthread_mutex_lock(&mt.lock);
	err = rfuse_loop_start_thread(&mt);
	pthread_mutex_unlock(&mt.lock);
	if (!err) {
		/* sem_wait() is interruptible */
		while (!fuse_session_exited(se))
			sem_wait(&mt.finish);

		pthread_mutex_lock(&mt.lock);
		for (w = mt.main.next; w != &mt.main; w = w->next)
			pthread_cancel(w->thread_id);
		mt.exit = 1;
		pthread_mutex_unlock(&mt.lock);

		while (mt.main.next != &mt.main)
			rfuse_join_worker(&mt, mt.main.next);

		err = mt.error;
	}

	pthread_mutex_destroy(&mt.lock);
	sem_destroy(&mt.finish);
	rfuse_sleep_ctx_dump(&mt);

	if(se->error != 0)
		err = se->error;

	return (void *)err;
}

int fuse_session_loop_mt_32(struct fuse_session *se, struct fuse_loop_config *config)
{
	int res = 0, err = 0;
	int i, stop_point;
	struct rfuse_main_worker mw[RFUSE_NUM_IQUEUE];

	for(i = 0; i < RFUSE_NUM_IQUEUE; i++) {
		mw[i].args.se = se;
		mw[i].args.config = config;
		mw[i].args.riq_id = i;

		res = pthread_create(&mw[i].main_thread_id, NULL, rfuse_session_loop_mt_mriq, (void *)&mw[i].args);
		if(res) {
			fuse_log(FUSE_LOG_ERR, "rfuse: failed to create main_worker");
			break;
		}
	}

	if(res) {
		// pthread_create is failed at some point
		stop_point = i;
		for(i = 0; i < stop_point; i++) {
			pthread_join(mw[i].main_thread_id, (void **)&err);
		}
	} else {
		// All pthread_create() succeeds
		for(i = 0; i < RFUSE_NUM_IQUEUE; i++) {
			pthread_join(mw[i].main_thread_id, NULL);
		}
	}

	fuse_session_reset(se);
	return err;
}

int fuse_session_loop_mt_31(struct fuse_session *se, int clone_fd)
{
	struct fuse_loop_config config;
	config.clone_fd = clone_fd;
	config.max_idle_threads = 4;
	return fuse_session_loop_mt_32(se, &config);
}
