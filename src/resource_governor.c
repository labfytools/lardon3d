#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#include <sys/syscall.h>
#include <sys/types.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/resource_governor.h>

#include "resource_governor_internal.h"

struct Lardon3DResourceReservation {
    Lardon3DResourceReservationInfo information;
    uint64_t charged_memory_bytes;
    struct Lardon3DResourceReservation *next;
};

enum {
    LARDON3D_BATCH_METRICS_CAPACITY = 8,
    LARDON3D_CAPABILITY_FEEDBACK_CAPACITY = 32,
    LARDON3D_CAPABILITY_THROUGHPUT_OBSERVATIONS = 2,
    LARDON3D_GPU_BATCH_THROUGHPUT_OBSERVATIONS = 8,
    LARDON3D_TELEMETRY_TEXT_CAPACITY = 16384,
    LARDON3D_PROC_STAT_CAPACITY = 196608,
};

typedef struct {
    size_t batch_size;
    uint64_t duration_ns;
    size_t peak_memory_bytes;
} Lardon3DBatchMetrics;

typedef enum {
    LARDON3D_CAPABILITY_TRIAL_NONE = 0,
    LARDON3D_CAPABILITY_TRIAL_CPU = 1,
    LARDON3D_CAPABILITY_TRIAL_INFLIGHT = 2,
    LARDON3D_CAPABILITY_TRIAL_BATCH = 3,
} Lardon3DCapabilityTrialDimension;

typedef struct {
    bool used;
    char task_kind[LARDON3D_TASK_KIND_CAPACITY];
    uint32_t task_kind_version;
    Lardon3DResourceBackend backend;
    unsigned int adaptive_cpu_limit;
    unsigned int accepted_cpu_limit;
    size_t adaptive_batch_limit;
    size_t accepted_batch_limit;
    size_t adaptive_inflight_limit;
    size_t accepted_inflight_limit;
    Lardon3DCapabilityTrialDimension trial_dimension;
    unsigned int baseline_observations;
    unsigned int trial_observations;
    uint64_t baseline_rate_milli;
    uint64_t baseline_rates_milli[
        LARDON3D_GPU_BATCH_THROUGHPUT_OBSERVATIONS];
    uint64_t trial_rates_milli[
        LARDON3D_GPU_BATCH_THROUGHPUT_OBSERVATIONS];
    bool cpu_growth_stopped;
    bool inflight_growth_stopped;
    bool batch_growth_stopped;
    uint64_t diagnostic_serial;
    uint64_t diagnostic_update_order;
    Lardon3DResourceSequenceDiagnostic diagnostic;
    Lardon3DResourceSequenceAggregate aggregate;
    bool aggregate_selection_known;
    unsigned int aggregate_cpu_threads;
    unsigned int aggregate_gpu_slots;
    unsigned int aggregate_io_slots;
    size_t aggregate_batch_size;
    size_t aggregate_inflight_limit;
    unsigned int aggregate_helper_limit;
} Lardon3DCapabilityFeedback;

struct Lardon3DResourceGovernor {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint64_t generation;
    Lardon3DHardwareProfile profile;
    Lardon3DResourcePolicy policy;
    uint64_t next_reservation_id;
    uint64_t memory_reserved_bytes;
    uint64_t gpu_memory_reserved_bytes;
    unsigned int cpu_reserved;
    unsigned int gpu_slots_reserved;
    unsigned int io_slots_reserved;
    Lardon3DBatchMetrics batch_metrics[LARDON3D_RESOURCE_TASK_MIXED + 1][LARDON3D_BATCH_METRICS_CAPACITY];
    size_t batch_metrics_count[LARDON3D_RESOURCE_TASK_MIXED + 1];
    size_t batch_metrics_head[LARDON3D_RESOURCE_TASK_MIXED + 1];
    Lardon3DCapabilityFeedback capability_feedback[
        LARDON3D_CAPABILITY_FEEDBACK_CAPACITY
    ];
    size_t capability_feedback_replace;
    uint64_t capability_diagnostic_serial;
    uint64_t capability_diagnostic_update_order;
    bool orb_vulkan_backend_available;
    Lardon3DResourceCpuTopologyInput cpu_topology;
    Lardon3DResourceCpuPolicyDiagnostic cpu_policy;
    bool internal_force_worker_affinity_failure;
    size_t active_count;
    bool swap_baseline_known;
    uint64_t last_swap_pages_in;
    uint64_t last_swap_pages_out;
    unsigned int pressure_streak;
    unsigned int recovery_streak;
    unsigned int slow_start_streak;
    size_t slow_start_limit;
    bool slow_start_active;
    Lardon3DResourcePressure pressure;
    bool internal_now_known;
    struct timespec internal_now;
    bool internal_force_capture_failure;
#if defined(LARDON3D_RESOURCE_GOVERNOR_CAPTURE_TESTING)
    bool internal_capture_snapshot_override;
    Lardon3DResourceSnapshot internal_capture_snapshot;
#endif
    /* Host observations are private operational evidence. In particular RSS
     * is never charged as Task-owned memory and pool utilization never
     * substitutes for Gate G load/pressure admission. Cumulative inputs are
     * retained only long enough to form the next bounded delta. */
    Lardon3DResourceHostTelemetry host_telemetry;
    bool telemetry_override;
    bool telemetry_cpu_baseline_known;
    uint64_t telemetry_cpu_total[LARDON3D_RESOURCE_CPU_MAX];
    uint64_t telemetry_cpu_idle[LARDON3D_RESOURCE_CPU_MAX];
    bool telemetry_swap_baseline_known;
    uint64_t telemetry_swap_pages_in;
    uint64_t telemetry_swap_pages_out;
    Lardon3DResourceReservation *active;
    Lardon3DResourceReservation *released;
    /* The Governor owns orchestration metadata, never the physical controller
     * or caller lease storage. The fixed address registry makes wrapper
     * release exact and bounds all external-resource bookkeeping. */
    bool external_storage_registered;
    Lardon3DSsdController *external_storage_controller;
    Lardon3DResourceExternalStorage external_storage;
    /* Begin freezes provenance and the last physical count before releasing
     * this mutex. Finish may then distinguish its exact saturated completion
     * from an arbitrary equal-generation public copy. */
    bool external_storage_operation_active;
    bool external_storage_operation_acquire;
    Lardon3DSsdScratchLease *external_storage_operation_lease;
    size_t external_storage_operation_start_lease_count;
    Lardon3DSsdScratchLease *external_storage_leases[
        LARDON3D_SSD_MAX_SCRATCH_LEASES];
    size_t external_storage_lease_count;
};

static bool cpu_mask_test(
    const uint64_t mask[LARDON3D_RESOURCE_CPU_MASK_WORDS],
    unsigned int cpu
);

static bool
ascii_space(char value)
{
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

static bool
parse_u64_token(const char **cursor, const char *end, uint64_t *value)
{
    const char *position = *cursor;
    while (position < end && (*position == ' ' || *position == '\t')) {
        ++position;
    }
    if (position == end || *position < '0' || *position > '9') return false;
    uint64_t parsed = 0;
    do {
        unsigned int digit = (unsigned int)(*position - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        ++position;
    } while (position < end && *position >= '0' && *position <= '9');
    *cursor = position;
    *value = parsed;
    return true;
}

static bool
parse_percent_basis_points(
    const char *begin,
    const char *end,
    uint32_t *basis_points
)
{
    const char *position = begin;
    uint64_t whole = 0;
    if (!parse_u64_token(&position, end, &whole) || whole > 100) return false;
    uint32_t fraction = 0;
    unsigned int digits = 0;
    if (position < end && *position == '.') {
        ++position;
        if (position == end || *position < '0' || *position > '9') return false;
        while (position < end && *position >= '0' && *position <= '9') {
            if (digits == 2) return false;
            fraction = fraction * 10 + (uint32_t)(*position - '0');
            ++digits;
            ++position;
        }
    }
    while (digits < 2) {
        fraction *= 10;
        ++digits;
    }
    if (position != end || (whole == 100 && fraction != 0)) return false;
    *basis_points = (uint32_t)whole * 100 + fraction;
    return true;
}

static bool
line_value_u64(
    const char *text,
    const char *name,
    uint64_t *value,
    bool kilobytes
)
{
    size_t name_length = strlen(name);
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if ((size_t)(end - line) > name_length
            && memcmp(line, name, name_length) == 0
            && (line[name_length] == ' ' || line[name_length] == '\t'
                || line[name_length] == ':')) {
            const char *cursor = line + name_length;
            if (*cursor == ':') ++cursor;
            uint64_t parsed = 0;
            if (!parse_u64_token(&cursor, end, &parsed)) return false;
            while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
            if (kilobytes) {
                if ((size_t)(end - cursor) != 2 || cursor[0] != 'k'
                    || cursor[1] != 'B' || parsed > UINT64_MAX / 1024) {
                    return false;
                }
                parsed *= 1024;
            } else if (cursor != end) {
                return false;
            }
            *value = parsed;
            return true;
        }
        line = *end ? end + 1 : end;
    }
    return false;
}

static bool
psi_value(const char *text, const char *category, uint32_t *basis_points)
{
    size_t category_length = strlen(category);
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *cursor = line;
        const char *token_end = cursor;
        while (token_end < end && !ascii_space(*token_end)) ++token_end;
        if ((size_t)(token_end - cursor) == category_length
            && memcmp(cursor, category, category_length) == 0) {
            bool found = false;
            uint32_t parsed = 0;
            cursor = token_end;
            while (cursor < end) {
                while (cursor < end && ascii_space(*cursor)) ++cursor;
                if (cursor == end) break;
                token_end = cursor;
                while (token_end < end && !ascii_space(*token_end)) ++token_end;
                if ((size_t)(token_end - cursor) >= 6
                    && memcmp(cursor, "avg10=", 6) == 0) {
                    /* PSI input is operational safety evidence. Accept one
                     * exact standalone avg10 field only: substring matches,
                     * duplicates, signs, and silently rounded precision must
                     * degrade the selected line to unknown. */
                    if (found || !parse_percent_basis_points(
                            cursor + 6, token_end, &parsed)) {
                        return false;
                    }
                    found = true;
                }
                cursor = token_end;
            }
            if (!found) return false;
            *basis_points = parsed;
            return true;
        }
        line = *end ? end + 1 : end;
    }
    return false;
}

typedef struct {
    bool seen[LARDON3D_RESOURCE_CPU_MAX];
    uint64_t total[LARDON3D_RESOURCE_CPU_MAX];
    uint64_t idle[LARDON3D_RESOURCE_CPU_MAX];
} Lardon3DComputeCpuTicks;

static bool
compute_cpu_ticks_locked(
    const Lardon3DResourceGovernor *governor,
    const char *text,
    Lardon3DComputeCpuTicks *ticks
)
{
    if (!governor->cpu_policy.affinity_configured) {
        /* A portable count is an admission budget, not an invented CPU mask.
         * Without an active compute mask, whole-host ticks cannot honestly be
         * relabelled as compute-pool utilization. */
        return false;
    }
    *ticks = (Lardon3DComputeCpuTicks){0};
    unsigned int found_count = 0;
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if ((size_t)(end - line) > 4 && memcmp(line, "cpu", 3) == 0
            && line[3] >= '0' && line[3] <= '9') {
            const char *cursor = line + 3;
            uint64_t cpu = 0;
            if (!parse_u64_token(&cursor, end, &cpu)
                || cpu >= LARDON3D_RESOURCE_CPU_MAX
                || (cursor < end && *cursor != ' ' && *cursor != '\t')) {
                return false;
            }
            bool selected = cpu_mask_test(governor->cpu_policy.compute_mask,
                (unsigned int)cpu);
            if (selected) {
                if (ticks->seen[cpu]) return false;
                uint64_t fields[10] = {0};
                unsigned int count = 0;
                while (cursor < end) {
                    while (cursor < end
                        && (*cursor == ' ' || *cursor == '\t')) ++cursor;
                    if (cursor == end) break;
                    if (count == 10) return false;
                    if (!parse_u64_token(&cursor, end, &fields[count])) return false;
                    ++count;
                }
                if (count < 4) return false;
                uint64_t line_total = 0;
                unsigned int accounting_fields = count < 8 ? count : 8;
                for (unsigned int index = 0; index < accounting_fields; ++index) {
                    if (line_total > UINT64_MAX - fields[index]) return false;
                    line_total += fields[index];
                }
                uint64_t line_idle = fields[3];
                if (count > 4) {
                    if (line_idle > UINT64_MAX - fields[4]) return false;
                    line_idle += fields[4];
                }
                if (line_idle > line_total) return false;
                ticks->seen[cpu] = true;
                ticks->total[cpu] = line_total;
                ticks->idle[cpu] = line_idle;
                ++found_count;
            }
        }
        line = *end ? end + 1 : end;
    }
    if (found_count != governor->cpu_policy.compute_cpu_count) return false;
    for (unsigned int cpu = 0; cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
        if (cpu_mask_test(governor->cpu_policy.compute_mask, cpu)
            && !ticks->seen[cpu]) {
            return false;
        }
    }
    return true;
}

static bool
parse_gpu_busy(const char *text, uint32_t *basis_points)
{
    const char *end = text + strlen(text);
    const char *cursor = text;
    uint64_t percent = 0;
    if (!parse_u64_token(&cursor, end, &percent) || percent > 100) return false;
    while (cursor < end && ascii_space(*cursor)) ++cursor;
    if (cursor != end) return false;
    *basis_points = (uint32_t)percent * 100;
    return true;
}

static bool
read_bounded_text(const char *path, char *text, size_t capacity)
{
    if (!path || !text || capacity < 2) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    size_t used = 0;
    bool ok = true;
    while (used < capacity - 1) {
        ssize_t count = read(fd, text + used, capacity - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    if (ok && used == capacity - 1) {
        char extra;
        ssize_t count;
        do {
            count = read(fd, &extra, 1);
        } while (count < 0 && errno == EINTR);
        ok = count == 0;
    }
    if (close(fd) != 0) ok = false;
    if (!ok) return false;
    text[used] = '\0';
    return true;
}

static bool
read_gpu_busy_at_root(
    const char *root,
    unsigned int card_index,
    char *text,
    size_t capacity,
    uint32_t *basis_points
)
{
    if (!root || !root[0] || !text || !basis_points || card_index > 63) {
        return false;
    }
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path),
        "%s/card%u/device/gpu_busy_percent", root, card_index);
    return written > 0 && (size_t)written < sizeof(path)
        && read_bounded_text(path, text, capacity)
        && parse_gpu_busy(text, basis_points);
}

bool
lardon3d_resource_governor_internal_read_gpu_busy_at_root(
    const char *root,
    unsigned int card_index,
    uint32_t *basis_points
)
{
    char text[32];
    return read_gpu_busy_at_root(
        root, card_index, text, sizeof(text), basis_points);
}

static void
sample_telemetry_raw_locked(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceTelemetryRaw *raw,
    Lardon3DResourceHostTelemetry *telemetry
)
{
    *telemetry = (Lardon3DResourceHostTelemetry){0};
    if (raw->meminfo) {
        telemetry->memory_available_known = line_value_u64(
            raw->meminfo, "MemAvailable", &telemetry->memory_available_bytes,
            true);
    }
    if (raw->memory_psi) {
        telemetry->memory_psi_some_known = psi_value(
            raw->memory_psi, "some",
            &telemetry->memory_psi_some_basis_points);
        telemetry->memory_psi_full_known = psi_value(
            raw->memory_psi, "full",
            &telemetry->memory_psi_full_basis_points);
    }
    if (raw->io_psi) {
        telemetry->io_psi_some_known = psi_value(
            raw->io_psi, "some", &telemetry->io_psi_some_basis_points);
        telemetry->io_psi_full_known = psi_value(
            raw->io_psi, "full", &telemetry->io_psi_full_basis_points);
    }
    if (raw->process_status) {
        telemetry->process_rss_known = line_value_u64(
            raw->process_status, "VmRSS", &telemetry->process_rss_bytes,
            true);
        telemetry->process_peak_rss_known = line_value_u64(
            raw->process_status, "VmHWM", &telemetry->process_peak_rss_bytes,
            true);
    }
    if (raw->gpu_busy_percent) {
        telemetry->gpu_busy_known = parse_gpu_busy(
            raw->gpu_busy_percent, &telemetry->gpu_busy_basis_points);
    }
    if (raw->proc_stat) {
        Lardon3DComputeCpuTicks ticks;
        if (compute_cpu_ticks_locked(governor, raw->proc_stat, &ticks)) {
            if (governor->telemetry_cpu_baseline_known) {
                uint64_t total_delta = 0;
                uint64_t idle_delta = 0;
                bool delta_valid = true;
                for (unsigned int cpu = 0;
                     cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
                    if (!ticks.seen[cpu]) continue;
                    if (ticks.total[cpu] < governor->telemetry_cpu_total[cpu]
                        || ticks.idle[cpu]
                            < governor->telemetry_cpu_idle[cpu]) {
                        delta_valid = false;
                        break;
                    }
                    uint64_t cpu_total_delta = ticks.total[cpu]
                        - governor->telemetry_cpu_total[cpu];
                    uint64_t cpu_idle_delta = ticks.idle[cpu]
                        - governor->telemetry_cpu_idle[cpu];
                    if (cpu_idle_delta > cpu_total_delta
                        || total_delta > UINT64_MAX - cpu_total_delta
                        || idle_delta > UINT64_MAX - cpu_idle_delta) {
                        delta_valid = false;
                        break;
                    }
                    total_delta += cpu_total_delta;
                    idle_delta += cpu_idle_delta;
                }
                if (delta_valid && total_delta > 0) {
                    uint64_t busy_delta = total_delta - idle_delta;
                    __uint128_t scaled = (__uint128_t)busy_delta * 10000;
                    telemetry->compute_pool_utilization_basis_points =
                        (uint32_t)(scaled / total_delta);
                    telemetry->compute_pool_utilization_known = true;
                }
            }
            memcpy(governor->telemetry_cpu_total, ticks.total,
                sizeof(ticks.total));
            memcpy(governor->telemetry_cpu_idle, ticks.idle,
                sizeof(ticks.idle));
            governor->telemetry_cpu_baseline_known = true;
        } else {
            governor->telemetry_cpu_baseline_known = false;
        }
    }
    if (raw->vmstat) {
        uint64_t pages_in = 0;
        uint64_t pages_out = 0;
        if (line_value_u64(raw->vmstat, "pswpin", &pages_in, false)
            && line_value_u64(raw->vmstat, "pswpout", &pages_out, false)) {
            if (governor->telemetry_swap_baseline_known
                && pages_in >= governor->telemetry_swap_pages_in
                && pages_out >= governor->telemetry_swap_pages_out) {
                telemetry->swap_delta_known = true;
                telemetry->swap_pages_in_delta =
                    pages_in - governor->telemetry_swap_pages_in;
                telemetry->swap_pages_out_delta =
                    pages_out - governor->telemetry_swap_pages_out;
            }
            governor->telemetry_swap_pages_in = pages_in;
            governor->telemetry_swap_pages_out = pages_out;
            governor->telemetry_swap_baseline_known = true;
        } else {
            governor->telemetry_swap_baseline_known = false;
        }
    }
    governor->host_telemetry = *telemetry;
}

bool
lardon3d_resource_governor_internal_sample_telemetry_raw(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceTelemetryRaw *raw,
    Lardon3DResourceHostTelemetry *telemetry
)
{
    if (!governor || !raw || !telemetry) return false;
    (void)pthread_mutex_lock(&governor->mutex);
    sample_telemetry_raw_locked(governor, raw, telemetry);
    /* Tests may install deterministic telemetry while exercising admissions;
     * production capture resumes only on a fresh Governor. This is a private
     * seam and cannot affect persisted Task identity or restart state. */
    governor->telemetry_override = true;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

static void
capture_host_telemetry(Lardon3DResourceGovernor *governor)
{
    (void)pthread_mutex_lock(&governor->mutex);
    bool overridden = governor->telemetry_override;
    (void)pthread_mutex_unlock(&governor->mutex);
    if (overridden) return;

    char *proc_stat = malloc(LARDON3D_PROC_STAT_CAPACITY);
    if (!proc_stat) return;
    char meminfo[LARDON3D_TELEMETRY_TEXT_CAPACITY];
    char memory_psi[LARDON3D_TELEMETRY_TEXT_CAPACITY];
    char io_psi[LARDON3D_TELEMETRY_TEXT_CAPACITY];
    char vmstat[LARDON3D_TELEMETRY_TEXT_CAPACITY];
    char status[LARDON3D_TELEMETRY_TEXT_CAPACITY];
    char gpu_busy[LARDON3D_TELEMETRY_TEXT_CAPACITY];
    bool stat_ok = read_bounded_text(
        "/proc/stat", proc_stat, LARDON3D_PROC_STAT_CAPACITY);
    bool meminfo_ok = read_bounded_text(
        "/proc/meminfo", meminfo, sizeof(meminfo));
    bool memory_psi_ok = read_bounded_text(
        "/proc/pressure/memory", memory_psi, sizeof(memory_psi));
    bool io_psi_ok = read_bounded_text(
        "/proc/pressure/io", io_psi, sizeof(io_psi));
    bool vmstat_ok = read_bounded_text(
        "/proc/vmstat", vmstat, sizeof(vmstat));
    bool status_ok = read_bounded_text(
        "/proc/self/status", status, sizeof(status));
    uint32_t ignored_gpu_busy = 0;
    /* Hardware discovery owns DRM identity. Falling through to another card
     * could train this Task from an unrelated GPU, so a missing/malformed
     * retained card is unknown rather than an invitation to scan. */
    bool gpu_busy_ok = governor->profile.gpu_available
        && read_gpu_busy_at_root(
            "/sys/class/drm", governor->profile.gpu_drm_card_index,
            gpu_busy, sizeof(gpu_busy), &ignored_gpu_busy);
    Lardon3DResourceTelemetryRaw raw = {
        .proc_stat = stat_ok ? proc_stat : NULL,
        .meminfo = meminfo_ok ? meminfo : NULL,
        .memory_psi = memory_psi_ok ? memory_psi : NULL,
        .io_psi = io_psi_ok ? io_psi : NULL,
        .vmstat = vmstat_ok ? vmstat : NULL,
        .process_status = status_ok ? status : NULL,
        .gpu_busy_percent = gpu_busy_ok ? gpu_busy : NULL,
    };
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceHostTelemetry telemetry;
    sample_telemetry_raw_locked(governor, &raw, &telemetry);
    (void)pthread_mutex_unlock(&governor->mutex);
    free(proc_stat);
}

static void
cpu_mask_set(uint64_t mask[LARDON3D_RESOURCE_CPU_MASK_WORDS], unsigned int cpu)
{
    mask[cpu / 64] |= UINT64_C(1) << (cpu % 64);
}

static bool
cpu_mask_test(
    const uint64_t mask[LARDON3D_RESOURCE_CPU_MASK_WORDS],
    unsigned int cpu
)
{
    return (mask[cpu / 64] & (UINT64_C(1) << (cpu % 64))) != 0;
}

static bool
mesa_shader_cache_disabled_value(const char *value)
{
    return value
        && (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
}

Lardon3DResourceDriverPolicyResult
lardon3d_resource_governor_internal_configure_driver_policy(void)
{
    const char *value = getenv("MESA_SHADER_CACHE_DISABLE");
    if (value) {
        return mesa_shader_cache_disabled_value(value)
            ? LARDON3D_RESOURCE_DRIVER_POLICY_INHERITED_SAFE
            : LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE;
    }
    /* CONTRACT: this executes on the startup thread before application pthread
     * creation. Mesa's disk-cache workers are known to widen affinity after
     * inheritance, while Linux exposes no stable-identity affinity operation
     * for a foreign thread. Defaulting the cache off removes those workers;
     * explicit user values are never overwritten. */
    if (setenv("MESA_SHADER_CACHE_DISABLE", "true", 0) != 0) {
        return LARDON3D_RESOURCE_DRIVER_POLICY_FAILED;
    }
    value = getenv("MESA_SHADER_CACHE_DISABLE");
    return mesa_shader_cache_disabled_value(value)
        ? LARDON3D_RESOURCE_DRIVER_POLICY_DEFAULTED
        : LARDON3D_RESOURCE_DRIVER_POLICY_FAILED;
}

static void
cpu_policy_count_fallback(
    unsigned int allowed_cpu_count,
    unsigned int reserve_cpu_count,
    bool externally_constrained,
    Lardon3DResourceCpuPolicyDiagnostic *diagnostic,
    const char *reason
)
{
    bool mesa_cache_disabled = mesa_shader_cache_disabled_value(
        getenv("MESA_SHADER_CACHE_DISABLE"));
    *diagnostic = (Lardon3DResourceCpuPolicyDiagnostic) {
        .runtime_thread_policy_active = mesa_cache_disabled,
        .mesa_shader_cache_disabled = mesa_cache_disabled,
        .externally_constrained = externally_constrained,
        .compute_cpu_count = allowed_cpu_count - reserve_cpu_count,
        .reserved_cpu_count = reserve_cpu_count,
    };
    (void)snprintf(diagnostic->reason, sizeof(diagnostic->reason), "%s",
        reason);
    (void)snprintf(diagnostic->runtime_thread_policy_reason,
        sizeof(diagnostic->runtime_thread_policy_reason), "%s",
        mesa_cache_disabled
            ? "worker-self-affinity-plus-mesa-disk-cache-disabled"
            : "mesa-disk-cache-safety-policy-not-established");
}

static unsigned int
additional_allowed_cpu_reserve(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy,
    size_t allowed_cpu_count
)
{
    /* CPUs already removed by an external affinity mask are host capacity the
     * application cannot consume. Count them toward the requested reserve so
     * the Governor does not subtract the same host allowance twice. */
    unsigned int unavailable = profile->logical_cpu_count
        - (unsigned int)allowed_cpu_count;
    return policy->system_cpu_reserve > unavailable
        ? policy->system_cpu_reserve - unavailable : 0;
}

static bool
cpu_topology_input_masks(
    const Lardon3DResourceCpuTopologyInput *input,
    uint64_t allowed[LARDON3D_RESOURCE_CPU_MASK_WORDS],
    unsigned int package_ids[LARDON3D_RESOURCE_CPU_MAX],
    unsigned int core_ids[LARDON3D_RESOURCE_CPU_MAX]
)
{
    if (!input || !input->affinity_available
        || input->allowed_cpu_count == 0
        || input->allowed_cpu_count > LARDON3D_RESOURCE_CPU_MAX) {
        return false;
    }
    memset(allowed, 0,
        sizeof(uint64_t) * LARDON3D_RESOURCE_CPU_MASK_WORDS);
    for (size_t index = 0; index < input->allowed_cpu_count; ++index) {
        unsigned int cpu = input->allowed_cpu_ids[index];
        if (cpu >= LARDON3D_RESOURCE_CPU_MAX
            || cpu_mask_test(allowed, cpu)) {
            return false;
        }
        cpu_mask_set(allowed, cpu);
    }
    if (!input->topology_available) {
        return true;
    }
    if (input->topology_entry_count != input->allowed_cpu_count) {
        return false;
    }
    bool seen[LARDON3D_RESOURCE_CPU_MAX] = {false};
    for (size_t index = 0; index < input->topology_entry_count; ++index) {
        const Lardon3DResourceCpuTopologyEntry *entry =
            &input->topology_entries[index];
        if (entry->cpu_id >= LARDON3D_RESOURCE_CPU_MAX
            || !cpu_mask_test(allowed, entry->cpu_id)
            || seen[entry->cpu_id]) {
            return false;
        }
        seen[entry->cpu_id] = true;
        package_ids[entry->cpu_id] = entry->package_id;
        core_ids[entry->cpu_id] = entry->core_id;
    }
    return true;
}

static void
build_cpu_policy(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy,
    const Lardon3DResourceCpuTopologyInput *input,
    Lardon3DResourceCpuPolicyDiagnostic *diagnostic
)
{
    cpu_policy_count_fallback(profile->logical_cpu_count,
        policy->system_cpu_reserve, false, diagnostic,
        "fallback-portable-affinity-unavailable");
    if (!input || !input->affinity_available) {
        return;
    }
    uint64_t allowed[LARDON3D_RESOURCE_CPU_MASK_WORDS];
    unsigned int package_ids[LARDON3D_RESOURCE_CPU_MAX] = {0};
    unsigned int core_ids[LARDON3D_RESOURCE_CPU_MAX] = {0};
    if (!cpu_topology_input_masks(input, allowed, package_ids, core_ids)
        || input->allowed_cpu_count > profile->logical_cpu_count) {
        cpu_policy_count_fallback(profile->logical_cpu_count,
            policy->system_cpu_reserve, false, diagnostic,
            "fallback-portable-affinity-invalid");
        return;
    }
    memcpy(diagnostic->allowed_mask, allowed, sizeof(allowed));
    bool externally_constrained =
        input->allowed_cpu_count < profile->logical_cpu_count;
    unsigned int requested_reserve = additional_allowed_cpu_reserve(
        profile, policy, input->allowed_cpu_count);
    if (requested_reserve == 0) {
        diagnostic->affinity_configured = true;
        diagnostic->externally_constrained = externally_constrained;
        diagnostic->compute_cpu_count = (unsigned int)input->allowed_cpu_count;
        diagnostic->reserved_cpu_count = 0;
        memcpy(diagnostic->compute_mask, allowed, sizeof(allowed));
        (void)snprintf(diagnostic->reason, sizeof(diagnostic->reason),
            "external-allowed-mask-is-compute-pool");
        return;
    }
    if (!input->topology_available) {
        cpu_policy_count_fallback(
            (unsigned int)input->allowed_cpu_count, requested_reserve,
            externally_constrained, diagnostic,
            "fallback-portable-topology-unavailable");
        memcpy(diagnostic->allowed_mask, allowed, sizeof(allowed));
        return;
    }

    typedef struct {
        unsigned int package_id;
        unsigned int core_id;
        unsigned int cpu_count;
    } CpuCoreGroup;
    CpuCoreGroup groups[LARDON3D_RESOURCE_CPU_MAX];
    size_t group_count = 0;
    for (size_t index = 0; index < input->allowed_cpu_count; ++index) {
        unsigned int cpu = input->allowed_cpu_ids[index];
        size_t group = 0;
        while (group < group_count
            && (groups[group].package_id != package_ids[cpu]
                || groups[group].core_id != core_ids[cpu])) {
            ++group;
        }
        if (group == group_count) {
            groups[group_count++] = (CpuCoreGroup) {
                .package_id = package_ids[cpu],
                .core_id = core_ids[cpu],
            };
        }
        ++groups[group].cpu_count;
    }
    /* Descending physical identity preserves the established deterministic
     * preference when several whole-core subsets have the same minimal size. */
    for (size_t left = 0; left < group_count; ++left) {
        for (size_t right = left + 1; right < group_count; ++right) {
            if (groups[right].package_id > groups[left].package_id
                || (groups[right].package_id == groups[left].package_id
                    && groups[right].core_id > groups[left].core_id)) {
                CpuCoreGroup temporary = groups[left];
                groups[left] = groups[right];
                groups[right] = temporary;
            }
        }
    }

    /* Fixed CPU_MAX subset DP finds the smallest complete-core reserve at or
     * above the logical target. It performs bounded O(CPU_MAX^2) work during
     * Governor construction/policy changes only; no scheduler or heap state is
     * introduced. Parent links reconstruct one deterministic selected subset. */
    bool reachable[LARDON3D_RESOURCE_CPU_MAX + 1] = {false};
    unsigned int parent_sum[LARDON3D_RESOURCE_CPU_MAX + 1] = {0};
    size_t parent_group[LARDON3D_RESOURCE_CPU_MAX + 1] = {0};
    reachable[0] = true;
    unsigned int allowed_count = (unsigned int)input->allowed_cpu_count;
    for (size_t group = 0; group < group_count; ++group) {
        unsigned int amount = groups[group].cpu_count;
        for (unsigned int sum = allowed_count - amount + 1; sum-- > 0;) {
            unsigned int next = sum + amount;
            if (reachable[sum] && !reachable[next]) {
                reachable[next] = true;
                parent_sum[next] = sum;
                parent_group[next] = group;
            }
        }
    }
    unsigned int reserved_count = requested_reserve;
    while (reserved_count < allowed_count && !reachable[reserved_count]) {
        ++reserved_count;
    }
    if (reserved_count >= allowed_count) {
        /* Tiny/asymmetric hosts may have no whole-core subset large enough to
         * meet the logical target while leaving compute capacity. Prefer the
         * largest complete-core reserve below target over splitting SMT peers. */
        reserved_count = requested_reserve;
        while (reserved_count > 0 && !reachable[reserved_count]) {
            --reserved_count;
        }
    }
    if (reserved_count == 0) {
        cpu_policy_count_fallback(allowed_count, requested_reserve,
            externally_constrained, diagnostic,
            "fallback-portable-topology-unsplittable");
        memcpy(diagnostic->allowed_mask, allowed, sizeof(allowed));
        return;
    }

    bool selected_groups[LARDON3D_RESOURCE_CPU_MAX] = {false};
    for (unsigned int sum = reserved_count; sum != 0;
         sum = parent_sum[sum]) {
        selected_groups[parent_group[sum]] = true;
    }
    uint64_t compute[LARDON3D_RESOURCE_CPU_MASK_WORDS];
    uint64_t reserved[LARDON3D_RESOURCE_CPU_MASK_WORDS] = {0};
    memcpy(compute, allowed, sizeof(compute));
    for (size_t index = 0; index < input->allowed_cpu_count; ++index) {
        unsigned int cpu = input->allowed_cpu_ids[index];
        for (size_t group = 0; group < group_count; ++group) {
            if (selected_groups[group]
                && groups[group].package_id == package_ids[cpu]
                && groups[group].core_id == core_ids[cpu]) {
                compute[cpu / 64] &= ~(UINT64_C(1) << (cpu % 64));
                cpu_mask_set(reserved, cpu);
                break;
            }
        }
    }
    diagnostic->affinity_configured = true;
    diagnostic->externally_constrained = externally_constrained;
    diagnostic->compute_cpu_count = allowed_count - reserved_count;
    diagnostic->reserved_cpu_count = reserved_count;
    memcpy(diagnostic->compute_mask, compute, sizeof(compute));
    memcpy(diagnostic->reserved_mask, reserved, sizeof(reserved));
    (void)snprintf(diagnostic->reason, sizeof(diagnostic->reason),
        "topology-complete-core-reserve");
}

static bool
parse_topology_value(
    const char *text,
    size_t length,
    unsigned int *value
)
{
    if (!text || length == 0 || !value) {
        return false;
    }
    size_t index = 0;
    unsigned int parsed = 0;
    while (index < length && text[index] >= '0' && text[index] <= '9') {
        unsigned int digit = (unsigned int)(text[index] - '0');
        if (parsed > (UINT_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
        ++index;
    }
    if (index == 0) {
        return false;
    }
    for (; index < length; ++index) {
        if (text[index] != ' ' && text[index] != '\t'
            && text[index] != '\r' && text[index] != '\n') {
            return false;
        }
    }
    *value = parsed;
    return true;
}

static bool
read_topology_value_file(const char *path, unsigned int *value)
{
#ifdef __linux__
    if (!path || !value) {
        return false;
    }
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    char text[32];
    size_t used = 0;
    bool eof = false;
    bool read_ok = true;
    while (used < sizeof(text) && !eof) {
        ssize_t count;
        do {
            count = read(descriptor, text + used, sizeof(text) - used);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            read_ok = false;
        } else if (count == 0) {
            eof = true;
        } else {
            used += (size_t)count;
        }
        if (!read_ok) {
            break;
        }
    }
    if (read_ok && !eof) {
        char extra;
        ssize_t count;
        do {
            count = read(descriptor, &extra, 1);
        } while (count < 0 && errno == EINTR);
        read_ok = count == 0;
        eof = count == 0;
    }
    bool closed = close(descriptor) == 0;
    /* sysfs values are tiny. EOF is part of the contract: accepting a full
     * buffer without the sentinel read would silently parse a truncated CPU
     * identity and could reserve the wrong physical core group. */
    return read_ok && eof && closed
        && parse_topology_value(text, used, value);
#else
    (void)path;
    (void)value;
    return false;
#endif
}

static bool
read_topology_value(unsigned int cpu, const char *field, unsigned int *value)
{
#ifdef __linux__
    char path[160];
    int written = snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpu%u/topology/%s", cpu, field);
    return written >= 0 && (size_t)written < sizeof(path)
        && read_topology_value_file(path, value);
#else
    (void)cpu;
    (void)field;
    (void)value;
    return false;
#endif
}

bool
lardon3d_resource_governor_internal_read_topology_value_file(
    const char *path,
    unsigned int *value
)
{
    /* Private deterministic seam: tests use ordinary temporary files to
     * exercise the exact bounded sysfs reader without mutating host sysfs. */
    return read_topology_value_file(path, value);
}

static void
capture_live_cpu_topology(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceCpuTopologyInput *input
)
{
    *input = (Lardon3DResourceCpuTopologyInput) {0};
#ifdef __linux__
    long system_cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (system_cpu_count < 1
        || (unsigned long)system_cpu_count != profile->logical_cpu_count
        || profile->logical_cpu_count > LARDON3D_RESOURCE_CPU_MAX) {
        return;
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return;
    }
    for (unsigned int cpu = 0; cpu < LARDON3D_RESOURCE_CPU_MAX
         && cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET((size_t)cpu, &allowed)) {
            input->allowed_cpu_ids[input->allowed_cpu_count++] = cpu;
        }
    }
    if (input->allowed_cpu_count == 0) {
        *input = (Lardon3DResourceCpuTopologyInput) {0};
        return;
    }
    input->affinity_available = true;
    input->topology_available = true;
    for (size_t index = 0; index < input->allowed_cpu_count; ++index) {
        unsigned int cpu = input->allowed_cpu_ids[index];
        Lardon3DResourceCpuTopologyEntry *entry =
            &input->topology_entries[input->topology_entry_count];
        entry->cpu_id = cpu;
        if (!read_topology_value(cpu, "physical_package_id",
                &entry->package_id)
            || !read_topology_value(cpu, "core_id", &entry->core_id)) {
            input->topology_available = false;
            input->topology_entry_count = 0;
            return;
        }
        ++input->topology_entry_count;
    }
#else
    (void)profile;
#endif
}

static bool
valid_profile(const Lardon3DHardwareProfile *profile)
{
    return profile && profile->logical_cpu_count > 0
        && profile->memory_total_bytes > 0
        && (!profile->gpu_available || profile->gpu_drm_card_index < 64)
        && (!profile->gpu_memory_known
            || (profile->gpu_available
                && profile->gpu_memory_total_bytes > 0));
}

static bool
valid_policy(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy
)
{
    return valid_profile(profile) && policy
        && policy->system_memory_reserve_bytes < profile->memory_total_bytes
        && policy->emergency_memory_floor_bytes
            <= policy->system_memory_reserve_bytes
        && policy->system_cpu_reserve < profile->logical_cpu_count
        && policy->maximum_cpu_load_ratio > 0.0
        && policy->maximum_cpu_load_ratio <= 1.0
        && policy->maximum_cpu_pressure_avg10 >= 0.0
        && policy->maximum_cpu_pressure_avg10 <= 100.0
        && policy->maximum_memory_pressure_avg10 >= 0.0
        && policy->maximum_memory_pressure_avg10 <= 100.0
        && policy->maximum_io_pressure_avg10 >= 0.0
        && policy->maximum_io_pressure_avg10 <= 100.0
        && policy->io_slot_capacity > 0
        && (!profile->gpu_available || policy->gpu_slot_capacity > 0)
        && (!profile->gpu_memory_known
            || policy->gpu_memory_reserve_bytes
                < profile->gpu_memory_total_bytes);
}

static bool
valid_snapshot(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourceSnapshot *snapshot
)
{
    return snapshot && snapshot->captured_at.tv_sec >= 0
        && snapshot->captured_at.tv_nsec >= 0
        && snapshot->captured_at.tv_nsec < 1000000000L
        && snapshot->cpu_load_1m >= 0.0
        && (!snapshot->cpu_pressure_known
            || (snapshot->cpu_pressure_avg10 >= 0.0
                && snapshot->cpu_pressure_avg10 <= 100.0))
        && (!snapshot->memory_pressure_known
            || (snapshot->memory_pressure_avg10 >= 0.0
                && snapshot->memory_pressure_avg10 <= 100.0))
        && (!snapshot->io_pressure_known
            || (snapshot->io_pressure_avg10 >= 0.0
                && snapshot->io_pressure_avg10 <= 100.0))
        && (!snapshot->gpu_memory_available_known
            || !profile->gpu_memory_known
            || profile->gpu_uses_shared_memory
            || snapshot->gpu_memory_available_bytes
                <= profile->gpu_memory_total_bytes);
}

static unsigned int
increment_to_limit(unsigned int value, unsigned int limit)
{
    return value < limit ? value + 1 : limit;
}

static bool
snapshot_is_fresh_locked(
    const Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    bool *fresh
)
{
    struct timespec now;
    if (governor->internal_now_known) {
        now = governor->internal_now;
    } else if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    if (snapshot->captured_at.tv_sec > now.tv_sec
        || (snapshot->captured_at.tv_sec == now.tv_sec
            && snapshot->captured_at.tv_nsec > now.tv_nsec)) {
        *fresh = false;
        return true;
    }
    if (snapshot->captured_at.tv_sec == now.tv_sec
        && snapshot->captured_at.tv_nsec == now.tv_nsec) {
        *fresh = true;
        return true;
    }
    time_t seconds = now.tv_sec - snapshot->captured_at.tv_sec;
    const uint64_t maximum_seconds =
        LARDON3D_RESOURCE_SNAPSHOT_MAX_AGE_MILLISECONDS / 1000U;
    const long maximum_nanoseconds = (long)(
        LARDON3D_RESOURCE_SNAPSHOT_MAX_AGE_MILLISECONDS % 1000U
    ) * 1000000L;
    if ((uint64_t)seconds < maximum_seconds) {
        *fresh = true;
    } else if ((uint64_t)seconds > maximum_seconds) {
        *fresh = false;
    } else {
        long elapsed_nanoseconds = now.tv_nsec - snapshot->captured_at.tv_nsec;
        *fresh = elapsed_nanoseconds <= maximum_nanoseconds;
    }
    return true;
}

static bool
valid_estimate(const Lardon3DResourceEstimate *estimate)
{
    return estimate && estimate->minimum_batch_size > 0
        && estimate->maximum_batch_size >= estimate->minimum_batch_size
        && estimate->desired_cpu_threads > 0
        && estimate->task_class >= LARDON3D_RESOURCE_TASK_GENERAL
        && estimate->task_class <= LARDON3D_RESOURCE_TASK_MIXED
        && ((estimate->gpu_memory_fixed_bytes == 0
                && estimate->gpu_memory_bytes_per_item == 0)
            || estimate->desired_gpu_slots > 0);
}

static uint64_t
subtract_floor(uint64_t value, uint64_t amount)
{
    return value > amount ? value - amount : 0;
}

static unsigned int
subtract_unsigned(unsigned int value, unsigned int amount)
{
    return value > amount ? value - amount : 0;
}

static uint64_t
minimum_uint64(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static size_t
minimum_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

static bool
resource_size(
    uint64_t fixed,
    uint64_t per_item,
    size_t batch,
    uint64_t *total
)
{
    if (per_item > 0 && batch > UINT64_MAX / per_item) {
        return false;
    }
    uint64_t variable = per_item * (uint64_t)batch;
    if (fixed > UINT64_MAX - variable) {
        return false;
    }
    *total = fixed + variable;
    return true;
}

static bool
add_uint64(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (!sum || left > UINT64_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static void
aggregate_add(uint64_t *value, uint64_t amount, bool *saturated)
{
    if (*value > UINT64_MAX - amount) {
        *value = UINT64_MAX;
        *saturated = true;
    } else {
        *value += amount;
    }
}

static void
aggregate_admission(
    Lardon3DResourceGovernor *governor,
    Lardon3DCapabilityFeedback *feedback,
    const Lardon3DResourceCapabilitySelection *selection
)
{
    Lardon3DResourceSequenceAggregate *aggregate = &feedback->aggregate;
    aggregate_add(&aggregate->admission_count, 1, &aggregate->saturated);
    aggregate_add(
        &aggregate->selected_backend_admissions[selection->capability.backend],
        1, &aggregate->saturated);
    /* CONTRACT: a backend switch is a contract change too. Feedback remains
     * keyed by backend for adaptation, so compare this admission with the most
     * recently updated selection for the whole kind/version while the Governor
     * lock is held. The private update-order tie-break is bounded and already
     * renormalized by diagnostic ownership; no history or persistence grows. */
    const Lardon3DCapabilityFeedback *previous = NULL;
    for (size_t index = 0; index < LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
         ++index) {
        const Lardon3DCapabilityFeedback *candidate =
            &governor->capability_feedback[index];
        if (candidate->used && candidate->aggregate_selection_known
            && candidate->task_kind_version == feedback->task_kind_version
            && strcmp(candidate->task_kind, feedback->task_kind) == 0
            && (!previous || candidate->diagnostic_update_order
                > previous->diagnostic_update_order)) {
            previous = candidate;
        }
    }
    bool changed = previous
        && (previous->backend != selection->capability.backend
            || previous->aggregate_cpu_threads
                != selection->decision.cpu_threads
            || previous->aggregate_gpu_slots != selection->decision.gpu_slots
            || previous->aggregate_io_slots != selection->decision.io_slots
            || previous->aggregate_batch_size != selection->decision.batch_size
            || previous->aggregate_inflight_limit
                != selection->inflight_limit
            || previous->aggregate_helper_limit
                != selection->capability.helper_limit);
    if (changed) {
        aggregate_add(&aggregate->contract_change_count, 1,
            &aggregate->saturated);
    }
    feedback->aggregate_selection_known = true;
    feedback->aggregate_cpu_threads = selection->decision.cpu_threads;
    feedback->aggregate_gpu_slots = selection->decision.gpu_slots;
    feedback->aggregate_io_slots = selection->decision.io_slots;
    feedback->aggregate_batch_size = selection->decision.batch_size;
    feedback->aggregate_inflight_limit = selection->inflight_limit;
    feedback->aggregate_helper_limit = selection->capability.helper_limit;
}

static void
aggregate_sequence(
    Lardon3DCapabilityFeedback *feedback,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason,
    const Lardon3DResourceExecutionMetrics *metrics
)
{
    Lardon3DResourceSequenceAggregate *aggregate = &feedback->aggregate;
    uint64_t items = (uint64_t)items_completed;
    if ((size_t)items != items_completed) {
        items = UINT64_MAX;
        aggregate->saturated = true;
    }
    aggregate_add(&aggregate->sequence_count, 1, &aggregate->saturated);
    aggregate_add(&aggregate->durable_items, items, &aggregate->saturated);
    aggregate_add(&aggregate->total_wall_time_ns, wall_time_ns,
        &aggregate->saturated);
    aggregate_add(&aggregate->actual_backend_sequences[actual_backend], 1,
        &aggregate->saturated);
    if (actual_backend != feedback->backend) {
        aggregate_add(&aggregate->backend_fallback_sequences, 1,
            &aggregate->saturated);
        if (strcmp(backend_reason,
                "vulkan-and-ineligible-pair-cpu-fallback") == 0
            || strcmp(backend_reason,
                "vulkan-ineligible-whole-pair-cpu-fallback") == 0) {
            aggregate_add(
                &aggregate->backend_ineligible_fallback_sequences, 1,
                &aggregate->saturated);
        } else if (strcmp(backend_reason,
                       "vulkan-and-whole-pair-cpu-fallback") == 0
                   || strcmp(backend_reason,
                       "vulkan-failed-whole-pair-cpu-fallback") == 0) {
            aggregate_add(&aggregate->backend_failure_fallback_sequences, 1,
                &aggregate->saturated);
        } else {
            /* Unknown fallback reasons fail closed in benchmark evidence.
             * Keeping a separate bounded bucket prevents a new operational
             * reason from being silently treated as local ineligibility. */
            aggregate_add(&aggregate->backend_other_fallback_sequences, 1,
                &aggregate->saturated);
        }
    }
    const Lardon3DResourceHostTelemetry *host = &feedback->diagnostic.host;
    if (host->memory_available_known
        && (!aggregate->memory_available_known
            || host->memory_available_bytes
                < aggregate->minimum_memory_available_bytes)) {
        aggregate->memory_available_known = true;
        aggregate->minimum_memory_available_bytes =
            host->memory_available_bytes;
    }
    if (host->gpu_busy_known
        && (!aggregate->gpu_busy_known
            || host->gpu_busy_basis_points
                > aggregate->maximum_gpu_busy_basis_points)) {
        aggregate->gpu_busy_known = true;
        aggregate->maximum_gpu_busy_basis_points =
            host->gpu_busy_basis_points;
    }
    if (host->process_rss_known
        && (!aggregate->process_rss_known
            || host->process_rss_bytes > aggregate->maximum_process_rss_bytes)) {
        aggregate->process_rss_known = true;
        aggregate->maximum_process_rss_bytes = host->process_rss_bytes;
    }
    if (host->process_peak_rss_known
        && (!aggregate->process_peak_rss_known
            || host->process_peak_rss_bytes
                > aggregate->maximum_process_peak_rss_bytes)) {
        aggregate->process_peak_rss_known = true;
        aggregate->maximum_process_peak_rss_bytes =
            host->process_peak_rss_bytes;
    }
    if (!metrics) return;
#define AGGREGATE_METRIC(field) \
    aggregate_add(&aggregate->field, metrics->field, &aggregate->saturated)
    AGGREGATE_METRIC(vulkan_submits);
    AGGREGATE_METRIC(vulkan_completions);
    AGGREGATE_METRIC(vulkan_submit_cpu_ns);
    AGGREGATE_METRIC(vulkan_fence_wait_ns);
    AGGREGATE_METRIC(vulkan_readback_ns);
    if (metrics->vulkan_gpu_time_known) {
        aggregate_add(&aggregate->vulkan_gpu_known_sequences, 1,
            &aggregate->saturated);
        AGGREGATE_METRIC(vulkan_gpu_ns);
    }
    AGGREGATE_METRIC(vulkan_starvation_ns);
    AGGREGATE_METRIC(matcher_cpu_ns);
    AGGREGATE_METRIC(publication_ns);
#undef AGGREGATE_METRIC
}

static Lardon3DCapabilityFeedback *
capability_feedback_locked(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DResourceBackend backend,
    bool create
)
{
    if (!task_kind || !task_kind[0]) {
        return NULL;
    }
    for (size_t index = 0; index < LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
         ++index) {
        Lardon3DCapabilityFeedback *feedback =
            &governor->capability_feedback[index];
        if (feedback->used && feedback->task_kind_version == task_kind_version
            && feedback->backend == backend
            && strcmp(feedback->task_kind, task_kind) == 0) {
            return feedback;
        }
    }
    if (!create) {
        return NULL;
    }
    size_t selected = LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
    for (size_t index = 0; index < LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
         ++index) {
        if (!governor->capability_feedback[index].used) {
            selected = index;
            break;
        }
    }
    if (selected == LARDON3D_CAPABILITY_FEEDBACK_CAPACITY) {
        /* The history is deliberately bounded. Replacement is deterministic
         * and does not affect durable Task state or scientific output. */
        selected = governor->capability_feedback_replace;
        governor->capability_feedback_replace =
            (selected + 1) % LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
    }
    Lardon3DCapabilityFeedback *feedback =
        &governor->capability_feedback[selected];
    *feedback = (Lardon3DCapabilityFeedback) {
        .used = true,
        .task_kind_version = task_kind_version,
        .backend = backend,
    };
    (void)snprintf(feedback->task_kind, sizeof(feedback->task_kind), "%s",
        task_kind);
    return feedback;
}

static void
clear_baseline_rate_observations(Lardon3DCapabilityFeedback *feedback)
{
    feedback->baseline_observations = 0;
    memset(feedback->baseline_rates_milli, 0,
        sizeof(feedback->baseline_rates_milli));
}

static void
clear_trial_rate_observations(Lardon3DCapabilityFeedback *feedback)
{
    feedback->trial_observations = 0;
    memset(feedback->trial_rates_milli, 0,
        sizeof(feedback->trial_rates_milli));
}

static unsigned int
required_throughput_observations(
    const Lardon3DTaskCapability *capability,
    Lardon3DCapabilityTrialDimension dimension
)
{
    return capability->sustained_gpu_batch_feedback
            && (dimension == LARDON3D_CAPABILITY_TRIAL_NONE
                || dimension == LARDON3D_CAPABILITY_TRIAL_BATCH)
        ? LARDON3D_GPU_BATCH_THROUGHPUT_OBSERVATIONS
        : LARDON3D_CAPABILITY_THROUGHPUT_OBSERVATIONS;
}

static uint64_t
average_rate_observations(const uint64_t *rates, unsigned int count)
{
    if (!rates || count == 0
        || count > LARDON3D_GPU_BATCH_THROUGHPUT_OBSERVATIONS) {
        return 0;
    }
    uint64_t quotient_sum = 0;
    unsigned int remainder_sum = 0;
    for (unsigned int index = 0; index < count; ++index) {
        /* Sum floor(rate/count), then the bounded remainders. With at most
         * `count` UINT64 inputs the quotient sum cannot exceed UINT64_MAX;
         * this computes floor(sum/count) without ever forming `sum`. */
        quotient_sum += rates[index] / count;
        remainder_sum += (unsigned int)(rates[index] % count);
    }
    return quotient_sum + remainder_sum / count;
}

static const char *
trial_observation_reason(
    const Lardon3DTaskCapability *capability,
    Lardon3DCapabilityTrialDimension dimension
)
{
    if (dimension == LARDON3D_CAPABILITY_TRIAL_CPU) {
        return "cpu-throughput-trial";
    }
    if (dimension == LARDON3D_CAPABILITY_TRIAL_INFLIGHT) {
        return "inflight-throughput-trial";
    }
    return capability->sustained_gpu_batch_feedback
        ? "gpu-batch-throughput-trial" : "throughput-trial";
}

static const char *
trial_result_reason(
    const Lardon3DTaskCapability *capability,
    Lardon3DCapabilityTrialDimension dimension,
    bool improved
)
{
    if (dimension == LARDON3D_CAPABILITY_TRIAL_CPU) {
        return improved
            ? "cpu-throughput-improved" : "cpu-throughput-no-gain";
    }
    if (dimension == LARDON3D_CAPABILITY_TRIAL_INFLIGHT) {
        return improved
            ? "inflight-throughput-improved"
            : "inflight-throughput-no-gain";
    }
    if (capability->sustained_gpu_batch_feedback) {
        return improved
            ? "gpu-batch-throughput-improved"
            : "gpu-batch-throughput-no-gain";
    }
    return improved ? "throughput-improved" : "throughput-no-gain";
}

static void
reset_capability_throughput_feedback(
    Lardon3DCapabilityFeedback *feedback,
    const Lardon3DTaskCapability *capability
)
{
    /* Pressure abandons any trial immediately. Recovery starts from the full
     * capability-owned baseline window (eight for ORB Vulkan batch, two for
     * generic adaptation); no stale fast sample can make the next immutable
     * sequence jump back to a memory-heavy limit. */
    unsigned int minimum_cpu = capability->cpu_reducible
        ? 1 : capability->estimate.desired_cpu_threads;
    feedback->adaptive_cpu_limit = minimum_cpu;
    feedback->accepted_cpu_limit = minimum_cpu;
    feedback->adaptive_batch_limit = capability->batch_adaptive
        ? capability->estimate.minimum_batch_size
        : capability->estimate.maximum_batch_size;
    feedback->accepted_batch_limit = feedback->adaptive_batch_limit;
    size_t minimum_inflight = capability->minimum_inflight_limit != 0
        ? capability->minimum_inflight_limit : capability->inflight_limit;
    feedback->adaptive_inflight_limit = minimum_inflight;
    feedback->accepted_inflight_limit = minimum_inflight;
    feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_NONE;
    clear_baseline_rate_observations(feedback);
    clear_trial_rate_observations(feedback);
    feedback->baseline_rate_milli = 0;
    feedback->cpu_growth_stopped = false;
    feedback->inflight_growth_stopped = false;
    feedback->batch_growth_stopped = false;
}

static bool
throughput_materially_improved(uint64_t trial, uint64_t baseline)
{
    if (baseline == 0) return trial > 0;
    /* Five percent is a deadband, not a scientific threshold. Expressing it
     * as baseline + ceil(baseline/20) avoids overflow-prone multiplication. */
    uint64_t increase = baseline / 20 + (baseline % 20 != 0);
    return baseline <= UINT64_MAX - increase
        && trial >= baseline + increase;
}

static size_t
next_trial_batch(size_t current, size_t maximum)
{
    if (current >= maximum) return maximum;
    return current > maximum / 2 ? maximum : current * 2;
}

static unsigned int
next_trial_cpu(unsigned int current, unsigned int maximum)
{
    if (current >= maximum) return maximum;
    /* CPU trials are portable capability rungs, not a fingerprint of any
     * validation host. Double without overflow, then use a
     * non-power-of-two capability/compute maximum as the exact final rung. */
    unsigned int next = current > UINT_MAX / 2 ? maximum : current * 2;
    return next < maximum ? next : maximum;
}

static void
open_next_capability_trial_locked(
    const Lardon3DResourceGovernor *governor,
    Lardon3DCapabilityFeedback *feedback,
    const Lardon3DTaskCapability *capability
)
{
    unsigned int cpu_max = capability->estimate.desired_cpu_threads
            < governor->cpu_policy.compute_cpu_count
        ? capability->estimate.desired_cpu_threads
        : governor->cpu_policy.compute_cpu_count;
    if (capability->cpu_reducible && !feedback->cpu_growth_stopped
        && feedback->accepted_cpu_limit < cpu_max) {
        feedback->adaptive_cpu_limit = next_trial_cpu(
            feedback->accepted_cpu_limit, cpu_max);
        feedback->adaptive_batch_limit = capability->cpu_batch_coupled
            ? minimum_size(feedback->adaptive_cpu_limit,
                capability->estimate.maximum_batch_size)
            : feedback->accepted_batch_limit;
        feedback->adaptive_inflight_limit =
            feedback->accepted_inflight_limit;
        feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_CPU;
        clear_trial_rate_observations(feedback);
        return;
    }
    if (capability->inflight_adaptive
        && !feedback->inflight_growth_stopped
        && feedback->accepted_inflight_limit < capability->inflight_limit) {
        /* Depth two cannot be exercised by a one-item sequence. Establish an
         * accepted batch of at least two first, changing only that dimension;
         * if batch two has already shown no gain, do not run a fake inflight
         * trial whose second slot can never be used. */
        if (capability->batch_adaptive
            && feedback->accepted_batch_limit < 2) {
            if (!feedback->batch_growth_stopped
                && capability->estimate.maximum_batch_size >= 2) {
                feedback->adaptive_cpu_limit = feedback->accepted_cpu_limit;
                feedback->adaptive_inflight_limit =
                    feedback->accepted_inflight_limit;
                feedback->adaptive_batch_limit = 2;
                feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_BATCH;
                clear_trial_rate_observations(feedback);
            }
            return;
        }
        feedback->adaptive_cpu_limit = feedback->accepted_cpu_limit;
        feedback->adaptive_inflight_limit =
            feedback->accepted_inflight_limit;
        feedback->adaptive_batch_limit = feedback->accepted_batch_limit;
        feedback->adaptive_inflight_limit = next_trial_batch(
            feedback->accepted_inflight_limit, capability->inflight_limit);
        feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_INFLIGHT;
        clear_trial_rate_observations(feedback);
        return;
    }
    if (capability->batch_adaptive && !feedback->batch_growth_stopped
        && feedback->accepted_batch_limit
            < capability->estimate.maximum_batch_size) {
        feedback->adaptive_cpu_limit = feedback->accepted_cpu_limit;
        feedback->adaptive_batch_limit = next_trial_batch(
            feedback->accepted_batch_limit,
            capability->estimate.maximum_batch_size);
        feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_BATCH;
        clear_trial_rate_observations(feedback);
        return;
    }
    feedback->adaptive_cpu_limit = feedback->accepted_cpu_limit;
    feedback->adaptive_batch_limit = feedback->accepted_batch_limit;
    feedback->adaptive_inflight_limit = feedback->accepted_inflight_limit;
    feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_NONE;
}

typedef struct {
    uint64_t admission_floor;
    uint64_t caution_floor;
    uint64_t hard_floor;
    bool caution_is_non_escalating_band;
} Lardon3DMemoryFloors;

static uint64_t
default_host_memory_reserve(uint64_t total)
{
    const uint64_t normal = UINT64_C(3) * 1024 * 1024 * 1024;
    if (total > normal) {
        return normal;
    }
    /* A sub-3-GiB host cannot retain the normal absolute floor and still admit
     * any work. Keep a deterministic quarter for the host; clipping avoids a
     * zero/invalid reserve on synthetic tiny profiles without underflow. */
    uint64_t degraded = total / 4;
    if (degraded == 0 && total > 1) {
        degraded = 1;
    }
    return degraded < total ? degraded : total - 1;
}

static uint64_t
default_host_memory_caution(uint64_t total, uint64_t admission_floor)
{
    const uint64_t normal = UINT64_C(3) * 1024 * 1024 * 1024;
    const uint64_t caution = UINT64_C(4) * 1024 * 1024 * 1024;
    if (admission_floor == normal) {
        return total < caution ? total : caution;
    }
    uint64_t degraded = total / 3;
    return degraded > admission_floor ? degraded : admission_floor;
}

static void
internal_memory_floors_locked(
    const Lardon3DResourceGovernor *governor,
    Lardon3DMemoryFloors *floors
)
{
    uint64_t default_floor = default_host_memory_reserve(
        governor->profile.memory_total_bytes);
    *floors = (Lardon3DMemoryFloors) {
        .admission_floor = governor->policy.system_memory_reserve_bytes,
        .caution_floor = governor->policy.system_memory_reserve_bytes,
        .hard_floor = governor->policy.emergency_memory_floor_bytes,
    };
    if (governor->policy.system_memory_reserve_bytes == default_floor
        && governor->policy.emergency_memory_floor_bytes == default_floor) {
        /* Default capacity always subtracts the hard reserve, never the 4 GiB
         * caution level. The caution band only abandons aggressive trials and
         * reduces current work while MemAvailable remains above the reserve. */
        floors->caution_floor = default_host_memory_caution(
            governor->profile.memory_total_bytes, default_floor);
        floors->hard_floor = default_floor;
        floors->caution_is_non_escalating_band = true;
    }
}

static size_t
batch_capacity(uint64_t available, uint64_t fixed, uint64_t per_item)
{
    if (available < fixed) {
        return 0;
    }
    if (per_item == 0) {
        return SIZE_MAX;
    }
    uint64_t capacity = (available - fixed) / per_item;
    return capacity > SIZE_MAX ? SIZE_MAX : (size_t)capacity;
}

static bool
record_batch_locked(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t batch_size,
    uint64_t duration_ns,
    size_t peak_memory_bytes
)
{
    if (batch_size == 0) {
        return false;
    }
    size_t class_index = (size_t)task_class;
    if (class_index > (size_t)LARDON3D_RESOURCE_TASK_MIXED) {
        return false;
    }
    Lardon3DBatchMetrics *metrics = governor->batch_metrics[class_index];
    size_t count = governor->batch_metrics_count[class_index];
    size_t head = governor->batch_metrics_head[class_index];
    metrics[head] = (Lardon3DBatchMetrics) {
        .batch_size = batch_size,
        .duration_ns = duration_ns,
        /* Zéro est le marqueur persistant « mesure inconnue ». La boucle
         * d'adaptation ignore explicitement ces échantillons. */
        .peak_memory_bytes = peak_memory_bytes,
    };
    head = (head + 1) % LARDON3D_BATCH_METRICS_CAPACITY;
    governor->batch_metrics_head[class_index] = head;
    if (count < LARDON3D_BATCH_METRICS_CAPACITY) {
        governor->batch_metrics_count[class_index] = count + 1;
    }
    return true;
}

static size_t
adaptive_batch_limit(
    const Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t static_batch,
    uint64_t memory_bytes_per_item
)
{
    if (memory_bytes_per_item == 0 || static_batch == 0) {
        return static_batch;
    }
    size_t class_index = (size_t)task_class;
    if (class_index > (size_t)LARDON3D_RESOURCE_TASK_MIXED) {
        return static_batch;
    }
    size_t count = governor->batch_metrics_count[class_index];
    if (count == 0) {
        return static_batch;
    }
    size_t head = governor->batch_metrics_head[class_index];
    uint64_t measured_per_item = 0;
    size_t samples = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (head + LARDON3D_BATCH_METRICS_CAPACITY - count + i)
            % LARDON3D_BATCH_METRICS_CAPACITY;
        const Lardon3DBatchMetrics *m = &governor->batch_metrics[class_index][idx];
        if (m->batch_size > 0 && m->peak_memory_bytes > 0) {
            /* Coût par élément le plus défavorable observé : une moyenne
             * sous-estimerait le pic et laisserait un lot dépasser son
             * budget. La stabilité de l'hôte prime sur le débit. */
            uint64_t per_item = m->peak_memory_bytes / m->batch_size
                + (m->peak_memory_bytes % m->batch_size != 0);
            if (per_item > measured_per_item) {
                measured_per_item = per_item;
            }
            ++samples;
        }
    }
    if (samples == 0) {
        return static_batch;
    }
    if (measured_per_item <= memory_bytes_per_item) {
        return static_batch;
    }
    /* Éviter l'overflow de la multiplication : si static_batch est trop
     * grand pour être multiplié sans débordement, on retourne 1 (le lot le
     * plus conservateur possible) plutôt que de saturer à SIZE_MAX. */
    if (static_batch > UINT64_MAX / memory_bytes_per_item) {
        return 1;
    }
    uint64_t corrected = (uint64_t)static_batch * memory_bytes_per_item
        / measured_per_item;
    size_t result = corrected > SIZE_MAX ? SIZE_MAX : (size_t)corrected;
    return result > 0 ? result : 1;
}

static void
set_decision(
    Lardon3DResourceDecision *decision,
    Lardon3DResourceDecisionKind kind,
    size_t batch_size,
    unsigned int cpu_threads,
    unsigned int gpu_slots,
    unsigned int io_slots,
    const char *reason
)
{
    *decision = (Lardon3DResourceDecision) {
        .kind = kind,
        .batch_size = batch_size,
        .cpu_threads = cpu_threads,
        .gpu_slots = gpu_slots,
        .io_slots = io_slots,
    };
    (void)snprintf(decision->reason, sizeof(decision->reason), "%s", reason);
}

bool
lardon3d_resource_policy_default(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourcePolicy *policy
)
{
    if (!valid_profile(profile) || !policy) {
        return false;
    }
    /* Four logical CPUs are the normal host reserve. Small systems degrade
     * deterministically by retaining one compute CPU; complete topology may
     * later adjust this logical target to whole physical-core groups. */
    unsigned int cpu_reserve = profile->logical_cpu_count > 4
        ? 4 : profile->logical_cpu_count - 1;
    uint64_t memory_reserve = default_host_memory_reserve(
        profile->memory_total_bytes);
    *policy = (Lardon3DResourcePolicy) {
        .system_memory_reserve_bytes = memory_reserve,
        .emergency_memory_floor_bytes = memory_reserve,
        .gpu_memory_reserve_bytes = profile->gpu_memory_known
            ? profile->gpu_memory_total_bytes / 8
            : 0,
        .system_cpu_reserve = cpu_reserve,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_cpu_pressure_avg10 = 20.0,
        .maximum_memory_pressure_avg10 = 1.0,
        .maximum_io_pressure_avg10 = 80.0,
        .gpu_slot_capacity = profile->gpu_available ? 1 : 0,
        .io_slot_capacity = 1,
    };
    return valid_policy(profile, policy);
}

Lardon3DResourceGovernor *
lardon3d_resource_governor_create(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy
)
{
    if (!valid_policy(profile, policy)) {
        return NULL;
    }
    Lardon3DResourceGovernor *governor = calloc(1, sizeof(*governor));
    if (!governor) {
        return NULL;
    }
    if (pthread_mutex_init(&governor->mutex, NULL) != 0) {
        free(governor);
        return NULL;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        (void)pthread_mutex_destroy(&governor->mutex);
        free(governor);
        return NULL;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&governor->mutex);
        free(governor);
        return NULL;
    }
    if (pthread_cond_init(&governor->cond, &attr) != 0) {
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&governor->mutex);
        free(governor);
        return NULL;
    }
    (void)pthread_condattr_destroy(&attr);
    governor->generation = 0;
    governor->profile = *profile;
    governor->policy = *policy;
    capture_live_cpu_topology(profile, &governor->cpu_topology);
    build_cpu_policy(profile, policy, &governor->cpu_topology,
        &governor->cpu_policy);
    governor->next_reservation_id = 1;
    governor->slow_start_limit = SIZE_MAX;
    governor->pressure = LARDON3D_RESOURCE_PRESSURE_GREEN;
    return governor;
}

void
lardon3d_resource_governor_destroy(Lardon3DResourceGovernor *governor)
{
    if (!governor) {
        return;
    }
    Lardon3DResourceReservation *reservation = governor->active;
    while (reservation) {
        Lardon3DResourceReservation *next = reservation->next;
        free(reservation);
        reservation = next;
    }
    reservation = governor->released;
    while (reservation) {
        Lardon3DResourceReservation *next = reservation->next;
        free(reservation);
        reservation = next;
    }
    (void)pthread_cond_destroy(&governor->cond);
    (void)pthread_mutex_destroy(&governor->mutex);
    free(governor);
}

bool
lardon3d_resource_governor_set_policy(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourcePolicy *policy
)
{
    if (!governor || !policy) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool policy_valid = valid_policy(&governor->profile, policy);
    uint64_t memory_budget = policy_valid
        ? governor->profile.memory_total_bytes
            - policy->system_memory_reserve_bytes
        : 0;
    uint64_t gpu_budget = policy_valid && governor->profile.gpu_memory_known
        && !governor->profile.gpu_uses_shared_memory
        ? governor->profile.gpu_memory_total_bytes
            - policy->gpu_memory_reserve_bytes
        : UINT64_MAX;
    Lardon3DResourceCpuPolicyDiagnostic cpu_policy;
    if (policy_valid) {
        build_cpu_policy(&governor->profile, policy, &governor->cpu_topology,
            &cpu_policy);
    } else {
        memset(&cpu_policy, 0, sizeof(cpu_policy));
    }
    bool accepted = policy_valid
        && memory_budget >= governor->memory_reserved_bytes
        && gpu_budget >= governor->gpu_memory_reserved_bytes
        && cpu_policy.compute_cpu_count >= governor->cpu_reserved
        && policy->gpu_slot_capacity >= governor->gpu_slots_reserved
        && policy->io_slot_capacity >= governor->io_slots_reserved;
    if (accepted) {
        governor->policy = *policy;
        governor->cpu_policy = cpu_policy;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return accepted;
}

bool
lardon3d_resource_governor_get_policy(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourcePolicy *policy
)
{
    if (!governor || !policy) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    *policy = governor->policy;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

static bool
bounded_text_terminated(const char *text, size_t capacity)
{
    return text && memchr(text, '\0', capacity) != NULL;
}

static bool
bounded_exact_identity(const char *text, size_t capacity)
{
    return bounded_text_terminated(text, capacity) && text[0] != '\0'
        && strcmp(text, "UNKNOWN") != 0;
}

static bool
ssd_snapshot_has_complete_pair(const Lardon3DSsdSnapshot *snapshot)
{
    return snapshot->device_detected && snapshot->swap_detected
        && snapshot->scratch_detected && snapshot->pairing_valid
        && bounded_exact_identity(snapshot->drive_identity,
            sizeof(snapshot->drive_identity))
        && bounded_exact_identity(snapshot->swap_uuid,
            sizeof(snapshot->swap_uuid))
        && bounded_exact_identity(snapshot->scratch_uuid,
            sizeof(snapshot->scratch_uuid))
        && snapshot->swap_partition_size_known
        && snapshot->swap_partition_size_bytes > 0
        && snapshot->scratch_partition_size_known
        && snapshot->scratch_partition_size_bytes > 0;
}

static bool
valid_ssd_controller_snapshot(const Lardon3DSsdSnapshot *snapshot)
{
    unsigned int action_count = snapshot
        ? (unsigned int)snapshot->can_enable
            + (unsigned int)snapshot->can_disable
            + (unsigned int)snapshot->can_cancel_drain
        : 0;
    if (!snapshot || snapshot->state < LARDON3D_SSD_ABSENT
        || snapshot->state > LARDON3D_SSD_ERROR
        || !bounded_text_terminated(snapshot->model, sizeof(snapshot->model))
        || !bounded_text_terminated(snapshot->serial, sizeof(snapshot->serial))
        || !bounded_text_terminated(snapshot->drive_identity,
            sizeof(snapshot->drive_identity))
        || !bounded_text_terminated(snapshot->swap_uuid,
            sizeof(snapshot->swap_uuid))
        || !bounded_text_terminated(snapshot->scratch_uuid,
            sizeof(snapshot->scratch_uuid))
        || !bounded_text_terminated(snapshot->swap_device,
            sizeof(snapshot->swap_device))
        || !bounded_text_terminated(snapshot->scratch_device,
            sizeof(snapshot->scratch_device))
        || !bounded_text_terminated(snapshot->scratch_mount_path,
            sizeof(snapshot->scratch_mount_path))
        || !bounded_text_terminated(snapshot->reason,
            sizeof(snapshot->reason))
        || (!snapshot->connection_speed_known
            && snapshot->connection_speed_mbps != 0)
        || snapshot->scratch_lease_capacity
            != LARDON3D_SSD_MAX_SCRATCH_LEASES
        || snapshot->scratch_lease_count > snapshot->scratch_lease_capacity
        || (!snapshot->swap_partition_size_known
            && snapshot->swap_partition_size_bytes != 0)
        || (snapshot->swap_partition_size_known
            && (snapshot->swap_partition_size_bytes == 0
                || !snapshot->swap_detected))
        || (!snapshot->scratch_partition_size_known
            && snapshot->scratch_partition_size_bytes != 0)
        || (snapshot->scratch_partition_size_known
            && (snapshot->scratch_partition_size_bytes == 0
                || !snapshot->scratch_detected))
        || (!snapshot->swap_total_known
            && snapshot->swap_total_bytes != 0)
        || (!snapshot->swap_used_known
            && snapshot->swap_used_bytes != 0)
        || (!snapshot->scratch_total_known
            && snapshot->scratch_total_bytes != 0)
        || (!snapshot->scratch_free_known
            && snapshot->scratch_free_bytes != 0)
        || (snapshot->swap_used_known
            && (!snapshot->swap_total_known
                || snapshot->swap_used_bytes > snapshot->swap_total_bytes))
        || (snapshot->scratch_free_known
            && (!snapshot->scratch_total_known
                || snapshot->scratch_free_bytes
                    > snapshot->scratch_total_bytes))
        || snapshot->device_detected
            != (snapshot->swap_detected || snapshot->scratch_detected)
        || (snapshot->swap_active && !snapshot->swap_detected)
        || (snapshot->scratch_mounted && !snapshot->scratch_detected)
        || (snapshot->swap_total_known && !snapshot->swap_detected)
        || (snapshot->scratch_total_known && !snapshot->scratch_detected)
        || (snapshot->scratch_mounted
            && !bounded_exact_identity(snapshot->scratch_mount_path,
                sizeof(snapshot->scratch_mount_path)))
        || (!snapshot->scratch_mounted
            && bounded_exact_identity(snapshot->scratch_mount_path,
                sizeof(snapshot->scratch_mount_path)))
        || action_count > 1) {
        return false;
    }

    const bool complete_pair = ssd_snapshot_has_complete_pair(snapshot);
    const bool authority_bearing = snapshot->scratch_allocations_allowed
        || action_count != 0;
    const bool complete_pair_state = snapshot->state == LARDON3D_SSD_ENABLING
        || snapshot->state == LARDON3D_SSD_ENABLED
        || snapshot->state == LARDON3D_SSD_IN_USE
        || snapshot->state == LARDON3D_SSD_DRAINING
        || snapshot->state == LARDON3D_SSD_SAFE_TO_UNPLUG;
    if ((snapshot->pairing_valid || authority_bearing || complete_pair_state)
        && !complete_pair) {
        /* CONTRACT: neither a friendly state nor a capability bit can stand
         * in for the controller's complete Drive/UUID/detection/extent proof.
         * Incomplete DETECTED and disconnected sticky ERROR remain observable
         * only because they carry no authority. */
        return false;
    }

    if (snapshot->scratch_allocations_allowed
        && (!complete_pair || snapshot->drain_requested
            || !snapshot->swap_active || !snapshot->scratch_mounted
            || strcmp(snapshot->scratch_mount_path,
                   LARDON3D_SSD_SCRATCH_MOUNT_PATH) != 0
            || (snapshot->state != LARDON3D_SSD_ENABLED
                && snapshot->state != LARDON3D_SSD_IN_USE)
            || snapshot->scratch_lease_count
                >= snapshot->scratch_lease_capacity)) {
        return false;
    }
    if (snapshot->can_enable
        && (!complete_pair
            || (snapshot->state != LARDON3D_SSD_DETECTED
                && snapshot->state != LARDON3D_SSD_SAFE_TO_UNPLUG)
            || snapshot->swap_active || snapshot->scratch_mounted
            || snapshot->drain_requested
            || snapshot->scratch_lease_count != 0)) {
        return false;
    }
    if (snapshot->can_disable
        && (!complete_pair
            || (snapshot->state != LARDON3D_SSD_DETECTED
                && snapshot->state != LARDON3D_SSD_ENABLED
                && snapshot->state != LARDON3D_SSD_IN_USE
                && snapshot->state != LARDON3D_SSD_ERROR)
            || (snapshot->state == LARDON3D_SSD_DETECTED
                && !snapshot->swap_active && !snapshot->scratch_mounted))) {
        return false;
    }
    if (snapshot->can_cancel_drain
        && (!complete_pair || !snapshot->drain_requested
            || snapshot->state != LARDON3D_SSD_DRAINING)) {
        return false;
    }

    switch (snapshot->state) {
    case LARDON3D_SSD_ABSENT:
        /* ABSENT is absence, not a bucket for contradictory stale telemetry.
         * Sticky ownership is represented as ERROR with its retained tuple. */
        return !snapshot->device_detected && !snapshot->pairing_valid
            && !snapshot->model_known && !snapshot->serial_known
            && !snapshot->connection_speed_known
            && !snapshot->swap_detected && !snapshot->scratch_detected
            && !snapshot->swap_partition_size_known
            && !snapshot->scratch_partition_size_known
            && !snapshot->swap_active && !snapshot->swap_total_known
            && !snapshot->swap_used_known && !snapshot->scratch_mounted
            && !snapshot->scratch_total_known
            && !snapshot->scratch_free_known
            && snapshot->scratch_lease_count == 0
            && !snapshot->drain_requested
            && !snapshot->scratch_allocations_allowed
            && action_count == 0
            && !bounded_exact_identity(snapshot->drive_identity,
                sizeof(snapshot->drive_identity))
            && !bounded_exact_identity(snapshot->swap_uuid,
                sizeof(snapshot->swap_uuid))
            && !bounded_exact_identity(snapshot->scratch_uuid,
                sizeof(snapshot->scratch_uuid));
    case LARDON3D_SSD_DETECTED:
        if (!snapshot->device_detected
            || snapshot->scratch_lease_count != 0
            || snapshot->drain_requested
            || snapshot->scratch_allocations_allowed
            || (snapshot->swap_active && snapshot->scratch_mounted)) {
            return false;
        }
        if (!snapshot->pairing_valid) {
            return action_count == 0;
        }
        return snapshot->swap_active || snapshot->scratch_mounted
            ? snapshot->can_disable
            : snapshot->can_enable;
    case LARDON3D_SSD_ENABLING:
        return snapshot->scratch_lease_count == 0
            && !snapshot->drain_requested
            && !snapshot->scratch_allocations_allowed
            && action_count == 0;
    case LARDON3D_SSD_ENABLED:
        return snapshot->scratch_lease_count == 0
            && snapshot->swap_active && snapshot->scratch_mounted
            && !snapshot->drain_requested && snapshot->can_disable
            && !snapshot->can_enable && !snapshot->can_cancel_drain
            && strcmp(snapshot->scratch_mount_path,
                LARDON3D_SSD_SCRATCH_MOUNT_PATH) == 0;
    case LARDON3D_SSD_IN_USE:
        return snapshot->scratch_lease_count > 0
            && snapshot->swap_active && snapshot->scratch_mounted
            && !snapshot->drain_requested && snapshot->can_disable
            && !snapshot->can_enable && !snapshot->can_cancel_drain
            && strcmp(snapshot->scratch_mount_path,
                LARDON3D_SSD_SCRATCH_MOUNT_PATH) == 0;
    case LARDON3D_SSD_DRAINING:
        return snapshot->drain_requested
            && !snapshot->scratch_allocations_allowed
            && snapshot->can_cancel_drain && !snapshot->can_enable
            && !snapshot->can_disable;
    case LARDON3D_SSD_SAFE_TO_UNPLUG:
        return snapshot->scratch_lease_count == 0
            && !snapshot->swap_active && !snapshot->scratch_mounted
            && !snapshot->drain_requested
            && !snapshot->scratch_allocations_allowed
            && snapshot->can_enable && !snapshot->can_disable
            && !snapshot->can_cancel_drain;
    case LARDON3D_SSD_ERROR:
        /* A disconnected sticky hazard may retain the exact tuple and leases
         * without current detection. Drain authority is accepted only for the
         * controller-published, completely reconnected pair. */
        return !snapshot->scratch_allocations_allowed
            && !snapshot->can_enable && !snapshot->can_cancel_drain;
    }
    return false;
}

static bool
normalize_external_storage(
    const Lardon3DResourceExternalStorage *input,
    Lardon3DResourceExternalStorage *output
)
{
    if (output) {
        memset(output, 0, sizeof(*output));
    }
    if (!input || !output
        || input->status < LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT
        || input->status > LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR
        || !bounded_text_terminated(input->stable_identity,
            sizeof(input->stable_identity))
        || !bounded_text_terminated(input->reason, sizeof(input->reason))
        || (!input->scratch_total_known && input->scratch_total_bytes != 0)
        || (!input->scratch_free_known && input->scratch_free_bytes != 0)
        || (input->scratch_free_known
            && (!input->scratch_total_known
                || input->scratch_free_bytes > input->scratch_total_bytes))
        || (!input->swap_total_known && input->swap_total_bytes != 0)
        || (!input->swap_used_known && input->swap_used_bytes != 0)
        || (input->swap_used_known
            && (!input->swap_total_known
                || input->swap_used_bytes > input->swap_total_bytes))
        || input->active_scratch_leases
            > LARDON3D_SSD_MAX_SCRATCH_LEASES
        || (input->new_scratch_allocations_allowed
            && input->status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE
            && input->status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE)
        || (input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE
            && input->active_scratch_leases != 0)
        || (input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE
            && input->active_scratch_leases == 0)
        || ((input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT
                || input->status
                    == LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED
                || input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE)
            && input->active_scratch_leases != 0)
        || (input->active_scratch_leases > 0
            && input->status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE
            && input->status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING
            && input->status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR)
        || ((input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE
                || input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE
                || input->status
                    == LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING
                || input->status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE)
            && !bounded_exact_identity(input->stable_identity,
                sizeof(input->stable_identity)))) {
        return false;
    }

    output->generation = input->generation;
    output->status = input->status;
    output->new_scratch_allocations_allowed =
        input->new_scratch_allocations_allowed;
    output->scratch_total_known = input->scratch_total_known;
    output->scratch_free_known = input->scratch_free_known;
    output->scratch_total_bytes = input->scratch_total_known
        ? input->scratch_total_bytes : 0;
    output->scratch_free_bytes = input->scratch_free_known
        ? input->scratch_free_bytes : 0;
    output->swap_total_known = input->swap_total_known;
    output->swap_used_known = input->swap_used_known;
    output->swap_total_bytes = input->swap_total_known
        ? input->swap_total_bytes : 0;
    output->swap_used_bytes = input->swap_used_known
        ? input->swap_used_bytes : 0;
    output->active_scratch_leases = input->active_scratch_leases;
    (void)snprintf(output->stable_identity,
        sizeof(output->stable_identity), "%s", input->stable_identity);
    (void)snprintf(output->reason, sizeof(output->reason), "%s",
        input->reason);
    return true;
}

bool
lardon3d_resource_external_storage_from_ssd_snapshot(
    const Lardon3DSsdSnapshot *snapshot,
    Lardon3DResourceExternalStorage *storage
)
{
    if (storage) {
        memset(storage, 0, sizeof(*storage));
    }
    if (!storage || !valid_ssd_controller_snapshot(snapshot)) {
        return false;
    }
    Lardon3DResourceExternalStorageStatus status;
    switch (snapshot->state) {
    case LARDON3D_SSD_ABSENT:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT;
        break;
    case LARDON3D_SSD_DETECTED:
    case LARDON3D_SSD_ENABLING:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED;
        break;
    case LARDON3D_SSD_ENABLED:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE;
        break;
    case LARDON3D_SSD_IN_USE:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE;
        break;
    case LARDON3D_SSD_DRAINING:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING;
        break;
    case LARDON3D_SSD_SAFE_TO_UNPLUG:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE;
        break;
    case LARDON3D_SSD_ERROR:
        status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR;
        break;
    default:
        return false;
    }
    *storage = (Lardon3DResourceExternalStorage) {
        .generation = snapshot->generation,
        .status = status,
        .new_scratch_allocations_allowed =
            snapshot->scratch_allocations_allowed,
        .scratch_total_known = snapshot->scratch_total_known,
        .scratch_free_known = snapshot->scratch_free_known,
        .scratch_total_bytes = snapshot->scratch_total_known
            ? snapshot->scratch_total_bytes : 0,
        .scratch_free_bytes = snapshot->scratch_free_known
            ? snapshot->scratch_free_bytes : 0,
        .swap_total_known = snapshot->swap_total_known,
        .swap_used_known = snapshot->swap_used_known,
        .swap_total_bytes = snapshot->swap_total_known
            ? snapshot->swap_total_bytes : 0,
        .swap_used_bytes = snapshot->swap_used_known
            ? snapshot->swap_used_bytes : 0,
        .active_scratch_leases = snapshot->scratch_lease_count,
    };
    (void)snprintf(storage->stable_identity,
        sizeof(storage->stable_identity), "%s", snapshot->drive_identity);
    (void)snprintf(storage->reason, sizeof(storage->reason), "%s",
        snapshot->reason);
    return true;
}

static bool
external_storage_material_equal(
    const Lardon3DResourceExternalStorage *left,
    const Lardon3DResourceExternalStorage *right
)
{
    return left->status == right->status
        && left->new_scratch_allocations_allowed
            == right->new_scratch_allocations_allowed
        && left->scratch_total_known == right->scratch_total_known
        && left->scratch_free_known == right->scratch_free_known
        && left->scratch_total_bytes == right->scratch_total_bytes
        && left->scratch_free_bytes == right->scratch_free_bytes
        && left->swap_total_known == right->swap_total_known
        && left->swap_used_known == right->swap_used_known
        && left->swap_total_bytes == right->swap_total_bytes
        && left->swap_used_bytes == right->swap_used_bytes
        && left->active_scratch_leases == right->active_scratch_leases
        && strcmp(left->stable_identity, right->stable_identity) == 0
        && strcmp(left->reason, right->reason) == 0;
}

static void
external_storage_notify_locked(Lardon3DResourceGovernor *governor)
{
    if (governor->generation != UINT64_MAX) {
        ++governor->generation;
    }
    (void)pthread_cond_broadcast(&governor->cond);
}

typedef enum {
    EXTERNAL_STORAGE_APPLY_PUBLIC = 0,
    EXTERNAL_STORAGE_APPLY_WRAPPER_COMPLETION,
} ExternalStorageApplyProvenance;

static bool
external_storage_apply_locked(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceExternalStorage *storage,
    ExternalStorageApplyProvenance provenance
)
{
    Lardon3DResourceExternalStorage next;
    if (!normalize_external_storage(storage, &next)
        || (next.status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR
            && next.active_scratch_leases
                < governor->external_storage_lease_count)) {
        return false;
    }

    const Lardon3DResourceExternalStorage *current =
        &governor->external_storage;
    bool same_material = external_storage_material_equal(current, &next);
    const bool exact_saturated_wrapper = provenance
            == EXTERNAL_STORAGE_APPLY_WRAPPER_COMPLETION
        && current->generation == UINT64_MAX
        && next.generation == UINT64_MAX;
    const bool exact_wrapper_error = provenance
            == EXTERNAL_STORAGE_APPLY_WRAPPER_COMPLETION
        && next.generation == current->generation
        && next.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR;
    /* UINT64_MAX is a legal saturated controller watermark: only the exact
     * serialized wrapper that already owns operation_active may publish a
     * material equal-watermark completion. Public telemetry retains the
     * ordinary stale rule and therefore cannot regrant authority at MAX. An
     * equal-watermark wrapper ERROR may always revoke authority and reconcile
     * its exact address-backed count. */
    if ((next.generation < current->generation
            || (next.generation == current->generation
                && !exact_saturated_wrapper && !exact_wrapper_error))
        && !same_material) {
        if (next.status != LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR) {
            return false;
        }
        /* WHY: malformed/stale telemetry may revoke authority immediately,
         * but it may not splice stale device metrics or identity into a newer
         * exact observation. Preserve the current watermark and evidence. */
        Lardon3DResourceExternalStorage conservative = *current;
        conservative.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR;
        conservative.new_scratch_allocations_allowed = false;
        (void)snprintf(conservative.reason, sizeof(conservative.reason), "%s",
            next.reason);
        next = conservative;
        same_material = external_storage_material_equal(current, &next);
    }
    if (next.status == LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR
        && next.active_scratch_leases
            < governor->external_storage_lease_count) {
        next.active_scratch_leases = governor->external_storage_lease_count;
    }
    if (same_material) {
        if (next.generation > current->generation) {
            governor->external_storage.generation = next.generation;
        }
        return true;
    }
    governor->external_storage = next;
    external_storage_notify_locked(governor);
    return true;
}

bool
lardon3d_resource_governor_register_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    const Lardon3DResourceExternalStorage *storage
)
{
    Lardon3DResourceExternalStorage normalized;
    if (!governor || !controller
        || !normalize_external_storage(storage, &normalized)) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (governor->external_storage_registered) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    governor->external_storage_registered = true;
    governor->external_storage_controller = controller;
    governor->external_storage = normalized;
    external_storage_notify_locked(governor);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_update_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    const Lardon3DResourceExternalStorage *storage
)
{
    Lardon3DResourceExternalStorage normalized;
    if (!governor || !controller
        || !normalize_external_storage(storage, &normalized)) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
#ifdef LARDON3D_RESOURCE_EXTERNAL_STORAGE_TESTING
    lardon3d_resource_governor_external_update_engaged_for_test(
        governor, controller);
#endif
    bool exact_registration = governor->external_storage_registered
        && governor->external_storage_controller == controller;
    bool deferred_release_snapshot = exact_registration
        && governor->external_storage_operation_active
        && normalized.active_scratch_leases
            < governor->external_storage_lease_count;
    /* A controller release becomes visible just before the wrapper removes
     * its address under this mutex. Coalesce that narrow observation instead
     * of publishing an impossible under-count; finish immediately applies its
     * own post-operation cached snapshot. */
    bool updated = exact_registration
        && (deferred_release_snapshot
            || external_storage_apply_locked(governor, &normalized,
                EXTERNAL_STORAGE_APPLY_PUBLIC));
    (void)pthread_mutex_unlock(&governor->mutex);
    return updated;
}

bool
lardon3d_resource_governor_unregister_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller
)
{
    if (!governor || !controller) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (!governor->external_storage_registered
        || governor->external_storage_controller != controller
        || governor->external_storage_operation_active
        || governor->external_storage_lease_count != 0
        || governor->external_storage.active_scratch_leases != 0) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    governor->external_storage_registered = false;
    governor->external_storage_controller = NULL;
    governor->external_storage = (Lardon3DResourceExternalStorage) {0};
    external_storage_notify_locked(governor);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_get_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceExternalStorage *storage
)
{
    if (storage) {
        memset(storage, 0, sizeof(*storage));
    }
    if (!governor || !storage) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool registered = governor->external_storage_registered;
    if (registered) {
        *storage = governor->external_storage;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return registered;
}

bool
lardon3d_resource_governor_internal_begin_scratch_operation(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease,
    bool acquire
)
{
    if (!governor || !controller || !lease) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool permitted = governor->external_storage_registered
        && governor->external_storage_controller == controller
        && !governor->external_storage_operation_active;
    size_t found = LARDON3D_SSD_MAX_SCRATCH_LEASES;
    for (size_t index = 0;
         index < governor->external_storage_lease_count; ++index) {
        if (governor->external_storage_leases[index] == lease) {
            found = index;
            break;
        }
    }
    if (acquire) {
        permitted = permitted
            && found == LARDON3D_SSD_MAX_SCRATCH_LEASES
            && governor->external_storage_lease_count
                < LARDON3D_SSD_MAX_SCRATCH_LEASES
            && governor->external_storage.new_scratch_allocations_allowed
            && (governor->external_storage.status
                    == LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE
                || governor->external_storage.status
                    == LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE);
    } else {
        permitted = permitted
            && found != LARDON3D_SSD_MAX_SCRATCH_LEASES;
    }
    if (permitted) {
        governor->external_storage_operation_active = true;
        governor->external_storage_operation_acquire = acquire;
        governor->external_storage_operation_lease = lease;
        governor->external_storage_operation_start_lease_count =
            governor->external_storage.active_scratch_leases;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return permitted;
}

static void
external_storage_operation_error_locked(
    Lardon3DResourceGovernor *governor,
    size_t expected_physical_lease_count
)
{
    Lardon3DResourceExternalStorage error = governor->external_storage;
    error.status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR;
    error.new_scratch_allocations_allowed = false;
    /* The current copied count may already contain an updater's pre/post-call
     * view. The frozen start count plus the physical outcome is the only
     * serialized evidence that cannot double-apply acquire/release. */
    error.active_scratch_leases = expected_physical_lease_count;
    (void)snprintf(error.reason, sizeof(error.reason), "%s",
        "SSD lease completed but cached reconciliation was malformed; "
        "new scratch allocation is blocked");
    (void)external_storage_apply_locked(governor, &error,
        EXTERNAL_STORAGE_APPLY_WRAPPER_COMPLETION);
}

bool
lardon3d_resource_governor_internal_finish_scratch_operation(
    Lardon3DResourceGovernor *governor,
    Lardon3DSsdController *controller,
    Lardon3DSsdScratchLease *lease,
    bool acquire,
    bool physical_success,
    const Lardon3DResourceExternalStorage *storage
)
{
    if (!governor || !controller || !lease) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (!governor->external_storage_registered
        || governor->external_storage_controller != controller
        || !governor->external_storage_operation_active
        || governor->external_storage_operation_acquire != acquire
        || governor->external_storage_operation_lease != lease) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }

    size_t expected_physical_lease_count =
        governor->external_storage_operation_start_lease_count;
    bool physical_count_valid = true;
    if (physical_success && acquire) {
        if (expected_physical_lease_count
            == LARDON3D_SSD_MAX_SCRATCH_LEASES) {
            physical_count_valid = false;
        } else {
            ++expected_physical_lease_count;
        }
    } else if (physical_success) {
        if (expected_physical_lease_count == 0) {
            physical_count_valid = false;
        } else {
            --expected_physical_lease_count;
        }
    }

    bool bookkeeping_valid = true;
    size_t found = LARDON3D_SSD_MAX_SCRATCH_LEASES;
    for (size_t index = 0;
         index < governor->external_storage_lease_count; ++index) {
        if (governor->external_storage_leases[index] == lease) {
            found = index;
            break;
        }
    }
    if (physical_success && acquire) {
        if (found != LARDON3D_SSD_MAX_SCRATCH_LEASES
            || governor->external_storage_lease_count
                >= LARDON3D_SSD_MAX_SCRATCH_LEASES) {
            bookkeeping_valid = false;
        } else {
            governor->external_storage_leases[
                governor->external_storage_lease_count++] = lease;
        }
    } else if (physical_success) {
        if (found == LARDON3D_SSD_MAX_SCRATCH_LEASES) {
            bookkeeping_valid = false;
        } else {
            --governor->external_storage_lease_count;
            governor->external_storage_leases[found] =
                governor->external_storage_leases[
                    governor->external_storage_lease_count];
            governor->external_storage_leases[
                governor->external_storage_lease_count] = NULL;
        }
    }

    Lardon3DResourceExternalStorage normalized;
    if (expected_physical_lease_count
        < governor->external_storage_lease_count) {
        physical_count_valid = false;
        expected_physical_lease_count =
            governor->external_storage_lease_count;
    }

    bool reconciled = bookkeeping_valid && physical_count_valid
        && normalize_external_storage(storage, &normalized);
    if (reconciled) {
        reconciled = normalized.active_scratch_leases
                == expected_physical_lease_count
            && external_storage_apply_locked(governor, &normalized,
                EXTERNAL_STORAGE_APPLY_WRAPPER_COMPLETION);
        /* A concurrently published newer controller generation is already the
         * stronger exact evidence and need not be overwritten by this copy. */
        if (!reconciled
            && normalized.generation < governor->external_storage.generation
            && governor->external_storage.active_scratch_leases
                >= governor->external_storage_lease_count) {
            reconciled = true;
        }
    }
    if (!reconciled) {
        external_storage_operation_error_locked(governor,
            expected_physical_lease_count);
    }
    governor->external_storage_operation_active = false;
    governor->external_storage_operation_acquire = false;
    governor->external_storage_operation_lease = NULL;
    governor->external_storage_operation_start_lease_count = 0;
    (void)pthread_mutex_unlock(&governor->mutex);
    /* A successful physical acquire must be reported to its owner so the
     * caller can later release it, even if telemetry had to fail closed. */
    return physical_success && bookkeeping_valid;
}

const char *
lardon3d_resource_external_storage_status_name(
    Lardon3DResourceExternalStorageStatus status
)
{
    switch (status) {
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT:
        return "ABSENT";
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED:
        return "DETECTED";
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE:
        return "AVAILABLE";
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE:
        return "IN_USE";
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING:
        return "DRAINING";
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE:
        return "SAFE";
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR:
        return "ERROR";
    }
    return "UNKNOWN";
}

static bool
availability_locked(
    const Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    uint64_t memory_reserve_bytes,
    Lardon3DResourceAvailability *availability
)
{
    if (!valid_snapshot(&governor->profile, snapshot)) {
        return false;
    }
    uint64_t detected_memory = minimum_uint64(
        snapshot->memory_available_bytes,
        governor->profile.memory_total_bytes
    );
    uint64_t memory_budget = subtract_floor(
        detected_memory,
        memory_reserve_bytes
    );
    uint64_t gpu_budget = 0;
    bool gpu_known = false;
    if (governor->profile.gpu_uses_shared_memory) {
        gpu_known = true;
        gpu_budget = memory_budget;
    } else if (governor->profile.gpu_memory_known
        && snapshot->gpu_memory_available_known) {
        gpu_known = true;
        gpu_budget = subtract_floor(
            snapshot->gpu_memory_available_bytes,
            governor->policy.gpu_memory_reserve_bytes
        );
    }
    unsigned int cpu_budget = governor->cpu_policy.compute_cpu_count;
    *availability = (Lardon3DResourceAvailability) {
        .memory_budget_bytes = memory_budget,
        .memory_reserved_bytes = governor->memory_reserved_bytes,
        .memory_available_bytes = subtract_floor(
            memory_budget,
            governor->memory_reserved_bytes
        ),
        .gpu_memory_known = gpu_known,
        .gpu_memory_budget_bytes = gpu_budget,
        .gpu_memory_reserved_bytes = governor->gpu_memory_reserved_bytes,
        .gpu_memory_available_bytes = governor->profile.gpu_uses_shared_memory
            ? subtract_floor(memory_budget, governor->memory_reserved_bytes)
            : subtract_floor(gpu_budget, governor->gpu_memory_reserved_bytes),
        .cpu_budget = cpu_budget,
        .cpu_reserved = governor->cpu_reserved,
        .cpu_available = subtract_unsigned(cpu_budget, governor->cpu_reserved),
        .gpu_slot_budget = governor->policy.gpu_slot_capacity,
        .gpu_slots_reserved = governor->gpu_slots_reserved,
        .gpu_slots_available = subtract_unsigned(
            governor->policy.gpu_slot_capacity,
            governor->gpu_slots_reserved
        ),
        .io_slot_budget = governor->policy.io_slot_capacity,
        .io_slots_reserved = governor->io_slots_reserved,
        .io_slots_available = subtract_unsigned(
            governor->policy.io_slot_capacity,
            governor->io_slots_reserved
        ),
        .active_reservations = governor->active_count,
    };
    return true;
}

bool
lardon3d_resource_governor_availability(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    Lardon3DResourceAvailability *availability
)
{
    if (!governor || !snapshot || !availability) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool success = availability_locked(
        governor,
        snapshot,
        governor->policy.system_memory_reserve_bytes,
        availability
    );
    (void)pthread_mutex_unlock(&governor->mutex);
    return success;
}

uint64_t
lardon3d_resource_governor_generation(
    Lardon3DResourceGovernor *governor
)
{
    if (!governor) {
        return 0;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    uint64_t generation = governor->generation;
    (void)pthread_mutex_unlock(&governor->mutex);
    return generation;
}

bool
lardon3d_resource_governor_wait_for_change(
    Lardon3DResourceGovernor *governor,
    uint64_t observed_generation,
    uint64_t timeout_ns
)
{
    if (!governor) {
        return false;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    uint64_t remainder = timeout_ns % 1000000000ULL;
    deadline.tv_nsec += (long)remainder;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool changed = false;
    while (governor->generation == observed_generation) {
        int result = pthread_cond_timedwait(
            &governor->cond,
            &governor->mutex,
            &deadline
        );
        if (result == ETIMEDOUT) {
            break;
        }
        if (result != 0) {
            (void)pthread_mutex_unlock(&governor->mutex);
            return false;
        }
    }
    changed = governor->generation != observed_generation;
    (void)pthread_mutex_unlock(&governor->mutex);
    return changed;
}

static bool
evaluate_locked(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    bool update_pressure,
    const Lardon3DMemoryFloors *floors
)
{
    if (!valid_estimate(estimate)) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Estimation de ressources invalide.");
        return true;
    }
    bool fresh;
    if (!snapshot_is_fresh_locked(governor, snapshot, &fresh)) {
        return false;
    }
    if (!fresh) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0,
            "Instantané de ressources périmé.");
        return true;
    }
    bool psi_pressure = (governor->policy.maximum_cpu_pressure_avg10 > 0.0
            && snapshot->cpu_pressure_known
            && snapshot->cpu_pressure_avg10
                >= governor->policy.maximum_cpu_pressure_avg10)
        || (governor->policy.maximum_memory_pressure_avg10 > 0.0
            && snapshot->memory_pressure_known
            && snapshot->memory_pressure_avg10
                >= governor->policy.maximum_memory_pressure_avg10)
        || (governor->policy.maximum_io_pressure_avg10 > 0.0
            && snapshot->io_pressure_known
            && snapshot->io_pressure_avg10
                >= governor->policy.maximum_io_pressure_avg10);
    if (update_pressure) {
        bool swap_changed = false;
        if (snapshot->swap_activity_known) {
            if (governor->swap_baseline_known) {
                swap_changed = snapshot->swap_pages_in
                        > governor->last_swap_pages_in
                    || snapshot->swap_pages_out
                        > governor->last_swap_pages_out;
            }
            governor->last_swap_pages_in = snapshot->swap_pages_in;
            governor->last_swap_pages_out = snapshot->swap_pages_out;
            governor->swap_baseline_known = true;
        }
        bool hard_memory_pressure = floors->hard_floor > 0
            && snapshot->memory_available_bytes <= floors->hard_floor;
        bool caution_memory_pressure = floors->caution_floor > 0
            && snapshot->memory_available_bytes <= floors->caution_floor;
        bool pressure_signal = psi_pressure || swap_changed
            || (caution_memory_pressure
                && !floors->caution_is_non_escalating_band);

        if (hard_memory_pressure) {
            governor->pressure = LARDON3D_RESOURCE_PRESSURE_RED;
            governor->pressure_streak = 0;
            governor->recovery_streak = 0;
            governor->slow_start_streak = 0;
            governor->slow_start_limit = 1;
            governor->slow_start_active = true;
        } else if (pressure_signal) {
            governor->recovery_streak = 0;
            governor->slow_start_streak = 0;
            governor->pressure_streak = increment_to_limit(
                governor->pressure_streak,
                2
            );
            if (governor->pressure == LARDON3D_RESOURCE_PRESSURE_RED
                || governor->pressure_streak >= 2) {
                governor->pressure = LARDON3D_RESOURCE_PRESSURE_RED;
                governor->slow_start_limit = 1;
                governor->slow_start_active = true;
            } else {
                governor->pressure = LARDON3D_RESOURCE_PRESSURE_YELLOW;
            }
        } else if (caution_memory_pressure) {
            /* The default 3..4 GiB band is caution, not exhaustion: hold the
             * Governor at YELLOW and reset aggressive growth without promoting
             * a stable caution-only host to RED. Capacity below still subtracts
             * the hard admission floor, so work that would cross it must WAIT. */
            governor->pressure_streak = 0;
            governor->recovery_streak = 0;
            governor->slow_start_streak = 0;
            governor->slow_start_limit = 1;
            governor->slow_start_active = true;
            if (governor->pressure != LARDON3D_RESOURCE_PRESSURE_RED) {
                governor->pressure = LARDON3D_RESOURCE_PRESSURE_YELLOW;
            }
        } else {
            governor->pressure_streak = 0;
            if (governor->pressure == LARDON3D_RESOURCE_PRESSURE_RED) {
                governor->recovery_streak = increment_to_limit(
                    governor->recovery_streak,
                    3
                );
                if (governor->recovery_streak >= 3) {
                    governor->pressure = LARDON3D_RESOURCE_PRESSURE_YELLOW;
                    governor->recovery_streak = 0;
                }
            } else if (governor->pressure
                    == LARDON3D_RESOURCE_PRESSURE_YELLOW) {
                if (!governor->slow_start_active) {
                    governor->slow_start_active = true;
                    governor->slow_start_limit = 1;
                    governor->slow_start_streak = 0;
                }
                governor->recovery_streak = increment_to_limit(
                    governor->recovery_streak,
                    3
                );
                if (governor->recovery_streak >= 3) {
                    governor->pressure = LARDON3D_RESOURCE_PRESSURE_GREEN;
                    governor->recovery_streak = 0;
                    governor->slow_start_streak = 0;
                }
            } else if (governor->slow_start_active) {
                governor->slow_start_streak = increment_to_limit(
                    governor->slow_start_streak,
                    3
                );
                if (governor->slow_start_streak >= 3) {
                    governor->slow_start_streak = 0;
                    if (governor->slow_start_limit > SIZE_MAX / 2) {
                        governor->slow_start_limit = SIZE_MAX;
                        governor->slow_start_active = false;
                    } else {
                        governor->slow_start_limit *= 2;
                    }
                }
            }
        }
    }
    if (governor->pressure == LARDON3D_RESOURCE_PRESSURE_RED) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0,
            "Pression mémoire ou swap persistante.");
        return true;
    }
    if ((estimate->desired_gpu_slots > 0
            || estimate->gpu_memory_fixed_bytes > 0
            || estimate->gpu_memory_bytes_per_item > 0)
        && !governor->profile.gpu_available) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Aucun GPU disponible.");
        return true;
    }

    uint64_t memory_fixed = estimate->memory_fixed_bytes;
    uint64_t memory_per_item = estimate->memory_bytes_per_item;
    if (governor->profile.gpu_uses_shared_memory) {
        if (memory_fixed > UINT64_MAX - estimate->gpu_memory_fixed_bytes
            || memory_per_item
                > UINT64_MAX - estimate->gpu_memory_bytes_per_item) {
            set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Estimation mémoire trop grande.");
            return true;
        }
        memory_fixed += estimate->gpu_memory_fixed_bytes;
        memory_per_item += estimate->gpu_memory_bytes_per_item;
    }

    size_t theoretical_batch = batch_capacity(
        governor->profile.memory_total_bytes
            - floors->admission_floor,
        memory_fixed,
        memory_per_item
    );
    if (!governor->profile.gpu_uses_shared_memory
        && (estimate->gpu_memory_fixed_bytes > 0
            || estimate->gpu_memory_bytes_per_item > 0)
        && governor->profile.gpu_memory_known) {
        theoretical_batch = minimum_size(
            theoretical_batch,
            batch_capacity(
                governor->profile.gpu_memory_total_bytes
                    - governor->policy.gpu_memory_reserve_bytes,
                estimate->gpu_memory_fixed_bytes,
                estimate->gpu_memory_bytes_per_item
            )
        );
    }
    if (theoretical_batch < estimate->minimum_batch_size) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Tâche impossible sur cette machine.");
        return true;
    }

    double load_limit = (double)governor->profile.logical_cpu_count
        * governor->policy.maximum_cpu_load_ratio;
    if (snapshot->cpu_load_1m >= load_limit) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Charge CPU trop élevée.");
        return true;
    }
    if (governor->policy.maximum_cpu_pressure_avg10 > 0.0
        && snapshot->cpu_pressure_known
        && snapshot->cpu_pressure_avg10
            >= governor->policy.maximum_cpu_pressure_avg10) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0,
            "Pression CPU trop élevée.");
        return true;
    }
    if (governor->policy.maximum_memory_pressure_avg10 > 0.0
        && snapshot->memory_pressure_known
        && snapshot->memory_pressure_avg10
            >= governor->policy.maximum_memory_pressure_avg10) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0,
            "Pression mémoire trop élevée.");
        return true;
    }
    if (estimate->desired_io_slots > 0 && snapshot->io_pressure_known
        && snapshot->io_pressure_avg10
            >= governor->policy.maximum_io_pressure_avg10) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Pression d'entrée-sortie trop élevée.");
        return true;
    }

    Lardon3DResourceAvailability available;
    if (!availability_locked(
            governor,
            snapshot,
            floors->admission_floor,
            &available
        )) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Instantané de ressources invalide.");
        return true;
    }
    size_t batch = batch_capacity(
        available.memory_available_bytes,
        memory_fixed,
        memory_per_item
    );
    if (!governor->profile.gpu_uses_shared_memory
        && (estimate->gpu_memory_fixed_bytes > 0
            || estimate->gpu_memory_bytes_per_item > 0)) {
        if (!available.gpu_memory_known) {
            set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Mémoire GPU disponible inconnue.");
            return true;
        }
        batch = minimum_size(
            batch,
            batch_capacity(
                available.gpu_memory_available_bytes,
                estimate->gpu_memory_fixed_bytes,
                estimate->gpu_memory_bytes_per_item
            )
        );
    }
    /* Le lot maximal visé est corrigé par les métriques mesurées : c'est la
     * nouvelle cible du contrat, pas une réduction faute de ressources. La
     * correction ne descend jamais sous minimum_batch_size pour éviter un
     * WAIT persistant. */
    size_t adapted_maximum = adaptive_batch_limit(
        governor,
        estimate->task_class,
        estimate->maximum_batch_size,
        memory_per_item
    );
    if (adapted_maximum < estimate->minimum_batch_size) {
        adapted_maximum = estimate->minimum_batch_size;
    }
    if (governor->slow_start_active) {
        adapted_maximum = minimum_size(adapted_maximum, governor->slow_start_limit);
    }
    if (governor->pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW
        && adapted_maximum > estimate->minimum_batch_size) {
        adapted_maximum /= 2;
        if (adapted_maximum < estimate->minimum_batch_size) {
            adapted_maximum = estimate->minimum_batch_size;
        }
    }
    batch = minimum_size(batch, adapted_maximum);
    if (batch < estimate->minimum_batch_size) {
        set_decision(
            decision,
            LARDON3D_RESOURCE_WAIT,
            0,
            0,
            0,
            0,
            "Ressources déjà réservées ou temporairement insuffisantes."
        );
        return true;
    }
    if (available.cpu_available == 0
        || (estimate->desired_gpu_slots > 0
            && available.gpu_slots_available == 0)
        || (estimate->desired_io_slots > 0
            && available.io_slots_available == 0)) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Slots de calcul déjà réservés.");
        return true;
    }
    unsigned int cpu = estimate->desired_cpu_threads < available.cpu_available
        ? estimate->desired_cpu_threads
        : available.cpu_available;
    unsigned int gpu = estimate->desired_gpu_slots
        < available.gpu_slots_available
        ? estimate->desired_gpu_slots
        : available.gpu_slots_available;
    unsigned int io = estimate->desired_io_slots < available.io_slots_available
        ? estimate->desired_io_slots
        : available.io_slots_available;
    bool reduced = batch < adapted_maximum
        || cpu < estimate->desired_cpu_threads
        || gpu < estimate->desired_gpu_slots
        || io < estimate->desired_io_slots;
    set_decision(
        decision,
        reduced ? LARDON3D_RESOURCE_REDUCE_BATCH : LARDON3D_RESOURCE_START,
        batch,
        cpu,
        gpu,
        io,
        reduced ? "Contrat réduit aux ressources disponibles."
            : "Ressources disponibles."
    );
    return true;
}

static bool
activate_reservation_locked(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceEstimate *estimate,
    const Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation *created
)
{
    if (governor->next_reservation_id == 0) {
        return false;
    }
    uint64_t memory_bytes;
    uint64_t gpu_memory_bytes;
    uint64_t charged_memory_bytes = 0;
    struct timespec created_at;
    if (!resource_size(
            estimate->memory_fixed_bytes,
            estimate->memory_bytes_per_item,
            decision->batch_size,
            &memory_bytes
        )
        || !resource_size(
            estimate->gpu_memory_fixed_bytes,
            estimate->gpu_memory_bytes_per_item,
            decision->batch_size,
            &gpu_memory_bytes
        )
        || (governor->profile.gpu_uses_shared_memory
            && !add_uint64(memory_bytes, gpu_memory_bytes,
                &charged_memory_bytes))
        || clock_gettime(CLOCK_REALTIME, &created_at) != 0) {
        return false;
    }
    if (!governor->profile.gpu_uses_shared_memory) {
        charged_memory_bytes = memory_bytes;
    }
    /* UMA memory is charged to the host reservation exactly once. The public
     * diagnostic retains the GPU component, but it is not a second physical
     * budget alongside charged_memory_bytes. */
    created->information = (Lardon3DResourceReservationInfo) {
        .id = governor->next_reservation_id++,
        .memory_bytes = memory_bytes,
        .gpu_memory_bytes = gpu_memory_bytes,
        .cpu_threads = decision->cpu_threads,
        .gpu_slots = decision->gpu_slots,
        .io_slots = decision->io_slots,
        .batch_size = decision->batch_size,
        .task_class = estimate->task_class,
        .state = LARDON3D_RESERVATION_ACTIVE,
        .created_at = created_at,
    };
    created->charged_memory_bytes = charged_memory_bytes;
    created->next = governor->active;
    governor->active = created;
    governor->memory_reserved_bytes += charged_memory_bytes;
    governor->gpu_memory_reserved_bytes += gpu_memory_bytes;
    governor->cpu_reserved += decision->cpu_threads;
    governor->gpu_slots_reserved += decision->gpu_slots;
    governor->io_slots_reserved += decision->io_slots;
    ++governor->active_count;
    ++governor->generation;
    (void)pthread_cond_broadcast(&governor->cond);
    return true;
}

bool
lardon3d_resource_governor_reserve(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
)
{
    if (!governor || !snapshot || !estimate || !decision || !reservation) {
        return false;
    }
    *reservation = NULL;
    Lardon3DResourceReservation *created = calloc(1, sizeof(*created));
    if (!created) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (!valid_snapshot(&governor->profile, snapshot)) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return false;
    }
    Lardon3DMemoryFloors floors;
    internal_memory_floors_locked(governor, &floors);
    if (!evaluate_locked(
            governor,
            snapshot,
            estimate,
            decision,
            true,
            &floors
        )) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return false;
    }
    if (decision->kind != LARDON3D_RESOURCE_START
        && decision->kind != LARDON3D_RESOURCE_REDUCE_BATCH) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return true;
    }
    if (!activate_reservation_locked(governor, estimate, decision, created)) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0,
            "Impossible d'activer la réservation.");
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return false;
    }
    *reservation = created;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_reserve_available(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
)
{
    if (!governor || !estimate || !decision || !reservation) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DHardwareProfile profile = governor->profile;
    bool force_capture_failure = governor->internal_force_capture_failure;
#if defined(LARDON3D_RESOURCE_GOVERNOR_CAPTURE_TESTING)
    bool override_capture = governor->internal_capture_snapshot_override;
    Lardon3DResourceSnapshot override_snapshot =
        governor->internal_capture_snapshot;
#endif
    (void)pthread_mutex_unlock(&governor->mutex);
    if (force_capture_failure) {
        return false;
    }
    Lardon3DResourceSnapshot snapshot;
#if defined(LARDON3D_RESOURCE_GOVERNOR_CAPTURE_TESTING)
    /* TEST CONTRACT: exact-output fixtures must not inherit ambient host load
     * or pressure. The template is per-Governor and copied under its mutex;
     * only freshness is regenerated at the same boundary as production. */
    if (override_capture) {
        snapshot = override_snapshot;
        if (clock_gettime(CLOCK_MONOTONIC, &snapshot.captured_at) != 0) {
            return false;
        }
    } else
#endif
    if (!lardon3d_resource_snapshot_capture(&profile, &snapshot, NULL, 0)) {
        return false;
    }
    return lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        estimate,
        decision,
        reservation
    );
}

static Lardon3DResourceReservation *
find_reservation(
    Lardon3DResourceReservation *head,
    const Lardon3DResourceReservation *wanted
)
{
    for (Lardon3DResourceReservation *current = head;
         current;
         current = current->next) {
        if (current == wanted) {
            return current;
        }
    }
    return NULL;
}

bool
lardon3d_resource_governor_release(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation *reservation
)
{
    if (!governor || !reservation) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceReservation *previous = NULL;
    Lardon3DResourceReservation *current = governor->active;
    while (current && current != reservation) {
        previous = current;
        current = current->next;
    }
    if (!current) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    if (previous) {
        previous->next = current->next;
    } else {
        governor->active = current->next;
    }
    governor->memory_reserved_bytes -= current->charged_memory_bytes;
    governor->gpu_memory_reserved_bytes -= current->information.gpu_memory_bytes;
    governor->cpu_reserved -= current->information.cpu_threads;
    governor->gpu_slots_reserved -= current->information.gpu_slots;
    governor->io_slots_reserved -= current->information.io_slots;
    --governor->active_count;
    current->information.state = LARDON3D_RESERVATION_RELEASED;
    current->next = governor->released;
    governor->released = current;
    ++governor->generation;
    (void)pthread_cond_broadcast(&governor->cond);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_reservation_is_valid(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation
)
{
    if (!governor || !reservation) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool valid = find_reservation(governor->active, reservation) != NULL;
    (void)pthread_mutex_unlock(&governor->mutex);
    return valid;
}

bool
lardon3d_resource_reservation_get(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation,
    Lardon3DResourceReservationInfo *information
)
{
    if (!governor || !reservation || !information) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceReservation *found = find_reservation(
        governor->active,
        reservation
    );
    if (!found) {
        found = find_reservation(governor->released, reservation);
    }
    if (found) {
        *information = found->information;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return found != NULL;
}

bool
lardon3d_resource_reservation_get_active(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation,
    Lardon3DResourceReservationInfo *information
)
{
    if (!governor || !reservation || !information) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceReservation *found = find_reservation(
        governor->active,
        reservation
    );
    if (found) {
        *information = found->information;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return found != NULL;
}

size_t
lardon3d_resource_governor_list_reservations(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservationInfo *reservations,
    size_t capacity
)
{
    if (!governor || (!reservations && capacity > 0)) {
        return 0;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    size_t count = 0;
    for (Lardon3DResourceReservation *current = governor->active;
         current && count < capacity;
         current = current->next) {
        reservations[count++] = current->information;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return count;
}

size_t
lardon3d_resource_governor_reservation_count(
    Lardon3DResourceGovernor *governor
)
{
    if (!governor) {
        return 0;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    size_t count = governor->active_count;
    (void)pthread_mutex_unlock(&governor->mutex);
    return count;
}

bool
lardon3d_resource_governor_record_batch(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t batch_size,
    uint64_t duration_ns,
    size_t peak_memory_bytes
)
{
    if (!governor) {
        return false;
    }
    if (batch_size == 0) {
        /* No-op réussi : aucune métrique, aucun réveil inutile. */
        return true;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool recorded = record_batch_locked(
        governor,
        task_class,
        batch_size,
        duration_ns,
        peak_memory_bytes
    );
    if (recorded) {
        ++governor->generation;
        (void)pthread_cond_broadcast(&governor->cond);
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return recorded;
}

bool
lardon3d_resource_governor_decide(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceRequest *request,
    Lardon3DResourceDecision *decision
)
{
    if (!governor || !snapshot || !request || !decision) {
        return false;
    }
    Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = request->memory_bytes_per_item,
        .gpu_memory_bytes_per_item = request->gpu_memory_bytes_per_item,
        .minimum_batch_size = request->minimum_batch_size,
        .maximum_batch_size = request->preferred_batch_size,
        .desired_cpu_threads = request->requested_cpu_threads,
        .desired_gpu_slots = request->gpu_memory_bytes_per_item > 0 ? 1 : 0,
        .desired_io_slots = request->io_intensive ? 1 : 0,
        .task_class = request->io_intensive
            ? LARDON3D_RESOURCE_TASK_IO
            : LARDON3D_RESOURCE_TASK_GENERAL,
    };
    (void)pthread_mutex_lock(&governor->mutex);
    if (!valid_snapshot(&governor->profile, snapshot)) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    Lardon3DMemoryFloors floors;
    internal_memory_floors_locked(governor, &floors);
    bool success = evaluate_locked(
        governor,
        snapshot,
        &estimate,
        decision,
        true,
        &floors
    );
    (void)pthread_mutex_unlock(&governor->mutex);
    return success;
}

static bool
valid_capability_envelope(const Lardon3DTaskCapabilityEnvelope *envelope)
{
    if (!envelope || envelope->count == 0
        || envelope->count > LARDON3D_RESOURCE_CAPABILITY_MAX) {
        return false;
    }
    for (size_t index = 0; index < envelope->count; ++index) {
        const Lardon3DTaskCapability *capability =
            &envelope->capabilities[index];
        if (!valid_estimate(&capability->estimate)
            || capability->backend < LARDON3D_RESOURCE_BACKEND_FIXED
            || capability->backend > LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
            || capability->inflight_limit == 0) {
            return false;
        }
        size_t minimum_inflight = capability->minimum_inflight_limit != 0
            ? capability->minimum_inflight_limit : capability->inflight_limit;
        if (minimum_inflight > capability->inflight_limit
            || (capability->inflight_adaptive
                && (capability->minimum_inflight_limit == 0
                    || capability->gpu_memory_bytes_per_inflight == 0))
            || (!capability->inflight_adaptive
                && capability->minimum_inflight_limit != 0
                && capability->minimum_inflight_limit
                    != capability->inflight_limit)
            || (capability->gpu_memory_bytes_per_inflight != 0
                && capability->inflight_limit
                    > UINT64_MAX / capability->gpu_memory_bytes_per_inflight)
            || (capability->gpu_memory_bytes_per_inflight != 0
                && capability->estimate.gpu_memory_fixed_bytes
                    > UINT64_MAX
                        - (uint64_t)capability->inflight_limit
                            * capability->gpu_memory_bytes_per_inflight)) {
            return false;
        }
    }
    return true;
}

static void
selection_reason(
    Lardon3DResourceCapabilitySelection *selection,
    const char *reason
)
{
    (void)snprintf(selection->reason, sizeof(selection->reason), "%s", reason);
}

static bool
try_capability_locked(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DTaskCapability *capability,
    size_t capability_index,
    bool update_pressure,
    const Lardon3DMemoryFloors *floors,
    Lardon3DResourceCapabilitySelection *selection
)
{
    Lardon3DResourceEstimate operational = capability->estimate;
    Lardon3DCapabilityFeedback *feedback = capability_feedback_locked(
        governor,
        task_kind,
        task_kind_version,
        capability->backend,
        true
    );
    if (!feedback) {
        return false;
    }
    if (capability->cpu_reducible || capability->batch_adaptive
        || capability->inflight_adaptive) {
        if (feedback->adaptive_cpu_limit == 0
            || feedback->adaptive_batch_limit == 0
            || feedback->adaptive_inflight_limit == 0) {
            reset_capability_throughput_feedback(feedback, capability);
        }
    }
    size_t selected_inflight = capability->inflight_adaptive
        ? feedback->adaptive_inflight_limit : capability->inflight_limit;
    if (capability->cpu_reducible) {
        operational.desired_cpu_threads = feedback->adaptive_cpu_limit;
    }
    if (capability->batch_adaptive) {
        operational.maximum_batch_size = minimum_size(
            operational.maximum_batch_size,
            feedback->adaptive_batch_limit
        );
    }
    if (capability->gpu_memory_bytes_per_inflight != 0) {
        uint64_t inflight_bytes =
            (uint64_t)selected_inflight
                * capability->gpu_memory_bytes_per_inflight;
        if (!add_uint64(capability->estimate.gpu_memory_fixed_bytes,
                inflight_bytes, &operational.gpu_memory_fixed_bytes)) {
            return false;
        }
    }
    Lardon3DResourceDecision decision;
    if (!evaluate_locked(
            governor,
            snapshot,
            &operational,
            &decision,
            update_pressure,
            floors
        )) {
        return false;
    }
    if ((capability->batch_adaptive || capability->cpu_reducible
            || capability->inflight_adaptive)
        && governor->pressure != LARDON3D_RESOURCE_PRESSURE_GREEN) {
        /* The generic Gate G evaluator deliberately reduces YELLOW batches
         * gradually. A private adaptive capability has a stronger contract:
         * the first active pressure signal abandons its memory-heavy trial in
         * this admission, before the immutable sequence selection and its
         * reservation are installed. WAIT/REJECT decisions remain untouched. */
        bool admitting = decision.kind == LARDON3D_RESOURCE_START
            || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH;
        if (admitting) {
            if (capability->batch_adaptive) {
                decision.batch_size = operational.minimum_batch_size;
            }
            if (capability->cpu_reducible) {
                decision.cpu_threads = 1;
            }
            if (capability->inflight_adaptive) {
                selected_inflight = capability->minimum_inflight_limit;
            }
            if ((capability->batch_adaptive
                    && decision.batch_size
                        < capability->estimate.maximum_batch_size)
                || (capability->cpu_reducible
                    && decision.cpu_threads
                        < capability->estimate.desired_cpu_threads)
                || (capability->inflight_adaptive
                    && selected_inflight < capability->inflight_limit)) {
                decision.kind = LARDON3D_RESOURCE_REDUCE_BATCH;
            }
            (void)snprintf(decision.reason, sizeof(decision.reason),
                "Pression active : capacité adaptative réduite au minimum.");
        }
        reset_capability_throughput_feedback(feedback, capability);
    }
    if (capability->gpu_memory_bytes_per_inflight != 0) {
        uint64_t inflight_bytes =
            (uint64_t)selected_inflight
                * capability->gpu_memory_bytes_per_inflight;
        if (!add_uint64(capability->estimate.gpu_memory_fixed_bytes,
                inflight_bytes, &operational.gpu_memory_fixed_bytes)) {
            return false;
        }
    }
    if ((decision.kind == LARDON3D_RESOURCE_START
            || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH)
        && !capability->cpu_reducible
        && decision.cpu_threads != capability->estimate.desired_cpu_threads) {
        set_decision(
            &decision,
            LARDON3D_RESOURCE_WAIT,
            0,
            0,
            0,
            0,
            "Le compte CPU fixe n'est pas disponible."
        );
    }
    *selection = (Lardon3DResourceCapabilitySelection) {
        .capability_index = capability_index,
        .capability = *capability,
        .reservation_estimate = operational,
        .decision = decision,
        .inflight_limit = selected_inflight,
        .pressure = governor->pressure,
    };
    if (decision.kind == LARDON3D_RESOURCE_START
        || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH) {
        if (governor->pressure != LARDON3D_RESOURCE_PRESSURE_GREEN) {
            selection_reason(selection, "pressure-decrease");
        } else if (feedback->trial_dimension
            == LARDON3D_CAPABILITY_TRIAL_CPU) {
            selection_reason(selection, "cpu-throughput-trial");
        } else if (feedback->trial_dimension
            == LARDON3D_CAPABILITY_TRIAL_INFLIGHT) {
            selection_reason(selection, "inflight-throughput-trial");
        } else if (feedback->trial_dimension
            == LARDON3D_CAPABILITY_TRIAL_BATCH) {
            bool starvation_signal = capability->backend
                    == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
                && feedback->diagnostic.execution.vulkan_starvation_ns > 0;
            bool low_or_unknown_busy =
                !feedback->diagnostic.host.gpu_busy_known
                || feedback->diagnostic.host.gpu_busy_basis_points < 8000;
            selection_reason(selection,
                capability->sustained_gpu_batch_feedback
                    ? "gpu-batch-throughput-trial"
                : starvation_signal && low_or_unknown_busy
                    ? "gpu-starvation-throughput-trial"
                    : "throughput-trial");
        } else if (capability->batch_adaptive
            && feedback->batch_growth_stopped) {
            selection_reason(selection,
                capability->sustained_gpu_batch_feedback
                    ? "gpu-batch-throughput-no-gain"
                    : "throughput-no-gain");
        } else if (capability->cpu_reducible
            && feedback->cpu_growth_stopped) {
            selection_reason(selection, "cpu-throughput-no-gain");
        } else if (capability->inflight_adaptive
            && feedback->inflight_growth_stopped) {
            selection_reason(selection, "inflight-throughput-no-gain");
        } else if ((capability->batch_adaptive
                && operational.maximum_batch_size
                    < capability->estimate.maximum_batch_size)
            || (capability->cpu_reducible
                && operational.desired_cpu_threads
                    < capability->estimate.desired_cpu_threads)
            || (capability->inflight_adaptive
                && selected_inflight < capability->inflight_limit)) {
            selection_reason(selection, "healthy-slow-start");
        } else if (capability->backend
            == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN) {
            selection_reason(selection, "gpu-first");
        } else {
            selection_reason(selection,
                capability->backend == LARDON3D_RESOURCE_BACKEND_FIXED
                    ? "fixed-envelope" : "cpu-selected");
        }
    }
    return true;
}

static bool
capability_hardware_safe_locked(
    const Lardon3DResourceGovernor *governor,
    const Lardon3DTaskCapability *capability
)
{
    /* A capability maximum is an upper operational bound, not a mandatory
     * allocation. Memory and UMA safety belong to evaluate_locked(), which
     * sizes the exact immutable sequence contract against total RAM, current
     * availability, pressure and both desktop floors. Rejecting here because
     * the maximum batch does not fit would suppress a safe smaller contract
     * before the Governor can reduce it. */
    return capability->backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
        && governor->profile.gpu_available;
}

static bool
runtime_capability_available_locked(
    const Lardon3DResourceGovernor *governor,
    const Lardon3DTaskCapability *capability
)
{
    return !capability->requires_runtime_backend
        || (governor->orb_vulkan_backend_available
            && capability_hardware_safe_locked(governor, capability));
}

static void
advance_diagnostic_serial_locked(
    Lardon3DResourceGovernor *governor,
    Lardon3DCapabilityFeedback *feedback
)
{
    /* The public-like private pull cursor saturates instead of wrapping, so an
     * ancient after_serial never observes a false new decision. A separate
     * bounded update order preserves deterministic most-recent selection once
     * multiple backend records share UINT64_MAX. It may be renormalized
     * because no caller observes it and the Governor owns all 32 entries. */
    if (governor->capability_diagnostic_serial < UINT64_MAX) {
        ++governor->capability_diagnostic_serial;
    }
    if (governor->capability_diagnostic_update_order == UINT64_MAX) {
        for (size_t index = 0;
             index < LARDON3D_CAPABILITY_FEEDBACK_CAPACITY; ++index) {
            governor->capability_feedback[index].diagnostic_update_order = 0;
        }
        governor->capability_diagnostic_update_order = 0;
    }
    feedback->diagnostic_update_order =
        ++governor->capability_diagnostic_update_order;
    feedback->diagnostic_serial = governor->capability_diagnostic_serial;
    feedback->diagnostic.serial = feedback->diagnostic_serial;
}

bool
lardon3d_resource_governor_internal_reserve_capability(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DTaskCapabilityEnvelope *envelope,
    Lardon3DResourceCapabilitySelection *selection,
    Lardon3DResourceReservation **reservation
)
{
    if (!governor || !snapshot || !selection || !reservation
        || !valid_capability_envelope(envelope)) {
        return false;
    }
    *reservation = NULL;
    const char *feedback_kind = task_kind && task_kind[0]
        ? task_kind : "task.untyped";
    Lardon3DResourceReservation *created = calloc(1, sizeof(*created));
    if (!created) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (!valid_snapshot(&governor->profile, snapshot)) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return false;
    }
    Lardon3DMemoryFloors floors;
    internal_memory_floors_locked(governor, &floors);
    bool pressure_updated = false;
    bool saw_wait = false;
    bool evaluated = false;
    Lardon3DResourceCapabilitySelection last = {0};
    for (unsigned int preference = 0; preference < 2; ++preference) {
        for (size_t index = 0; index < envelope->count; ++index) {
            const Lardon3DTaskCapability *capability =
                &envelope->capabilities[index];
            if (capability->preferred != (preference == 0)) {
                continue;
            }
            if (!runtime_capability_available_locked(governor, capability)) {
                continue;
            }
            Lardon3DResourceCapabilitySelection candidate;
            if (!try_capability_locked(
                    governor,
                    snapshot,
                    feedback_kind,
                    task_kind_version,
                    capability,
                    index,
                    !pressure_updated,
                    &floors,
                    &candidate
                )) {
                (void)pthread_mutex_unlock(&governor->mutex);
                free(created);
                return false;
            }
            pressure_updated = true;
            evaluated = true;
            last = candidate;
            if (candidate.decision.kind == LARDON3D_RESOURCE_WAIT) {
                saw_wait = true;
            }
            if (candidate.decision.kind != LARDON3D_RESOURCE_START
                && candidate.decision.kind
                    != LARDON3D_RESOURCE_REDUCE_BATCH) {
                continue;
            }
            if (candidate.capability.backend
                    == LARDON3D_RESOURCE_BACKEND_CPU
                && envelope->count > 1 && index != 0) {
                selection_reason(
                    &candidate,
                    "cpu-fallback-gpu-non-admitted"
                );
            }
            if (!activate_reservation_locked(
                    governor,
                    &candidate.reservation_estimate,
                    &candidate.decision,
                    created
                )) {
                (void)pthread_mutex_unlock(&governor->mutex);
                free(created);
                return false;
            }
            Lardon3DCapabilityFeedback *feedback = capability_feedback_locked(
                governor,
                feedback_kind,
                task_kind_version,
                candidate.capability.backend,
                true
            );
            uint64_t previous_wall_time_ns =
                feedback->diagnostic.previous_wall_time_ns;
            size_t previous_items_completed =
                feedback->diagnostic.items_completed;
            uint64_t previous_rate_milli =
                feedback->diagnostic.durable_items_per_second_milli;
            Lardon3DResourceExecutionMetrics previous_execution =
                feedback->diagnostic.execution;
            feedback->diagnostic = (Lardon3DResourceSequenceDiagnostic) {
                .task_kind_version = task_kind_version,
                .backend = candidate.capability.backend,
                .actual_backend = candidate.capability.backend,
                .previous_wall_time_ns = previous_wall_time_ns,
                .items_completed = previous_items_completed,
                .durable_items_per_second_milli = previous_rate_milli,
                .execution = previous_execution,
                .batch_size = candidate.decision.batch_size,
                .inflight_limit = candidate.inflight_limit,
                .helper_limit = candidate.capability.helper_limit,
                .memory_bytes = created->information.memory_bytes,
                .gpu_memory_bytes = created->information.gpu_memory_bytes,
                .cpu_threads = candidate.decision.cpu_threads,
                .gpu_slots = candidate.decision.gpu_slots,
                .io_slots = candidate.decision.io_slots,
                .pressure = candidate.pressure,
                .host = governor->host_telemetry,
            };
            (void)snprintf(feedback->diagnostic.task_kind,
                sizeof(feedback->diagnostic.task_kind), "%s", feedback_kind);
            (void)snprintf(feedback->diagnostic.reason,
                sizeof(feedback->diagnostic.reason), "%s", candidate.reason);
            (void)snprintf(feedback->diagnostic.backend_reason,
                sizeof(feedback->diagnostic.backend_reason),
                "selected-backend-executing");
            /* Admission changes and completed-sequence totals are deliberately
             * separate: a failed/cancelled callback may have a contract but no
             * durable work observation. Both remain fixed-size operational
             * evidence owned by this Governor instance. */
            aggregate_admission(governor, feedback, &candidate);
            advance_diagnostic_serial_locked(governor, feedback);
            *selection = candidate;
            *reservation = created;
            (void)pthread_mutex_unlock(&governor->mutex);
            return true;
        }
    }
    if (!evaluated) {
        set_decision(&last.decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0,
            "Aucune capacité d'exécution disponible.");
        selection_reason(&last, "no-runtime-capability");
    } else if (saw_wait) {
        set_decision(&last.decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0,
            "Aucune capacité n'est actuellement admissible.");
        selection_reason(&last, "all-capabilities-wait");
    }
    last.pressure = governor->pressure;
    *selection = last;
    (void)pthread_mutex_unlock(&governor->mutex);
    free(created);
    return true;
}

bool
lardon3d_resource_governor_internal_reserve_capability_available(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DTaskCapabilityEnvelope *envelope,
    Lardon3DResourceCapabilitySelection *selection,
    Lardon3DResourceReservation **reservation
)
{
    if (!governor) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DHardwareProfile profile = governor->profile;
    bool force_capture_failure = governor->internal_force_capture_failure;
#if defined(LARDON3D_RESOURCE_GOVERNOR_CAPTURE_TESTING)
    bool override_capture = governor->internal_capture_snapshot_override;
    Lardon3DResourceSnapshot override_snapshot =
        governor->internal_capture_snapshot;
#endif
    (void)pthread_mutex_unlock(&governor->mutex);
    if (force_capture_failure) {
        return false;
    }
    /* Optional telemetry is sampled before the stable Gate G snapshot so a
     * slow/missing proc/sysfs reader cannot age an otherwise valid admission
     * snapshot. Its failure leaves private fields unknown only. */
    capture_host_telemetry(governor);
    Lardon3DResourceSnapshot snapshot;
#if defined(LARDON3D_RESOURCE_GOVERNOR_CAPTURE_TESTING)
    /* Keep the deterministic seam at the stable Gate G capture boundary.
     * Optional host diagnostics above remain observational and never acquire
     * admission authority from this test template. */
    if (override_capture) {
        snapshot = override_snapshot;
        if (clock_gettime(CLOCK_MONOTONIC, &snapshot.captured_at) != 0) {
            return false;
        }
    } else
#endif
    if (!lardon3d_resource_snapshot_capture(&profile, &snapshot, NULL, 0)) {
        return false;
    }
    return lardon3d_resource_governor_internal_reserve_capability(
        governor,
        &snapshot,
        task_kind,
        task_kind_version,
        envelope,
        selection,
        reservation
    );
}

bool
lardon3d_resource_governor_internal_record_sequence_execution(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason
)
{
    return lardon3d_resource_governor_internal_record_sequence_execution_metrics(
        governor, task_kind, task_kind_version, selection, wall_time_ns,
        items_completed, actual_backend, backend_reason, NULL);
}

bool
lardon3d_resource_governor_internal_record_sequence_execution_metrics(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend,
    const char *backend_reason,
    const Lardon3DResourceExecutionMetrics *metrics
)
{
    if (!governor || !selection || !task_kind || !task_kind[0]
        || wall_time_ns == 0 || !backend_reason
        || actual_backend < LARDON3D_RESOURCE_BACKEND_FIXED
        || actual_backend > LARDON3D_RESOURCE_BACKEND_MIXED) {
        return false;
    }
    capture_host_telemetry(governor);
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DCapabilityFeedback *feedback = capability_feedback_locked(
        governor,
        task_kind,
        task_kind_version,
        selection->capability.backend,
        true
    );
    if (!feedback) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    feedback->diagnostic.previous_wall_time_ns = wall_time_ns;
    feedback->diagnostic.items_completed = items_completed;
    feedback->diagnostic.actual_backend = actual_backend;
    feedback->diagnostic.backend_fallback =
        actual_backend != selection->capability.backend;
    feedback->diagnostic.host = governor->host_telemetry;
    feedback->diagnostic.execution = metrics
        ? *metrics : (Lardon3DResourceExecutionMetrics){0};
    (void)snprintf(feedback->diagnostic.backend_reason,
        sizeof(feedback->diagnostic.backend_reason), "%s", backend_reason);
    if (items_completed > 0
        && items_completed <= UINT64_MAX / 1000000000000ULL) {
        feedback->diagnostic.durable_items_per_second_milli =
            (uint64_t)items_completed * 1000000000000ULL / wall_time_ns;
    } else {
        feedback->diagnostic.durable_items_per_second_milli = 0;
    }
    bool adaptive = selection->capability.cpu_reducible
        || selection->capability.batch_adaptive
        || selection->capability.inflight_adaptive;
    if (adaptive) {
        if (selection->pressure != LARDON3D_RESOURCE_PRESSURE_GREEN) {
            reset_capability_throughput_feedback(
                feedback, &selection->capability);
            (void)snprintf(feedback->diagnostic.reason,
                sizeof(feedback->diagnostic.reason), "pressure-decrease");
        } else if (selection->capability.backend
                == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
            && actual_backend != LARDON3D_RESOURCE_BACKEND_ORB_VULKAN) {
            /* Complete CPU fallback is valid scientific output, but cannot be
             * a pure GPU observation. Abandon the trial and rebuild the full
             * capability-owned baseline window on later pure sequences. */
            feedback->adaptive_cpu_limit = feedback->accepted_cpu_limit;
            feedback->adaptive_batch_limit = feedback->accepted_batch_limit;
            feedback->adaptive_inflight_limit =
                feedback->accepted_inflight_limit;
            feedback->trial_dimension = LARDON3D_CAPABILITY_TRIAL_NONE;
            clear_baseline_rate_observations(feedback);
            feedback->baseline_rate_milli = 0;
            clear_trial_rate_observations(feedback);
            (void)snprintf(feedback->diagnostic.reason,
                sizeof(feedback->diagnostic.reason), "backend-fallback-hold");
        } else if (feedback->diagnostic.durable_items_per_second_milli == 0) {
            if (selection->capability.sustained_gpu_batch_feedback) {
                clear_baseline_rate_observations(feedback);
                clear_trial_rate_observations(feedback);
            }
            (void)snprintf(feedback->diagnostic.reason,
                sizeof(feedback->diagnostic.reason), "throughput-no-work");
        } else if (feedback->trial_dimension
                != LARDON3D_CAPABILITY_TRIAL_NONE) {
            uint64_t rate =
                feedback->diagnostic.durable_items_per_second_milli;
            bool exercised = feedback->trial_dimension
                    == LARDON3D_CAPABILITY_TRIAL_CPU
                ? selection->decision.cpu_threads
                        == feedback->adaptive_cpu_limit
                    && (!selection->capability.cpu_batch_coupled
                        || selection->decision.batch_size
                            == feedback->adaptive_batch_limit)
                : feedback->trial_dimension
                        == LARDON3D_CAPABILITY_TRIAL_INFLIGHT
                    ? selection->inflight_limit
                        == feedback->adaptive_inflight_limit
                    : selection->decision.batch_size
                        == feedback->adaptive_batch_limit;
            if (!exercised) {
                /* A reduced contract is not part of a consecutive GPU batch
                 * trial. Generic CPU/inflight behavior remains unchanged. */
                if (selection->capability.sustained_gpu_batch_feedback
                    && feedback->trial_dimension
                        == LARDON3D_CAPABILITY_TRIAL_BATCH) {
                    clear_trial_rate_observations(feedback);
                }
                (void)snprintf(feedback->diagnostic.reason,
                    sizeof(feedback->diagnostic.reason), "%s",
                    trial_observation_reason(
                        &selection->capability,
                        feedback->trial_dimension));
            } else {
                unsigned int required = required_throughput_observations(
                    &selection->capability, feedback->trial_dimension);
                if (feedback->trial_observations >= required) {
                    clear_trial_rate_observations(feedback);
                }
                feedback->trial_rates_milli[
                    feedback->trial_observations++] = rate;
                if (feedback->trial_observations < required) {
                    (void)snprintf(feedback->diagnostic.reason,
                        sizeof(feedback->diagnostic.reason), "%s",
                        trial_observation_reason(
                            &selection->capability,
                            feedback->trial_dimension));
                } else {
                    /* Generic adaptation retains the conservative minimum of
                     * two. Only the explicit ORB GPU batch property uses an
                     * overflow-safe eight-observation arithmetic mean. */
                    uint64_t trial_rate =
                        selection->capability.sustained_gpu_batch_feedback
                            && feedback->trial_dimension
                                == LARDON3D_CAPABILITY_TRIAL_BATCH
                        ? average_rate_observations(
                            feedback->trial_rates_milli, required)
                        : minimum_uint64(
                            feedback->trial_rates_milli[0],
                            feedback->trial_rates_milli[1]);
                    clear_trial_rate_observations(feedback);
                    Lardon3DCapabilityTrialDimension completed_dimension =
                        feedback->trial_dimension;
                    feedback->trial_dimension =
                        LARDON3D_CAPABILITY_TRIAL_NONE;
                    bool improved = throughput_materially_improved(
                        trial_rate, feedback->baseline_rate_milli);
                    if (improved) {
                        if (completed_dimension
                                == LARDON3D_CAPABILITY_TRIAL_CPU) {
                            feedback->accepted_cpu_limit =
                                feedback->adaptive_cpu_limit;
                            if (selection->capability.cpu_batch_coupled) {
                                /* The measured gain belongs to the complete
                                 * cross-image rung, not to an unusable CPU
                                 * count detached from its participant window. */
                                feedback->accepted_batch_limit =
                                    feedback->adaptive_batch_limit;
                            }
                        } else if (completed_dimension
                                == LARDON3D_CAPABILITY_TRIAL_INFLIGHT) {
                            feedback->accepted_inflight_limit =
                                feedback->adaptive_inflight_limit;
                        } else {
                            feedback->accepted_batch_limit =
                                feedback->adaptive_batch_limit;
                        }
                        feedback->baseline_rate_milli = trial_rate;
                        feedback->baseline_observations = required;
                    } else if (completed_dimension
                            == LARDON3D_CAPABILITY_TRIAL_CPU) {
                        feedback->adaptive_cpu_limit =
                            feedback->accepted_cpu_limit;
                        feedback->cpu_growth_stopped = true;
                    } else if (completed_dimension
                            == LARDON3D_CAPABILITY_TRIAL_INFLIGHT) {
                        feedback->adaptive_inflight_limit =
                            feedback->accepted_inflight_limit;
                        feedback->inflight_growth_stopped = true;
                    } else {
                        feedback->adaptive_batch_limit =
                            feedback->accepted_batch_limit;
                        feedback->batch_growth_stopped = true;
                    }
                    open_next_capability_trial_locked(
                        governor, feedback, &selection->capability);
                    (void)snprintf(feedback->diagnostic.reason,
                        sizeof(feedback->diagnostic.reason), "%s",
                        trial_result_reason(
                            &selection->capability, completed_dimension,
                            improved));
                }
            }
        } else {
            uint64_t rate =
                feedback->diagnostic.durable_items_per_second_milli;
            if (feedback->baseline_rate_milli == 0) {
                unsigned int required = required_throughput_observations(
                    &selection->capability,
                    LARDON3D_CAPABILITY_TRIAL_NONE);
                bool exercised =
                    !selection->capability.sustained_gpu_batch_feedback
                    || selection->decision.batch_size
                        == feedback->accepted_batch_limit;
                if (!exercised) {
                    clear_baseline_rate_observations(feedback);
                } else {
                    if (feedback->baseline_observations >= required) {
                        clear_baseline_rate_observations(feedback);
                    }
                    feedback->baseline_rates_milli[
                        feedback->baseline_observations++] = rate;
                }
                bool complete = exercised
                    && feedback->baseline_observations == required;
                if (complete) {
                    feedback->baseline_rate_milli =
                        average_rate_observations(
                            feedback->baseline_rates_milli, required);
                    open_next_capability_trial_locked(
                        governor, feedback, &selection->capability);
                }
                (void)snprintf(feedback->diagnostic.reason,
                    sizeof(feedback->diagnostic.reason), "%s",
                    complete
                        ? trial_observation_reason(
                            &selection->capability,
                            feedback->trial_dimension)
                        : selection->capability.sustained_gpu_batch_feedback
                            ? "gpu-batch-baseline" : "hold-deadband");
            } else {
                (void)snprintf(feedback->diagnostic.reason,
                    sizeof(feedback->diagnostic.reason), "hold-deadband");
            }
        }
    }
    aggregate_sequence(feedback, wall_time_ns, items_completed, actual_backend,
        backend_reason, metrics);
    advance_diagnostic_serial_locked(governor, feedback);
    ++governor->generation;
    (void)pthread_cond_broadcast(&governor->cond);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_record_fallback_items(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    Lardon3DResourceFallbackItemCause cause,
    uint64_t item_count
)
{
    if (!governor || !selection || !task_kind || !task_kind[0]
        || selection->capability.backend
            != LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
        || cause < LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE
        || cause > LARDON3D_RESOURCE_FALLBACK_ITEM_OTHER) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DCapabilityFeedback *feedback = capability_feedback_locked(
        governor, task_kind, task_kind_version,
        selection->capability.backend, true);
    if (!feedback) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    uint64_t *counter = NULL;
    switch (cause) {
    case LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE:
        counter = &feedback->aggregate.local_ineligible_fallback_items;
        break;
    case LARDON3D_RESOURCE_FALLBACK_ITEM_BACKEND_FAILURE:
        counter = &feedback->aggregate.backend_failure_fallback_items;
        break;
    case LARDON3D_RESOURCE_FALLBACK_ITEM_OTHER:
        counter = &feedback->aggregate.backend_other_fallback_items;
        break;
    }
    /* CONTRACT: an item becomes evidence at its own durable publication,
     * even if a later item aborts the sequence. This fixed counter update is
     * deliberately isolated from diagnostics, serials, pressure, trials and
     * throughput observations; it cannot mutate the frozen execution
     * selection or teach the Governor from a failed sequence. */
    aggregate_add(counter, item_count, &feedback->aggregate.saturated);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_record_sequence(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DResourceCapabilitySelection *selection,
    uint64_t wall_time_ns,
    size_t items_completed
)
{
    if (!selection) {
        return false;
    }
    return lardon3d_resource_governor_internal_record_sequence_execution(
        governor,
        task_kind,
        task_kind_version,
        selection,
        wall_time_ns,
        items_completed,
        selection->capability.backend,
        "selected-backend-completed"
    );
}

bool
lardon3d_resource_governor_internal_last_diagnostic(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DResourceSequenceDiagnostic *diagnostic
)
{
    if (!governor || !task_kind || !diagnostic) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DCapabilityFeedback *best = NULL;
    for (size_t index = 0; index < LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
         ++index) {
        Lardon3DCapabilityFeedback *candidate =
            &governor->capability_feedback[index];
        if (candidate->used && candidate->task_kind_version == task_kind_version
            && strcmp(candidate->task_kind, task_kind) == 0
            && candidate->diagnostic.task_kind[0]
            && (!best
                || candidate->diagnostic_serial > best->diagnostic_serial
                || (candidate->diagnostic_serial == best->diagnostic_serial
                    && candidate->diagnostic_update_order
                        > best->diagnostic_update_order))) {
            best = candidate;
        }
    }
    if (best) {
        *diagnostic = best->diagnostic;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return best != NULL;
}

bool
lardon3d_resource_governor_internal_diagnostic_since(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    uint64_t after_serial,
    Lardon3DResourceSequenceDiagnostic *diagnostic
)
{
    return lardon3d_resource_governor_internal_last_diagnostic(
            governor, task_kind, task_kind_version, diagnostic)
        && diagnostic->serial > after_serial;
}

static void
aggregate_merge_counter(uint64_t *target, uint64_t source, bool *saturated)
{
    aggregate_add(target, source, saturated);
}

bool
lardon3d_resource_governor_internal_sequence_aggregate(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DResourceSequenceAggregate *aggregate
)
{
    if (!governor || !task_kind || !task_kind[0] || !aggregate) {
        return false;
    }
    Lardon3DResourceSequenceAggregate result = {0};
    bool found = false;
    (void)pthread_mutex_lock(&governor->mutex);
    for (size_t index = 0; index < LARDON3D_CAPABILITY_FEEDBACK_CAPACITY;
         ++index) {
        const Lardon3DCapabilityFeedback *feedback =
            &governor->capability_feedback[index];
        if (!feedback->used || feedback->task_kind_version != task_kind_version
            || strcmp(feedback->task_kind, task_kind) != 0) {
            continue;
        }
        found = true;
        const Lardon3DResourceSequenceAggregate *source = &feedback->aggregate;
        result.saturated = result.saturated || source->saturated;
#define MERGE_COUNTER(field) \
        aggregate_merge_counter(&result.field, source->field, &result.saturated)
        MERGE_COUNTER(admission_count);
        MERGE_COUNTER(sequence_count);
        MERGE_COUNTER(durable_items);
        MERGE_COUNTER(total_wall_time_ns);
        MERGE_COUNTER(backend_fallback_sequences);
        MERGE_COUNTER(backend_ineligible_fallback_sequences);
        MERGE_COUNTER(backend_failure_fallback_sequences);
        MERGE_COUNTER(backend_other_fallback_sequences);
        MERGE_COUNTER(local_ineligible_fallback_items);
        MERGE_COUNTER(backend_failure_fallback_items);
        MERGE_COUNTER(backend_other_fallback_items);
        MERGE_COUNTER(contract_change_count);
        MERGE_COUNTER(vulkan_submits);
        MERGE_COUNTER(vulkan_completions);
        MERGE_COUNTER(vulkan_submit_cpu_ns);
        MERGE_COUNTER(vulkan_fence_wait_ns);
        MERGE_COUNTER(vulkan_readback_ns);
        MERGE_COUNTER(vulkan_gpu_known_sequences);
        MERGE_COUNTER(vulkan_gpu_ns);
        MERGE_COUNTER(vulkan_starvation_ns);
        MERGE_COUNTER(matcher_cpu_ns);
        MERGE_COUNTER(publication_ns);
#undef MERGE_COUNTER
        for (size_t backend = 0;
             backend <= LARDON3D_RESOURCE_BACKEND_MIXED; ++backend) {
            aggregate_merge_counter(
                &result.selected_backend_admissions[backend],
                source->selected_backend_admissions[backend],
                &result.saturated);
            aggregate_merge_counter(
                &result.actual_backend_sequences[backend],
                source->actual_backend_sequences[backend],
                &result.saturated);
        }
        if (source->memory_available_known
            && (!result.memory_available_known
                || source->minimum_memory_available_bytes
                    < result.minimum_memory_available_bytes)) {
            result.memory_available_known = true;
            result.minimum_memory_available_bytes =
                source->minimum_memory_available_bytes;
        }
        if (source->gpu_busy_known
            && (!result.gpu_busy_known
                || source->maximum_gpu_busy_basis_points
                    > result.maximum_gpu_busy_basis_points)) {
            result.gpu_busy_known = true;
            result.maximum_gpu_busy_basis_points =
                source->maximum_gpu_busy_basis_points;
        }
        if (source->process_rss_known
            && (!result.process_rss_known
                || source->maximum_process_rss_bytes
                    > result.maximum_process_rss_bytes)) {
            result.process_rss_known = true;
            result.maximum_process_rss_bytes =
                source->maximum_process_rss_bytes;
        }
        if (source->process_peak_rss_known
            && (!result.process_peak_rss_known
                || source->maximum_process_peak_rss_bytes
                    > result.maximum_process_peak_rss_bytes)) {
            result.process_peak_rss_known = true;
            result.maximum_process_peak_rss_bytes =
                source->maximum_process_peak_rss_bytes;
        }
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    if (found) *aggregate = result;
    return found;
}

bool
lardon3d_resource_governor_internal_format_diagnostic(
    const Lardon3DResourceSequenceDiagnostic *diagnostic,
    char *text,
    size_t capacity
)
{
    if (!diagnostic || !text || capacity == 0) return false;
    /* Explicit pull formatting is bounded and intentionally not wired to
     * ncurses or an unbounded logger. Unknown telemetry is paired with its
     * known bit so zero never masquerades as an observed idle system. */
    int written = snprintf(text, capacity,
        "serial=%llu kind=%s/%u selected=%d actual=%d fallback=%u "
        "pressure=%d cpu=%u gpu=%u batch=%zu inflight=%zu helpers=%u "
        "memory=%llu gpu_memory=%llu io=%u wall_ns=%llu items=%zu "
        "rate_milli=%llu mem_known=%u mem_available=%llu "
        "mem_psi_some=%u/%u mem_psi_full=%u/%u io_psi_some=%u/%u "
        "io_psi_full=%u/%u swap_delta=%u/%llu/%llu pool_util=%u/%u "
        "gpu_busy=%u/%u rss=%u/%llu peak_rss=%u/%llu "
        "vk_submit=%llu vk_complete=%llu vk_submit_cpu_ns=%llu "
        "vk_fence_ns=%llu vk_readback_ns=%llu vk_gpu_known=%u vk_gpu_ns=%llu "
        "vk_starvation_ns=%llu matcher_cpu_ns=%llu publication_ns=%llu "
        "fallback_items=%llu/%llu/%llu fallback_items_saturated=%u "
        "reason=%s backend_reason=%s",
        (unsigned long long)diagnostic->serial,
        diagnostic->task_kind, diagnostic->task_kind_version,
        (int)diagnostic->backend, (int)diagnostic->actual_backend,
        diagnostic->backend_fallback ? 1U : 0U,
        (int)diagnostic->pressure, diagnostic->cpu_threads,
        diagnostic->gpu_slots, diagnostic->batch_size,
        diagnostic->inflight_limit, diagnostic->helper_limit,
        (unsigned long long)diagnostic->memory_bytes,
        (unsigned long long)diagnostic->gpu_memory_bytes,
        diagnostic->io_slots,
        (unsigned long long)diagnostic->previous_wall_time_ns,
        diagnostic->items_completed,
        (unsigned long long)diagnostic->durable_items_per_second_milli,
        diagnostic->host.memory_available_known ? 1U : 0U,
        (unsigned long long)diagnostic->host.memory_available_bytes,
        diagnostic->host.memory_psi_some_known ? 1U : 0U,
        diagnostic->host.memory_psi_some_basis_points,
        diagnostic->host.memory_psi_full_known ? 1U : 0U,
        diagnostic->host.memory_psi_full_basis_points,
        diagnostic->host.io_psi_some_known ? 1U : 0U,
        diagnostic->host.io_psi_some_basis_points,
        diagnostic->host.io_psi_full_known ? 1U : 0U,
        diagnostic->host.io_psi_full_basis_points,
        diagnostic->host.swap_delta_known ? 1U : 0U,
        (unsigned long long)diagnostic->host.swap_pages_in_delta,
        (unsigned long long)diagnostic->host.swap_pages_out_delta,
        diagnostic->host.compute_pool_utilization_known ? 1U : 0U,
        diagnostic->host.compute_pool_utilization_basis_points,
        diagnostic->host.gpu_busy_known ? 1U : 0U,
        diagnostic->host.gpu_busy_basis_points,
        diagnostic->host.process_rss_known ? 1U : 0U,
        (unsigned long long)diagnostic->host.process_rss_bytes,
        diagnostic->host.process_peak_rss_known ? 1U : 0U,
        (unsigned long long)diagnostic->host.process_peak_rss_bytes,
        (unsigned long long)diagnostic->execution.vulkan_submits,
        (unsigned long long)diagnostic->execution.vulkan_completions,
        (unsigned long long)diagnostic->execution.vulkan_submit_cpu_ns,
        (unsigned long long)diagnostic->execution.vulkan_fence_wait_ns,
        (unsigned long long)diagnostic->execution.vulkan_readback_ns,
        diagnostic->execution.vulkan_gpu_time_known ? 1U : 0U,
        (unsigned long long)diagnostic->execution.vulkan_gpu_ns,
        (unsigned long long)diagnostic->execution.vulkan_starvation_ns,
        (unsigned long long)diagnostic->execution.matcher_cpu_ns,
        (unsigned long long)diagnostic->execution.publication_ns,
        (unsigned long long)
            diagnostic->execution.local_ineligible_fallback_items,
        (unsigned long long)
            diagnostic->execution.backend_failure_fallback_items,
        (unsigned long long)
            diagnostic->execution.backend_other_fallback_items,
        diagnostic->execution.fallback_items_saturated ? 1U : 0U,
        diagnostic->reason, diagnostic->backend_reason);
    return written >= 0 && (size_t)written < capacity;
}

bool
lardon3d_resource_governor_internal_set_backend_available(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceBackend backend,
    bool available
)
{
    if (!governor || backend != LARDON3D_RESOURCE_BACKEND_ORB_VULKAN) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->orb_vulkan_backend_available = available;
    ++governor->generation;
    (void)pthread_cond_broadcast(&governor->cond);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_capability_hardware_safe(
    Lardon3DResourceGovernor *governor,
    const Lardon3DTaskCapability *capability
)
{
    if (!governor || !capability) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool safe = capability_hardware_safe_locked(governor, capability);
    (void)pthread_mutex_unlock(&governor->mutex);
    return safe;
}

bool
lardon3d_resource_governor_internal_cpu_policy(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceCpuPolicyDiagnostic *diagnostic
)
{
    if (!governor || !diagnostic) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    *diagnostic = governor->cpu_policy;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_configure_cpu_topology(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceCpuTopologyInput *input
)
{
    if (!governor || !input) {
        return false;
    }
    uint64_t allowed[LARDON3D_RESOURCE_CPU_MASK_WORDS];
    unsigned int packages[LARDON3D_RESOURCE_CPU_MAX] = {0};
    unsigned int cores[LARDON3D_RESOURCE_CPU_MAX] = {0};
    if (input->affinity_available
        && !cpu_topology_input_masks(input, allowed, packages, cores)) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool accepted = governor->cpu_reserved == 0;
    if (accepted) {
        Lardon3DResourceCpuPolicyDiagnostic policy;
        build_cpu_policy(&governor->profile, &governor->policy, input,
            &policy);
        if (policy.compute_cpu_count == 0) {
            accepted = false;
        } else {
            /* This is ephemeral host policy only: no durable Task estimate or
             * public resource contract is rewritten by topology discovery. */
            governor->cpu_topology = *input;
            governor->cpu_policy = policy;
            ++governor->generation;
            (void)pthread_cond_broadcast(&governor->cond);
        }
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return accepted;
}

void
lardon3d_resource_governor_internal_force_worker_affinity_failure(
    Lardon3DResourceGovernor *governor,
    bool force_failure
)
{
    if (!governor) {
        return;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->internal_force_worker_affinity_failure = force_failure;
    (void)pthread_mutex_unlock(&governor->mutex);
}

#ifdef __linux__
static bool
cpu_set_from_policy(
    const Lardon3DResourceCpuPolicyDiagnostic *policy,
    cpu_set_t *requested
)
{
    CPU_ZERO(requested);
    for (unsigned int cpu = 0; cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
        if (!cpu_mask_test(policy->compute_mask, cpu)) continue;
        if (cpu >= CPU_SETSIZE) return false;
        CPU_SET((size_t)cpu, requested);
    }
    return policy->compute_cpu_count > 0;
}

#endif

bool
lardon3d_resource_governor_internal_apply_worker_affinity(
    Lardon3DResourceGovernor *governor
)
{
    if (!governor) {
        return false;
    }
    Lardon3DResourceCpuPolicyDiagnostic policy;
    bool force_failure;
    (void)pthread_mutex_lock(&governor->mutex);
    policy = governor->cpu_policy;
    force_failure = governor->internal_force_worker_affinity_failure;
    (void)pthread_mutex_unlock(&governor->mutex);

    bool applied = true;
    const char *reason = policy.reason;
    if (policy.affinity_configured) {
        applied = false;
        reason = "worker-affinity-apply-failed";
#ifdef __linux__
        cpu_set_t previous;
        cpu_set_t requested;
        cpu_set_t verified;
        CPU_ZERO(&previous);
        CPU_ZERO(&verified);
        bool representable = cpu_set_from_policy(&policy, &requested);
        bool previous_known = sched_getaffinity(
            0, sizeof(previous), &previous) == 0;
        if (!force_failure && representable && previous_known
            && sched_setaffinity(0, sizeof(requested), &requested) == 0) {
            bool verified_known = sched_getaffinity(
                0, sizeof(verified), &verified) == 0;
            applied = verified_known && CPU_EQUAL(&requested, &verified);
            if (!applied) {
                /* Verification failure may follow a successful mutation.
                 * Restore the worker's prior mask before any Task callback. */
                (void)sched_setaffinity(0, sizeof(previous), &previous);
            }
        }
#else
        (void)force_failure;
#endif
        if (applied) {
            reason = policy.externally_constrained
                ? "worker-affinity-active-external-mask"
                : "worker-affinity-active-compute-mask";
        }
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->cpu_policy.affinity_attempted = true;
    governor->cpu_policy.affinity_active =
        policy.affinity_configured && applied;
    (void)snprintf(governor->cpu_policy.reason,
        sizeof(governor->cpu_policy.reason), "%s", reason);
    (void)pthread_mutex_unlock(&governor->mutex);
    return applied;
}

bool
lardon3d_resource_governor_internal_set_next_reservation_id(
    Lardon3DResourceGovernor *governor,
    uint64_t next_reservation_id
)
{
    if (!governor || next_reservation_id == 0) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->next_reservation_id = next_reservation_id;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_set_diagnostic_serial(
    Lardon3DResourceGovernor *governor,
    uint64_t diagnostic_serial
)
{
    if (!governor) return false;
    (void)pthread_mutex_lock(&governor->mutex);
    governor->capability_diagnostic_serial = diagnostic_serial;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_set_counters(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceGovernorInternalCounters *counters
)
{
    if (!governor || !counters) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->pressure_streak = counters->pressure_streak;
    governor->recovery_streak = counters->recovery_streak;
    governor->slow_start_streak = counters->slow_start_streak;
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_get_counters(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceGovernorInternalCounters *counters
)
{
    if (!governor || !counters) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    *counters = (Lardon3DResourceGovernorInternalCounters) {
        .pressure_streak = governor->pressure_streak,
        .recovery_streak = governor->recovery_streak,
        .slow_start_streak = governor->slow_start_streak,
    };
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_internal_set_monotonic_now(
    Lardon3DResourceGovernor *governor,
    const struct timespec *now
)
{
    if (!governor || (now && (now->tv_sec < 0 || now->tv_nsec < 0
            || now->tv_nsec >= 1000000000L))) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->internal_now_known = now != NULL;
    if (now) {
        governor->internal_now = *now;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

void
lardon3d_resource_governor_internal_force_capture_failure(
    Lardon3DResourceGovernor *governor,
    bool force_failure
)
{
    if (!governor) {
        return;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    governor->internal_force_capture_failure = force_failure;
    (void)pthread_mutex_unlock(&governor->mutex);
}

#if defined(LARDON3D_RESOURCE_GOVERNOR_CAPTURE_TESTING)
bool
lardon3d_resource_governor_internal_set_capture_snapshot(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot
)
{
    if (!governor) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (snapshot && !valid_snapshot(&governor->profile, snapshot)) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    governor->internal_capture_snapshot_override = snapshot != NULL;
    governor->internal_capture_snapshot = snapshot
        ? *snapshot : (Lardon3DResourceSnapshot) {0};
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}
#endif

const char *
lardon3d_resource_decision_name(Lardon3DResourceDecisionKind kind)
{
    switch (kind) {
    case LARDON3D_RESOURCE_START:
        return "Démarrer";
    case LARDON3D_RESOURCE_WAIT:
        return "Attendre";
    case LARDON3D_RESOURCE_REDUCE_BATCH:
        return "Réduire le lot";
    case LARDON3D_RESOURCE_REJECT:
        return "Refuser";
    default:
        return "Inconnue";
    }
}

Lardon3DResourcePressure
lardon3d_resource_governor_pressure(Lardon3DResourceGovernor *governor)
{
    if (!governor) {
        return LARDON3D_RESOURCE_PRESSURE_RED;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourcePressure pressure = governor->pressure;
    (void)pthread_mutex_unlock(&governor->mutex);
    return pressure;
}
