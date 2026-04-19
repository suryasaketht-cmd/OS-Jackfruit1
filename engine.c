/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 * - command-line shape is defined
 * - key runtime data structures are defined
 * - bounded-buffer skeleton is defined
 * - supervisor / client split is outlined
 *
 * Students are expected to design:
 * - the control-plane IPC implementation
 * - container lifecycle and metadata synchronization
 * - clone + namespace setup for each container
 * - producer/consumer behavior for log buffering
 * - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_HARD_LIMIT_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs_path[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    void *child_stack;
    pthread_t producer_thread;
    int producer_thread_started;
    int producer_thread_joined;
    int in_use;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct supervisor_ctx supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int log_read_fd;
    char container_id[CONTAINER_ID_LEN];
} log_producer_ctx_t;

typedef struct supervisor_ctx {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    size_t container_count;
    size_t container_capacity;
} supervisor_ctx_t;

static volatile sig_atomic_t g_supervisor_stop = 0;
static volatile sig_atomic_t g_supervisor_sigchld = 0;
static volatile sig_atomic_t g_client_interrupt = 0;

static void supervisor_signal_handler(int signo)
{
    if (signo == SIGCHLD) {
        g_supervisor_sigchld = 1;
        return;
    }

    g_supervisor_stop = 1;
}

static void client_signal_handler(int signo)
{
    (void)signo;
    g_client_interrupt = 1;
}

typedef struct {
    supervisor_ctx_t *ctx;
    int client_fd;
} control_client_ctx_t;

static void stop_container(supervisor_ctx_t *ctx, const char *id);
static int send_stop_control_request(const char *container_id);
static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx, const char *id);

static int normalize_rootfs_path(const char *input, char *output, size_t output_len)
{
    char resolved[PATH_MAX];

    if (realpath(input, resolved) != NULL) {
        if (snprintf(output, output_len, "%s", resolved) >= (int)output_len)
            return -1;
        return 0;
    }

    if (snprintf(output, output_len, "%s", input) >= (int)output_len)
        return -1;

    return 0;
}

static int parse_exit_code_line(const char *line, int *exit_code_out)
{
    const char *marker = strstr(line, "exit_code=");
    char *end = NULL;
    long value;

    if (!marker)
        return 0;

    marker += strlen("exit_code=");
    errno = 0;
    value = strtol(marker, &end, 10);
    if (errno != 0 || marker == end)
        return 0;

    *exit_code_out = (int)value;
    return 1;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const char *cursor = buffer;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        cursor += (size_t)written;
        length -= (size_t)written;
    }

    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    char *cursor = buffer;

    while (length > 0) {
        ssize_t read_bytes = read(fd, cursor, length);
        if (read_bytes < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (read_bytes == 0)
            return -1;

        cursor += (size_t)read_bytes;
        length -= (size_t)read_bytes;
    }

    return 0;
}

static int send_textf(int fd, const char *format, ...)
{
    char buffer[2048];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length < 0)
        return -1;
    if ((size_t)length >= sizeof(buffer))
        length = (int)(sizeof(buffer) - 1);

    return write_all(fd, buffer, (size_t)length);
}

static int connect_to_supervisor(void)
{
    int fd;
    struct sockaddr_un address;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, CONTROL_PATH, sizeof(address.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int send_control_request_once(const control_request_t *req)
{
    int fd;
    int rc = 0;
    char buffer[1024];
    int stop_forwarded = 0;
    int run_exit_code = 0;
    int run_exit_code_seen = 0;
    char line_buffer[2048];
    size_t line_buffer_len = 0;

    fd = connect_to_supervisor();
    if (fd < 0) {
        fprintf(stderr, "Could not connect to supervisor at %s\n", CONTROL_PATH);
        return 1;
    }

    if (write_all(fd, req, sizeof(*req)) != 0) {
        perror("write");
        close(fd);
        return 1;
    }

    while (1) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
        if (bytes_read < 0) {
            if (errno == EINTR)
            {
                if (req->kind == CMD_RUN && g_client_interrupt && !stop_forwarded) {
                    g_client_interrupt = 0;
                    if (send_stop_control_request(req->container_id) == 0)
                        stop_forwarded = 1;
                }
                continue;
            }
            perror("read");
            rc = 1;
            break;
        }
        if (bytes_read == 0)
            break;

        buffer[bytes_read] = '\0';
        fputs(buffer, stdout);
        fflush(stdout);

        if (req->kind == CMD_RUN) {
            size_t offset = 0;

            while (offset < (size_t)bytes_read) {
                size_t available = (size_t)bytes_read - offset;
                char *newline = memchr(buffer + offset, '\n', available);
                size_t chunk_len;

                if (newline)
                    chunk_len = (size_t)(newline - (buffer + offset)) + 1;
                else
                    chunk_len = available;

                if (line_buffer_len + chunk_len >= sizeof(line_buffer))
                    line_buffer_len = 0;

                memcpy(line_buffer + line_buffer_len, buffer + offset, chunk_len);
                line_buffer_len += chunk_len;

                if (newline) {
                    int parsed_exit_code = 0;

                    line_buffer[line_buffer_len - 1] = '\0';
                    if (parse_exit_code_line(line_buffer, &parsed_exit_code) != 0) {
                        run_exit_code = parsed_exit_code;
                        run_exit_code_seen = 1;
                    }
                    line_buffer_len = 0;
                }

                offset += chunk_len;
            }
        }
    }

    close(fd);

    if (req->kind == CMD_RUN) {
        if (rc != 0)
            return rc;

        if (!run_exit_code_seen)
            return 1;

        return run_exit_code;
    }

    return rc;
}

static int send_stop_control_request(const char *container_id)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return send_control_request_once(&req);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 * - block or fail according to your chosen policy when the buffer is full
 * - wake consumers correctly
 * - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 * - wait correctly while the buffer is empty
 * - return a useful status when shutdown is in progress
 * - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 1;
}

static int resolve_log_path(supervisor_ctx_t *ctx,
                            const char *container_id,
                            char *path,
                            size_t path_len)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_by_id_locked(ctx, container_id);
    if (record != NULL)
        strncpy(path, record->log_path, path_len - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (record != NULL)
        return 0;

    if (snprintf(path, path_len, "%s/%s.log", LOG_DIR, container_id) >= (int)path_len)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 * - remove log chunks from the bounded buffer
 * - route each chunk to the correct per-container log file
 * - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (1) {
        char path[PATH_MAX] = {0};
        int log_fd;

        int pop_rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (pop_rc == 0)
            break;

        if (resolve_log_path(ctx, item.container_id, path, sizeof(path)) != 0)
            continue;

        log_fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (log_fd < 0)
            continue;

        if (write_all(log_fd, item.data, item.length) != 0)
            perror("write(log)");

        close(log_fd);
    }

    return NULL;
}

static void *log_producer_thread(void *arg)
{
    log_producer_ctx_t *producer = arg;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, producer->container_id, sizeof(item.container_id) - 1);

    while (1) {
        ssize_t bytes_read = read(producer->log_read_fd, item.data, sizeof(item.data));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (bytes_read == 0)
            break;

        item.length = (size_t)bytes_read;
        if (bounded_buffer_push(&producer->ctx->log_buffer, &item) != 0)
            break;
    }

    close(producer->log_read_fd);
    free(producer);
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 * - isolated PID / UTS / mount context
 * - chroot or pivot_root into rootfs
 * - working /proc inside container
 * - stdout / stderr redirected to the supervisor logging path
 * - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    if (cfg->nice_value != 0 && setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    if (sethostname(cfg->id, strnlen(cfg->id, sizeof(cfg->id))) < 0)
        perror("sethostname");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount-private-root");

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir(rootfs)");
        return 1;
    }

    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir(/)");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir(/proc)");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount(/proc)");
        return 1;
    }

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2(log fd)");
        return 1;
    }

    close(cfg->log_write_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 * - create and bind the control-plane IPC endpoint
 * - initialize shared metadata and the bounded buffer
 * - start the logging thread
 * - accept control requests and update container state
 * - reap children and respond to signals
 */
static int ensure_metadata_capacity(supervisor_ctx_t *ctx)
{
    size_t new_capacity;
    container_record_t *new_array;

    if (ctx->container_count < ctx->container_capacity)
        return 0;

    new_capacity = (ctx->container_capacity == 0) ? 16 : ctx->container_capacity * 2;
    new_array = realloc(ctx->containers, new_capacity * sizeof(*new_array));
    if (!new_array)
        return -1;

    memset(new_array + ctx->container_capacity,
           0,
           (new_capacity - ctx->container_capacity) * sizeof(*new_array));

    ctx->containers = new_array;
    ctx->container_capacity = new_capacity;
    return 0;
}

static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx, const char *id)
{
    size_t i;

    for (i = 0; i < ctx->container_count; i++) {
        if (ctx->containers[i].in_use && strncmp(ctx->containers[i].id, id, CONTAINER_ID_LEN) == 0)
            return &ctx->containers[i];
    }

    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    size_t i;

    for (i = 0; i < ctx->container_count; i++) {
        if (ctx->containers[i].in_use && ctx->containers[i].host_pid == pid)
            return &ctx->containers[i];
    }

    return NULL;
}

static int add_container_record(supervisor_ctx_t *ctx,
                                const char *id,
                                const char *rootfs_path,
                                pid_t pid,
                                unsigned long soft_limit_bytes,
                                unsigned long hard_limit_bytes,
                                const char *log_path,
                                void *child_stack,
                                pthread_t producer_thread)
{
    container_record_t *record;

    if (ensure_metadata_capacity(ctx) != 0)
        return -1;

    record = &ctx->containers[ctx->container_count++];
    memset(record, 0, sizeof(*record));

    strncpy(record->id, id, sizeof(record->id) - 1);
    strncpy(record->rootfs_path, rootfs_path, sizeof(record->rootfs_path) - 1);
    record->host_pid = pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    record->soft_limit_bytes = soft_limit_bytes;
    record->hard_limit_bytes = hard_limit_bytes;
    record->exit_code = -1;
    record->exit_signal = 0;
    record->stop_requested = 0;
    record->child_stack = child_stack;
    record->producer_thread = producer_thread;
    record->producer_thread_started = 1;
    record->producer_thread_joined = 0;
    record->in_use = 1;
    strncpy(record->log_path, log_path, sizeof(record->log_path) - 1);

    return 0;
}

static int has_running_rootfs_conflict_locked(supervisor_ctx_t *ctx, const char *rootfs_path)
{
    size_t i;

    for (i = 0; i < ctx->container_count; i++) {
        const container_record_t *record = &ctx->containers[i];

        if (!record->in_use)
            continue;

        if (record->state != CONTAINER_RUNNING && record->state != CONTAINER_STARTING)
            continue;

        if (strncmp(record->rootfs_path, rootfs_path, sizeof(record->rootfs_path)) == 0)
            return 1;
    }

    return 0;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t child;

    while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *record = NULL;
        pthread_t producer_thread_to_join;
        int join_needed = 0;

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_by_pid_locked(ctx, child);
        if (record) {
            producer_thread_to_join = record->producer_thread;
            if (record->producer_thread_started && !record->producer_thread_joined) {
                record->producer_thread_joined = 1;
                join_needed = 1;
            }

            if (WIFEXITED(status)) {
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, record->id, record->host_pid);
                record->state = CONTAINER_EXITED;
                record->exit_code = WEXITSTATUS(status);
                record->exit_signal = 0;
            } else if (WIFSIGNALED(status)) {
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, record->id, record->host_pid);
                record->exit_signal = WTERMSIG(status);
                record->exit_code = 128 + record->exit_signal;
                if (record->stop_requested)
                    record->state = CONTAINER_STOPPED;
                else if (record->exit_signal == SIGKILL)
                    record->state = CONTAINER_HARD_LIMIT_KILLED;
                else
                    record->state = CONTAINER_KILLED;
            }
            free(record->child_stack);
            record->child_stack = NULL;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (join_needed)
            pthread_join(producer_thread_to_join, NULL);
    }
}

static int launch_container(supervisor_ctx_t *ctx,
                            const char *id,
                            const char *rootfs,
                            const char *command,
                            unsigned long soft_limit_bytes,
                            unsigned long hard_limit_bytes,
                            int nice_value,
                            pid_t *child_pid_out)
{
    child_config_t *cfg;
    log_producer_ctx_t *producer_cfg;
    void *child_stack;
    pthread_t producer_thread;
    int log_fd;
    int log_pipe[2] = {-1, -1};
    char log_path[PATH_MAX];
    char normalized_rootfs[PATH_MAX];
    pid_t child_pid;

    if (normalize_rootfs_path(rootfs, normalized_rootfs, sizeof(normalized_rootfs)) != 0) {
        fprintf(stderr, "Invalid rootfs path for %s\n", id);
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_by_id_locked(ctx, id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "Container id already exists: %s\n", id);
        return -1;
    }

    if (has_running_rootfs_conflict_locked(ctx, normalized_rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr,
                "Rootfs already in use by another running container: %s\n",
                normalized_rootfs);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        perror("mkdir(logs)");
        return -1;
    }

    if (snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, id) >= (int)sizeof(log_path)) {
        fprintf(stderr, "Log path too long for container %s\n", id);
        return -1;
    }

    log_fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("open(log)");
        return -1;
    }
    close(log_fd);

    if (pipe(log_pipe) < 0) {
        perror("pipe(log)");
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    producer_cfg = calloc(1, sizeof(*producer_cfg));
    child_stack = malloc(STACK_SIZE);
    if (!cfg || !producer_cfg || !child_stack) {
        perror("alloc child config/stack");
        free(cfg);
        free(producer_cfg);
        free(child_stack);
        close(log_pipe[0]);
        close(log_pipe[1]);
        return -1;
    }

    strncpy(cfg->id, id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, command, sizeof(cfg->command) - 1);
    cfg->nice_value = nice_value;
    cfg->log_write_fd = log_pipe[1];

    producer_cfg->ctx = ctx;
    producer_cfg->log_read_fd = log_pipe[0];
    strncpy(producer_cfg->container_id, id, sizeof(producer_cfg->container_id) - 1);

    if (pthread_create(&producer_thread, NULL, log_producer_thread, producer_cfg) != 0) {
        perror("pthread_create(log_producer)");
        free(cfg);
        free(producer_cfg);
        free(child_stack);
        close(log_pipe[0]);
        close(log_pipe[1]);
        return -1;
    }

    child_pid = clone(child_fn,
                      (char *)child_stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (child_pid < 0) {
        perror("clone");
        close(log_pipe[1]);
        close(log_pipe[0]);
        pthread_join(producer_thread, NULL);
        free(cfg);
        free(child_stack);
        return -1;
    }

    close(log_pipe[1]);
    free(cfg);

    pthread_mutex_lock(&ctx->metadata_lock);
    if (add_container_record(ctx,
                             id,
                             normalized_rootfs,
                             child_pid,
                             soft_limit_bytes,
                             hard_limit_bytes,
                             log_path,
                             child_stack,
                             producer_thread) != 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        kill(child_pid, SIGTERM);
        pthread_join(producer_thread, NULL);
        fprintf(stderr, "Failed to store metadata for %s\n", id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd,
                                  id,
                                  child_pid,
                                  soft_limit_bytes,
                                  hard_limit_bytes) != 0) {
            perror("ioctl(MONITOR_REGISTER)");
            fprintf(stderr, "Warning: monitor registration failed for %s\n", id);
        }
    }

    if (child_pid_out != NULL)
        *child_pid_out = child_pid;

    printf("Started container id=%s pid=%d rootfs=%s command=%s\n",
           id,
           child_pid,
           rootfs,
           command);
    return 0;
}

static int get_container_snapshot(supervisor_ctx_t *ctx,
                                  const char *id,
                                  container_record_t *snapshot)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_by_id_locked(ctx, id);
    if (record != NULL)
        *snapshot = *record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    return record != NULL ? 0 : -1;
}

static int wait_for_container_completion(supervisor_ctx_t *ctx,
                                         const char *id,
                                         int client_fd)
{
    container_record_t snapshot;

    while (1) {
        if (get_container_snapshot(ctx, id, &snapshot) != 0)
            return send_textf(client_fd, "ERROR container not found: %s\n", id);

        if (snapshot.state != CONTAINER_RUNNING && snapshot.state != CONTAINER_STARTING)
            break;

        usleep(100000);
    }

    return send_textf(client_fd,
                      "EXIT id=%s state=%s exit_code=%d signal=%d\n",
                      snapshot.id,
                      state_to_string(snapshot.state),
                      snapshot.exit_code,
                      snapshot.exit_signal);
}

static int stream_container_logs(supervisor_ctx_t *ctx,
                                 const char *id,
                                 int client_fd)
{
    container_record_t snapshot;
    int log_fd;
    ssize_t bytes_read;
    char buffer[1024];

    if (get_container_snapshot(ctx, id, &snapshot) != 0)
        return send_textf(client_fd, "ERROR container not found: %s\n", id);

    log_fd = open(snapshot.log_path, O_RDONLY);
    if (log_fd < 0)
        return send_textf(client_fd, "ERROR cannot open log file: %s\n", snapshot.log_path);

    while ((bytes_read = read(log_fd, buffer, sizeof(buffer))) > 0) {
        if (write_all(client_fd, buffer, (size_t)bytes_read) != 0) {
            close(log_fd);
            return -1;
        }
    }

    close(log_fd);
    if (bytes_read < 0)
        return send_textf(client_fd, "ERROR failed to read log file\n");

    return 0;
}

static int handle_control_request(supervisor_ctx_t *ctx,
                                  const control_request_t *req,
                                  int client_fd)
{
    pid_t child_pid = -1;

    switch (req->kind) {
    case CMD_START:
        if (launch_container(ctx,
                             req->container_id,
                             req->rootfs,
                             req->command,
                             req->soft_limit_bytes,
                             req->hard_limit_bytes,
                             req->nice_value,
                             &child_pid) != 0) {
            return send_textf(client_fd, "ERROR failed to start %s\n", req->container_id);
        }

        return send_textf(client_fd,
                          "OK started id=%s pid=%d soft_mib=%lu hard_mib=%lu nice=%d\n",
                          req->container_id,
                          child_pid,
                          req->soft_limit_bytes >> 20,
                          req->hard_limit_bytes >> 20,
                          req->nice_value);

    case CMD_RUN:
        if (launch_container(ctx,
                             req->container_id,
                             req->rootfs,
                             req->command,
                             req->soft_limit_bytes,
                             req->hard_limit_bytes,
                             req->nice_value,
                             &child_pid) != 0) {
            return send_textf(client_fd, "ERROR failed to start %s\n", req->container_id);
        }

        if (send_textf(client_fd, "RUNNING id=%s pid=%d\n", req->container_id, child_pid) != 0)
            return -1;

        return wait_for_container_completion(ctx, req->container_id, client_fd);

    case CMD_PS:
        pthread_mutex_lock(&ctx->metadata_lock);
        if (send_textf(client_fd,
                       "%-16s %-8s %-12s %-12s %-12s %-10s %-10s %-20s\n",
                       "id",
                       "pid",
                       "state",
                       "soft(MiB)",
                       "hard(MiB)",
                       "exit_code",
                       "signal",
                       "log_path") != 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return -1;
        }

        for (size_t i = 0; i < ctx->container_count; i++) {
            const container_record_t *record = &ctx->containers[i];

            if (!record->in_use)
                continue;

            if (send_textf(client_fd,
                           "%-16s %-8d %-12s %-12lu %-12lu %-10d %-10d %-20s\n",
                           record->id,
                           record->host_pid,
                           state_to_string(record->state),
                           record->soft_limit_bytes >> 20,
                           record->hard_limit_bytes >> 20,
                           record->exit_code,
                           record->exit_signal,
                           record->log_path) != 0) {
                pthread_mutex_unlock(&ctx->metadata_lock);
                return -1;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        return 0;

    case CMD_LOGS:
        return stream_container_logs(ctx, req->container_id, client_fd);

    case CMD_STOP:
        stop_container(ctx, req->container_id);
        return send_textf(client_fd, "OK stop requested id=%s\n", req->container_id);

    default:
        return send_textf(client_fd, "ERROR unknown command\n");
    }
}

static void *control_client_thread(void *arg)
{
    control_client_ctx_t *client = arg;
    control_request_t req;

    if (read_all(client->client_fd, &req, sizeof(req)) != 0) {
        close(client->client_fd);
        free(client);
        return NULL;
    }

    if (handle_control_request(client->ctx, &req, client->client_fd) != 0)
        send_textf(client->client_fd, "ERROR request handling failed\n");

    close(client->client_fd);
    free(client);
    return NULL;
}

static void stop_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_by_id_locked(ctx, id);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "No such container: %s\n", id);
        return;
    }

    record->stop_requested = 1;
    if (record->state == CONTAINER_RUNNING || record->state == CONTAINER_STARTING)
        kill(record->host_pid, SIGKILL);
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void stop_all_running(supervisor_ctx_t *ctx)
{
    size_t i;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (i = 0; i < ctx->container_count; i++) {
        container_record_t *record = &ctx->containers[i];
        if (!record->in_use)
            continue;
        if (record->state == CONTAINER_RUNNING || record->state == CONTAINER_STARTING) {
            record->stop_requested = 1;
            kill(record->host_pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sigaction sa;
    struct sockaddr_un address;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        return 1;
    }

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    ctx.container_capacity = 16;
    ctx.containers = calloc(ctx.container_capacity, sizeof(*ctx.containers));
    if (!ctx.containers) {
        perror("calloc(containers)");
        pthread_mutex_destroy(&ctx.metadata_lock);
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create(logging_thread)");
        free(ctx.containers);
        pthread_mutex_destroy(&ctx.metadata_lock);
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        free(ctx.containers);
        pthread_mutex_destroy(&ctx.metadata_lock);
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, CONTROL_PATH, sizeof(address.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        free(ctx.containers);
        pthread_mutex_destroy(&ctx.metadata_lock);
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        free(ctx.containers);
        pthread_mutex_destroy(&ctx.metadata_lock);
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        free(ctx.containers);
        pthread_mutex_destroy(&ctx.metadata_lock);
        bounded_buffer_destroy(&ctx.log_buffer);
        return 1;
    }

    printf("Supervisor started. Base rootfs template: %s\n", rootfs);
    printf("Control socket: %s\n", CONTROL_PATH);
    fflush(stdout);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open(/dev/container_monitor)");

    while (!ctx.should_stop) {
        struct sockaddr_un client_address;
        socklen_t client_length = sizeof(client_address);
        int client_fd;

        if (g_supervisor_stop)
            ctx.should_stop = 1;

        if (g_supervisor_sigchld) {
            g_supervisor_sigchld = 0;
            reap_children(&ctx);
        }

        if (ctx.should_stop)
            break;

        client_fd = accept(ctx.server_fd, (struct sockaddr *)&client_address, &client_length);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        control_client_ctx_t *client = calloc(1, sizeof(*client));
        pthread_t thread;

        if (!client) {
            close(client_fd);
            continue;
        }

        client->ctx = &ctx;
        client->client_fd = client_fd;

        if (pthread_create(&thread, NULL, control_client_thread, client) != 0) {
            close(client_fd);
            free(client);
            continue;
        }

        pthread_detach(thread);
    }

    stop_all_running(&ctx);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    while (waitpid(-1, NULL, 0) > 0)
        ;

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    for (size_t i = 0; i < ctx.container_count; i++) {
        if (ctx.containers[i].producer_thread_started && !ctx.containers[i].producer_thread_joined) {
            ctx.containers[i].producer_thread_joined = 1;
            pthread_mutex_unlock(&ctx.metadata_lock);
            pthread_join(ctx.containers[i].producer_thread, NULL);
            pthread_mutex_lock(&ctx.metadata_lock);
        }
        free(ctx.containers[i].child_stack);
        ctx.containers[i].child_stack = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    free(ctx.containers);
    pthread_mutex_destroy(&ctx.metadata_lock);
    bounded_buffer_destroy(&ctx.log_buffer);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    struct sigaction sa;
    struct sigaction old_int;
    struct sigaction old_term;
    int rc;

    if (req->kind != CMD_RUN)
        return send_control_request_once(req);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = client_signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, &old_int) < 0 || sigaction(SIGTERM, &sa, &old_term) < 0)
        return send_control_request_once(req);

    g_client_interrupt = 0;
    rc = send_control_request_once(req);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);

    if (g_client_interrupt) {
        g_client_interrupt = 0;
        send_stop_control_request(req->container_id);
    }

    return rc;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
