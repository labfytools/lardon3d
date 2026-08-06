#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/resource_snapshot.h>

enum {
    PROC_BUFFER_CAPACITY = 32768,
};

static void
set_error(char *message, size_t size, const char *text)
{
    if (message && size > 0) {
        (void)snprintf(message, size, "%s", text);
    }
}

static bool
read_file(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    size_t total = 0;
    while (total + 1 < capacity) {
        ssize_t count = read(descriptor, buffer + total, capacity - total - 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            (void)close(descriptor);
            return false;
        }
        if (count == 0) {
            break;
        }
        total += (size_t)count;
    }
    bool success = close(descriptor) == 0;
    buffer[total] = '\0';
    return success;
}

static bool
meminfo_bytes(const char *buffer, const char *key, uint64_t *bytes)
{
    const char *line = buffer;
    size_t key_length = strlen(key);
    while (*line) {
        if (strncmp(line, key, key_length) == 0 && line[key_length] == ':') {
            const char *number = line + key_length + 1;
            while (*number == ' ' || *number == '\t') {
                ++number;
            }
            errno = 0;
            char *end;
            unsigned long long kibibytes = strtoull(number, &end, 10);
            if (errno != 0 || end == number || kibibytes > UINT64_MAX / 1024) {
                return false;
            }
            while (*end == ' ' || *end == '\t') {
                ++end;
            }
            if (strncmp(end, "kB", 2) != 0) {
                return false;
            }
            *bytes = (uint64_t)kibibytes * 1024;
            return true;
        }
        const char *newline = strchr(line, '\n');
        if (!newline) {
            break;
        }
        line = newline + 1;
    }
    return false;
}

static bool
capture_load(Lardon3DResourceSnapshot *snapshot)
{
    char buffer[256];
    if (!read_file("/proc/loadavg", buffer, sizeof(buffer))) {
        return false;
    }
    return sscanf(
        buffer,
        "%lf %lf %lf",
        &snapshot->cpu_load_1m,
        &snapshot->cpu_load_5m,
        &snapshot->cpu_load_15m
    ) == 3;
}

static void
capture_io_pressure(Lardon3DResourceSnapshot *snapshot)
{
    char buffer[512];
    if (!read_file("/proc/pressure/io", buffer, sizeof(buffer))) {
        return;
    }
    double average;
    if (sscanf(buffer, "some avg10=%lf", &average) == 1 && average >= 0.0) {
        snapshot->io_pressure_known = true;
        snapshot->io_pressure_avg10 = average;
    }
}

static void
capture_gpu_memory(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot
)
{
    if (!profile->gpu_available) {
        return;
    }
    if (profile->gpu_uses_shared_memory && !profile->gpu_memory_known) {
        snapshot->gpu_memory_available_known = true;
        snapshot->gpu_memory_available_bytes = snapshot->memory_available_bytes;
        return;
    }
    if (!profile->gpu_memory_known) {
        return;
    }
    for (unsigned int index = 0; index < 64; ++index) {
        char path[256];
        int written = snprintf(
            path,
            sizeof(path),
            "/sys/class/drm/card%u/device/mem_info_vram_used",
            index
        );
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        char buffer[64];
        if (!read_file(path, buffer, sizeof(buffer))) {
            continue;
        }
        errno = 0;
        char *end;
        unsigned long long used = strtoull(buffer, &end, 10);
        if (errno == 0 && end != buffer
            && (uint64_t)used <= profile->gpu_memory_total_bytes) {
            snapshot->gpu_memory_available_known = true;
            snapshot->gpu_memory_available_bytes =
                profile->gpu_memory_total_bytes - (uint64_t)used;
            return;
        }
    }
}

bool
lardon3d_resource_snapshot_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    char *error_message,
    size_t error_message_size
)
{
    set_error(error_message, error_message_size, "");
    if (!profile || !snapshot || profile->logical_cpu_count == 0
        || profile->memory_total_bytes == 0) {
        set_error(error_message, error_message_size, "Profil matériel invalide.");
        return false;
    }
    *snapshot = (Lardon3DResourceSnapshot) {0};
    char buffer[PROC_BUFFER_CAPACITY];
    if (!read_file("/proc/meminfo", buffer, sizeof(buffer))
        || !meminfo_bytes(
            buffer,
            "MemAvailable",
            &snapshot->memory_available_bytes
        )
        || !meminfo_bytes(buffer, "MemFree", &snapshot->memory_free_bytes)
        || !meminfo_bytes(buffer, "SwapFree", &snapshot->swap_available_bytes)
        || !capture_load(snapshot)
        || clock_gettime(CLOCK_REALTIME, &snapshot->captured_at) != 0) {
        set_error(error_message, error_message_size, "Instantané système impossible.");
        return false;
    }
    capture_io_pressure(snapshot);
    capture_gpu_memory(profile, snapshot);
    return true;
}
