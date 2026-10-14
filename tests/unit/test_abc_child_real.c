/*
 * test_abc_child_real.c — the real odin3-abc as the child (3D spec §7.9, AB-33; plan Task 9c, the
 * exit gate of 3D-5a). Built with the adapter's sources (as test_abc_adapter is) so the linked
 * engine runs in this process beside the child: a read / strash / if / write_hie run through the
 * child gives the same BLIF bytes as the linked engine; a command file that makes real ABC call
 * exit(1) is an EXIT and this process, and the linked engine, carry on; a runaway command is a
 * TIMEOUT that leaves no process behind; the run directory holds only the files of §6.2; and the
 * pin check passes on the build's own odin3-abc and fails with the exact text on another commit.
 * The child runs under prlimit --as=2 GiB through a wrapper script (this process cannot: ASan).
 */
#include "odin3/odin3.h"

#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/child.h"
#include "session.h"
#include "unity.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ODIN3_CHILD_DATA
#error "ODIN3_CHILD_DATA must name tests/unit/data/abc/child"
#endif

enum {
    PATH_CAP = 512,
    DIR_CAP = 64,
    CMD_CAP = 1200,
    FILE_CAP = 16384,
    N_COMMANDS = 4,
    TIMEOUT_SECONDS = 2,
    GONE_WAIT_MS = 3000,
    POLL_MS = 20,
    SMALL_CAP = 100000,
    READ_SLACK = 65536,
    EXIT_STATUS = 1,
    CHILD_RUN = 0,
    LINKED_RUN = 1
};

static const char LIMITED_AS[] = "2147483648"; /* 2 GiB */
static const char *const PRLIMIT = "/usr/bin/prlimit";

static char g_dir[DIR_CAP];
static char g_tool_dir[DIR_CAP]; /* holds the wrapper script */
static char g_real[PATH_CAP];    /* the build's odin3-abc */
static char g_wrapper[PATH_CAP]; /* the 2 GiB wrapper around it */
static char g_log[FILE_CAP];     /* the child's output, for the tests that read it */

void setUp(void) {
    (void)snprintf(g_dir, sizeof g_dir, "/tmp/odin3-abc-real-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
}

static void remove_dir_files(void) {
    DIR *dir = opendir(g_dir);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[2 * PATH_CAP];
        (void)snprintf(path, sizeof path, "%s/%s", g_dir, entry->d_name);
        (void)unlink(path);
    }
    (void)closedir(dir);
}

void tearDown(void) {
    (void)unsetenv("ODIN3_ABC_EXE");
    odin3_abc_test_set_child_limits(NULL);
    (void)unsetenv("HOME");
    (void)setenv("HOME", "/", 1);
    g_log[0] = '\0';
    remove_dir_files();
    (void)rmdir(g_dir);
}

/* --- files ---------------------------------------------------------------------------------- */

typedef struct file_text {
    const char *path;
    const char *text;
} file_text;

static void write_file(const file_text *file_text_in) {
    const char *path = file_text_in->path;
    const char *text = file_text_in->text;
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(text, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* The whole file in buf (NUL-terminated); its length. */
static size_t read_file(const char *path, char *buf, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(buf, 1, cap - 1, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buf[got] = '\0';
    return got;
}

static void data_path(const char *name, char *path, size_t cap) {
    (void)snprintf(path, cap, "%s/%s", ODIN3_CHILD_DATA, name);
}

/* abc<run>.<suffix> in the run directory. */
static void run_path(uint32_t run, const char *suffix, char *path, size_t cap) {
    (void)snprintf(path, cap, "%s/abc%u.%s", g_dir, (unsigned)run, suffix);
}

/* Writes the fixture template name with @DATA@ replaced, as the run's command file. */
static void write_command_file(const char *template_name, uint32_t run, char *cmd_path) {
    char from[PATH_CAP];
    char text[FILE_CAP];
    char out[FILE_CAP];
    data_path(template_name, from, sizeof from);
    read_file(from, text, sizeof text);
    size_t used = 0;
    for (const char *cur = text; *cur != '\0';) {
        if (strncmp(cur, "@DATA@", 6) == 0) {
            used += (size_t)snprintf(out + used, sizeof out - used, "%s", ODIN3_CHILD_DATA);
            cur += 6;
        } else {
            out[used++] = *cur++;
        }
    }
    out[used] = '\0';
    run_path(run, "cmd", cmd_path, PATH_CAP);
    write_file(&(file_text){cmd_path, out});
}

/* --- the two ways to run the same commands -------------------------------------------------- */

/* read / strash / if -K 6 / write_hie over two_gates.blif, as the BLIF exchange builds them. */
static void blif_commands(uint32_t run, char cmds[N_COMMANDS][CMD_CAP]) {
    char in[PATH_CAP];
    char out[PATH_CAP];
    run_path(CHILD_RUN, "in.blif", in, sizeof in); /* one input for both runs */
    run_path(run, "out.blif", out, sizeof out);
    (void)snprintf(cmds[0], CMD_CAP, "read \"%s\"", in);
    (void)snprintf(cmds[1], CMD_CAP, "strash");
    (void)snprintf(cmds[2], CMD_CAP, "if -K 6");
    (void)snprintf(cmds[3], CMD_CAP, "write_hie \"%s\" \"%s\"", in, out);
}

static void copy_input(void) {
    char from[PATH_CAP];
    char in[PATH_CAP];
    char text[FILE_CAP];
    data_path("two_gates.blif", from, sizeof from);
    read_file(from, text, sizeof text);
    run_path(CHILD_RUN, "in.blif", in, sizeof in);
    write_file(&(file_text){in, text});
}

static void write_blif_command_file(uint32_t run, char *cmd_path) {
    char cmds[N_COMMANDS][CMD_CAP];
    blif_commands(run, cmds);
    char text[N_COMMANDS * CMD_CAP];
    text[0] = '\0';
    for (size_t i = 0; i < N_COMMANDS; i++) {
        strncat(text, cmds[i], sizeof text - strlen(text) - 1);
        strncat(text, "\n", sizeof text - strlen(text) - 1);
    }
    run_path(run, "cmd", cmd_path, PATH_CAP);
    write_file(&(file_text){cmd_path, text});
}

static void log_line(void *user, const char *line) {
    (void)user;
    size_t used = strlen(g_log);
    (void)snprintf(g_log + used, sizeof g_log - used, "%s\n", line);
}

/* One child run of a command file. */
typedef struct child_job {
    const char *program;
    const char *cmd_path;
    uint32_t run;
    uint32_t seconds;
} child_job;

static odin3_status run_child(const child_job *job, odin3_abc_child_res *res) {
    odin3_abc_child_req req = {0};
    req.program = job->program;
    req.option = "-f";
    req.argument = job->cmd_path;
    req.run_dir = g_dir;
    req.seconds = job->seconds;
    req.run = job->run;
    req.text = log_line;
    return odin3_abc_child_run(&req, res);
}

/* The same commands through the linked engine's session (the seam the engine table points at). */
static odin3_status run_linked(uint32_t run) {
    char cmds[N_COMMANDS][CMD_CAP];
    blif_commands(run, cmds);
    void *self = odin3_abc_session_self();
    odin3_abc_fail fail = {0};
    odin3_status st = odin3_abc_session_open(self, NULL, NULL, &fail);
    for (size_t i = 0; st == ODIN3_OK && i < N_COMMANDS; i++) {
        st = odin3_abc_session_exec(self, cmds[i], &fail);
    }
    odin3_abc_session_close(self);
    return st;
}

/* --- processes ------------------------------------------------------------------------------ */

static bool process_gone(pid_t pid) {
    if (kill(pid, 0) != 0) {
        return errno == ESRCH;
    }
    char path[64];
    (void)snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return true;
    }
    char text[PATH_CAP];
    size_t got = fread(text, 1, sizeof text - 1, file);
    (void)fclose(file);
    text[got] = '\0';
    const char *close_paren = strrchr(text, ')');
    return close_paren != NULL && close_paren[1] == ' ' && close_paren[2] == 'Z';
}

static bool wait_gone(pid_t pid) {
    for (int waited = 0; waited < GONE_WAIT_MS && !process_gone(pid); waited += POLL_MS) {
        struct timespec nap = {0, POLL_MS * 1000000L};
        (void)nanosleep(&nap, NULL);
    }
    return process_gone(pid);
}

static bool no_children(void) {
    int status = 0;
    pid_t got = waitpid(-1, &status, WNOHANG);
    return got == -1 && errno == ECHILD;
}

/* --- the tests ------------------------------------------------------------------------------ */

/*
 * The BLIF without ABC's first line, `# Benchmark "<m>" written by ABC on <date>`: the only line
 * of ABC's output that differs between two runs (the date), a comment that §6.2's read-back skips.
 */
static const char *skip_banner(const char *text) {
    static const char banner[] = "# Benchmark ";
    if (strncmp(text, banner, sizeof banner - 1) != 0) {
        return text;
    }
    const char *eol = strchr(text, '\n');
    return eol != NULL ? eol + 1 : text + strlen(text);
}

static void test_child_and_linked_blif_are_identical(void) {
    char cmd_path[PATH_CAP];
    copy_input();
    write_blif_command_file(CHILD_RUN, cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
    TEST_ASSERT_TRUE(res.output_bytes > 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_linked(LINKED_RUN));
    char child_out[PATH_CAP];
    char linked_out[PATH_CAP];
    run_path(CHILD_RUN, "out.blif", child_out, sizeof child_out);
    run_path(LINKED_RUN, "out.blif", linked_out, sizeof linked_out);
    char child_text[FILE_CAP];
    char linked_text[FILE_CAP];
    size_t child_len = read_file(child_out, child_text, sizeof child_text);
    size_t linked_len = read_file(linked_out, linked_text, sizeof linked_text);
    TEST_ASSERT_TRUE(child_len > 0);
    TEST_ASSERT_EQUAL_STRING(skip_banner(linked_text), skip_banner(child_text));
    TEST_ASSERT_EQUAL_size_t(linked_len, child_len); /* the banner's date has a fixed width */
    TEST_ASSERT_NOT_NULL(strstr(child_text, ".model two_gates"));
}

/* True when name is one of the files §6.2 lists for run 0. */
static bool is_exchange_file(const char *name) {
    static const char *const allowed[] = {"abc0.in.blif", "abc0.cmd", "abc0.log", "abc0.out.blif"};
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Asserts that the run directory holds only files of the exchange; the number of entries. */
static size_t count_exchange_files(void) {
    DIR *dir = opendir(g_dir);
    TEST_ASSERT_NOT_NULL(dir);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(is_exchange_file(entry->d_name), entry->d_name);
        count++;
    }
    (void)closedir(dir);
    return count;
}

static void test_run_directory_holds_only_the_files_of_the_exchange(void) {
    char cmd_path[PATH_CAP];
    copy_input();
    write_blif_command_file(CHILD_RUN, cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_size_t(3, count_exchange_files()); /* .in.blif, .cmd and .out.blif */
    char path[PATH_CAP];
    run_path(CHILD_RUN, "cmd", path, sizeof path);
    TEST_ASSERT_EQUAL_INT(0, access(path, R_OK));
}

static void test_real_abc_exit_is_contained(void) {
    char cmd_path[PATH_CAP];
    write_command_file("exits.abc", CHILD_RUN, cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_EXIT, res.kind);
    TEST_ASSERT_EQUAL_INT(EXIT_STATUS, res.code);
    TEST_ASSERT_TRUE(no_children());
    /* This process is untouched: the linked engine still works, and so does another child run. */
    copy_input();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_linked(LINKED_RUN));
    write_blif_command_file(CHILD_RUN, cmd_path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
}

static void test_runaway_command_times_out_and_leaves_nothing(void) {
    char cmd_path[PATH_CAP];
    write_command_file("loops.abc", CHILD_RUN, cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_ENGINE,
        run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, TIMEOUT_SECONDS}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_TIMEOUT, res.kind);
    TEST_ASSERT_EQUAL_UINT32(TIMEOUT_SECONDS, res.seconds);
    TEST_ASSERT_TRUE(no_children());
    char pid_path[PATH_CAP];
    char text[PATH_CAP];
    (void)snprintf(pid_path, sizeof pid_path, "%s/spin.pid", g_dir);
    read_file(pid_path, text, sizeof text);
    long spinner = strtol(text, NULL, 10);
    TEST_ASSERT_TRUE(spinner > 1);
    TEST_ASSERT_TRUE(wait_gone((pid_t)spinner));
}

static void test_pin_check_on_the_builds_own_program(void) {
    odin3_abc_child_res res = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_check_pin(g_real, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
}

static void test_pin_check_refuses_another_commit(void) {
    char fixture[PATH_CAP];
    data_path("badpin.sh", fixture, sizeof fixture);
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXE", fixture, 1));
    char program[PATH_CAP];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_program(program, sizeof program));
    TEST_ASSERT_EQUAL_STRING(fixture, program);
    odin3_abc_child_res res = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_check_pin(program, &res));
    char want[2 * PATH_CAP];
    (void)snprintf(want, sizeof want, "abc: ABC subprocess '%s' is 0123456789ab, expected %.12s",
                   fixture, odin3_abc_child_pin());
    char got[ODIN3_ABC_CHILD_MESSAGE_BYTES];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_message(&res, got, sizeof got));
    TEST_ASSERT_EQUAL_STRING(want, got);
}

/* A command file of one text, as run 0's. */
static void write_text_command_file(const char *text, char *cmd_path) {
    run_path(CHILD_RUN, "cmd", cmd_path, PATH_CAP);
    write_file(&(file_text){cmd_path, text});
}

static bool exists_in_run_dir(const char *name) {
    char path[PATH_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", g_dir, name);
    return access(path, F_OK) == 0;
}

static void test_user_rc_files_are_not_sourced(void) {
    char rc[PATH_CAP];
    char text[PATH_CAP];
    char cmd_path[PATH_CAP];
    (void)snprintf(rc, sizeof rc, "%s/.abc.rc", g_dir);
    (void)snprintf(text, sizeof text, "!touch %s/rc.ran\n", g_dir);
    write_file(&(file_text){rc, text}); /* HOME's, and the working directory's, both */
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", g_dir, 1));
    write_text_command_file("version\n", cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_FALSE(exists_in_run_dir("rc.ran"));
    odin3_abc_child_res pin = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_check_pin(g_real, &pin));
    TEST_ASSERT_FALSE(exists_in_run_dir("rc.ran"));
}

static void test_child_has_no_core_file_limit(void) {
    char text[PATH_CAP];
    char cmd_path[PATH_CAP];
    (void)snprintf(text, sizeof text, "!grep -h 'core file' /proc/self/limits > %s/core.txt\n", g_dir);
    write_text_command_file(text, cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    char core_path[PATH_CAP];
    char got[PATH_CAP];
    (void)snprintf(core_path, sizeof core_path, "%s/core.txt", g_dir);
    read_file(core_path, got, sizeof got);
    char soft[PATH_CAP];
    char hard[PATH_CAP];
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, sscanf(got, "Max core file size %511s %511s", soft, hard), got);
    TEST_ASSERT_EQUAL_STRING("0", soft);
    TEST_ASSERT_EQUAL_STRING("0", hard);
}

/* Output past the cap kills the child and its group; the status says so; nothing is left. */
static void test_output_cap_kills_a_real_child(void) {
    char cmd_path[PATH_CAP];
    write_text_command_file("!yes flood\n", cmd_path);
    odin3_abc_child_limits lowered = {0, SMALL_CAP};
    odin3_abc_test_set_child_limits(&lowered);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OUTPUT, res.kind);
    TEST_ASSERT_EQUAL_UINT64(SMALL_CAP, res.output_limit);
    TEST_ASSERT_TRUE(res.output_bytes > SMALL_CAP);
    TEST_ASSERT_TRUE(res.output_bytes <= SMALL_CAP + READ_SLACK);
    TEST_ASSERT_TRUE(no_children());
}

/* A failing command that is not `exit` leaves ABC's exit status 0 (Abc_RealMain returns 0): the
 * runner says OK and the failure is in the log and in the missing out.blif, which is what the
 * engine (3D-5) must test for. */
static void test_failing_command_exits_zero_with_a_diagnostic(void) {
    char text[PATH_CAP];
    char cmd_path[PATH_CAP];
    (void)snprintf(text, sizeof text, "read_blif %s/nonexistent.blif\nwrite_blif %s/abc0.out.blif\n",
                   g_dir, g_dir);
    write_text_command_file(text, cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run_child(&(child_job){g_wrapper, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
    TEST_ASSERT_EQUAL_INT(0, res.code);
    TEST_ASSERT_FALSE(exists_in_run_dir("abc0.out.blif"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log, "rror"), g_log);
}

static const char *const k_scripts[] = {"foreign.sh",  "orphan.sh", "pin_ok.sh", "pin39.sh",
                                        "pin41.sh",    "pinjunk.sh", "pinupper.sh"};

/* A program whose only output is the line (and, for the last case, a following ok one). */
static void make_pin_script(const char *name, const char *line, char *path, size_t cap) {
    (void)snprintf(path, cap, "%s/%s", g_tool_dir, name);
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fprintf(file, "#!/bin/sh\necho '%s'\n", line);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
}

/* The pin line is exactly the prefix and 40 lower-case hex digits: a script that prints the right
 * commit with a digit missing or added, with junk after it or in upper case is refused. */
static void test_pin_line_is_parsed_exactly(void) {
    static const char *const names[] = {"pin_ok.sh", "pin39.sh", "pin41.sh", "pinjunk.sh",
                                        "pinupper.sh"};
    const char *pin = odin3_abc_child_pin();
    char lines[5][100];
    (void)snprintf(lines[0], sizeof lines[0], "odin3-abc: commit %s", pin);
    (void)snprintf(lines[1], sizeof lines[1], "odin3-abc: commit %.39s", pin);
    (void)snprintf(lines[2], sizeof lines[2], "odin3-abc: commit %s0", pin);
    (void)snprintf(lines[3], sizeof lines[3], "odin3-abc: commit %s JUNK", pin);
    (void)snprintf(lines[4], sizeof lines[4], "odin3-abc: commit %s", pin);
    for (char *cur = lines[4]; *cur != '\0'; cur++) {
        *cur = (char)(*cur >= 'a' && *cur <= 'f' && cur > lines[4] + 17 ? *cur - 'a' + 'A' : *cur);
    }
    for (size_t i = 0; i < 5; i++) {
        char script[PATH_CAP];
        make_pin_script(names[i], lines[i], script, sizeof script);
        odin3_abc_child_res res = {0};
        odin3_status st = odin3_abc_child_check_pin(script, &res);
        if (i == 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, lines[i]);
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_ENGINE, st, lines[i]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ABC_CHILD_PIN, res.kind, lines[i]);
        }
    }
}

/* A script standing in for the wrapper: the real program with ODIN3_ABC_PARENT set as given
 * (pid 1, or the script's own pid) and, for "own", in the background and its own process group so
 * the runner's group kill does not reach it; the script then exits at once. */
static void make_parent_script(const char *name, bool background, char *path, size_t cap) {
    (void)snprintf(path, cap, "%s/%s", g_tool_dir, name);
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    if (background) {
        (void)fprintf(file,
                      "#!/bin/sh\nset -m\nODIN3_ABC_PARENT=$$ '%s' \"$@\" >/dev/null 2>&1 &\n"
                      "echo $! > spin.pid\nsleep 0.3\n",
                      g_real);
    } else {
        (void)fprintf(file, "#!/bin/sh\nODIN3_ABC_PARENT=1 exec '%s' \"$@\"\n", g_real);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
}

/* odin3-abc whose named parent is not its parent exits 1 at once. */
static void test_child_refuses_a_foreign_parent(void) {
    char script[PATH_CAP];
    char cmd_path[PATH_CAP];
    make_parent_script("foreign.sh", false, script, sizeof script);
    write_text_command_file("version\n", cmd_path);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          run_child(&(child_job){script, cmd_path, CHILD_RUN, 0}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_EXIT, res.kind);
    TEST_ASSERT_EQUAL_INT(1, res.code);
}

/* The child dies with its parent: the script that started it exits while it sleeps in a long
 * command. */
static void test_child_dies_with_its_parent(void) {
    char script[PATH_CAP];
    char cmd_path[PATH_CAP];
    make_parent_script("orphan.sh", true, script, sizeof script);
    write_text_command_file("!sleep 600\n", cmd_path);
    odin3_abc_child_res res;
    (void)run_child(&(child_job){script, cmd_path, CHILD_RUN, 0}, &res);
    char pid_path[PATH_CAP];
    char text[PATH_CAP];
    (void)snprintf(pid_path, sizeof pid_path, "%s/spin.pid", g_dir);
    read_file(pid_path, text, sizeof text);
    long orphan = strtol(text, NULL, 10);
    TEST_ASSERT_TRUE(orphan > 1);
    TEST_ASSERT_TRUE(wait_gone((pid_t)orphan));
}

/* The 2 GiB wrapper: prlimit around the real program (the real one itself without prlimit). */
static void make_wrapper(void) {
    (void)snprintf(g_wrapper, sizeof g_wrapper, "%s", g_real);
    if (access(PRLIMIT, X_OK) != 0) {
        return;
    }
    char path[PATH_CAP];
    (void)snprintf(path, sizeof path, "%s/limited.sh", g_tool_dir);
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fprintf(file, "#!/bin/sh\nexec %s --as=%s '%s' \"$@\"\n", PRLIMIT, LIMITED_AS, g_real);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
    (void)snprintf(g_wrapper, sizeof g_wrapper, "%s", path);
}

int main(void) {
    UNITY_BEGIN();
    (void)unsetenv("ODIN3_ABC_EXE");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_program(g_real, sizeof g_real));
    TEST_ASSERT_EQUAL_INT(0, access(g_real, X_OK));
    (void)snprintf(g_tool_dir, sizeof g_tool_dir, "/tmp/odin3-abc-tool-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_tool_dir));
    make_wrapper();
    RUN_TEST(test_child_and_linked_blif_are_identical);
    RUN_TEST(test_run_directory_holds_only_the_files_of_the_exchange);
    RUN_TEST(test_real_abc_exit_is_contained);
    RUN_TEST(test_runaway_command_times_out_and_leaves_nothing);
    RUN_TEST(test_pin_check_on_the_builds_own_program);
    RUN_TEST(test_pin_check_refuses_another_commit);
    RUN_TEST(test_user_rc_files_are_not_sourced);
    RUN_TEST(test_child_has_no_core_file_limit);
    RUN_TEST(test_output_cap_kills_a_real_child);
    RUN_TEST(test_failing_command_exits_zero_with_a_diagnostic);
    RUN_TEST(test_child_refuses_a_foreign_parent);
    RUN_TEST(test_child_dies_with_its_parent);
    RUN_TEST(test_pin_line_is_parsed_exactly);
    char wrapper[PATH_CAP];
    (void)snprintf(wrapper, sizeof wrapper, "%s/limited.sh", g_tool_dir);
    (void)unlink(wrapper);
    for (size_t i = 0; i < sizeof k_scripts / sizeof k_scripts[0]; i++) {
        (void)snprintf(wrapper, sizeof wrapper, "%s/%s", g_tool_dir, k_scripts[i]);
        (void)unlink(wrapper);
    }
    (void)rmdir(g_tool_dir);
    return UNITY_END();
}
