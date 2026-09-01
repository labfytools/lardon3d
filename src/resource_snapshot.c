#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/resource_snapshot.h>

#include "resource_snapshot_internal.h"

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
read_exact_decimal_u64(const char *path, uint64_t *value)
{
    if (!path || !value) return false;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    char buffer[64];
    size_t total = 0;
    bool success = true;
    while (total + 1 < sizeof(buffer)) {
        ssize_t count = read(descriptor, buffer + total,
            sizeof(buffer) - total - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            success = false;
            break;
        }
        if (count == 0) break;
        total += (size_t)count;
    }
    if (success && total + 1 == sizeof(buffer)) {
        char extra;
        ssize_t count;
        do {
            count = read(descriptor, &extra, 1);
        } while (count < 0 && errno == EINTR);
        success = count == 0;
    }
    if (close(descriptor) != 0) success = false;
    if (!success || total == 0) return false;
    buffer[total] = '\0';
    const unsigned char *cursor = (const unsigned char *)buffer;
    uint64_t parsed = 0;
    size_t digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        ++cursor;
        ++digits;
    }
    if (digits == 0) return false;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n'
        || *cursor == '\r' || *cursor == '\f' || *cursor == '\v') {
        ++cursor;
    }
    if (*cursor) return false;
    *value = parsed;
    return true;
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

static bool
capture_pressure(const char *path, double *average)
{
    char buffer[512];
    if (!read_file(path, buffer, sizeof(buffer))) {
        return false;
    }
    return sscanf(buffer, "some avg10=%lf", average) == 1
        && *average >= 0.0 && *average <= 100.0;
}

static bool
vmstat_counter(const char *buffer, const char *key, uint64_t *value)
{
    const char *line = buffer;
    size_t key_length = strlen(key);
    while (*line) {
        if (strncmp(line, key, key_length) == 0 && line[key_length] == ' ') {
            errno = 0;
            char *end;
            unsigned long long parsed = strtoull(line + key_length + 1, &end, 10);
            if (errno == 0 && end != line + key_length + 1) {
                *value = (uint64_t)parsed;
                return true;
            }
            return false;
        }
        const char *newline = strchr(line, '\n');
        if (!newline) break;
        line = newline + 1;
    }
    return false;
}

void
lardon3d_resource_snapshot_capture_gpu_at_root(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    const char *drm_root
)
{
    if (!profile || !snapshot || !drm_root || !profile->gpu_available) {
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
    char path[256];
    int written = snprintf(
        path,
        sizeof(path),
        "%s/card%u/device/mem_info_vram_used",
        drm_root,
        profile->gpu_drm_card_index
    );
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return;
    }
    uint64_t used;
    if (read_exact_decimal_u64(path, &used)
        && used <= profile->gpu_memory_total_bytes) {
        snapshot->gpu_memory_available_known = true;
        snapshot->gpu_memory_available_bytes =
            profile->gpu_memory_total_bytes - used;
    }
}

static bool
capture_resource_snapshot(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    uint64_t *swap_total_bytes,
    char *error_message,
    size_t error_message_size
)
{
    set_error(error_message, error_message_size, "");
    if (!profile || !snapshot || !swap_total_bytes
        || profile->logical_cpu_count == 0
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
        || !meminfo_bytes(buffer, "SwapTotal", swap_total_bytes)
        || !meminfo_bytes(buffer, "SwapFree", &snapshot->swap_available_bytes)
        || !capture_load(snapshot)
        || clock_gettime(CLOCK_MONOTONIC, &snapshot->captured_at) != 0) {
        set_error(error_message, error_message_size, "Instantané système impossible.");
        return false;
    }
    snapshot->cpu_pressure_known = capture_pressure(
        "/proc/pressure/cpu", &snapshot->cpu_pressure_avg10);
    snapshot->memory_pressure_known = capture_pressure(
        "/proc/pressure/memory", &snapshot->memory_pressure_avg10);
    snapshot->io_pressure_known = capture_pressure(
        "/proc/pressure/io", &snapshot->io_pressure_avg10);
    if (read_file("/proc/vmstat", buffer, sizeof(buffer))) {
        snapshot->swap_activity_known = vmstat_counter(
            buffer, "pswpin", &snapshot->swap_pages_in)
            && vmstat_counter(buffer, "pswpout", &snapshot->swap_pages_out);
    }
    lardon3d_resource_snapshot_capture_gpu_at_root(
        profile,
        snapshot,
        "/sys/class/drm"
    );
    return true;
}

bool
lardon3d_resource_snapshot_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceSnapshot *snapshot,
    char *error_message,
    size_t error_message_size
)
{
    uint64_t ignored_swap_total = 0;
    /* CONTRACT: this historical entry point writes exactly the frozen
     * ResourceSnapshot size; extended telemetry must use observation_capture.
     */
    return capture_resource_snapshot(profile, snapshot, &ignored_swap_total,
        error_message, error_message_size);
}

bool
lardon3d_resource_observation_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceObservation *observation,
    char *error_message,
    size_t error_message_size
)
{
    if (observation) {
        *observation = (Lardon3DResourceObservation) {0};
    }
    if (!observation) {
        set_error(error_message, error_message_size,
            "Instantané système invalide.");
        return false;
    }
    if (!capture_resource_snapshot(profile, &observation->snapshot,
            &observation->swap_total_bytes, error_message,
            error_message_size)) {
        return false;
    }
    observation->swap_total_known = true;
    return true;
}
