/*
 * test_buffer.c - test suite for the buffer library.
 *
 * Build/run with the Makefile:  make test        (ASan + UBSan)
 *                               make test-plain  (no sanitizers)
 *                               make test-valgrind
 *
 * Environment variables:
 *   VERBOSE=1   show the library's own error messages on stderr
 *   SEED=<n>    seed for the randomized tests (printed on failure)
 *
 * The library prints a message to stderr on every rejected call, so the
 * tests silence stderr around calls that are expected to fail.
 */
#define _GNU_SOURCE
#include "../include/buffer.h"   /* first on purpose: header must be self-contained */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#endif
#endif

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                */
/* ------------------------------------------------------------------ */

static int g_checks, g_failed, g_test_failed, g_skipped;
static uint64_t g_seed = 0x9E3779B97F4A7C15ull;

#define CHECK(cond) do{ g_checks++; if(!(cond)){ g_failed++; g_test_failed++; \
	printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fflush(stdout);} }while(0)

#define CHECKF(cond, ...) do{ g_checks++; if(!(cond)){ g_failed++; g_test_failed++; \
	printf("    FAIL %s:%d: %s  [", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
	printf("]\n"); fflush(stdout);} }while(0)

#define CHECK_INT(actual, expected) do{ long long a_=(long long)(actual), e_=(long long)(expected); \
	g_checks++; if(a_!=e_){ g_failed++; g_test_failed++; \
	printf("    FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); \
	fflush(stdout);} }while(0)

#define CHECK_SZ(actual, expected) do{ size_t a_=(size_t)(actual), e_=(size_t)(expected); \
	g_checks++; if(a_!=e_){ g_failed++; g_test_failed++; \
	printf("    FAIL %s:%d: %s = %zu, expected %zu\n", __FILE__, __LINE__, #actual, a_, e_); \
	fflush(stdout);} }while(0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define SKIP(msg) do{ printf("    skipped: %s\n", msg); g_skipped++; return; }while(0)

typedef void (*test_fn)(void);

static void run(const char *name, test_fn fn){
	g_test_failed = 0;
	printf("[ RUN  ] %s\n", name);
	fflush(stdout);
	fn();
	printf("[ %s ] %s\n", g_test_failed ? "FAIL" : " OK ", name);
	fflush(stdout);
}
#define RUN(fn) run(#fn, fn)

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int g_saved_err = -1;

static void quiet(void){
	if(getenv("VERBOSE") || g_saved_err >= 0)
		return;
	fflush(stderr);
	g_saved_err = dup(2);
	int n = open("/dev/null", O_WRONLY);
	if(n >= 0){ dup2(n, 2); close(n); }
}

static void loud(void){
	if(g_saved_err < 0)
		return;
	fflush(stderr);
	dup2(g_saved_err, 2);
	close(g_saved_err);
	g_saved_err = -1;
}

static buffer_t make_buf(size_t n){
	buffer_t b;
	memset(&b, 0, sizeof b);
	int rc = buffer_init(&b, n);
	CHECK_INT(rc, 0);
	return b;
}

static void free_buf(buffer_t *b){ buffer_delete(b, false, NULL); }

static int put(buffer_t *b, size_t off, const char *s){
	return buffer_write(b, off, s, strlen(s));
}

static int all_zero(const uint8_t *p, size_t n){
	for(size_t i = 0; i < n; i++)
		if(p[i]) return 0;
	return 1;
}

static void fill_pattern(uint8_t *p, size_t n, uint32_t seed){
	uint32_t x = seed * 2654435761u + 12345u;
	for(size_t i = 0; i < n; i++){
		x = x * 1664525u + 1013904223u;
		p[i] = (uint8_t)(x >> 24);
	}
}

static uint64_t g_rng;
static uint64_t rnd(void){
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return g_rng;
}
static size_t rnd_range(size_t lo, size_t hi){ /* inclusive */
	return lo + (size_t)(rnd() % (uint64_t)(hi - lo + 1));
}

/* --- temp directory and file helpers --- */

static char g_dir[PATH_MAX];

static void make_tmpdir(void){
	const char *t = getenv("TMPDIR");
	if(!t || !*t) t = "/tmp";
	snprintf(g_dir, sizeof g_dir, "%s/buffer_test_XXXXXX", t);
	if(!mkdtemp(g_dir)){
		perror("mkdtemp");
		exit(2);
	}
}

static void cleanup_tmpdir(void){
	DIR *d = opendir(g_dir);
	if(d){
		struct dirent *e;
		while((e = readdir(d))){
			if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
			if(unlink(p) != 0)
				rmdir(p);
		}
		closedir(d);
	}
	rmdir(g_dir);
}

static const char *path_of(const char *name){
	static char ring[8][PATH_MAX];
	static int i;
	char *r = ring[i++ % 8];
	snprintf(r, PATH_MAX, "%s/%s", g_dir, name);
	return r;
}

static int write_file(const char *path, const void *data, size_t n){
	FILE *f = fopen(path, "wb");
	if(!f) return -1;
	if(n && fwrite(data, 1, n, f) != n){ fclose(f); return -1; }
	return fclose(f) == 0 ? 0 : -1;
}

static uint8_t *slurp(const char *path, size_t *n){
	FILE *f = fopen(path, "rb");
	if(!f) return NULL;
	size_t cap = 4096, len = 0;
	uint8_t *p = malloc(cap);
	size_t got;
	while(p && (got = fread(p + len, 1, cap - len, f)) > 0){
		len += got;
		if(len == cap){
			cap *= 2;
			uint8_t *q = realloc(p, cap);
			if(!q){ free(p); p = NULL; }
			else p = q;
		}
	}
	fclose(f);
	*n = len;
	return p;
}

static long long file_size_of(const char *path){
	struct stat st;
	if(stat(path, &st) != 0) return -1;
	return (long long)st.st_size;
}

static int exists(const char *path){ return access(path, F_OK) == 0; }

/* Run fn in a forked child (for tests that change rlimits). Returns the
   child's exit status: 0 = all checks inside passed. */
static int run_in_child(void (*fn)(void)){
	fflush(stdout);
	fflush(stderr);
	pid_t pid = fork();
	if(pid < 0){ perror("fork"); return -1; }
	if(pid == 0){
		g_test_failed = 0;
		fn();
		fflush(stdout);
		_exit(g_test_failed ? 1 : 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if(WIFEXITED(st)) return WEXITSTATUS(st);
	return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* ------------------------------------------------------------------ */
/* buffer_init                                                        */
/* ------------------------------------------------------------------ */

static void test_init_basic(void){
	buffer_t b;
	memset(&b, 0xAB, sizeof b);
	CHECK_INT(buffer_init(&b, 64), 0);
	CHECK(b.base != NULL);
	CHECK_SZ(b.len, 0);
	CHECK_SZ(b.size, 64);
	CHECK(all_zero(b.base, 64));   /* calloc: whole buffer starts zeroed */
	free_buf(&b);
}

static void test_init_various_sizes(void){
	size_t sizes[] = {1, 2, 3, 7, 8, 15, 16, 17, 4095, 4096, 4097, 65536, 100000};
	for(size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++){
		buffer_t b = make_buf(sizes[i]);
		CHECKF(b.size == sizes[i], "size=%zu", sizes[i]);
		CHECKF(b.base != NULL, "size=%zu", sizes[i]);
		if(b.base){
			CHECKF(all_zero(b.base, sizes[i]), "size=%zu", sizes[i]);
			b.base[sizes[i] - 1] = 0xEE;   /* ASan catches an under-allocation */
		}
		free_buf(&b);
	}
}

static void test_init_null_buffer(void){
	quiet();
	CHECK_INT(buffer_init(NULL, 16), -1);
	CHECK_INT(buffer_init(NULL, 0), -1);
	CHECK_INT(buffer_init(NULL, (size_t)BUFFER_MAX_SIZE + 1), -1);
	loud();
}

static void test_init_invalid_sizes_rejected(void){
	size_t bad[] = {0, (size_t)BUFFER_MAX_SIZE + 1, (size_t)BUFFER_MAX_SIZE * 2, SIZE_MAX / 2, SIZE_MAX};
	for(size_t i = 0; i < sizeof bad / sizeof bad[0]; i++){
		buffer_t b;
		memset(&b, 0, sizeof b);
		quiet();
		int rc = buffer_init(&b, bad[i]);
		loud();
		CHECKF(rc == -1, "size=%zu rc=%d", bad[i], rc);
		if(rc == 0) free_buf(&b);
	}
}

/* A failed init must not leave the struct pointing at stale memory, otherwise
   the NULL checks in read/write/empty/fread/fwrite are useless. */
static void test_init_failure_leaves_clean_state(void){
	size_t bad[] = {0, (size_t)BUFFER_MAX_SIZE + 1, SIZE_MAX};
	for(size_t i = 0; i < sizeof bad / sizeof bad[0]; i++){
		uint8_t sentinel = 0;
		buffer_t b = { &sentinel, 7, 9 };
		quiet();
		int rc = buffer_init(&b, bad[i]);
		loud();
		CHECKF(rc == -1, "size=%zu", bad[i]);
		CHECKF(b.base == NULL, "size=%zu: base left as %p", bad[i], (void*)b.base);
		CHECKF(b.len == 0, "size=%zu: len=%zu", bad[i], b.len);
		CHECKF(b.size == 0, "size=%zu: size=%zu", bad[i], b.size);
	}
}

static void test_init_max_size(void){
	buffer_t b = make_buf(BUFFER_MAX_SIZE);
	CHECK_SZ(b.size, BUFFER_MAX_SIZE);
	CHECK_INT(buffer_write(&b, BUFFER_MAX_SIZE - 1, "x", 1), 0);
	CHECK_SZ(b.len, BUFFER_MAX_SIZE);
	quiet();
	CHECK_INT(buffer_write(&b, BUFFER_MAX_SIZE, "x", 1), -1);
	loud();
	free_buf(&b);
}

#ifndef UNDER_ASAN
static void child_alloc_failure(void){
	long pages = 0;
	FILE *f = fopen("/proc/self/statm", "r");
	if(!f || fscanf(f, "%ld", &pages) != 1){
		if(f) fclose(f);
		printf("    skipped: cannot read /proc/self/statm\n");
		return;
	}
	fclose(f);
	struct rlimit rl;
	rl.rlim_cur = rl.rlim_max = (rlim_t)(pages * sysconf(_SC_PAGESIZE)) + (1u << 20);
	CHECK(setrlimit(RLIMIT_AS, &rl) == 0);

	buffer_t b;
	memset(&b, 0xAB, sizeof b);
	quiet();
	int rc = buffer_init(&b, BUFFER_MAX_SIZE);   /* 16 MiB > the 1 MiB of headroom */
	loud();
	CHECK_INT(rc, -1);
	CHECK(b.base == NULL);
	CHECK_SZ(b.len, 0);
	CHECK_SZ(b.size, 0);
}
#endif

static void test_init_allocation_failure(void){
#ifdef UNDER_ASAN
	SKIP("RLIMIT_AS is incompatible with ASan (run `make test-plain`)");
#else
	CHECK_INT(run_in_child(child_alloc_failure), 0);
#endif
}

/* ------------------------------------------------------------------ */
/* buffer_read                                                        */
/* ------------------------------------------------------------------ */

static void test_read_basic(void){
	buffer_t b = make_buf(64);
	CHECK_INT(put(&b, 0, "hello world"), 0);
	CHECK_SZ(b.len, 11);

	const char *p = buffer_read(&b, 0, 5);
	CHECK(p != NULL && memcmp(p, "hello", 5) == 0);
	p = buffer_read(&b, 6, 5);                   /* ends exactly at len */
	CHECK(p != NULL && memcmp(p, "world", 5) == 0);
	CHECK(buffer_read(&b, 0, 11) == (void*)b.base);   /* whole data */
	CHECK(buffer_read(&b, 10, 1) == (void*)(b.base + 10));   /* last byte */
	free_buf(&b);
}

static void test_read_pointer_aliases_storage(void){
	buffer_t b = make_buf(32);
	put(&b, 0, "abcdef");
	uint8_t *p = buffer_read(&b, 2, 2);
	CHECK(p == b.base + 2);
	p[0] = 'X';                                   /* visible through the buffer */
	CHECK_MEM(b.base, "abXdef", 6);
	free_buf(&b);
}

static void test_read_out_of_range(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "hello world");                    /* len = 11 */
	quiet();
	CHECK(buffer_read(&b, 0, 12) == NULL);        /* one byte too long */
	CHECK(buffer_read(&b, 5, 7) == NULL);
	CHECK(buffer_read(&b, 10, 2) == NULL);
	CHECK(buffer_read(&b, 11, 1) == NULL);        /* starts at len */
	CHECK(buffer_read(&b, 100, 1) == NULL);
	CHECK(buffer_read(&b, 0, 64) == NULL);        /* capacity, but beyond len */
	loud();
	free_buf(&b);
}

static void test_read_limited_by_len_not_capacity(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "0123456789");                     /* len = 10, size = 64 */
	quiet();
	CHECK(buffer_read(&b, 10, 1) == NULL);
	CHECK(buffer_read(&b, 20, 4) == NULL);
	CHECK(buffer_read(&b, 63, 1) == NULL);
	loud();
	free_buf(&b);
}

static void test_read_size_overflow(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "hello world");
	quiet();
	CHECK(buffer_read(&b, 1, SIZE_MAX) == NULL);          /* offset+size wraps to 0 */
	CHECK(buffer_read(&b, 8, SIZE_MAX) == NULL);
	CHECK(buffer_read(&b, SIZE_MAX, 1) == NULL);
	CHECK(buffer_read(&b, SIZE_MAX, SIZE_MAX) == NULL);
	CHECK(buffer_read(&b, SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1) == NULL);
	loud();
	free_buf(&b);
}

static void test_read_zero_size_inside_range(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "hello world");
	CHECK(buffer_read(&b, 3, 0) == (void*)(b.base + 3));
	free_buf(&b);
}

static void test_read_at_len_boundary(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "hello world");                    /* len = 11 */
	CHECK(buffer_read(&b, 11, 0) == (void*)(b.base + 11));   /* one-past-end, zero bytes */
	quiet();
	CHECK(buffer_read(&b, 12, 0) == NULL);
	CHECK(buffer_read(&b, 11, 1) == NULL);
	loud();
	free_buf(&b);
}

static void test_read_empty_buffer(void){
	buffer_t b = make_buf(64);                    /* len = 0 */
	CHECK(buffer_read(&b, 0, 0) != NULL);                 /* zero bytes at offset == len: allowed */
	quiet();
	CHECK(buffer_read(&b, 0, 1) == NULL);
	CHECK(buffer_read(&b, 1, 0) == NULL);                 /* offset beyond len */
	loud();
	free_buf(&b);
}

static void test_read_null_args(void){
	buffer_t z;
	memset(&z, 0, sizeof z);
	quiet();
	CHECK(buffer_read(NULL, 0, 1) == NULL);
	CHECK(buffer_read(&z, 0, 1) == NULL);         /* base == NULL */
	loud();
}

/* ------------------------------------------------------------------ */
/* buffer_write                                                       */
/* ------------------------------------------------------------------ */

static void test_write_basic_and_len(void){
	buffer_t b = make_buf(64);
	CHECK_INT(put(&b, 0, "hello"), 0);
	CHECK_SZ(b.len, 5);
	CHECK_INT(put(&b, 5, " abc"), 0);
	CHECK_SZ(b.len, 9);
	CHECK_MEM(b.base, "hello abc", 9);
	CHECK_SZ(b.size, 64);
	free_buf(&b);
}

static void test_write_middle_overwrite_keeps_len(void){
	buffer_t b = make_buf(256);
	uint8_t data[200], patch[20];
	fill_pattern(data, sizeof data, 1);
	memset(patch, 0xEE, sizeof patch);

	CHECK_INT(buffer_write(&b, 0, data, 200), 0);
	CHECK_SZ(b.len, 200);
	CHECK_INT(buffer_write(&b, 80, patch, 20), 0);   /* 80..99 replaced */
	CHECK_SZ(b.len, 200);                            /* must not shrink to 100 */
	CHECK_MEM(b.base, data, 80);
	CHECK_MEM(b.base + 80, patch, 20);
	CHECK_MEM(b.base + 100, data + 100, 100);
	free_buf(&b);
}

static void test_write_overlapping_end_extends_len(void){
	buffer_t b = make_buf(256);
	uint8_t data[200], more[30];
	fill_pattern(data, sizeof data, 2);
	fill_pattern(more, sizeof more, 3);
	buffer_write(&b, 0, data, 200);
	CHECK_INT(buffer_write(&b, 190, more, 30), 0);
	CHECK_SZ(b.len, 220);
	CHECK_MEM(b.base, data, 190);
	CHECK_MEM(b.base + 190, more, 30);
	free_buf(&b);
}

static void test_write_append_exactly_at_len(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "abc");
	CHECK_INT(put(&b, 3, "def"), 0);
	CHECK_SZ(b.len, 6);
	CHECK_MEM(b.base, "abcdef", 6);
	free_buf(&b);
}

static void test_write_gap_reads_as_zero(void){
	buffer_t b = make_buf(128);
	put(&b, 0, "0123456789");
	CHECK_INT(put(&b, 50, "tail!"), 0);
	CHECK_SZ(b.len, 55);
	CHECK(all_zero(b.base + 10, 40));
	CHECK_MEM(b.base + 50, "tail!", 5);
	free_buf(&b);
}

static void test_write_fills_capacity_exactly(void){
	buffer_t b = make_buf(16);
	uint8_t data[16];
	fill_pattern(data, sizeof data, 4);
	CHECK_INT(buffer_write(&b, 0, data, 16), 0);
	CHECK_SZ(b.len, 16);
	CHECK_INT(buffer_write(&b, 15, "z", 1), 0);        /* last byte */
	CHECK(b.base[15] == 'z');
	free_buf(&b);
}

static void test_write_rejected_past_capacity(void){
	buffer_t b = make_buf(16);
	uint8_t data[32];
	fill_pattern(data, sizeof data, 5);
	quiet();
	CHECK_INT(buffer_write(&b, 0, data, 17), -1);       /* 1 byte too long */
	CHECK_INT(buffer_write(&b, 16, data, 1), -1);       /* starts at capacity */
	CHECK_INT(buffer_write(&b, 15, data, 2), -1);
	CHECK_INT(buffer_write(&b, 17, data, 1), -1);
	CHECK_INT(buffer_write(&b, 1000, data, 1), -1);
	CHECK_INT(buffer_write(&b, 17, data, 0), -1);       /* offset beyond capacity */
	loud();
	CHECK_SZ(b.len, 0);
	CHECK(all_zero(b.base, 16));
	free_buf(&b);
}

static void test_write_rejected_leaves_state_untouched(void){
	buffer_t b = make_buf(32);
	put(&b, 0, "known!!!");
	uint8_t snap[32], junk[64];
	memcpy(snap, b.base, 32);
	memset(junk, 0xCC, sizeof junk);
	quiet();
	CHECK_INT(buffer_write(&b, 0, junk, 33), -1);
	CHECK_INT(buffer_write(&b, 30, junk, 3), -1);
	CHECK_INT(buffer_write(&b, 33, junk, 1), -1);
	loud();
	CHECK_SZ(b.len, 8);
	CHECK_MEM(b.base, snap, 32);
	free_buf(&b);
}

static void test_write_size_offset_overflow(void){
	buffer_t b = make_buf(16);
	uint8_t one = 0x55;
	quiet();
	CHECK_INT(buffer_write(&b, 8, &one, SIZE_MAX), -1);              /* 8 + SIZE_MAX wraps to 7 */
	CHECK_INT(buffer_write(&b, 1, &one, SIZE_MAX), -1);              /* wraps to 0 */
	CHECK_INT(buffer_write(&b, SIZE_MAX, &one, 1), -1);              /* wraps to 0 */
	CHECK_INT(buffer_write(&b, SIZE_MAX, &one, SIZE_MAX), -1);
	CHECK_INT(buffer_write(&b, SIZE_MAX / 2 + 1, &one, SIZE_MAX / 2 + 1), -1);
	CHECK_INT(buffer_write(&b, 16, &one, SIZE_MAX - 15), -1);
	loud();
	CHECK_SZ(b.len, 0);
	CHECK(all_zero(b.base, 16));
	free_buf(&b);
}

static void test_write_zero_size(void){
	buffer_t b = make_buf(64);
	uint8_t d = 1;
	put(&b, 0, "0123456789");                    /* len = 10 */
	CHECK_INT(buffer_write(&b, 0, &d, 0), 0);
	CHECK_SZ(b.len, 10);
	CHECK_INT(buffer_write(&b, 10, &d, 0), 0);
	CHECK_SZ(b.len, 10);
	/* A zero-byte write far past len must not create a gap / grow len. */
	CHECK_INT(buffer_write(&b, 50, &d, 0), 0);
	CHECK_SZ(b.len, 10);
	free_buf(&b);
}

static void test_write_null_args(void){
	buffer_t b = make_buf(16), z;
	memset(&z, 0, sizeof z);
	quiet();
	CHECK_INT(buffer_write(NULL, 0, "x", 1), -1);
	CHECK_INT(buffer_write(&b, 0, NULL, 1), -1);
	CHECK_INT(buffer_write(&z, 0, "x", 1), -1);          /* base == NULL */
	loud();
	CHECK_SZ(b.len, 0);
	free_buf(&b);
}

static void test_write_const_source(void){
	static const char msg[] = "const data";
	buffer_t b = make_buf(32);
	CHECK_INT(buffer_write(&b, 0, msg, sizeof msg - 1), 0);   /* const void * accepted */
	CHECK_INT(buffer_write(&b, 10, "literal", 7), 0);
	CHECK_MEM(b.base, "const dataliteral", 17);
	free_buf(&b);
}

static void test_write_all_byte_values(void){
	buffer_t b = make_buf(256);
	uint8_t all[256];
	for(int i = 0; i < 256; i++) all[i] = (uint8_t)i;
	CHECK_INT(buffer_write(&b, 0, all, 256), 0);
	CHECK_SZ(b.len, 256);
	CHECK_MEM(b.base, all, 256);                          /* NULs and 0xFF survive */
	free_buf(&b);
}

static void test_write_data_with_embedded_nul(void){
	buffer_t b = make_buf(16);
	const char d[] = {'a', 0, 'b', 0, 'c'};
	CHECK_INT(buffer_write(&b, 0, d, sizeof d), 0);
	CHECK_SZ(b.len, 5);
	CHECK_MEM(b.base, d, 5);
	free_buf(&b);
}

/* ------------------------------------------------------------------ */
/* buffer_empty                                                       */
/* ------------------------------------------------------------------ */

static void test_empty_zeroes_and_resets(void){
	buffer_t b = make_buf(64);
	uint8_t data[64];
	memset(data, 0x7F, sizeof data);
	buffer_write(&b, 0, data, 64);
	uint8_t *base_before = b.base;

	CHECK_INT(buffer_empty(&b), 0);
	CHECK_SZ(b.len, 0);
	CHECK_SZ(b.size, 64);                                 /* capacity kept */
	CHECK(b.base == base_before);                         /* no realloc */
	CHECK(all_zero(b.base, 64));
	free_buf(&b);
}

static void test_empty_then_read_fails_then_write_works(void){
	buffer_t b = make_buf(32);
	put(&b, 0, "data");
	buffer_empty(&b);
	quiet();
	CHECK(buffer_read(&b, 0, 1) == NULL);
	loud();
	CHECK_INT(put(&b, 0, "new"), 0);
	CHECK_SZ(b.len, 3);
	CHECK_MEM(b.base, "new", 3);
	free_buf(&b);
}

static void test_empty_idempotent(void){
	buffer_t b = make_buf(8);
	CHECK_INT(buffer_empty(&b), 0);
	CHECK_INT(buffer_empty(&b), 0);
	CHECK_SZ(b.len, 0);
	free_buf(&b);
}

static void test_empty_null_args(void){
	buffer_t z;
	memset(&z, 0, sizeof z);
	quiet();
	CHECK_INT(buffer_empty(NULL), -1);
	CHECK_INT(buffer_empty(&z), -1);                      /* base == NULL */
	loud();
}

/* ------------------------------------------------------------------ */
/* buffer_delete                                                      */
/* ------------------------------------------------------------------ */

static int g_free_calls;
static void *g_free_arg;
static void counting_free(void *p){
	g_free_calls++;
	g_free_arg = p;
	free(p);
}

static void test_delete_stack_buffer_resets_struct(void){
	buffer_t b = make_buf(32);
	put(&b, 0, "abc");
	CHECK_INT(buffer_delete(&b, false, NULL), 0);
	CHECK(b.base == NULL);
	CHECK_SZ(b.len, 0);
	CHECK_SZ(b.size, 0);
}

static void test_delete_twice_is_safe(void){
	buffer_t b = make_buf(32);
	CHECK_INT(buffer_delete(&b, false, NULL), 0);
	CHECK_INT(buffer_delete(&b, false, NULL), 0);         /* ASan: no double free */
	CHECK(b.base == NULL);
}

static void test_delete_null(void){
	CHECK_INT(buffer_delete(NULL, false, NULL), -1);
	CHECK_INT(buffer_delete(NULL, true, NULL), -1);
}

static void test_delete_heap_allocated_struct(void){
	buffer_t *b = malloc(sizeof *b);
	CHECK(b != NULL);
	if(!b) return;
	CHECK_INT(buffer_init(b, 32), 0);
	CHECK_INT(buffer_delete(b, true, NULL), 0);           /* frees base and the struct */
}

static void test_delete_custom_free_fn(void){
	buffer_t b = make_buf(32);
	uint8_t *base = b.base;
	g_free_calls = 0;
	g_free_arg = NULL;
	CHECK_INT(buffer_delete(&b, false, counting_free), 0);
	CHECK_INT(g_free_calls, 1);
	CHECK(g_free_arg == base);
	CHECK(b.base == NULL);
}

static void test_delete_custom_free_fn_heap_struct(void){
	buffer_t *b = malloc(sizeof *b);
	CHECK(b != NULL);
	if(!b) return;
	buffer_init(b, 32);
	uint8_t *base = b->base;
	g_free_calls = 0;
	g_free_arg = NULL;
	CHECK_INT(buffer_delete(b, true, counting_free), 0);
	CHECK_INT(g_free_calls, 1);                           /* callback frees base only */
	CHECK(g_free_arg == base);
}

static void test_delete_free_fn_not_called_without_base(void){
	buffer_t z;
	memset(&z, 0, sizeof z);
	g_free_calls = 0;
	CHECK_INT(buffer_delete(&z, false, counting_free), 0);
	CHECK_INT(g_free_calls, 0);
}

static void test_ops_after_delete_are_rejected(void){
	buffer_t b = make_buf(32);
	put(&b, 0, "abc");
	buffer_delete(&b, false, NULL);
	quiet();
	CHECK(buffer_read(&b, 0, 1) == NULL);
	CHECK_INT(put(&b, 0, "x"), -1);
	CHECK_INT(buffer_empty(&b), -1);
	CHECK_INT(buffer_fwrite(&b, path_of("after_delete.bin")), -1);
	CHECK_INT(buffer_fread(&b, path_of("after_delete.bin"), 1), -1);
	loud();
	CHECK(!exists(path_of("after_delete.bin")));
}

static void test_reuse_after_delete(void){
	buffer_t b = make_buf(16);
	put(&b, 0, "first");
	free_buf(&b);
	CHECK_INT(buffer_init(&b, 8), 0);
	CHECK_SZ(b.size, 8);
	CHECK_SZ(b.len, 0);
	CHECK(all_zero(b.base, 8));
	free_buf(&b);
}

/* ------------------------------------------------------------------ */
/* buffer_fread(buff, filename, size): read exactly `size` bytes from */
/* the start of the file; fail on short file or size > capacity.      */
/* ------------------------------------------------------------------ */

static void test_fread_basic(void){
	uint8_t data[300];
	fill_pattern(data, sizeof data, 10);
	const char *path = path_of("fread_basic.bin");
	CHECK_INT(write_file(path, data, sizeof data), 0);

	buffer_t b = make_buf(1024);
	CHECK_INT(buffer_fread(&b, path, 300), 0);
	CHECK_SZ(b.len, 300);
	CHECK_SZ(b.size, 1024);
	CHECK_MEM(b.base, data, 300);
	free_buf(&b);
}

static void test_fread_prefix_of_file(void){
	uint8_t data[300];
	fill_pattern(data, sizeof data, 16);
	const char *path = path_of("fread_prefix.bin");
	write_file(path, data, sizeof data);

	buffer_t b = make_buf(1024);
	CHECK_INT(buffer_fread(&b, path, 100), 0);            /* file is longer than size: fine */
	CHECK_SZ(b.len, 100);
	CHECK_MEM(b.base, data, 100);
	free_buf(&b);
}

static void test_fread_size_exactly_fills_buffer(void){
	uint8_t data[128];
	fill_pattern(data, sizeof data, 11);
	const char *path = path_of("fread_exact.bin");
	write_file(path, data, sizeof data);

	buffer_t b = make_buf(128);
	CHECK_INT(buffer_fread(&b, path, 128), 0);
	CHECK_SZ(b.len, 128);
	CHECK_MEM(b.base, data, 128);
	free_buf(&b);
}

/* size larger than the buffer's capacity must be rejected *before* reading,
   otherwise fread() writes past the end of base. Done in a child so that a
   heap overflow shows up as a failed test instead of killing the whole run
   (rerun with VERBOSE=1 to see the sanitizer report). */
static void child_fread_size_over_capacity(void){
	uint8_t *data = malloc(1u << 20);
	if(!data){ CHECK(0); return; }
	fill_pattern(data, 1u << 20, 12);
	const char *path = path_of("fread_big.bin");
	write_file(path, data, 1u << 20);
	free(data);

	buffer_t b = make_buf(64);
	put(&b, 0, "OLD");
	size_t sizes[] = {65, 4096, 1u << 20, SIZE_MAX};
	for(size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++){
		quiet();
		int rc = buffer_fread(&b, path, sizes[i]);
		loud();
		CHECKF(rc == -1, "size=%zu rc=%d", sizes[i], rc);
		CHECKF(b.len == 3 && memcmp(b.base, "OLD", 3) == 0, "size=%zu: buffer modified", sizes[i]);
	}
	free_buf(&b);
}

static void test_fread_size_over_capacity_is_rejected(void){
	int st = run_in_child(child_fread_size_over_capacity);
	CHECKF(st == 0, "child exit status %d (>=128: killed by signal, 1: sanitizer abort or failed checks)", st);
}

static void test_fread_size_zero_is_invalid(void){
	const char *path = path_of("fread_zero.bin");
	write_file(path, "abc", 3);
	buffer_t b = make_buf(32);
	put(&b, 0, "keep");
	quiet();
	CHECK_INT(buffer_fread(&b, path, 0), -1);
	loud();
	CHECK_SZ(b.len, 4);
	CHECK_MEM(b.base, "keep", 4);
	free_buf(&b);
}

static void test_fread_file_shorter_than_size(void){
	const char *path = path_of("fread_short.bin");
	write_file(path, "0123456789", 10);
	buffer_t b = make_buf(64);
	put(&b, 0, "OLD");
	quiet();
	CHECK_INT(buffer_fread(&b, path, 11), -1);            /* one byte more than the file has */
	CHECK_INT(buffer_fread(&b, path, 64), -1);
	loud();
	CHECK_SZ(b.len, 0);                                   /* partial data is never exposed */
	quiet();
	CHECK(buffer_read(&b, 0, 1) == NULL);
	loud();
	free_buf(&b);
}

static void test_fread_empty_file_fails_for_nonzero_size(void){
	const char *path = path_of("fread_empty.bin");
	CHECK_INT(write_file(path, NULL, 0), 0);
	buffer_t b = make_buf(32);
	quiet();
	CHECK_INT(buffer_fread(&b, path, 1), -1);
	loud();
	CHECK_SZ(b.len, 0);
	free_buf(&b);
}

static void test_fread_replaces_previous_contents(void){
	uint8_t big[50], small[10];
	memset(big, 0x11, sizeof big);
	fill_pattern(small, sizeof small, 14);
	const char *path = path_of("fread_small.bin");
	write_file(path, small, sizeof small);

	buffer_t b = make_buf(64);
	buffer_write(&b, 0, big, 50);
	CHECK_INT(buffer_fread(&b, path, 10), 0);
	CHECK_SZ(b.len, 10);                                  /* len follows the read, not old data */
	CHECK_MEM(b.base, small, 10);
	quiet();
	CHECK(buffer_read(&b, 10, 1) == NULL);                /* old bytes are not exposed */
	loud();
	free_buf(&b);
}

static void test_fread_binary_content(void){
	uint8_t all[512];
	for(int i = 0; i < 512; i++) all[i] = (uint8_t)i;
	const char *path = path_of("fread_binary.bin");
	write_file(path, all, sizeof all);
	buffer_t b = make_buf(512);
	CHECK_INT(buffer_fread(&b, path, 512), 0);
	CHECK_SZ(b.len, 512);
	CHECK_MEM(b.base, all, 512);                          /* NUL bytes, 0x1A, 0xFF, \r\n ... */
	free_buf(&b);
}

static void test_fread_missing_file(void){
	buffer_t b = make_buf(32);
	put(&b, 0, "keep");
	quiet();
	CHECK_INT(buffer_fread(&b, path_of("does_not_exist.bin"), 4), -1);
	loud();
	CHECK_SZ(b.len, 4);
	CHECK_MEM(b.base, "keep", 4);
	free_buf(&b);
}

static void test_fread_null_args(void){
	buffer_t b = make_buf(32), z;
	memset(&z, 0, sizeof z);
	const char *path = path_of("fread_null.bin");
	write_file(path, "abc", 3);
	quiet();
	CHECK_INT(buffer_fread(NULL, path, 3), -1);
	CHECK_INT(buffer_fread(&b, NULL, 3), -1);
	CHECK_INT(buffer_fread(&z, path, 3), -1);             /* base == NULL */
	loud();
	CHECK_SZ(b.len, 0);
	free_buf(&b);
}

static void test_fread_large_file(void){
	size_t n = 1u << 20;
	uint8_t *data = malloc(n);
	CHECK(data != NULL);
	if(!data) return;
	fill_pattern(data, n, 15);
	const char *path = path_of("fread_1mib.bin");
	write_file(path, data, n);

	buffer_t b = make_buf(n);
	CHECK_INT(buffer_fread(&b, path, n), 0);
	CHECK_SZ(b.len, n);
	CHECK_MEM(b.base, data, n);
	free_buf(&b);
	free(data);
}

static void test_fread_special_file(void){
	/* /dev/zero has no size at all; reading a prefix of it must just work */
	if(!exists("/dev/zero"))
		SKIP("/dev/zero not available");
	buffer_t b = make_buf(4096);
	memset(b.base, 0xFF, 4096);
	CHECK_INT(buffer_fread(&b, "/dev/zero", 1000), 0);
	CHECK_SZ(b.len, 1000);
	CHECK(all_zero(b.base, 1000));
	free_buf(&b);
}

static void test_fread_directory_is_rejected(void){
	const char *dir = path_of("a_directory_for_fread");
	CHECK(mkdir(dir, 0700) == 0 || errno == EEXIST);
	buffer_t b = make_buf(65536);
	quiet();
	int rc = buffer_fread(&b, dir, 16);
	loud();
	CHECK_INT(rc, -1);
	CHECK_SZ(b.len, 0);
	free_buf(&b);
}

/* ------------------------------------------------------------------ */
/* buffer_fwrite                                                      */
/* ------------------------------------------------------------------ */

static void test_fwrite_roundtrip(void){
	uint8_t data[100];
	fill_pattern(data, sizeof data, 20);
	buffer_t b = make_buf(4096);
	buffer_write(&b, 0, data, sizeof data);

	const char *path = path_of("fwrite_basic.bin");
	CHECK_INT(buffer_fwrite(&b, path), 0);
	CHECK_INT(file_size_of(path), 100);                   /* len bytes, not capacity */

	size_t n = 0;
	uint8_t *got = slurp(path, &n);
	CHECK(got != NULL);
	CHECK_SZ(n, 100);
	if(got) CHECK_MEM(got, data, 100);
	free(got);
	free_buf(&b);
}

static void test_fwrite_does_not_modify_buffer(void){
	buffer_t b = make_buf(64);
	put(&b, 0, "unchanged");
	uint8_t snap[64];
	memcpy(snap, b.base, 64);
	uint8_t *base = b.base;
	CHECK_INT(buffer_fwrite(&b, path_of("fwrite_const.bin")), 0);
	CHECK(b.base == base);
	CHECK_SZ(b.len, 9);
	CHECK_SZ(b.size, 64);
	CHECK_MEM(b.base, snap, 64);
	free_buf(&b);
}

static void test_fwrite_empty_buffer_makes_empty_file(void){
	buffer_t b = make_buf(64);
	const char *path = path_of("fwrite_empty.bin");
	CHECK_INT(buffer_fwrite(&b, path), 0);
	CHECK(exists(path));
	CHECK_INT(file_size_of(path), 0);
	free_buf(&b);
}

static void test_fwrite_replaces_existing_longer_file(void){
	const char *path = path_of("fwrite_replace.bin");
	uint8_t old[1000];
	memset(old, 'O', sizeof old);
	write_file(path, old, sizeof old);

	buffer_t b = make_buf(64);
	put(&b, 0, "short");
	CHECK_INT(buffer_fwrite(&b, path), 0);
	CHECK_INT(file_size_of(path), 5);                     /* truncated, no leftover tail */

	size_t n = 0;
	uint8_t *got = slurp(path, &n);
	CHECK(got != NULL && n == 5 && memcmp(got, "short", 5) == 0);
	free(got);
	free_buf(&b);
}

static void test_fwrite_binary_content(void){
	uint8_t all[256];
	for(int i = 0; i < 256; i++) all[i] = (uint8_t)i;
	buffer_t b = make_buf(256);
	buffer_write(&b, 0, all, 256);
	const char *path = path_of("fwrite_binary.bin");
	CHECK_INT(buffer_fwrite(&b, path), 0);
	size_t n = 0;
	uint8_t *got = slurp(path, &n);
	CHECK(got != NULL && n == 256 && memcmp(got, all, 256) == 0);
	free(got);
	free_buf(&b);
}

static void test_fwrite_bad_path(void){
	buffer_t b = make_buf(16);
	put(&b, 0, "data");
	quiet();
	CHECK_INT(buffer_fwrite(&b, path_of("no_such_dir/file.bin")), -1);
	loud();
	CHECK(!exists(path_of("no_such_dir")));
	free_buf(&b);
}

static void test_fwrite_target_is_directory(void){
	const char *dir = path_of("a_directory_for_fwrite");
	CHECK(mkdir(dir, 0700) == 0 || errno == EEXIST);
	buffer_t b = make_buf(16);
	put(&b, 0, "data");
	quiet();
	CHECK_INT(buffer_fwrite(&b, dir), -1);
	loud();
	struct stat st;
	CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode));    /* directory not removed */
	free_buf(&b);
}

static void test_fwrite_null_args(void){
	buffer_t b = make_buf(16), z;
	memset(&z, 0, sizeof z);
	put(&b, 0, "abc");
	quiet();
	CHECK_INT(buffer_fwrite(NULL, path_of("fwrite_null.bin")), -1);
	CHECK_INT(buffer_fwrite(&b, NULL), -1);
	CHECK_INT(buffer_fwrite(&z, path_of("fwrite_null.bin")), -1);   /* base == NULL */
	loud();
	CHECK(!exists(path_of("fwrite_null.bin")));
	free_buf(&b);
}

static void test_fwrite_then_fread_roundtrip_sizes(void){
	/* sizes around common stdio buffer boundaries */
	size_t sizes[] = {0, 1, 2, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536, 65537, 1000003};
	for(size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++){
		size_t n = sizes[i];
		uint8_t *data = malloc(n ? n : 1);
		CHECK(data != NULL);
		if(!data) return;
		fill_pattern(data, n, (uint32_t)(100 + i));

		buffer_t src = make_buf(n ? n : 1);
		if(n) CHECK_INT(buffer_write(&src, 0, data, n), 0);
		const char *path = path_of("roundtrip.bin");
		CHECKF(buffer_fwrite(&src, path) == 0, "n=%zu", n);
		CHECKF(file_size_of(path) == (long long)n, "n=%zu file=%lld", n, file_size_of(path));

		buffer_t dst = make_buf(n ? n : 1);
		if(n){                              /* fread(size = 0) is invalid by design */
			CHECKF(buffer_fread(&dst, path, n) == 0, "n=%zu", n);
			CHECKF(dst.len == n, "n=%zu len=%zu", n, dst.len);
			CHECKF(memcmp(dst.base, data, n) == 0, "n=%zu", n);
		}

		free_buf(&src);
		free_buf(&dst);
		free(data);
		if(g_test_failed) break;
	}
}

static void test_fwrite_roundtrip_through_larger_buffer(void){
	uint8_t data[1000];
	fill_pattern(data, sizeof data, 30);
	buffer_t a = make_buf(1000), b = make_buf(5000);
	buffer_write(&a, 0, data, 1000);
	const char *path = path_of("rt_larger.bin");
	CHECK_INT(buffer_fwrite(&a, path), 0);
	CHECK_INT(buffer_fread(&b, path, 1000), 0);
	CHECK_SZ(b.len, 1000);
	CHECK_MEM(b.base, data, 1000);
	CHECK(all_zero(b.base + 1000, 4000));
	free_buf(&a);
	free_buf(&b);
}

/* --- write failures, simulated with RLIMIT_FSIZE (SIGXFSZ ignored) --- */

static void child_fwrite_fclose_fails(void){
	signal(SIGXFSZ, SIG_IGN);
	struct rlimit old, rl;
	getrlimit(RLIMIT_FSIZE, &old);
	rl = old;
	rl.rlim_cur = 100;
	CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);

	/* 1000 bytes fit in stdio's buffer, so fwrite() "succeeds" and the
	   failure only shows up when fclose() flushes. */
	buffer_t b = make_buf(2048);
	uint8_t data[1000];
	fill_pattern(data, sizeof data, 40);
	buffer_write(&b, 0, data, sizeof data);

	quiet();
	int rc = buffer_fwrite(&b, path_of("fsize_small.bin"));
	loud();
	rl.rlim_cur = old.rlim_cur;
	setrlimit(RLIMIT_FSIZE, &rl);
	CHECK_INT(rc, -1);
	free_buf(&b);
}

static void child_fwrite_short_write(void){
	signal(SIGXFSZ, SIG_IGN);
	struct rlimit old, rl;
	getrlimit(RLIMIT_FSIZE, &old);
	rl = old;
	rl.rlim_cur = 100;
	CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);

	buffer_t b = make_buf(200000);
	uint8_t *data = malloc(200000);
	if(!data){ free_buf(&b); return; }
	fill_pattern(data, 200000, 41);
	buffer_write(&b, 0, data, 200000);

	quiet();
	int rc = buffer_fwrite(&b, path_of("fsize_large.bin"));
	loud();
	rl.rlim_cur = old.rlim_cur;
	setrlimit(RLIMIT_FSIZE, &rl);
	CHECK_INT(rc, -1);
	CHECK(!exists(path_of("fsize_large.bin")));            /* partial file removed */
	free(data);
	free_buf(&b);
}

static void test_fwrite_reports_error_surfacing_at_fclose(void){
	CHECK_INT(run_in_child(child_fwrite_fclose_fails), 0);
}

static void test_fwrite_reports_short_write(void){
	CHECK_INT(run_in_child(child_fwrite_short_write), 0);
}

/* ------------------------------------------------------------------ */
/* Randomized / stress                                                */
/* ------------------------------------------------------------------ */

/* Compare the buffer against a trivially-correct shadow model after every
   operation. Writes use size >= 1; zero-size writes have their own test. */
static void test_random_ops_match_model(void){
	enum { CAP = 512, ITER = 30000 };
	buffer_t b = make_buf(CAP);
	uint8_t model[CAP];
	size_t mlen = 0;
	memset(model, 0, sizeof model);
	g_rng = g_seed ? g_seed : 1;

	quiet();
	for(int it = 0; it < ITER && !g_test_failed; it++){
		unsigned op = (unsigned)(rnd() % 100);
		if(op < 55){
			size_t off = rnd_range(0, CAP + 8), sz = rnd_range(1, 80);
			uint8_t data[80];
			fill_pattern(data, sz, (uint32_t)rnd());
			int ok = off <= CAP && sz <= CAP - off;
			int rc = buffer_write(&b, off, data, sz);
			CHECKF(rc == (ok ? 0 : -1), "iter %d write off=%zu sz=%zu rc=%d seed=%llu",
			       it, off, sz, rc, (unsigned long long)g_seed);
			if(ok){
				memcpy(model + off, data, sz);
				if(off + sz > mlen) mlen = off + sz;
			}
		}else if(op < 95){
			size_t off = rnd_range(0, CAP + 8), sz = rnd_range(0, 80);
			const uint8_t *p = buffer_read(&b, off, sz);
			int ok = off <= mlen && sz <= mlen - off;
			CHECKF((p != NULL) == ok, "iter %d read off=%zu sz=%zu len=%zu seed=%llu",
			       it, off, sz, mlen, (unsigned long long)g_seed);
			if(p && ok)
				CHECKF(memcmp(p, model + off, sz) == 0, "iter %d read content seed=%llu",
				       it, (unsigned long long)g_seed);
		}else if(op < 97){
			CHECK_INT(buffer_empty(&b), 0);
			memset(model, 0, sizeof model);
			mlen = 0;
		}
		CHECKF(b.len == mlen, "iter %d len=%zu model=%zu seed=%llu",
		       it, b.len, mlen, (unsigned long long)g_seed);
		CHECK(b.len <= b.size);
		if(it % 64 == 0)
			CHECKF(memcmp(b.base, model, CAP) == 0, "iter %d full compare seed=%llu",
			       it, (unsigned long long)g_seed);
	}
	loud();
	free_buf(&b);
}

static void test_random_file_roundtrips(void){
	g_rng = g_seed ? g_seed ^ 0xABCDEFu : 1;
	for(int it = 0; it < 200 && !g_test_failed; it++){
		size_t n = rnd_range(0, 20000);
		uint8_t *data = malloc(n ? n : 1);
		if(!data) return;
		fill_pattern(data, n, (uint32_t)rnd());

		buffer_t a = make_buf(n ? n : 1);
		if(n) buffer_write(&a, 0, data, n);
		const char *path = path_of("random_rt.bin");
		CHECKF(buffer_fwrite(&a, path) == 0, "iter %d n=%zu", it, n);

		size_t cap = n + rnd_range(0, 64);
		buffer_t c = make_buf(cap ? cap : 1);
		if(n){
			CHECKF(buffer_fread(&c, path, n) == 0, "iter %d n=%zu cap=%zu", it, n, cap);
			CHECKF(c.len == n, "iter %d n=%zu len=%zu", it, n, c.len);
			CHECKF(memcmp(c.base, data, n) == 0, "iter %d n=%zu", it, n);
		}

		free_buf(&a);
		free_buf(&c);
		free(data);
	}
}

static void test_stress_init_delete_loop(void){
	for(int i = 0; i < 20000; i++){
		size_t n = 1 + (size_t)(i * 37) % 9000;
		buffer_t b;
		memset(&b, 0, sizeof b);
		if(buffer_init(&b, n) != 0){ CHECK(0); return; }
		b.base[n - 1] = 1;
		buffer_write(&b, 0, "x", 1);
		buffer_delete(&b, false, NULL);
	}
	CHECK(1);
}

static void test_many_live_buffers(void){
	enum { N = 128 };
	buffer_t bufs[N];
	for(int i = 0; i < N; i++){
		memset(&bufs[i], 0, sizeof bufs[i]);
		CHECK_INT(buffer_init(&bufs[i], 100 + (size_t)i), 0);
		uint8_t d[100];
		memset(d, i, sizeof d);
		CHECK_INT(buffer_write(&bufs[i], 0, d, sizeof d), 0);
	}
	for(int i = 0; i < N; i++){          /* no buffer corrupted a neighbour */
		const uint8_t *p = buffer_read(&bufs[i], 0, 100);
		int ok = p != NULL;
		for(int j = 0; ok && j < 100; j++)
			ok = p[j] == (uint8_t)i;
		CHECKF(ok, "buffer %d corrupted", i);
		free_buf(&bufs[i]);
	}
}

/* ------------------------------------------------------------------ */

int main(void){
	const char *s = getenv("SEED");
	if(s && *s) g_seed = strtoull(s, NULL, 0);
	make_tmpdir();
	atexit(cleanup_tmpdir);
	printf("buffer library tests (seed=%llu)\n\n", (unsigned long long)g_seed);

	/* init */
	RUN(test_init_basic);
	RUN(test_init_various_sizes);
	RUN(test_init_null_buffer);
	RUN(test_init_invalid_sizes_rejected);
	RUN(test_init_failure_leaves_clean_state);
	RUN(test_init_max_size);
	RUN(test_init_allocation_failure);
	/* read */
	RUN(test_read_basic);
	RUN(test_read_pointer_aliases_storage);
	RUN(test_read_out_of_range);
	RUN(test_read_limited_by_len_not_capacity);
	RUN(test_read_size_overflow);
	RUN(test_read_zero_size_inside_range);
	RUN(test_read_at_len_boundary);
	RUN(test_read_empty_buffer);
	RUN(test_read_null_args);
	/* write */
	RUN(test_write_basic_and_len);
	RUN(test_write_middle_overwrite_keeps_len);
	RUN(test_write_overlapping_end_extends_len);
	RUN(test_write_append_exactly_at_len);
	RUN(test_write_gap_reads_as_zero);
	RUN(test_write_fills_capacity_exactly);
	RUN(test_write_rejected_past_capacity);
	RUN(test_write_rejected_leaves_state_untouched);
	RUN(test_write_size_offset_overflow);
	RUN(test_write_zero_size);
	RUN(test_write_null_args);
	RUN(test_write_const_source);
	RUN(test_write_all_byte_values);
	RUN(test_write_data_with_embedded_nul);
	/* empty */
	RUN(test_empty_zeroes_and_resets);
	RUN(test_empty_then_read_fails_then_write_works);
	RUN(test_empty_idempotent);
	RUN(test_empty_null_args);
	/* delete */
	RUN(test_delete_stack_buffer_resets_struct);
	RUN(test_delete_twice_is_safe);
	RUN(test_delete_null);
	RUN(test_delete_heap_allocated_struct);
	RUN(test_delete_custom_free_fn);
	RUN(test_delete_custom_free_fn_heap_struct);
	RUN(test_delete_free_fn_not_called_without_base);
	RUN(test_ops_after_delete_are_rejected);
	RUN(test_reuse_after_delete);
	/* fread */
	RUN(test_fread_basic);
	RUN(test_fread_prefix_of_file);
	RUN(test_fread_size_exactly_fills_buffer);
	RUN(test_fread_size_over_capacity_is_rejected);
	RUN(test_fread_size_zero_is_invalid);
	RUN(test_fread_file_shorter_than_size);
	RUN(test_fread_empty_file_fails_for_nonzero_size);
	RUN(test_fread_replaces_previous_contents);
	RUN(test_fread_binary_content);
	RUN(test_fread_missing_file);
	RUN(test_fread_null_args);
	RUN(test_fread_large_file);
	RUN(test_fread_special_file);
	RUN(test_fread_directory_is_rejected);
	/* fwrite */
	RUN(test_fwrite_roundtrip);
	RUN(test_fwrite_does_not_modify_buffer);
	RUN(test_fwrite_empty_buffer_makes_empty_file);
	RUN(test_fwrite_replaces_existing_longer_file);
	RUN(test_fwrite_binary_content);
	RUN(test_fwrite_bad_path);
	RUN(test_fwrite_target_is_directory);
	RUN(test_fwrite_null_args);
	RUN(test_fwrite_then_fread_roundtrip_sizes);
	RUN(test_fwrite_roundtrip_through_larger_buffer);
	RUN(test_fwrite_reports_error_surfacing_at_fclose);
	RUN(test_fwrite_reports_short_write);
	/* randomized / stress */
	RUN(test_random_ops_match_model);
	RUN(test_random_file_roundtrips);
	RUN(test_stress_init_delete_loop);
	RUN(test_many_live_buffers);

	printf("\n%d checks, %d failed, %d test(s) skipped\n", g_checks, g_failed, g_skipped);
	return g_failed ? 1 : 0;
}
