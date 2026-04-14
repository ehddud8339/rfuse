#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_FILE_SIZE (64ULL * 1024 * 1024)
#define DEFAULT_CHUNK_SIZE (64ULL * 1024)

static const char pattern_table[] =
	"0123456789"
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"-_=+[]{}|;:,.";

static void die_errno(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

static unsigned long long parse_size(const char *text)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || end == text) {
		fprintf(stderr, "invalid size: %s\n", text);
		exit(EXIT_FAILURE);
	}

	if (*end == '\0')
		return value;
	if (end[1] != '\0') {
		fprintf(stderr, "invalid size suffix: %s\n", text);
		exit(EXIT_FAILURE);
	}

	switch (*end) {
	case 'k':
	case 'K':
		return value * 1024ULL;
	case 'm':
	case 'M':
		return value * 1024ULL * 1024ULL;
	case 'g':
	case 'G':
		return value * 1024ULL * 1024ULL * 1024ULL;
	default:
		fprintf(stderr, "unsupported size suffix: %s\n", text);
		exit(EXIT_FAILURE);
	}
}

static char pattern_byte(uint64_t absolute_offset)
{
	size_t pattern_len = sizeof(pattern_table) - 1;

	if (absolute_offset % 64 == 63)
		return '\n';

	return pattern_table[(absolute_offset + absolute_offset / 64) %
			     pattern_len];
}

static void fill_pattern(char *buf, size_t len, uint64_t start_offset)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = pattern_byte(start_offset + i);
}

static void write_full(int fd, const char *buf, size_t len)
{
	size_t written = 0;

	while (written < len) {
		ssize_t ret = write(fd, buf + written, len - written);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die_errno("write");
		}
		if (ret == 0) {
			fprintf(stderr, "write returned 0 unexpectedly\n");
			exit(EXIT_FAILURE);
		}
		written += (size_t)ret;
	}
}

static void pread_full(int fd, char *buf, size_t len, off_t offset)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = pread(fd, buf + done, len - done, offset + (off_t)done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die_errno("pread");
		}
		if (ret == 0) {
			fprintf(stderr,
				"short read: offset=%jd requested=%zu completed=%zu\n",
				(intmax_t)offset, len, done);
			exit(EXIT_FAILURE);
		}
		done += (size_t)ret;
	}
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "read_tail_verify.dat";
	unsigned long long file_size = argc > 2 ? parse_size(argv[2]) :
					       DEFAULT_FILE_SIZE;
	unsigned long long chunk_size = argc > 3 ? parse_size(argv[3]) :
						DEFAULT_CHUNK_SIZE;
	char *write_buf;
	char *read_buf;
	unsigned long long offset = 0;
	int fd;
	size_t this_chunk;
	size_t i;
	struct stat st;

	if (argc > 4) {
		fprintf(stderr, "usage: %s [path] [file_size] [chunk_size]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	if (file_size == 0 || chunk_size == 0) {
		fprintf(stderr, "sizes must be non-zero\n");
		return EXIT_FAILURE;
	}
	if ((off_t)file_size < 0 ||
	    (unsigned long long)(off_t)file_size != file_size) {
		fprintf(stderr, "file_size exceeds off_t range\n");
		return EXIT_FAILURE;
	}

	write_buf = malloc((size_t)chunk_size);
	read_buf = malloc((size_t)chunk_size);
	if (!write_buf || !read_buf) {
		fprintf(stderr, "memory allocation failed\n");
		return EXIT_FAILURE;
	}

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0)
		die_errno("open");

	while (offset < file_size) {
		this_chunk = (size_t)((file_size - offset) > chunk_size ?
				      chunk_size : (file_size - offset));

		fill_pattern(write_buf, this_chunk, offset);
		write_full(fd, write_buf, this_chunk);
		pread_full(fd, read_buf, this_chunk, (off_t)offset);

		for (i = 0; i < this_chunk; i++) {
			if (read_buf[i] == write_buf[i])
				continue;

			fprintf(stderr,
				"mismatch: file_offset=%" PRIu64 " chunk_offset=%zu expected=%02x('%c') actual=%02x('%c')\n",
				(uint64_t)offset + i, i,
				(unsigned char)write_buf[i],
				write_buf[i] == '\n' ? 'N' : write_buf[i],
				(unsigned char)read_buf[i],
				read_buf[i] == '\n' ? 'N' : read_buf[i]);
			return EXIT_FAILURE;
		}

		offset += this_chunk;
	}

	if (fstat(fd, &st) < 0)
		die_errno("fstat");
	if ((unsigned long long)st.st_size != file_size) {
		fprintf(stderr, "size mismatch: expected=%llu actual=%jd\n",
			file_size, (intmax_t)st.st_size);
		return EXIT_FAILURE;
	}

	printf("ok: path=%s file_size=%llu chunk_size=%llu verified_bytes=%llu\n",
	       path, file_size, chunk_size, file_size);

	if (close(fd) < 0)
		die_errno("close");
	free(read_buf);
	free(write_buf);
	return EXIT_SUCCESS;
}
