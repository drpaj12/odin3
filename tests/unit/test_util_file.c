/*
 * test_util_file.c — unit tests for atomic file output (mkstemp beside the destination + rename).
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PATH_BUF = 512, MSG_MAX = 1024, TEXT_MAX = 256, MODE_BITS = 0777, PRIVATE_MODE = 0600 };

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_commit_writes_destination_only);
    RUN_TEST(test_new_file_mode_follows_umask);
    RUN_TEST(test_replace_keeps_mode);
    RUN_TEST(test_failure_status_discards);
    RUN_TEST(test_concurrent_temporaries_differ);
    RUN_TEST(test_old_tmp_name_untouched);
    RUN_TEST(test_open_failure_is_located);
    RUN_TEST(test_rename_failure_cleans_temp);
    RUN_TEST(test_bare_name);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
