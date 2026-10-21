/*
 * test_util_file.c — unit tests for atomic file output (mkstemp beside the destination + rename)
 * and whole-file reads.
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    PATH_BUF = 512,
    MSG_MAX = 1024,
    TEXT_MAX = 256,
    MODE_BITS = 0777,
    PRIVATE_MODE = 0600,
    SMALL_LIMIT = 64, /* RLIMIT_FSIZE bytes for the write-failure test */
    BIG_WRITE = 4096, /* bytes written past it (buffered: the failure shows at close) */
    READ_CAP = 16     /* max_bytes for the small read tests */
};

static char out_dir[PATH_BUF];
static char out_path[PATH_BUF + 16];
static char last_error[MSG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

/* Number of directory entries other than "." and "..". */
static int dir_entries(const char *dir) {
    DIR *handle = opendir(dir);
    TEST_ASSERT_NOT_NULL(handle);
    int count = 0;
    for (const struct dirent *ent = readdir(handle); ent != NULL; ent = readdir(handle)) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            count++;
        }
    }
    (void)closedir(handle);
    return count;
}

static void slurp(const char *path, char *buf) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    size_t got = fread(buf, 1, TEXT_MAX - 1, fp);
    buf[got] = '\0';
    (void)fclose(fp);
}

static void put_old(const char *text) {
    FILE *fp = fopen(out_path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs(text, fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

void setUp(void) {
    last_error[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    const char *base = getenv("TMPDIR");
    (void)snprintf(out_dir, sizeof out_dir, "%s/odin3_file_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(out_dir));
    (void)snprintf(out_path, sizeof out_path, "%s/out.txt", out_dir);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    (void)remove(out_path);
    TEST_ASSERT_EQUAL_INT(0, rmdir(out_dir));
}

static void test_commit_writes_destination_only(void) {
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_NOT_NULL(file.fp);
    TEST_ASSERT_TRUE(fputs("hello\n", file.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir)); /* the temporary only */
    TEST_ASSERT_NOT_EQUAL_INT(0, access(out_path, F_OK));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&file, ODIN3_OK));
    TEST_ASSERT_NULL(file.fp);
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("hello\n", text);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
}

/* A new file gets 0666 minus the umask, as fopen would give it, not mkstemp's 0600. */
static void test_new_file_mode_follows_umask(void) {
    mode_t mask = umask(022);
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&file, ODIN3_OK));
    (void)umask(mask);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, stat(out_path, &info));
    TEST_ASSERT_EQUAL_UINT(0644, info.st_mode & MODE_BITS);
}

/* Replacing a file keeps that file's permission bits. */
static void test_replace_keeps_mode(void) {
    put_old("old");
    TEST_ASSERT_EQUAL_INT(0, chmod(out_path, PRIVATE_MODE));
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_TRUE(fputs("new", file.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&file, ODIN3_OK));
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, stat(out_path, &info));
    TEST_ASSERT_EQUAL_UINT(PRIVATE_MODE, info.st_mode & MODE_BITS);
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("new", text);
}

/* Closing with a failure status keeps the old destination and removes the temporary file. */
static void test_failure_status_discards(void) {
    put_old("old");
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_TRUE(fputs("partial", file.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_atomic_file_close(&file, ODIN3_ERR_INVALID_ARG));
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("old", text);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
}

/* finish keeps the temporary and leaves the destination alone; commit renames it (OK) or removes
 * it (any failure status). */
static void test_finish_then_commit(void) {
    put_old("old");
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_TRUE(fputs("new", file.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_finish(&file, ODIN3_OK));
    TEST_ASSERT_NULL(file.fp);
    TEST_ASSERT_EQUAL_INT(2, dir_entries(out_dir)); /* old and the finished temporary */
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("old", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_commit(&file, ODIN3_OK));
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("new", text);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
}

static void test_finish_then_commit_failure(void) {
    put_old("old");
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_TRUE(fputs("new", file.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_finish(&file, ODIN3_OK));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_atomic_file_commit(&file, ODIN3_ERR_IO));
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("old", text);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
}

/* Two open temporaries for one destination never share a name. */
static void test_concurrent_temporaries_differ(void) {
    odin3_atomic_file first;
    odin3_atomic_file second;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&first, out_path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&second, out_path));
    TEST_ASSERT_EQUAL_INT(2, dir_entries(out_dir));
    TEST_ASSERT_TRUE(fputs("one", first.fp) >= 0);
    TEST_ASSERT_TRUE(fputs("two", second.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&first, ODIN3_OK));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&second, ODIN3_OK));
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("two", text);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
}

/* A pre-existing "<path>.tmp" (a symlink here) is neither followed nor overwritten. */
static void test_old_tmp_name_untouched(void) {
    char tmp[PATH_BUF + 32];
    (void)snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    TEST_ASSERT_EQUAL_INT(0, symlink("/nonexistent-target", tmp));
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&file, ODIN3_OK));
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, lstat(tmp, &info));
    TEST_ASSERT_TRUE(S_ISLNK(info.st_mode));
    TEST_ASSERT_EQUAL_INT(0, remove(tmp));
}

/* A write past RLIMIT_FSIZE (EFBIG, SIGXFSZ ignored) fails at the flush in close: IO, logged
 * "write failed", the temporary removed and the old destination intact. */
static void test_write_failure_at_close(void) {
    put_old("old");
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    struct rlimit old;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_FSIZE, &old));
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    struct rlimit small = {SMALL_LIMIT, old.rlim_max};
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &small));
    for (int i = 0; i < BIG_WRITE; i++) {
        (void)fputc('x', file.fp);
    }
    odin3_status st = odin3_atomic_file_close(&file, ODIN3_OK);
    (void)setrlimit(RLIMIT_FSIZE, &old);
    (void)signal(SIGXFSZ, prev);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
    TEST_ASSERT_NOT_NULL(strstr(last_error, "write failed"));
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("old", text);
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
}

static void test_open_failure_is_located(void) {
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_atomic_file_open(&file, "/nonexistent-dir/out.txt"));
    TEST_ASSERT_NULL(file.fp);
    TEST_ASSERT_NOT_NULL(strstr(last_error, "/nonexistent-dir/out.txt: cannot open"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_atomic_file_open(&file, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_atomic_file_open(&file, ""));
}

/* A destination that is a directory cannot be renamed over: IO, temporary removed. */
static void test_rename_failure_cleans_temp(void) {
    TEST_ASSERT_EQUAL_INT(0, mkdir(out_path, 0700));
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, out_path));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_atomic_file_close(&file, ODIN3_OK));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "cannot rename"));
    TEST_ASSERT_EQUAL_INT(1, dir_entries(out_dir));
    TEST_ASSERT_EQUAL_INT(0, rmdir(out_path));
}

/* A destination without a directory part is created in the current directory. */
static void test_bare_name(void) {
    char cwd[PATH_BUF];
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
    TEST_ASSERT_EQUAL_INT(0, chdir(out_dir));
    odin3_atomic_file file;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_open(&file, "out.txt"));
    TEST_ASSERT_TRUE(fputs("bare", file.fp) >= 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_atomic_file_close(&file, ODIN3_OK));
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
    char text[TEXT_MAX];
    slurp(out_path, text);
    TEST_ASSERT_EQUAL_STRING("bare", text);
}

static void test_out_of_memory(void) {
    odin3_atomic_file file;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_atomic_file_open(&file, out_path));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_NULL(file.fp);
    TEST_ASSERT_EQUAL_INT(0, dir_entries(out_dir));
}

/* --- odin3_file_read_all ------------------------------------------------------------------- */

static void put_bytes(const char *text, size_t len) {
    FILE *fp = fopen(out_path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(len, fwrite(text, 1, len, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

static void test_read_all_bytes(void) {
    static const char text[] = "a\rb\r\nc\0d";
    put_bytes(text, sizeof text - 1);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(out_path, READ_CAP, &bytes));
    TEST_ASSERT_EQUAL_size_t(sizeof text - 1, bytes.len);
    TEST_ASSERT_EQUAL_MEMORY(text, bytes.data, sizeof text - 1);
    TEST_ASSERT_FALSE(bytes.too_big);
    TEST_ASSERT_EQUAL_INT(0, bytes.err);
    odin3_file_bytes_free(&bytes);
    TEST_ASSERT_NULL(bytes.data);
    TEST_ASSERT_EQUAL_size_t(0, bytes.len);
    odin3_file_bytes_free(&bytes); /* twice is harmless */
}

static void test_read_all_empty_file(void) {
    put_bytes("", 0);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(out_path, READ_CAP, &bytes));
    TEST_ASSERT_EQUAL_size_t(0, bytes.len);
    odin3_file_bytes_free(&bytes);
}

/* Exactly max_bytes is accepted; one more is PARSE with too_big, and nothing is logged. */
static void test_read_all_cap(void) {
    put_bytes("0123456789abcdef", READ_CAP);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(out_path, READ_CAP, &bytes));
    TEST_ASSERT_EQUAL_size_t(READ_CAP, bytes.len);
    odin3_file_bytes_free(&bytes);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_file_read_all(out_path, READ_CAP - 1, &bytes));
    TEST_ASSERT_TRUE(bytes.too_big);
    TEST_ASSERT_NULL(bytes.data);
    TEST_ASSERT_EQUAL_STRING("", last_error);
}

/* A sparse file of 2^28 + 1 bytes is rejected from its size alone: allocations are capped low
 * so that a buffer of its size would fail as NO_MEMORY instead of PARSE. */
static void test_read_all_sparse_over_limit(void) {
    const off_t size = (off_t)((UINT64_C(1) << 28) + 1);
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, PRIVATE_MODE);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, size));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    odin3_file_bytes bytes;
    odin3_util_set_alloc_fail_after(0);
    odin3_status st = odin3_file_read_all(out_path, (size_t)1 << 28, &bytes);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_TRUE(bytes.too_big);
    TEST_ASSERT_EQUAL_size_t((size_t)size, bytes.len);
    TEST_ASSERT_NULL(bytes.data);
}

static void test_read_all_directory_and_missing(void) {
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_file_read_all(out_dir, READ_CAP, &bytes));
    TEST_ASSERT_EQUAL_INT(EISDIR, bytes.err);
    TEST_ASSERT_NULL(bytes.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_file_read_all(out_path, READ_CAP, &bytes));
    TEST_ASSERT_EQUAL_INT(ENOENT, bytes.err);
    TEST_ASSERT_EQUAL_STRING("", last_error);
}

/* An existing file without read permission is IO with EACCES (skipped when running as root,
 * which reads it anyway). */
static void test_read_all_unreadable(void) {
    put_bytes("x", 1);
    TEST_ASSERT_EQUAL_INT(0, chmod(out_path, 0));
    odin3_file_bytes bytes;
    odin3_status st = odin3_file_read_all(out_path, READ_CAP, &bytes);
    if (geteuid() == 0) {
        odin3_file_bytes_free(&bytes);
        TEST_IGNORE_MESSAGE("root reads unreadable files");
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
    TEST_ASSERT_EQUAL_INT(EACCES, bytes.err);
}

static void test_read_all_out_of_memory(void) {
    put_bytes("abc", 3);
    odin3_file_bytes bytes;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_file_read_all(out_path, READ_CAP, &bytes));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_NULL(bytes.data);
}

/* A pipe has no size: read in growing chunks until EOF, still capped. */
static void test_read_all_pipe(void) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));
    TEST_ASSERT_EQUAL_INT(5, (int)write(fds[1], "hello", 5));
    TEST_ASSERT_EQUAL_INT(0, close(fds[1]));
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "/dev/fd/%d", fds[0]);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(path, READ_CAP, &bytes));
    TEST_ASSERT_EQUAL_size_t(5, bytes.len);
    TEST_ASSERT_EQUAL_MEMORY("hello", bytes.data, 5);
    odin3_file_bytes_free(&bytes);
    TEST_ASSERT_EQUAL_INT(0, close(fds[0]));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_commit_writes_destination_only);
    RUN_TEST(test_new_file_mode_follows_umask);
    RUN_TEST(test_replace_keeps_mode);
    RUN_TEST(test_failure_status_discards);
    RUN_TEST(test_finish_then_commit);
    RUN_TEST(test_finish_then_commit_failure);
    RUN_TEST(test_concurrent_temporaries_differ);
    RUN_TEST(test_old_tmp_name_untouched);
    RUN_TEST(test_write_failure_at_close);
    RUN_TEST(test_open_failure_is_located);
    RUN_TEST(test_rename_failure_cleans_temp);
    RUN_TEST(test_bare_name);
    RUN_TEST(test_out_of_memory);
    RUN_TEST(test_read_all_bytes);
    RUN_TEST(test_read_all_empty_file);
    RUN_TEST(test_read_all_cap);
    RUN_TEST(test_read_all_sparse_over_limit);
    RUN_TEST(test_read_all_directory_and_missing);
    RUN_TEST(test_read_all_unreadable);
    RUN_TEST(test_read_all_out_of_memory);
    RUN_TEST(test_read_all_pipe);
    return UNITY_END();
}
