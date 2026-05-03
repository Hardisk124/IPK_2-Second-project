#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_TIMEOUT_MS 8000
#define NETEM_TIMEOUT_MS 12000
#define SERVER_START_DELAY_MS 150
#define DEFAULT_TIMEOUT_SEC 2
#define NSEC_PER_MSEC 1000000L
#define MIN_TEST_PORT 20000
#define TEST_PORT_RANGE 20000
#define TEMP_DIR_TEMPLATE "/tmp/ipk2-tests-XXXXXX"
#define TEST_DATA_SIZE_LARGE 65536
#define TEST_DATA_SIZE_MEDIUM 8192
#define TEST_DATA_SIZE_SMALL 4096
#define TEST_DATA_SIZE_IPV6 16384
#define TEST_DATA_SIZE_NETEM 32768

typedef struct {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
} ChildProcess;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * NSEC_PER_MSEC;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
        continue;
    }
}

static void log_msg(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static int open_devnull(void)
{
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        log_msg("Failed to open /dev/null: %s\n", strerror(errno));
    }
    return fd;
}

static const char *ipk_bin(void)
{
    const char *bin = getenv("IPK_RDT_BIN");
    return (bin && bin[0] != '\0') ? bin : "./ipk-rdt";
}

static char *path_join(const char *dir, const char *name)
{
    size_t len = strlen(dir) + strlen(name) + 2;
    char *out = malloc(len);
    if (!out) {
        return NULL;
    }
    snprintf(out, len, "%s/%s", dir, name);
    return out;
}

static char *make_temp_dir(void)
{
    char *tmpl = strdup(TEMP_DIR_TEMPLATE);
    if (!tmpl) {
        return NULL;
    }
    if (!mkdtemp(tmpl)) {
        free(tmpl);
        return NULL;
    }
    return tmpl;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t wrote = write(fd, data + offset, len - offset);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t)wrote;
    }
    return 0;
}

static int read_all(int fd, uint8_t **out, size_t *out_len)
{
    size_t cap = 4096;
    size_t len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        return -1;
    }
    while (true) {
        if (len == cap) {
            size_t new_cap = cap * 2;
            uint8_t *tmp = realloc(buf, new_cap);
            if (!tmp) {
                free(buf);
                return -1;
            }
            buf = tmp;
            cap = new_cap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return -1;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    int rc = write_all(fd, data, len);
    close(fd);
    return rc;
}

static uint8_t *read_file(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t offset = 0;
    while (offset < len) {
        ssize_t r = read(fd, buf + offset, len - offset);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) {
            break;
        }
        offset += (size_t)r;
    }
    close(fd);
    *len_out = offset;
    return buf;
}

static int fill_random(uint8_t *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t offset = 0;
        while (offset < len) {
            ssize_t r = read(fd, buf + offset, len - offset);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return -1;
            }
            if (r == 0) {
                break;
            }
            offset += (size_t)r;
        }
        close(fd);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(rand() % 256);
    }
    return 0;
}

static ChildProcess spawn_process(char *const argv[], bool provide_stdin, bool capture_stdout)
{
    ChildProcess proc = {
        .pid = -1,
        .stdin_fd = -1,
        .stdout_fd = -1,
    };
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int devnull = -1;

    if (provide_stdin && pipe(stdin_pipe) < 0) {
        return proc;
    }
    if (capture_stdout && pipe(stdout_pipe) < 0) {
        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }
        return proc;
    }

    devnull = open_devnull();

    pid_t pid = fork();
    if (pid < 0) {
        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] != -1) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (devnull != -1) {
            close(devnull);
        }
        return proc;
    }

    if (pid == 0) {
        if (provide_stdin) {
            dup2(stdin_pipe[0], STDIN_FILENO);
        } else if (devnull != -1) {
            dup2(devnull, STDIN_FILENO);
        }

        if (capture_stdout) {
            dup2(stdout_pipe[1], STDOUT_FILENO);
        } else if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
        }

        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
        }

        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
        }
        if (stdin_pipe[1] != -1) {
            close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] != -1) {
            close(stdout_pipe[0]);
        }
        if (stdout_pipe[1] != -1) {
            close(stdout_pipe[1]);
        }
        if (devnull != -1) {
            close(devnull);
        }

        execv(argv[0], argv);
        _exit(127);
    }

    if (stdin_pipe[0] != -1) {
        close(stdin_pipe[0]);
        proc.stdin_fd = stdin_pipe[1];
    }
    if (stdout_pipe[1] != -1) {
        close(stdout_pipe[1]);
        proc.stdout_fd = stdout_pipe[0];
    }
    if (devnull != -1) {
        close(devnull);
    }
    proc.pid = pid;
    return proc;
}

static int wait_process(pid_t pid, int timeout_ms, int *exit_code)
{
    long deadline = now_ms() + timeout_ms;
    int status = 0;
    while (now_ms() < deadline) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                *exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                *exit_code = 128 + WTERMSIG(status);
            } else {
                *exit_code = 1;
            }
            return 0;
        }
        sleep_ms(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

static void terminate_process(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

static int run_simple(char *const argv[], int timeout_ms)
{
    ChildProcess proc = spawn_process(argv, false, false);
    if (proc.pid < 0) {
        return -1;
    }
    int exit_code = 0;
    if (wait_process(proc.pid, timeout_ms, &exit_code) < 0) {
        return -1;
    }
    return exit_code;
}

static int pick_port(void)
{
    return MIN_TEST_PORT + (rand() % TEST_PORT_RANGE);
}

static int pick_distinct_port(int avoid)
{
    int port = pick_port();
    if (port == avoid) {
        int offset = (avoid - MIN_TEST_PORT + 1) % TEST_PORT_RANGE;
        port = MIN_TEST_PORT + offset;
    }
    return port;
}

static int ensure_running(pid_t pid)
{
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0) {
        return 0;
    }
    if (r == pid) {
        return -1;
    }
    return -1;
}

static ChildProcess start_server(const char *addr, const char *port, const char *output_path, int timeout_sec,
                                 bool capture_stdout)
{
    char timeout_buf[16];
    snprintf(timeout_buf, sizeof(timeout_buf), "%d", timeout_sec);

    const char *bin = ipk_bin();
    char *argv[16];
    int idx = 0;
    argv[idx++] = (char *)bin;
    argv[idx++] = (char *)"-s";
    argv[idx++] = (char *)"-p";
    argv[idx++] = (char *)port;
    if (addr) {
        argv[idx++] = (char *)"-a";
        argv[idx++] = (char *)addr;
    }
    if (output_path) {
        argv[idx++] = (char *)"-o";
        argv[idx++] = (char *)output_path;
    }
    argv[idx++] = (char *)"-w";
    argv[idx++] = timeout_buf;
    argv[idx] = NULL;

    ChildProcess proc = spawn_process(argv, false, capture_stdout);
    if (proc.pid < 0) {
        return proc;
    }
    sleep_ms(SERVER_START_DELAY_MS);
    if (ensure_running(proc.pid) < 0) {
        terminate_process(proc.pid);
        proc.pid = -1;
    }
    return proc;
}

static ChildProcess start_client(const char *addr, const char *port, const char *input_path, int timeout_sec,
                                 bool provide_stdin)
{
    char timeout_buf[16];
    snprintf(timeout_buf, sizeof(timeout_buf), "%d", timeout_sec);

    const char *bin = ipk_bin();
    char *argv[16];
    int idx = 0;
    argv[idx++] = (char *)bin;
    argv[idx++] = (char *)"-c";
    argv[idx++] = (char *)"-a";
    argv[idx++] = (char *)addr;
    argv[idx++] = (char *)"-p";
    argv[idx++] = (char *)port;
    if (input_path) {
        argv[idx++] = (char *)"-i";
        argv[idx++] = (char *)input_path;
    }
    argv[idx++] = (char *)"-w";
    argv[idx++] = timeout_buf;
    argv[idx] = NULL;

    return spawn_process(argv, provide_stdin, false);
}

static int compare_files(const char *a, const char *b)
{
    size_t len_a = 0;
    size_t len_b = 0;
    uint8_t *buf_a = read_file(a, &len_a);
    uint8_t *buf_b = read_file(b, &len_b);
    if (!buf_a || !buf_b) {
        free(buf_a);
        free(buf_b);
        return -1;
    }
    int rc = 0;
    if (len_a != len_b || (len_a && memcmp(buf_a, buf_b, len_a) != 0)) {
        rc = -1;
    }
    free(buf_a);
    free(buf_b);
    return rc;
}

static int test_basic_file_transfer(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        log_msg("Failed to create temp dir\n");
        return 1;
    }

    char *input_path = path_join(tmpdir, "input.bin");
    char *output_path = path_join(tmpdir, "output.bin");
    size_t data_len = TEST_DATA_SIZE_LARGE;
    uint8_t *data = malloc(data_len);
    if (!input_path || !output_path || !data) {
        log_msg("Allocation failure\n");
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }
    fill_random(data, data_len);
    if (write_file(input_path, data, data_len) < 0) {
        log_msg("Failed to write input file\n");
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("127.0.0.1", port_buf, output_path, DEFAULT_TIMEOUT_SEC, false);
    if (server.pid < 0) {
        log_msg("Server failed to start\n");
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    ChildProcess client = start_client("127.0.0.1", port_buf, input_path, DEFAULT_TIMEOUT_SEC, false);
    if (client.pid < 0) {
        log_msg("Client failed to start\n");
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int client_exit = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &client_exit) < 0 || client_exit != 0) {
        log_msg("Client failed with %d\n", client_exit);
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int server_exit = 0;
    if (wait_process(server.pid, TEST_TIMEOUT_MS, &server_exit) < 0 || server_exit != 0) {
        log_msg("Server failed with %d\n", server_exit);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    if (compare_files(input_path, output_path) < 0) {
        log_msg("File contents differ\n");
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    unlink(input_path);
    unlink(output_path);
    rmdir(tmpdir);
    free(tmpdir);
    free(input_path);
    free(output_path);
    free(data);
    return 0;
}

static int test_stdin_stdout(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    size_t data_len = TEST_DATA_SIZE_MEDIUM;
    uint8_t *data = malloc(data_len);
    if (!data) {
        free(tmpdir);
        return 1;
    }
    fill_random(data, data_len);

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("127.0.0.1", port_buf, NULL, DEFAULT_TIMEOUT_SEC, true);
    if (server.pid < 0) {
        log_msg("Server failed to start\n");
        free(tmpdir);
        free(data);
        return 1;
    }

    ChildProcess client = start_client("127.0.0.1", port_buf, NULL, DEFAULT_TIMEOUT_SEC, true);
    if (client.pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(data);
        return 1;
    }

    if (write_all(client.stdin_fd, data, data_len) < 0) {
        log_msg("Failed to write to client stdin\n");
        terminate_process(server.pid);
        terminate_process(client.pid);
        close(client.stdin_fd);
        if (server.stdout_fd != -1) {
            close(server.stdout_fd);
        }
        free(tmpdir);
        free(data);
        return 1;
    }
    close(client.stdin_fd);
    client.stdin_fd = -1;

    int client_exit = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &client_exit) < 0 || client_exit != 0) {
        terminate_process(server.pid);
        if (server.stdout_fd != -1) {
            close(server.stdout_fd);
        }
        free(tmpdir);
        free(data);
        return 1;
    }

    int server_exit = 0;
    if (wait_process(server.pid, TEST_TIMEOUT_MS, &server_exit) < 0 || server_exit != 0) {
        if (server.stdout_fd != -1) {
            close(server.stdout_fd);
        }
        free(tmpdir);
        free(data);
        return 1;
    }

    uint8_t *out_buf = NULL;
    size_t out_len = 0;
    if (read_all(server.stdout_fd, &out_buf, &out_len) < 0) {
        close(server.stdout_fd);
        free(tmpdir);
        free(data);
        return 1;
    }
    close(server.stdout_fd);

    int rc = 0;
    if (out_len != data_len || memcmp(out_buf, data, data_len) != 0) {
        log_msg("stdin/stdout data mismatch\n");
        rc = 1;
    }
    free(out_buf);
    free(tmpdir);
    free(data);
    return rc;
}

static int test_stdin_to_file(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    char *output_path = path_join(tmpdir, "output.bin");
    size_t data_len = TEST_DATA_SIZE_MEDIUM;
    uint8_t *data = malloc(data_len);
    if (!output_path || !data) {
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }
    fill_random(data, data_len);

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("127.0.0.1", port_buf, output_path, DEFAULT_TIMEOUT_SEC, false);
    if (server.pid < 0) {
        log_msg("Server failed to start\n");
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }

    ChildProcess client = start_client("127.0.0.1", port_buf, NULL, DEFAULT_TIMEOUT_SEC, true);
    if (client.pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }

    if (write_all(client.stdin_fd, data, data_len) < 0) {
        log_msg("Failed to write to client stdin\n");
        terminate_process(server.pid);
        terminate_process(client.pid);
        close(client.stdin_fd);
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }
    close(client.stdin_fd);
    client.stdin_fd = -1;

    int client_exit = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &client_exit) < 0 || client_exit != 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }

    int server_exit = 0;
    if (wait_process(server.pid, TEST_TIMEOUT_MS, &server_exit) < 0 || server_exit != 0) {
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }

    size_t out_len = 0;
    uint8_t *out_buf = read_file(output_path, &out_len);
    if (!out_buf) {
        free(tmpdir);
        free(output_path);
        free(data);
        return 1;
    }

    int rc = 0;
    if (out_len != data_len || memcmp(out_buf, data, data_len) != 0) {
        log_msg("stdin/file data mismatch\n");
        rc = 1;
    }
    free(out_buf);
    free(tmpdir);
    free(output_path);
    free(data);
    return rc;
}

static int test_file_to_stdout(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    char *input_path = path_join(tmpdir, "input.bin");
    size_t data_len = TEST_DATA_SIZE_MEDIUM;
    uint8_t *data = malloc(data_len);
    if (!input_path || !data) {
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }
    fill_random(data, data_len);
    if (write_file(input_path, data, data_len) < 0) {
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("127.0.0.1", port_buf, NULL, DEFAULT_TIMEOUT_SEC, true);
    if (server.pid < 0) {
        log_msg("Server failed to start\n");
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }

    ChildProcess client = start_client("127.0.0.1", port_buf, input_path, DEFAULT_TIMEOUT_SEC, false);
    if (client.pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }

    int client_exit = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &client_exit) < 0 || client_exit != 0) {
        terminate_process(server.pid);
        if (server.stdout_fd != -1) {
            close(server.stdout_fd);
        }
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }

    int server_exit = 0;
    if (wait_process(server.pid, TEST_TIMEOUT_MS, &server_exit) < 0 || server_exit != 0) {
        if (server.stdout_fd != -1) {
            close(server.stdout_fd);
        }
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }

    uint8_t *out_buf = NULL;
    size_t out_len = 0;
    if (read_all(server.stdout_fd, &out_buf, &out_len) < 0) {
        close(server.stdout_fd);
        free(tmpdir);
        free(input_path);
        free(data);
        return 1;
    }
    close(server.stdout_fd);

    int rc = 0;
    if (out_len != data_len || memcmp(out_buf, data, data_len) != 0) {
        log_msg("file/stdout data mismatch\n");
        rc = 1;
    }
    free(out_buf);
    free(tmpdir);
    free(input_path);
    free(data);
    return rc;
}

static int test_empty_input(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    char *input_path = path_join(tmpdir, "input.bin");
    char *output_path = path_join(tmpdir, "output.bin");
    if (!input_path || !output_path) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    if (write_file(input_path, NULL, 0) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("127.0.0.1", port_buf, output_path, DEFAULT_TIMEOUT_SEC, false);
    if (server.pid < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    ChildProcess client = start_client("127.0.0.1", port_buf, input_path, DEFAULT_TIMEOUT_SEC, false);
    if (client.pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    int exit_code = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    if (wait_process(server.pid, TEST_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    int fd = open(output_path, O_RDONLY);
    if (fd < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }
    close(fd);
    if (st.st_size != 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        return 1;
    }

    unlink(input_path);
    unlink(output_path);
    rmdir(tmpdir);
    free(tmpdir);
    free(input_path);
    free(output_path);
    return 0;
}

static int test_cli_invalid_args(void)
{
    const char *bin = ipk_bin();
    char *argv1[] = {(char *)bin, (char *)"-s", NULL};
    char *argv2[] = {(char *)bin, (char *)"-c", (char *)"-p", (char *)"5000", NULL};
    char *argv3[] = {(char *)bin, (char *)"-c", (char *)"-a", (char *)"127.0.0.1", NULL};

    int rc1 = run_simple(argv1, 2000);
    int rc2 = run_simple(argv2, 2000);
    int rc3 = run_simple(argv3, 2000);

    if (rc1 == 0 || rc2 == 0 || rc3 == 0) {
        return 1;
    }
    return 0;
}

static int test_client_timeout(void)
{
    const char *bin = ipk_bin();
    char *argv[] = {
        (char *)bin,
        (char *)"-c",
        (char *)"-a",
        (char *)"127.0.0.1",
        (char *)"-p",
        (char *)"39999",
        (char *)"-w",
        (char *)"1",
        NULL,
    };
    int rc = run_simple(argv, 4000);
    if (rc == 0) {
        return 1;
    }
    return 0;
}

static int send_garbage_packet(const char *addr, int port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &dest.sin_addr) != 1) {
        close(sock);
        return -1;
    }
    uint8_t buf[32];
    fill_random(buf, sizeof(buf));
    ssize_t sent = sendto(sock, buf, sizeof(buf), 0, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);
    return (sent < 0) ? -1 : 0;
}

static int test_prehandshake_garbage(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    char *input_path = path_join(tmpdir, "input.bin");
    char *output_path = path_join(tmpdir, "output.bin");
    size_t data_len = TEST_DATA_SIZE_SMALL;
    uint8_t *data = malloc(data_len);
    if (!input_path || !output_path || !data) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }
    fill_random(data, data_len);
    if (write_file(input_path, data, data_len) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("127.0.0.1", port_buf, output_path, DEFAULT_TIMEOUT_SEC, false);
    if (server.pid < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    send_garbage_packet("127.0.0.1", port);

    ChildProcess client = start_client("127.0.0.1", port_buf, input_path, DEFAULT_TIMEOUT_SEC, false);
    if (client.pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int exit_code = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    if (wait_process(server.pid, TEST_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    if (compare_files(input_path, output_path) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    unlink(input_path);
    unlink(output_path);
    rmdir(tmpdir);
    free(tmpdir);
    free(input_path);
    free(output_path);
    free(data);
    return 0;
}

static bool ipv6_supported(void)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }
    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }
    close(sock);
    return true;
}

static int test_ipv6_transfer(void)
{
    if (!ipv6_supported()) {
        return 77;
    }

    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    char *input_path = path_join(tmpdir, "input.bin");
    char *output_path = path_join(tmpdir, "output.bin");
    size_t data_len = TEST_DATA_SIZE_IPV6;
    uint8_t *data = malloc(data_len);
    if (!input_path || !output_path || !data) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }
    fill_random(data, data_len);
    if (write_file(input_path, data, data_len) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int port = pick_port();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    ChildProcess server = start_server("::1", port_buf, output_path, DEFAULT_TIMEOUT_SEC, false);
    if (server.pid < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 77;
    }

    ChildProcess client = start_client("::1", port_buf, input_path, DEFAULT_TIMEOUT_SEC, false);
    if (client.pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 77;
    }

    int exit_code = 0;
    if (wait_process(client.pid, TEST_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    if (wait_process(server.pid, TEST_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    if (compare_files(input_path, output_path) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    unlink(input_path);
    unlink(output_path);
    rmdir(tmpdir);
    free(tmpdir);
    free(input_path);
    free(output_path);
    free(data);
    return 0;
}

typedef struct {
    uint8_t data[MAX_PACKET];
    size_t len;
    struct sockaddr_in dest;
    bool in_use;
    long stored_at;
} DelayedPacket;

static bool is_data_packet(const uint8_t *buf, size_t len)
{
    if (len < RDT_HDR_SIZE) {
        return false;
    }
    RDTHeader hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    uint16_t data_len = ntohs(hdr.data_len);
    uint8_t flags = hdr.flags;
    if (data_len == 0) {
        return false;
    }
    if (flags & (FLAG_SYN | FLAG_FIN)) {
        return false;
    }
    return true;
}

static int run_proxy(int proxy_port, int server_port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return 1;
    }
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = htons((uint16_t)proxy_port);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(sock);
        return 1;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons((uint16_t)server_port);

    struct sockaddr_in client_addr = {0};
    bool client_known = false;
    unsigned long data_packet_count = 0;
    long last_activity = now_ms();
    long deadline = now_ms() + NETEM_TIMEOUT_MS;
    DelayedPacket delayed = {.in_use = false};

    while (now_ms() < deadline) {
        if (delayed.in_use && now_ms() - delayed.stored_at > 80) {
            sendto(sock, delayed.data, delayed.len, 0,
                   (struct sockaddr *)&delayed.dest, sizeof(delayed.dest));
            delayed.in_use = false;
        }

        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ret == 0) {
            if (now_ms() - last_activity > 2000 && !delayed.in_use) {
                break;
            }
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        uint8_t buf[MAX_PACKET];
        struct sockaddr_in src_addr = {0};
        socklen_t src_len = sizeof(src_addr);
        ssize_t r = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src_addr, &src_len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        last_activity = now_ms();

        bool from_server = (src_addr.sin_port == server_addr.sin_port &&
                            src_addr.sin_addr.s_addr == server_addr.sin_addr.s_addr);
        if (!from_server) {
            client_addr = src_addr;
            client_known = true;
        }
        if (from_server && !client_known) {
            continue;
        }

        struct sockaddr_in dest = from_server ? client_addr : server_addr;
        bool data_pkt = is_data_packet(buf, (size_t)r);
        if (data_pkt) {
            data_packet_count++;
            if (data_packet_count % 10 == 0) {
                continue;
            }
            if (data_packet_count % 7 == 0 && !delayed.in_use) {
                memcpy(delayed.data, buf, (size_t)r);
                delayed.len = (size_t)r;
                delayed.dest = dest;
                delayed.in_use = true;
                delayed.stored_at = now_ms();
                continue;
            }
        }

        sendto(sock, buf, (size_t)r, 0, (struct sockaddr *)&dest, sizeof(dest));

        if (data_pkt && data_packet_count % 13 == 0) {
            sendto(sock, buf, (size_t)r, 0, (struct sockaddr *)&dest, sizeof(dest));
        }
    }

    if (delayed.in_use) {
        sendto(sock, delayed.data, delayed.len, 0,
               (struct sockaddr *)&delayed.dest, sizeof(delayed.dest));
    }

    close(sock);
    return 0;
}

static int test_netem_impairments(void)
{
    char *tmpdir = make_temp_dir();
    if (!tmpdir) {
        return 1;
    }

    char *input_path = path_join(tmpdir, "input.bin");
    char *output_path = path_join(tmpdir, "output.bin");
    size_t data_len = TEST_DATA_SIZE_NETEM;
    uint8_t *data = malloc(data_len);
    if (!input_path || !output_path || !data) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }
    fill_random(data, data_len);
    if (write_file(input_path, data, data_len) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int server_port = pick_port();
    int proxy_port = pick_distinct_port(server_port);
    char server_port_buf[16];
    char proxy_port_buf[16];
    snprintf(server_port_buf, sizeof(server_port_buf), "%d", server_port);
    snprintf(proxy_port_buf, sizeof(proxy_port_buf), "%d", proxy_port);

    ChildProcess server = start_server("127.0.0.1", server_port_buf, output_path, 3, false);
    if (server.pid < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    pid_t proxy_pid = fork();
    if (proxy_pid == 0) {
        _exit(run_proxy(proxy_port, server_port));
    }
    if (proxy_pid < 0) {
        terminate_process(server.pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    ChildProcess client = start_client("127.0.0.1", proxy_port_buf, input_path, 3, false);
    if (client.pid < 0) {
        terminate_process(server.pid);
        terminate_process(proxy_pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    int exit_code = 0;
    if (wait_process(client.pid, NETEM_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        terminate_process(server.pid);
        terminate_process(proxy_pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    if (wait_process(server.pid, NETEM_TIMEOUT_MS, &exit_code) < 0 || exit_code != 0) {
        terminate_process(proxy_pid);
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    waitpid(proxy_pid, NULL, 0);

    if (compare_files(input_path, output_path) < 0) {
        free(tmpdir);
        free(input_path);
        free(output_path);
        free(data);
        return 1;
    }

    unlink(input_path);
    unlink(output_path);
    rmdir(tmpdir);
    free(tmpdir);
    free(input_path);
    free(output_path);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        log_msg("Usage: %s <test-name>\n", argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srand((unsigned int)(ts.tv_nsec ^ ts.tv_sec ^ (unsigned int)getpid()));

    if (strcmp(argv[1], "basic_file_transfer") == 0) {
        return test_basic_file_transfer();
    }
    if (strcmp(argv[1], "stdin_stdout_transfer") == 0) {
        return test_stdin_stdout();
    }
    if (strcmp(argv[1], "stdin_to_file_transfer") == 0) {
        return test_stdin_to_file();
    }
    if (strcmp(argv[1], "file_to_stdout_transfer") == 0) {
        return test_file_to_stdout();
    }
    if (strcmp(argv[1], "empty_input_transfer") == 0) {
        return test_empty_input();
    }
    if (strcmp(argv[1], "cli_invalid_args") == 0) {
        return test_cli_invalid_args();
    }
    if (strcmp(argv[1], "client_timeout") == 0) {
        return test_client_timeout();
    }
    if (strcmp(argv[1], "prehandshake_garbage") == 0) {
        return test_prehandshake_garbage();
    }
    if (strcmp(argv[1], "ipv6_transfer") == 0) {
        return test_ipv6_transfer();
    }
    if (strcmp(argv[1], "netem_impairments") == 0) {
        return test_netem_impairments();
    }

    log_msg("Unknown test: %s\n", argv[1]);
    return 2;
}