#include "fuse_i.h"
#include "fuse_kernel.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int create_count;

static void test_create(fuse_req_t req, fuse_ino_t parent, const char *name,
			mode_t mode, struct fuse_file_info *fi)
{
	(void)parent;
	(void)name;
	(void)mode;
	(void)fi;
	create_count++;
	fuse_reply_none(req);
}

static const struct fuse_lowlevel_ops test_ops = {
	.create = test_create,
};

static size_t process_create_and_capture(struct fuse_session *se,
					 const char *name, char *output,
					 size_t output_size)
{
	struct {
		struct fuse_in_header header;
		struct fuse_create_in create;
		char name[64];
	} request = { 0 };
	struct fuse_buf buf = { 0 };
	int output_pipe[2];
	int saved_stdout;
	ssize_t bytes_read;

	assert(strlen(name) < sizeof(request.name));
	request.header.len = sizeof(request.header) + sizeof(request.create) +
		strlen(name) + 1;
	request.header.opcode = FUSE_CREATE;
	request.header.unique = create_count + 1;
	request.header.nodeid = FUSE_ROOT_ID;
	request.create.mode = 0644;
	strcpy(request.name, name);
	buf.size = request.header.len;
	buf.mem = &request;

	assert(pipe(output_pipe) == 0);
	saved_stdout = dup(STDOUT_FILENO);
	assert(saved_stdout >= 0);
	assert(dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
	close(output_pipe[1]);

	fuse_session_process_buf(se, &buf);

	assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
	close(saved_stdout);
	bytes_read = read(output_pipe[0], output, output_size - 1);
	assert(bytes_read >= 0);
	close(output_pipe[0]);
	output[bytes_read] = '\0';

	return bytes_read;
}

static void seed_backend_read_latency(struct fuse_session *se, uint64_t count)
{
	struct fuse_latency_stat *stat =
		&se->latency_shards[0].stats[FUSE_LATENCY_OP_READ]
					       [FUSE_LATENCY_BACKEND];

	stat->count = count;
	stat->total_ns = count * 10;
	stat->min_ns = 5;
	stat->max_ns = 15;
}

static void assert_latency_is_reset(const struct fuse_session *se)
{
	int shard_id;

	for (shard_id = 0; shard_id < FUSE_LATENCY_SHARDS; shard_id++)
		assert(memcmp(se->latency_shards[shard_id].stats,
			      (struct fuse_latency_stat
			       [FUSE_LATENCY_OP_MAX][FUSE_LATENCY_MAX]) { 0 },
			      sizeof(se->latency_shards[shard_id].stats)) == 0);
}

int main(void)
{
	char program_name[] = "test_latency_breakdown";
	char *argv[] = { program_name, NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, argv);
	struct fuse_session *se;
	char output[4096];
	char name[64];
	char *backend_line;
	unsigned long long count;

	se = fuse_session_new(&args, &test_ops, sizeof(test_ops), NULL);
	assert(se != NULL);
	se->got_init = 1;
	se->conn.proto_minor = 31;

	seed_backend_read_latency(se, 2);
	assert(process_create_and_capture(se, ".breakdown", output,
					  sizeof(output)) > 0);
	backend_line = strstr(output, "backend_read");
	assert(backend_line != NULL);
	assert(sscanf(backend_line, "%63s %llu", name, &count) == 2);
	assert(strcmp(name, "backend_read") == 0);
	assert(count == 2);
	assert_latency_is_reset(se);

	seed_backend_read_latency(se, 3);
	assert(process_create_and_capture(se, "regular", output,
					  sizeof(output)) == 0);
	assert(se->latency_shards[0].stats[FUSE_LATENCY_OP_READ]
					     [FUSE_LATENCY_BACKEND].count == 3);
	assert(create_count == 2);

	fuse_session_destroy(se);
	fuse_opt_free_args(&args);
	return 0;
}
