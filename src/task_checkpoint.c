#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/task_checkpoint.h>

enum {
    CHECKPOINT_SIZE = 516,
    CHECKPOINT_HEADER_SIZE = 20,
};

static const unsigned char checkpoint_magic[8] = {
    'L', '3', 'D', 'T', 'A', 'S', 'K', '\0'
};

static void
put_u32(unsigned char *output, uint32_t value)
{
    for (size_t index = 0; index < 4; ++index) {
        output[index] = (unsigned char)(value >> (index * 8));
    }
}

static void
put_u64(unsigned char *output, uint64_t value)
{
    for (size_t index = 0; index < 8; ++index) {
        output[index] = (unsigned char)(value >> (index * 8));
    }
}

static uint32_t
get_u32(const unsigned char *input)
{
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
        value |= (uint32_t)input[index] << (index * 8);
    }
    return value;
}

static uint64_t
get_u64(const unsigned char *input)
{
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= (uint64_t)input[index] << (index * 8);
    }
    return value;
}

static uint32_t
payload_checksum(const unsigned char *data, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool
valid_text(const char *text, size_t capacity)
{
    return memchr(text, '\0', capacity) != NULL;
}

static bool
valid_snapshot(const Lardon3DTaskDurableSnapshot *snapshot)
{
    return snapshot && snapshot->id != 0 && snapshot->name[0]
        && valid_text(snapshot->name, sizeof(snapshot->name))
        && valid_text(snapshot->message, sizeof(snapshot->message))
        && snapshot->progress <= 100
        && snapshot->saved_state >= TASK_PENDING
        && snapshot->saved_state <= TASK_COMPLETED
        && snapshot->recovery_state >= TASK_PENDING
        && snapshot->recovery_state <= TASK_COMPLETED
        && snapshot->recovery_state
            == ((snapshot->saved_state == TASK_RUNNING
                    || snapshot->saved_state == TASK_PAUSED)
                ? TASK_PENDING : snapshot->saved_state)
        && (snapshot->recovery_state != TASK_COMPLETED
            || snapshot->progress == 100)
        && snapshot->estimate.minimum_batch_size > 0
        && snapshot->estimate.maximum_batch_size
            >= snapshot->estimate.minimum_batch_size
        && snapshot->estimate.desired_cpu_threads > 0
        && snapshot->estimate.task_class >= LARDON3D_RESOURCE_TASK_GENERAL
        && snapshot->estimate.task_class <= LARDON3D_RESOURCE_TASK_MIXED
        && ((snapshot->estimate.gpu_memory_fixed_bytes == 0
                && snapshot->estimate.gpu_memory_bytes_per_item == 0)
            || snapshot->estimate.desired_gpu_slots > 0)
        && snapshot->started_at.tv_nsec >= 0
        && snapshot->started_at.tv_nsec < 1000000000L
        && snapshot->started_at.tv_sec >= 0
        && snapshot->finished_at.tv_nsec >= 0
        && snapshot->finished_at.tv_nsec < 1000000000L
        && snapshot->finished_at.tv_sec >= 0;
}

static bool
decode_size(uint64_t value, size_t *decoded)
{
    if (value > SIZE_MAX) {
        return false;
    }
    *decoded = (size_t)value;
    return true;
}

static uint64_t
maximum_time_value(void)
{
    unsigned int bits = (unsigned int)(sizeof(time_t) * CHAR_BIT);
    bool signed_time = (time_t)-1 < (time_t)0;
    if (bits > 64 || !signed_time) {
        return UINT64_MAX;
    }
    if (bits == 64) {
        return UINT64_MAX >> 1;
    }
    return (UINT64_C(1) << (bits - 1)) - 1;
}

static bool
decode_time(uint64_t value, time_t *decoded)
{
    if (value > maximum_time_value()) {
        return false;
    }
    *decoded = (time_t)value;
    return true;
}

static void
encode(unsigned char data[CHECKPOINT_SIZE], const Lardon3DTaskDurableSnapshot *s)
{
    memset(data, 0, CHECKPOINT_SIZE);
    memcpy(data, checkpoint_magic, sizeof(checkpoint_magic));
    put_u32(data + 8, LARDON3D_TASK_CHECKPOINT_VERSION);
    put_u32(data + 12, CHECKPOINT_SIZE);
    size_t at = CHECKPOINT_HEADER_SIZE;
#define PUT64(value) do { put_u64(data + at, (uint64_t)(value)); at += 8; } while (0)
#define PUT32(value) do { put_u32(data + at, (uint32_t)(value)); at += 4; } while (0)
    PUT64(s->id);
    memcpy(data + at, s->name, sizeof(s->name)); at += sizeof(s->name);
    PUT64(s->estimate.memory_fixed_bytes);
    PUT64(s->estimate.gpu_memory_fixed_bytes);
    PUT64(s->estimate.memory_bytes_per_item);
    PUT64(s->estimate.gpu_memory_bytes_per_item);
    PUT64(s->estimate.minimum_batch_size);
    PUT64(s->estimate.maximum_batch_size);
    PUT32(s->estimate.desired_cpu_threads);
    PUT32(s->estimate.desired_gpu_slots);
    PUT32(s->estimate.desired_io_slots);
    PUT32(s->estimate.task_class);
    PUT32(s->progress);
    PUT32(s->saved_state);
    PUT32(s->recovery_state);
    memcpy(data + at, s->message, sizeof(s->message)); at += sizeof(s->message);
    PUT64(s->started_at.tv_sec); PUT32(s->started_at.tv_nsec);
    PUT64(s->finished_at.tv_sec); PUT32(s->finished_at.tv_nsec);
    PUT32(s->sequence_count);
#undef PUT64
#undef PUT32
    put_u32(
        data + 16,
        payload_checksum(data + CHECKPOINT_HEADER_SIZE,
            CHECKPOINT_SIZE - CHECKPOINT_HEADER_SIZE)
    );
}

static bool
decode(const unsigned char data[CHECKPOINT_SIZE], Lardon3DTaskDurableSnapshot *s)
{
    *s = (Lardon3DTaskDurableSnapshot) {0};
    size_t at = CHECKPOINT_HEADER_SIZE;
#define GET64(target) do { (target) = get_u64(data + at); at += 8; } while (0)
#define GET32(target) do { (target) = get_u32(data + at); at += 4; } while (0)
    GET64(s->id);
    memcpy(s->name, data + at, sizeof(s->name)); at += sizeof(s->name);
    GET64(s->estimate.memory_fixed_bytes);
    GET64(s->estimate.gpu_memory_fixed_bytes);
    GET64(s->estimate.memory_bytes_per_item);
    GET64(s->estimate.gpu_memory_bytes_per_item);
    uint64_t size_value;
    GET64(size_value);
    if (!decode_size(size_value, &s->estimate.minimum_batch_size)) {
        return false;
    }
    GET64(size_value);
    if (!decode_size(size_value, &s->estimate.maximum_batch_size)) {
        return false;
    }
    GET32(s->estimate.desired_cpu_threads);
    GET32(s->estimate.desired_gpu_slots);
    GET32(s->estimate.desired_io_slots);
    uint32_t task_class;
    GET32(task_class);
    s->estimate.task_class = (Lardon3DResourceTaskClass)task_class;
    GET32(s->progress);
    uint32_t state;
    GET32(state); s->saved_state = (Lardon3DTaskState)state;
    GET32(state); s->recovery_state = (Lardon3DTaskState)state;
    memcpy(s->message, data + at, sizeof(s->message)); at += sizeof(s->message);
    uint64_t seconds;
    uint32_t nanoseconds;
    GET64(seconds);
    if (!decode_time(seconds, &s->started_at.tv_sec)) {
        return false;
    }
    GET32(nanoseconds); s->started_at.tv_nsec = (long)nanoseconds;
    GET64(seconds);
    if (!decode_time(seconds, &s->finished_at.tv_sec)) {
        return false;
    }
    GET32(nanoseconds); s->finished_at.tv_nsec = (long)nanoseconds;
    GET32(s->sequence_count);
#undef GET64
#undef GET32
    return true;
}

static bool
write_all(int descriptor, const unsigned char *data, size_t size)
{
    while (size > 0) {
        ssize_t written = write(descriptor, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        data += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static bool
sync_parent_directory(const char *path)
{
#ifdef LARDON3D_CHECKPOINT_TESTING
    const char *forced_failure = getenv(
        "LARDON3D_TEST_CHECKPOINT_SYNC_DIRECTORY_FAILURE"
    );
    if (forced_failure && strcmp(forced_failure, "1") == 0) {
        errno = EIO;
        return false;
    }
#endif
    char parent[4096];
    int length = snprintf(parent, sizeof(parent), "%s", path);
    if (length < 0 || (size_t)length >= sizeof(parent)) {
        return false;
    }
    char *separator = strrchr(parent, '/');
    if (separator) {
        *separator = '\0';
        if (!parent[0]) {
            parent[0] = '/';
            parent[1] = '\0';
        }
    } else {
        (void)snprintf(parent, sizeof(parent), ".");
    }
    int descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    bool synced = fsync(descriptor) == 0;
    return close(descriptor) == 0 && synced;
}

Lardon3DTaskCheckpointResult
lardon3d_task_checkpoint_save(
    const char *path,
    const Lardon3DTaskDurableSnapshot *snapshot
)
{
    if (!path || !path[0] || !valid_snapshot(snapshot)) {
        return LARDON3D_TASK_CHECKPOINT_INVALID;
    }
    char temporary[4096];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        return LARDON3D_TASK_CHECKPOINT_INVALID;
    }
    unsigned char data[CHECKPOINT_SIZE];
    encode(data, snapshot);
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        return LARDON3D_TASK_CHECKPOINT_IO_ERROR;
    }
    bool ready_to_publish = write_all(descriptor, data, sizeof(data))
        && fsync(descriptor) == 0;
    if (close(descriptor) != 0) {
        ready_to_publish = false;
    }
    descriptor = -1;
    if (!ready_to_publish || rename(temporary, path) != 0) {
        int saved_errno = errno;
        (void)unlink(temporary);
        errno = saved_errno;
        return LARDON3D_TASK_CHECKPOINT_IO_ERROR;
    }
    return sync_parent_directory(path) ? LARDON3D_TASK_CHECKPOINT_OK
        : LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE;
}

Lardon3DTaskCheckpointResult
lardon3d_task_checkpoint_load(
    const char *path,
    Lardon3DTaskDurableSnapshot *snapshot,
    uint32_t *format_version
)
{
    if (!path || !path[0] || !snapshot) {
        return LARDON3D_TASK_CHECKPOINT_INVALID;
    }
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return errno == ENOENT ? LARDON3D_TASK_CHECKPOINT_NOT_FOUND
            : LARDON3D_TASK_CHECKPOINT_IO_ERROR;
    }
    unsigned char data[CHECKPOINT_SIZE + 1];
    size_t used = 0;
    while (used < sizeof(data)) {
        ssize_t amount = read(descriptor, data + used, sizeof(data) - used);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount < 0) {
            (void)close(descriptor);
            return LARDON3D_TASK_CHECKPOINT_IO_ERROR;
        }
        if (amount == 0) {
            break;
        }
        used += (size_t)amount;
    }
    if (close(descriptor) != 0) {
        return LARDON3D_TASK_CHECKPOINT_IO_ERROR;
    }
    if (used < CHECKPOINT_HEADER_SIZE
        || memcmp(data, checkpoint_magic, sizeof(checkpoint_magic)) != 0) {
        return LARDON3D_TASK_CHECKPOINT_INVALID;
    }
    uint32_t version = get_u32(data + 8);
    if (format_version) {
        *format_version = version;
    }
    if (version != LARDON3D_TASK_CHECKPOINT_VERSION) {
        return LARDON3D_TASK_CHECKPOINT_UNSUPPORTED_VERSION;
    }
    if (get_u32(data + 12) != CHECKPOINT_SIZE || used != CHECKPOINT_SIZE
        || get_u32(data + 16) != payload_checksum(
            data + CHECKPOINT_HEADER_SIZE,
            CHECKPOINT_SIZE - CHECKPOINT_HEADER_SIZE
        )) {
        return LARDON3D_TASK_CHECKPOINT_INVALID;
    }
    return decode(data, snapshot) && valid_snapshot(snapshot)
        ? LARDON3D_TASK_CHECKPOINT_OK
        : LARDON3D_TASK_CHECKPOINT_INVALID;
}
