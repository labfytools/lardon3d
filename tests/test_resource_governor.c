#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <limits.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/resource_governor.h>

#include "../src/resource_governor_internal.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

#define GIBIBYTES(value) ((uint64_t)(value) * 1024 * 1024 * 1024)
#define MEBIBYTES(value) ((uint64_t)(value) * 1024 * 1024)

static bool
use_fixed_test_clock(Lardon3DResourceGovernor *governor)
{
    const struct timespec now = {0};
    return lardon3d_resource_governor_internal_set_monotonic_now(governor, &now);
}

static bool
cpu_mask_has(const uint64_t mask[LARDON3D_RESOURCE_CPU_MASK_WORDS],
             unsigned int cpu)
{
    return cpu < LARDON3D_RESOURCE_CPU_MAX
        && (mask[cpu / 64] & (UINT64_C(1) << (cpu % 64))) != 0;
}

static bool
topology_file_result(
    const char *text,
    size_t length,
    bool expected_result,
    unsigned int expected_value
)
{
    char path[] = "/tmp/lardon3d-topology-token-XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor < 0) {
        return false;
    }
    size_t offset = 0;
    bool written = true;
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        if (count <= 0) {
            written = false;
            break;
        }
        offset += (size_t)count;
    }
    bool closed = close(descriptor) == 0;
    unsigned int value = UINT_MAX;
    bool result = written && closed
        && lardon3d_resource_governor_internal_read_topology_value_file(
            path, &value);
    bool removed = unlink(path) == 0;
    return removed && result == expected_result
        && (!expected_result || value == expected_value);
}

static bool
run_topology_value_reader_test(void)
{
    char maximum[32];
    int maximum_length = snprintf(maximum, sizeof(maximum), "%u\n", UINT_MAX);
    char truncated[33];
    truncated[0] = '1';
    memset(truncated + 1, ' ', sizeof(truncated) - 1);
    /* The first 32 bytes are independently valid. The 33rd whitespace byte
     * proves the reader checks EOF instead of accepting a truncated prefix. */
    CHECK(maximum_length > 0 && (size_t)maximum_length < sizeof(maximum));
    CHECK(topology_file_result("0\n", 2, true, 0));
    CHECK(topology_file_result(maximum, (size_t)maximum_length, true, UINT_MAX));
    CHECK(topology_file_result("12 \t\r\n", 6, true, 12));
    CHECK(topology_file_result("", 0, false, 0));
    CHECK(topology_file_result("-1\n", 3, false, 0));
    CHECK(topology_file_result("+1\n", 3, false, 0));
    CHECK(topology_file_result(" 1\n", 3, false, 0));
    CHECK(topology_file_result("1x\n", 3, false, 0));
    CHECK(topology_file_result("1\n2\n", 4, false, 0));
    CHECK(topology_file_result(truncated, sizeof(truncated), false, 0));
    return true;
}

static bool
write_text_file(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600);
    if (descriptor < 0) return false;
    size_t length = strlen(text);
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(descriptor, text + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            (void)close(descriptor);
            return false;
        }
        offset += (size_t)written;
    }
    return close(descriptor) == 0;
}

static bool
run_gpu_busy_identity_reader_test(void)
{
    char root[] = "/tmp/lardon3d-drm-telemetry-XXXXXX";
    CHECK(mkdtemp(root));
    char card0[PATH_MAX];
    char card37[PATH_MAX];
    char device0[PATH_MAX];
    char device37[PATH_MAX];
    char busy0[PATH_MAX];
    char busy37[PATH_MAX];
    CHECK(snprintf(card0, sizeof(card0), "%s/card0", root) > 0
        && snprintf(card37, sizeof(card37), "%s/card37", root) > 0
        && snprintf(device0, sizeof(device0), "%s/device", card0) > 0
        && snprintf(device37, sizeof(device37), "%s/device", card37) > 0
        && snprintf(busy0, sizeof(busy0), "%s/gpu_busy_percent", device0) > 0
        && snprintf(busy37, sizeof(busy37), "%s/gpu_busy_percent", device37) > 0
        && mkdir(card0, 0700) == 0 && mkdir(card37, 0700) == 0
        && mkdir(device0, 0700) == 0 && mkdir(device37, 0700) == 0
        && write_text_file(busy0, "99\n")
        && write_text_file(busy37, "42\n"));
    uint32_t basis_points = 0;
    CHECK(lardon3d_resource_governor_internal_read_gpu_busy_at_root(
            root, 37, &basis_points)
        && basis_points == 4200);
    /* A malformed retained card must never fall through to readable card0. */
    CHECK(write_text_file(busy37, "42 percent\n")
        && !lardon3d_resource_governor_internal_read_gpu_busy_at_root(
            root, 37, &basis_points)
        && !lardon3d_resource_governor_internal_read_gpu_busy_at_root(
            root, 38, &basis_points)
        && !lardon3d_resource_governor_internal_read_gpu_busy_at_root(
            root, 64, &basis_points));
    CHECK(unlink(busy0) == 0 && unlink(busy37) == 0
        && rmdir(device0) == 0 && rmdir(device37) == 0
        && rmdir(card0) == 0 && rmdir(card37) == 0 && rmdir(root) == 0);
    return true;
}

typedef struct {
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceSnapshot snapshot;
    Lardon3DResourceRequest request;
    atomic_bool *failed;
} ThreadContext;

typedef struct {
    Lardon3DResourceGovernor *governor;
    uint64_t observed_generation;
    uint64_t timeout_ns;
    atomic_bool *result;
} WaitContext;

static void *
wait_for_change_thread(void *argument)
{
    WaitContext *context = argument;
    bool changed = lardon3d_resource_governor_wait_for_change(
        context->governor,
        context->observed_generation,
        context->timeout_ns
    );
    atomic_store(context->result, changed);
    return NULL;
}

static void *
decide_repeatedly(void *argument)
{
    ThreadContext *context = argument;
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        Lardon3DResourceDecision decision;
        if (!lardon3d_resource_governor_decide(
                context->governor,
                &context->snapshot,
                &context->request,
                &decision
            )
            || decision.kind != LARDON3D_RESOURCE_START
            || decision.batch_size != context->request.preferred_batch_size) {
            (void)fprintf(
                stderr,
                "Décision concurrente : kind=%d batch=%zu cpu=%u\n",
                (int)decision.kind,
                decision.batch_size,
                decision.cpu_threads
            );
            atomic_store(context->failed, true);
            break;
        }
    }
    return NULL;
}

static bool
run_portable_default_policy_test(void)
{
    static const struct {
        unsigned int logical;
        unsigned int reserved;
        unsigned int compute;
    } cases[] = {
        {1, 0, 1},
        {2, 1, 1},
        {3, 2, 1},
        {4, 3, 1},
        {8, 4, 4},
        {16, 4, 12},
        {32, 4, 28},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        Lardon3DHardwareProfile profile = {
            .logical_cpu_count = cases[index].logical,
            .page_size_bytes = 4096,
            .memory_total_bytes = GIBIBYTES(16),
            .cpu_architecture = "synthetic",
        };
        Lardon3DResourcePolicy policy;
        CHECK(lardon3d_resource_policy_default(&profile, &policy)
            && policy.system_cpu_reserve == cases[index].reserved);
        Lardon3DResourceGovernor *governor =
            lardon3d_resource_governor_create(&profile, &policy);
        Lardon3DResourcePolicy observed_policy = {0};
        CHECK(governor
            && lardon3d_resource_governor_get_policy(
                governor, &observed_policy)
            && observed_policy.system_cpu_reserve
                == policy.system_cpu_reserve
            && observed_policy.system_memory_reserve_bytes
                == policy.system_memory_reserve_bytes
            && observed_policy.emergency_memory_floor_bytes
                == policy.emergency_memory_floor_bytes);
        Lardon3DResourceCpuTopologyInput unavailable = {0};
        Lardon3DResourceCpuPolicyDiagnostic diagnostic;
        /* Force the deterministic count-only seam so this matrix never
         * inherits the machine running the test suite. */
        CHECK(governor
            && lardon3d_resource_governor_internal_configure_cpu_topology(
                governor, &unavailable)
            && lardon3d_resource_governor_internal_cpu_policy(
                governor, &diagnostic)
            && !diagnostic.affinity_configured
            && diagnostic.compute_cpu_count == cases[index].compute
            && diagnostic.reserved_cpu_count == cases[index].reserved);
        lardon3d_resource_governor_destroy(governor);
    }
    return true;
}

static bool
run_default_memory_band_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 8,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "synthetic",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy)
        && policy.system_memory_reserve_bytes == GIBIBYTES(3)
        && policy.emergency_memory_floor_bytes == GIBIBYTES(3));
    policy.maximum_cpu_load_ratio = 1.0;
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    Lardon3DResourceCpuTopologyInput count_only = {0};
    CHECK(governor
        && lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &count_only)
        && use_fixed_test_clock(governor));
    Lardon3DResourceEstimate estimate = {
        .memory_fixed_bytes = GIBIBYTES(1),
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(4) + MEBIBYTES(512),
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;
    /* This 1 GiB admission proves capacity subtracts 3 GiB, not the 4 GiB
     * caution threshold: 4.5 - 3 fits, while 4.5 - 4 would not. */
    CHECK(lardon3d_resource_governor_reserve(governor, &snapshot, &estimate,
            &decision, &reservation)
        && reservation && decision.kind == LARDON3D_RESOURCE_START
        && lardon3d_resource_governor_pressure(governor)
            == LARDON3D_RESOURCE_PRESSURE_GREEN
        && lardon3d_resource_governor_release(governor, reservation));

    estimate.memory_fixed_bytes = 0;
    estimate.memory_bytes_per_item = MEBIBYTES(256);
    estimate.maximum_batch_size = 4;
    snapshot.memory_available_bytes = GIBIBYTES(3) + MEBIBYTES(512);
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_reserve(governor, &snapshot,
                &estimate, &decision, &reservation)
            && reservation && decision.kind == LARDON3D_RESOURCE_START
            && decision.batch_size == 1
            && lardon3d_resource_governor_pressure(governor)
                == LARDON3D_RESOURCE_PRESSURE_YELLOW
            && lardon3d_resource_governor_release(governor, reservation));
    }
    /* A stable default caution band remains YELLOW, while the 3 GiB admission
     * floor independently rejects work that would consume the host reserve. */
    estimate.memory_fixed_bytes = MEBIBYTES(600);
    estimate.memory_bytes_per_item = 0;
    estimate.maximum_batch_size = 1;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(governor, &snapshot, &estimate,
            &decision, &reservation)
        && !reservation && decision.kind == LARDON3D_RESOURCE_WAIT
        && lardon3d_resource_governor_pressure(governor)
            == LARDON3D_RESOURCE_PRESSURE_YELLOW);

    estimate.memory_fixed_bytes = MEBIBYTES(256);
    snapshot.memory_available_bytes = GIBIBYTES(5);
    for (unsigned int observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_reserve(governor, &snapshot,
                &estimate, &decision, &reservation)
            && reservation
            && lardon3d_resource_governor_release(governor, reservation));
    }
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_GREEN);
    snapshot.memory_available_bytes = GIBIBYTES(3);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(governor, &snapshot, &estimate,
            &decision, &reservation)
        && !reservation && decision.kind == LARDON3D_RESOURCE_WAIT
        && lardon3d_resource_governor_pressure(governor)
            == LARDON3D_RESOURCE_PRESSURE_RED);
    lardon3d_resource_governor_destroy(governor);

    profile.memory_total_bytes = GIBIBYTES(2);
    CHECK(lardon3d_resource_policy_default(&profile, &policy)
        && policy.system_memory_reserve_bytes == MEBIBYTES(512)
        && policy.emergency_memory_floor_bytes == MEBIBYTES(512));
    policy.maximum_cpu_load_ratio = 1.0;
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    estimate.memory_fixed_bytes = MEBIBYTES(64);
    snapshot.memory_available_bytes = MEBIBYTES(600);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(governor, &snapshot, &estimate,
            &decision, &reservation)
        && reservation
        && lardon3d_resource_governor_pressure(governor)
            == LARDON3D_RESOURCE_PRESSURE_YELLOW
        && lardon3d_resource_governor_release(governor, reservation));
    snapshot.memory_available_bytes = MEBIBYTES(512);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(governor, &snapshot, &estimate,
            &decision, &reservation)
        && !reservation && decision.kind == LARDON3D_RESOURCE_WAIT
        && lardon3d_resource_governor_pressure(governor)
            == LARDON3D_RESOURCE_PRESSURE_RED);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    CHECK(policy.system_memory_reserve_bytes == GIBIBYTES(3));
    CHECK(policy.emergency_memory_floor_bytes == GIBIBYTES(3));
    CHECK(policy.system_cpu_reserve == 4);
    CHECK(policy.maximum_cpu_pressure_avg10 == 20.0);
    CHECK(policy.maximum_memory_pressure_avg10 == 1.0);
    policy = (Lardon3DResourcePolicy) {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_cpu_pressure_avg10 = 20.0,
        .maximum_memory_pressure_avg10 = 1.0,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 8,
    };
    CHECK(!lardon3d_resource_governor_create(NULL, &policy));
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor && use_fixed_test_clock(governor));
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(10),
        .cpu_load_1m = 2.0,
    };
    Lardon3DResourceRequest request = {
        .memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 16,
    };
    Lardon3DResourceDecision decision;
    CHECK(lardon3d_resource_governor_decide(
        governor,
        &snapshot,
        &request,
        &decision
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 8);
    Lardon3DResourceCpuPolicyDiagnostic live_cpu_policy;
    CHECK(lardon3d_resource_governor_internal_cpu_policy(
        governor, &live_cpu_policy));
    CHECK(decision.cpu_threads == live_cpu_policy.compute_cpu_count);
    CHECK(decision.reason[0]);

    snapshot.swap_activity_known = true;
    snapshot.swap_pages_in = 100;
    snapshot.swap_pages_out = 200;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_GREEN);
    snapshot.swap_pages_out = 201;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 4);
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_YELLOW);
    snapshot.swap_pages_out = 202;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_RED);
    for (unsigned int observation = 0; observation < 2; ++observation) {
        CHECK(lardon3d_resource_governor_decide(
            governor, &snapshot, &request, &decision));
        CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
        CHECK(lardon3d_resource_governor_pressure(governor) ==
              LARDON3D_RESOURCE_PRESSURE_RED);
    }
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_YELLOW);
    request.minimum_batch_size = 1;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 1);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.batch_size == 1);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_GREEN);
    CHECK(decision.batch_size == 1);
    for (size_t expected = 2; expected <= 8; expected *= 2) {
        for (unsigned int observation = 0; observation < 3; ++observation) {
            CHECK(lardon3d_resource_governor_decide(
                governor, &snapshot, &request, &decision));
        }
        CHECK(decision.batch_size == expected);
    }
    request.minimum_batch_size = 2;
    snapshot.swap_activity_known = false;
    Lardon3DResourceSnapshot invalid_snapshot = snapshot;
    invalid_snapshot.cpu_load_1m = -1.0;
    CHECK(!lardon3d_resource_governor_decide(
        governor,
        &invalid_snapshot,
        &request,
        &decision
    ));

    snapshot.memory_available_bytes = GIBIBYTES(6);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 4);
    /* Even a fully available reported UMA payload aperture cannot bypass the
     * host MemAvailable target/floor; it is observation, not separate RAM. */
    snapshot.gpu_memory_available_known = true;
    snapshot.gpu_memory_available_bytes = MEBIBYTES(512);
    snapshot.memory_available_bytes = GIBIBYTES(3);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);

    snapshot.memory_available_bytes = GIBIBYTES(10);
    snapshot.cpu_load_1m = 15.0;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    snapshot.cpu_load_1m = 2.0;
    snapshot.cpu_pressure_known = true;
    snapshot.cpu_pressure_avg10 = 21.0;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    snapshot.cpu_pressure_avg10 = 0.0;
    snapshot.memory_pressure_known = true;
    snapshot.memory_pressure_avg10 = 1.5;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    snapshot.memory_pressure_avg10 = 0.0;
    snapshot.io_pressure_known = true;
    snapshot.io_pressure_avg10 = 90.0;
    request.io_intensive = true;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);

    lardon3d_resource_governor_destroy(governor);
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    snapshot.cpu_pressure_known = false;
    snapshot.memory_pressure_known = false;
    snapshot.io_pressure_known = false;
    request.io_intensive = false;
    request.memory_bytes_per_item = GIBIBYTES(8);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    request.memory_bytes_per_item = GIBIBYTES(1);
    request.gpu_memory_bytes_per_item = MEBIBYTES(512);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    lardon3d_resource_governor_destroy(governor);

    policy.emergency_memory_floor_bytes = GIBIBYTES(1);
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    request = (Lardon3DResourceRequest) {
        .minimum_batch_size = 1,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 1,
    };
    snapshot = (Lardon3DResourceSnapshot) {
        .memory_available_bytes = GIBIBYTES(2),
        .cpu_load_1m = 0.0,
        .swap_activity_known = true,
        .swap_pages_in = 10,
        .swap_pages_out = 10,
    };
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_YELLOW);
    snapshot.memory_available_bytes = GIBIBYTES(1);
    ++snapshot.swap_pages_out;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_RED);
    lardon3d_resource_governor_destroy(governor);

    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    snapshot.memory_available_bytes = GIBIBYTES(10);
    snapshot.swap_activity_known = true;
    snapshot.swap_pages_in = 50;
    snapshot.swap_pages_out = 75;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_GREEN);
    ++snapshot.swap_pages_in;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_YELLOW);
    for (unsigned int observation = 0; observation < 3; ++observation) {
        CHECK(lardon3d_resource_governor_decide(
            governor, &snapshot, &request, &decision));
    }
    CHECK(lardon3d_resource_governor_pressure(governor) ==
          LARDON3D_RESOURCE_PRESSURE_GREEN);
    CHECK(decision.batch_size == 1);
    lardon3d_resource_governor_destroy(governor);

    profile.gpu_available = true;
    profile.gpu_uses_shared_memory = true;
    policy.gpu_memory_reserve_bytes = 0;
    policy.gpu_slot_capacity = 2;
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    request = (Lardon3DResourceRequest) {
        .memory_bytes_per_item = GIBIBYTES(1),
        .gpu_memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 1,
        .preferred_batch_size = 4,
        .requested_cpu_threads = 4,
    };
    snapshot.memory_available_bytes = GIBIBYTES(6);
    snapshot.cpu_load_1m = 2.0;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 2);
    lardon3d_resource_governor_destroy(governor);

    profile.gpu_available = true;
    profile.gpu_uses_shared_memory = false;
    profile.gpu_memory_known = true;
    profile.gpu_memory_total_bytes = GIBIBYTES(4);
    policy.gpu_memory_reserve_bytes = MEBIBYTES(512);
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    request.minimum_batch_size = 1;
    request.preferred_batch_size = 3;
    request.gpu_memory_bytes_per_item = GIBIBYTES(1);
    snapshot.gpu_memory_available_known = false;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    snapshot.gpu_memory_available_known = true;
    snapshot.gpu_memory_available_bytes = GIBIBYTES(2) + MEBIBYTES(512);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 2);

    request.gpu_memory_bytes_per_item = 0;
    request.preferred_batch_size = 2;
    request.memory_bytes_per_item = GIBIBYTES(1);
    atomic_bool failed = false;
    ThreadContext context = {
        .governor = governor,
        .snapshot = snapshot,
        .request = request,
        .failed = &failed,
    };
    pthread_t threads[8];
    for (size_t index = 0; index < 8; ++index) {
        CHECK(pthread_create(&threads[index], NULL, decide_repeatedly, &context) == 0);
    }
    for (size_t index = 0; index < 8; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    CHECK(!atomic_load(&failed));

    Lardon3DResourcePolicy invalid = policy;
    invalid.system_cpu_reserve = profile.logical_cpu_count;
    CHECK(!lardon3d_resource_governor_set_policy(governor, &invalid));
    CHECK(lardon3d_resource_governor_set_policy(governor, &policy));
    CHECK(strcmp(lardon3d_resource_decision_name(LARDON3D_RESOURCE_START), "Démarrer") == 0);
    lardon3d_resource_governor_destroy(governor);
    lardon3d_resource_governor_destroy(NULL);
    return true;
}

static bool
run_generation_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 8,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor && use_fixed_test_clock(governor));

    /* Test 1 : génération initiale = 0. */
    CHECK(lardon3d_resource_governor_generation(governor) == 0);

    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(10),
        .cpu_load_1m = 2.0,
    };
    Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 2,
        .maximum_batch_size = 8,
        .desired_cpu_threads = 4,
        .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;

    /* Test 2 : WAIT ne change pas la génération. */
    snapshot.cpu_load_1m = 15.0;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(reservation == NULL);
    CHECK(lardon3d_resource_governor_generation(governor) == 0);
    snapshot.cpu_load_1m = 2.0;

    /* Test 3 : REJECT ne change pas la génération. */
    estimate.memory_bytes_per_item = GIBIBYTES(8);
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    CHECK(reservation == NULL);
    CHECK(lardon3d_resource_governor_generation(governor) == 0);
    estimate.memory_bytes_per_item = GIBIBYTES(1);

    /* Test 4 : création d'une réservation change la génération (0 -> 1). */
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_START
        || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(reservation != NULL);
    CHECK(lardon3d_resource_governor_generation(governor) == 1);

    /* Test 5 : libération change la génération (1 -> 2). */
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_generation(governor) == 2);

    /* Test 6 : double libération refusée ne change pas la génération. */
    CHECK(!lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_generation(governor) == 2);

    /* Test 7 : un thread attendant est réveillé après une libération. */
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(reservation != NULL);
    uint64_t generation = lardon3d_resource_governor_generation(governor);
    atomic_bool wait_result = false;
    WaitContext wait_context = {
        .governor = governor,
        .observed_generation = generation,
        .timeout_ns = 5000000000ULL,
        .result = &wait_result,
    };
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_change_thread, &wait_context) == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(pthread_join(waiter, NULL) == 0);
    CHECK(atomic_load(&wait_result) == true);

    /* Test 8 : l'attente expire proprement (timeout court). */
    generation = lardon3d_resource_governor_generation(governor);
    CHECK(!lardon3d_resource_governor_wait_for_change(
        governor,
        generation,
        1000000ULL
    ));

    /* Test 9 : une décision WAIT ne modifie pas la génération et
     * wait_for_change() expire sans faux changement. */
    generation = lardon3d_resource_governor_generation(governor);
    snapshot.cpu_load_1m = 15.0;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(reservation == NULL);
    snapshot.cpu_load_1m = 2.0;
    CHECK(lardon3d_resource_governor_generation(governor) == generation);
    CHECK(!lardon3d_resource_governor_wait_for_change(
        governor,
        generation,
        1000000ULL
    ));

    /* Test 10 : plusieurs attentes successives fonctionnent. */
    for (size_t index = 0; index < 3; ++index) {
        CHECK(lardon3d_resource_governor_reserve(
            governor,
            &snapshot,
            &estimate,
            &decision,
            &reservation
        ));
        CHECK(reservation != NULL);
        generation = lardon3d_resource_governor_generation(governor);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_wait_for_change(
            governor,
            generation,
            5000000000ULL
        ));
    }

    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_adaptive_batch_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 8,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor && use_fixed_test_clock(governor));

    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(10),
        .cpu_load_1m = 2.0,
    };
    Lardon3DResourceDecision decision;

    /* Test 1: Without metrics, batch is unchanged */
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Une mesure mémoire à zéro est inconnue : elle ne doit pas réduire un
     * lot, même si sa taille et sa durée sont enregistrées. */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_IO,
        8,
        1000000000ULL,
        0
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot,
        &(Lardon3DResourceRequest) {
            .memory_bytes_per_item = MEBIBYTES(100),
            .minimum_batch_size = 2,
            .preferred_batch_size = 8,
            .requested_cpu_threads = 4,
            .io_intensive = true,
        }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 2: Record batch matching estimate → no reduction */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        8,
        1000000000ULL,
        MEBIBYTES(800)
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 3: Record batch with higher memory usage → batch reduced */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        8,
        1000000000ULL,
        MEBIBYTES(1600)
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 4);

    /* Test 4: Buffer overflow (9 writes) → oldest overwritten */
    for (size_t i = 0; i < 9; ++i) {
        CHECK(lardon3d_resource_governor_record_batch(
            governor,
            LARDON3D_RESOURCE_TASK_GENERAL,
            4,
            1000000000ULL,
            MEBIBYTES(400)
        ));
    }
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 5: Different task class not affected */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_IO,
        8,
        1000000000ULL,
        MEBIBYTES(1600)
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 6: record_batch increments generation */
    uint64_t gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        4,
        1000000000ULL,
        MEBIBYTES(400)
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before + 1);

    /* Test 7: NULL governor returns false */
    CHECK(!lardon3d_resource_governor_record_batch(NULL, LARDON3D_RESOURCE_TASK_GENERAL, 4, 0, 0));

    /* Test 8: Zero batch_size is ignored */
    CHECK(lardon3d_resource_governor_record_batch(governor, LARDON3D_RESOURCE_TASK_GENERAL, 0, 0, 0));

    /* T1: Invalid task class → false, generation unchanged */
    gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(!lardon3d_resource_governor_record_batch(
        governor,
        (Lardon3DResourceTaskClass)999,
        4,
        1000000000ULL,
        MEBIBYTES(400)
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before);

    /* T2: batch_size == 0 → true, generation unchanged */
    gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        0,
        0,
        0
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before);

    /* T3: minimum_batch_size guaranteed */
    for (size_t i = 0; i < 8; ++i) {
        CHECK(lardon3d_resource_governor_record_batch(
            governor,
            LARDON3D_RESOURCE_TASK_GENERAL,
            8,
            1000000000ULL,
            GIBIBYTES(8)
        ));
    }
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 4,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 4);

    /* T4: Overflow of static_batch * memory_bytes_per_item in uint64_t.
     * With memory_bytes_per_item = 2 and preferred_batch_size = SIZE_MAX,
     * the multiplication SIZE_MAX * 2 would overflow uint64_t. The overflow
     * guard in adaptive_batch_limit returns 1 (most conservative). The result
     * is never SIZE_MAX, the adaptive limit never increases the batch, and
     * the final result is clamped to the minimum expected value. */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        1,
        1000000000ULL,
        100
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = 2,
        .minimum_batch_size = 1,
        .preferred_batch_size = SIZE_MAX,
        .requested_cpu_threads = 1,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size != SIZE_MAX);
    CHECK(decision.batch_size >= 1);
    CHECK(decision.batch_size <= 1);

    /* T5: generation increment only on real record */
    gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_IO,
        4,
        1000000000ULL,
        MEBIBYTES(400)
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before + 1);

    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_gate_g_boundary_test(void)
{
    CHECK(LARDON3D_RESOURCE_SNAPSHOT_MAX_AGE_MILLISECONDS == 1000);
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 8,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(8),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .emergency_memory_floor_bytes = GIBIBYTES(1),
        .system_cpu_reserve = 2,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_cpu_pressure_avg10 = 20.0,
        .maximum_memory_pressure_avg10 = 1.0,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    const struct timespec now = {.tv_sec = 10, .tv_nsec = 500};
    CHECK(lardon3d_resource_governor_internal_set_monotonic_now(governor, &now));
    Lardon3DResourceSnapshot snapshot = {
        .captured_at = {.tv_sec = 9, .tv_nsec = 500},
        .memory_available_bytes = GIBIBYTES(8),
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate estimate = {
        .memory_fixed_bytes = MEBIBYTES(1),
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(
        governor, &snapshot, &estimate, &decision, &reservation));
    CHECK(reservation);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    Lardon3DResourceGovernorInternalCounters counters = {
        .pressure_streak = 1,
        .recovery_streak = 2,
        .slow_start_streak = 2,
    };
    CHECK(lardon3d_resource_governor_internal_set_counters(governor, &counters));
    snapshot.captured_at = (struct timespec) {.tv_sec = 9, .tv_nsec = 499};
    snapshot.memory_pressure_known = true;
    snapshot.memory_pressure_avg10 = 100.0;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_reserve(
        governor, &snapshot, &estimate, &decision, &reservation));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(!reservation);
    Lardon3DResourceGovernorInternalCounters after;
    CHECK(lardon3d_resource_governor_internal_get_counters(governor, &after));
    CHECK(after.pressure_streak == 1);
    CHECK(after.recovery_streak == 2);
    CHECK(after.slow_start_streak == 2);

    const struct timespec future_snapshots[] = {
        {.tv_sec = 10, .tv_nsec = 501},
        {.tv_sec = 11, .tv_nsec = 0},
    };
    for (size_t i = 0; i < sizeof(future_snapshots) / sizeof(future_snapshots[0]);
         ++i) {
        snapshot.captured_at = future_snapshots[i];
        reservation = NULL;
        CHECK(lardon3d_resource_governor_reserve(
            governor, &snapshot, &estimate, &decision, &reservation));
        CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
        CHECK(!reservation);
        CHECK(lardon3d_resource_governor_internal_get_counters(governor, &after));
        CHECK(after.pressure_streak == 1);
        CHECK(after.recovery_streak == 2);
        CHECK(after.slow_start_streak == 2);
    }

    snapshot.captured_at = now;
    CHECK(lardon3d_resource_governor_internal_set_next_reservation_id(
        governor,
        UINT64_MAX
    ));
    snapshot.memory_pressure_known = false;
    CHECK(lardon3d_resource_governor_reserve(
        governor, &snapshot, &estimate, &decision, &reservation));
    CHECK(reservation);
    Lardon3DResourceReservationInfo information;
    CHECK(lardon3d_resource_reservation_get_active(
        governor, reservation, &information));
    CHECK(information.id == UINT64_MAX);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    Lardon3DResourceAvailability availability_before;
    Lardon3DResourceAvailability availability_after;
    CHECK(lardon3d_resource_governor_availability(
        governor,
        &snapshot,
        &availability_before
    ));
    reservation = NULL;
    CHECK(!lardon3d_resource_governor_reserve(
        governor, &snapshot, &estimate, &decision, &reservation));
    CHECK(!reservation);
    CHECK(decision.kind != LARDON3D_RESOURCE_START);
    CHECK(decision.kind != LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    CHECK(lardon3d_resource_governor_availability(
        governor,
        &snapshot,
        &availability_after
    ));
    CHECK(availability_after.memory_reserved_bytes
        == availability_before.memory_reserved_bytes);
    CHECK(availability_after.gpu_memory_reserved_bytes
        == availability_before.gpu_memory_reserved_bytes);
    CHECK(availability_after.cpu_reserved == availability_before.cpu_reserved);
    CHECK(availability_after.gpu_slots_reserved
        == availability_before.gpu_slots_reserved);
    CHECK(availability_after.io_slots_reserved
        == availability_before.io_slots_reserved);

    counters = (Lardon3DResourceGovernorInternalCounters) {
        .pressure_streak = UINT_MAX,
    };
    CHECK(lardon3d_resource_governor_internal_set_counters(governor, &counters));
    snapshot.memory_pressure_known = true;
    snapshot.memory_pressure_avg10 = 100.0;
    CHECK(lardon3d_resource_governor_decide(
        governor,
        &snapshot,
        &(Lardon3DResourceRequest) {
            .minimum_batch_size = 1,
            .preferred_batch_size = 1,
            .requested_cpu_threads = 1,
        },
        &decision
    ));
    CHECK(lardon3d_resource_governor_internal_get_counters(governor, &after));
    CHECK(after.pressure_streak == 2);
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_RED);

    counters = (Lardon3DResourceGovernorInternalCounters) {
        .recovery_streak = UINT_MAX,
    };
    CHECK(lardon3d_resource_governor_internal_set_counters(governor, &counters));
    snapshot.memory_pressure_known = false;
    CHECK(lardon3d_resource_governor_decide(
        governor,
        &snapshot,
        &(Lardon3DResourceRequest) {
            .minimum_batch_size = 1,
            .preferred_batch_size = 1,
            .requested_cpu_threads = 1,
        },
        &decision
    ));
    CHECK(lardon3d_resource_governor_internal_get_counters(governor, &after));
    CHECK(after.recovery_streak == 0);
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_YELLOW);

    counters = (Lardon3DResourceGovernorInternalCounters) {
        .recovery_streak = UINT_MAX,
    };
    CHECK(lardon3d_resource_governor_internal_set_counters(governor, &counters));
    CHECK(lardon3d_resource_governor_decide(
        governor,
        &snapshot,
        &(Lardon3DResourceRequest) {
            .minimum_batch_size = 1,
            .preferred_batch_size = 1,
            .requested_cpu_threads = 1,
        },
        &decision
    ));
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_GREEN);
    counters = (Lardon3DResourceGovernorInternalCounters) {
        .slow_start_streak = UINT_MAX,
    };
    CHECK(lardon3d_resource_governor_internal_set_counters(governor, &counters));
    CHECK(lardon3d_resource_governor_decide(
        governor,
        &snapshot,
        &(Lardon3DResourceRequest) {
            .minimum_batch_size = 1,
            .preferred_batch_size = 1,
            .requested_cpu_threads = 1,
        },
        &decision
    ));
    CHECK(lardon3d_resource_governor_internal_get_counters(governor, &after));
    CHECK(after.slow_start_streak == 0);

    lardon3d_resource_governor_destroy(governor);
    return true;
}

static Lardon3DTaskCapabilityEnvelope
adaptive_orb_envelope(void)
{
    Lardon3DResourceEstimate cpu = {
        .memory_bytes_per_item = MEBIBYTES(10),
        .minimum_batch_size = 1,
        .maximum_batch_size = 12,
        .desired_cpu_threads = 12,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    Lardon3DResourceEstimate gpu = cpu;
    gpu.gpu_memory_fixed_bytes = 640 * 1024;
    gpu.desired_cpu_threads = 1;
    gpu.desired_gpu_slots = 1;
    return (Lardon3DTaskCapabilityEnvelope) {
        .count = 2,
        .capabilities = {
            {
                .estimate = gpu,
                .backend = LARDON3D_RESOURCE_BACKEND_ORB_VULKAN,
                .inflight_limit = 1,
                .preferred = true,
                .batch_adaptive = true,
                .requires_runtime_backend = true,
            },
            {
                .estimate = cpu,
                .backend = LARDON3D_RESOURCE_BACKEND_CPU,
                .inflight_limit = 1,
                .helper_limit = 0,
                .cpu_reducible = true,
                .batch_adaptive = true,
            },
        },
    };
}

static bool
run_cpu_topology_policy_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_cpu_reserve = 4,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));

    Lardon3DResourceCpuTopologyInput topology = {
        .affinity_available = true,
        .topology_available = true,
        .allowed_cpu_count = 16,
        .topology_entry_count = 16,
    };
    for (unsigned int cpu = 0; cpu < 16; ++cpu) {
        topology.allowed_cpu_ids[cpu] = cpu;
        topology.topology_entries[cpu] =
            (Lardon3DResourceCpuTopologyEntry) {
                .cpu_id = cpu,
                .package_id = 0,
                .core_id = cpu % 8,
            };
    }
    CHECK(lardon3d_resource_governor_internal_configure_cpu_topology(
        governor, &topology));
    Lardon3DResourceCpuPolicyDiagnostic diagnostic;
    CHECK(lardon3d_resource_governor_internal_cpu_policy(
        governor, &diagnostic));
    CHECK(diagnostic.affinity_configured && !diagnostic.affinity_active
        && !diagnostic.externally_constrained
        && diagnostic.compute_cpu_count == 12
        && diagnostic.reserved_cpu_count == 4);
    for (unsigned int cpu = 0; cpu < 16; ++cpu) {
        bool reserved = cpu == 6 || cpu == 7 || cpu == 14 || cpu == 15;
        CHECK(cpu_mask_has(diagnostic.allowed_mask, cpu));
        CHECK(cpu_mask_has(diagnostic.reserved_mask, cpu) == reserved);
        CHECK(cpu_mask_has(diagnostic.compute_mask, cpu) == !reserved);
    }

    /* Admission consumes the derived compute count, not logical total minus
     * a second, unrelated reduction. */
    Lardon3DTaskCapabilityEnvelope envelope = {
        .count = 1,
        .capabilities = {{
            .estimate = {
                .minimum_batch_size = 1,
                .maximum_batch_size = 1,
                .desired_cpu_threads = 16,
                .task_class = LARDON3D_RESOURCE_TASK_CPU,
            },
            .backend = LARDON3D_RESOURCE_BACKEND_CPU,
            .inflight_limit = 1,
            .cpu_reducible = true,
        }},
    };
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(12),
    };
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.topology", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 1);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    /* A caller already constrained to the desired twelve CPUs supplies the
     * complete compute mask. Governor must not reserve four more. */
    topology = (Lardon3DResourceCpuTopologyInput) {
        .affinity_available = true,
        .topology_available = false,
        .allowed_cpu_count = 12,
    };
    size_t allowed = 0;
    for (unsigned int cpu = 0; cpu < 16; ++cpu) {
        if (cpu < 6 || (cpu >= 8 && cpu < 14)) {
            topology.allowed_cpu_ids[allowed++] = cpu;
        }
    }
    CHECK(allowed == 12
        && lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &topology)
        && lardon3d_resource_governor_internal_cpu_policy(
            governor, &diagnostic));
    CHECK(diagnostic.affinity_configured
        && diagnostic.externally_constrained
        && diagnostic.compute_cpu_count == 12
        && diagnostic.reserved_cpu_count == 0);
    for (unsigned int cpu = 0; cpu < 16; ++cpu) {
        bool compute = cpu < 6 || (cpu >= 8 && cpu < 14);
        CHECK(cpu_mask_has(diagnostic.compute_mask, cpu) == compute);
        CHECK(!cpu_mask_has(diagnostic.reserved_mask, cpu));
    }

    /* With a full allowed mask but no topology, portable count budgeting
     * remains available and no arbitrary CPU/sibling mask is fabricated. */
    topology = (Lardon3DResourceCpuTopologyInput) {
        .affinity_available = true,
        .topology_available = false,
        .allowed_cpu_count = 16,
    };
    for (unsigned int cpu = 0; cpu < 16; ++cpu) {
        topology.allowed_cpu_ids[cpu] = cpu;
    }
    CHECK(lardon3d_resource_governor_internal_configure_cpu_topology(
        governor, &topology)
        && lardon3d_resource_governor_internal_cpu_policy(
            governor, &diagnostic));
    CHECK(!diagnostic.affinity_configured && !diagnostic.affinity_active
        && diagnostic.compute_cpu_count == 12
        && diagnostic.reserved_cpu_count == 4
        && strcmp(diagnostic.reason,
            "fallback-portable-topology-unavailable") == 0);
    for (size_t word = 0; word < LARDON3D_RESOURCE_CPU_MASK_WORDS; ++word) {
        CHECK(diagnostic.compute_mask[word] == 0
            && diagnostic.reserved_mask[word] == 0);
    }

    /* Two CPUs excluded by an external mask already satisfy half of the host
     * reserve. Without topology, only the remaining two are removed by count;
     * no unverifiable sibling mask is fabricated. */
    topology = (Lardon3DResourceCpuTopologyInput) {
        .affinity_available = true,
        .topology_available = false,
        .allowed_cpu_count = 14,
    };
    for (unsigned int cpu = 0; cpu < 14; ++cpu) {
        topology.allowed_cpu_ids[cpu] = cpu;
    }
    CHECK(lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &topology)
        && lardon3d_resource_governor_internal_cpu_policy(
            governor, &diagnostic));
    CHECK(!diagnostic.affinity_configured
        && diagnostic.externally_constrained
        && diagnostic.compute_cpu_count == 12
        && diagnostic.reserved_cpu_count == 2
        && strcmp(diagnostic.reason,
            "fallback-portable-topology-unavailable") == 0);
    lardon3d_resource_governor_destroy(governor);

    /* Asymmetric complete-core groups of 3,3,2 cannot meet logical reserve 4
     * exactly. The minimum safe whole-core total is 5, not the greedy 6; the
     * highest physical identities win the deterministic equal-size tie. */
    profile.logical_cpu_count = 8;
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    topology = (Lardon3DResourceCpuTopologyInput) {
        .affinity_available = true,
        .topology_available = true,
        .allowed_cpu_count = 8,
        .topology_entry_count = 8,
    };
    const unsigned int asymmetric_core[] = {0, 0, 0, 1, 1, 1, 2, 2};
    for (unsigned int cpu = 0; cpu < 8; ++cpu) {
        topology.allowed_cpu_ids[cpu] = cpu;
        topology.topology_entries[cpu] =
            (Lardon3DResourceCpuTopologyEntry) {
                .cpu_id = cpu,
                .package_id = 0,
                .core_id = asymmetric_core[cpu],
            };
    }
    CHECK(lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &topology)
        && lardon3d_resource_governor_internal_cpu_policy(
            governor, &diagnostic)
        && diagnostic.affinity_configured
        && diagnostic.compute_cpu_count == 3
        && diagnostic.reserved_cpu_count == 5);
    for (unsigned int cpu = 0; cpu < 8; ++cpu) {
        bool reserved = cpu >= 3;
        CHECK(cpu_mask_has(diagnostic.reserved_mask, cpu) == reserved
            && cpu_mask_has(diagnostic.compute_mask, cpu) == !reserved);
    }
    lardon3d_resource_governor_destroy(governor);

    /* Read-only current-host assertion: when the caller really sees the
     * unrestricted 0..15 topology, the production discovery must reproduce
     * the validated compute/reserved masks. Other hosts skip this exact IDs. */
    profile.logical_cpu_count = 16;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && lardon3d_resource_governor_internal_cpu_policy(
        governor, &diagnostic));
    bool exact_host_mask = diagnostic.affinity_configured;
    for (unsigned int cpu = 0; cpu < 16 && exact_host_mask; ++cpu) {
        exact_host_mask = cpu_mask_has(diagnostic.allowed_mask, cpu);
    }
    if (exact_host_mask) {
        CHECK(diagnostic.compute_cpu_count == 12
            && diagnostic.reserved_cpu_count == 4);
        for (unsigned int cpu = 0; cpu < 16; ++cpu) {
            bool reserved = cpu == 6 || cpu == 7 || cpu == 14 || cpu == 15;
            CHECK(cpu_mask_has(diagnostic.reserved_mask, cpu) == reserved);
            CHECK(cpu_mask_has(diagnostic.compute_mask, cpu) == !reserved);
        }
    }
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_driver_runtime_policy_test(void)
{
    CHECK(unsetenv("MESA_SHADER_CACHE_DISABLE") == 0
        && lardon3d_resource_governor_internal_configure_driver_policy()
            == LARDON3D_RESOURCE_DRIVER_POLICY_DEFAULTED
        && getenv("MESA_SHADER_CACHE_DISABLE")
        && strcmp(getenv("MESA_SHADER_CACHE_DISABLE"), "true") == 0);

    CHECK(lardon3d_resource_governor_internal_configure_driver_policy()
            == LARDON3D_RESOURCE_DRIVER_POLICY_INHERITED_SAFE);

    /* An explicit request for Mesa disk-cache workers is not silently
     * overwritten. Startup rejects it before threads or Vulkan can exist. */
    CHECK(setenv("MESA_SHADER_CACHE_DISABLE", "false", 1) == 0
        && lardon3d_resource_governor_internal_configure_driver_policy()
            == LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE
        && strcmp(getenv("MESA_SHADER_CACHE_DISABLE"), "false") == 0);
    CHECK(setenv("MESA_SHADER_CACHE_DISABLE", "unsafe", 1) == 0
        && lardon3d_resource_governor_internal_configure_driver_policy()
            == LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE
        && strcmp(getenv("MESA_SHADER_CACHE_DISABLE"), "unsafe") == 0);

    /* Leave the test process in the production-safe inherited state before
     * any later test creates a Queue or another pthread. */
    CHECK(setenv("MESA_SHADER_CACHE_DISABLE", "1", 1) == 0
        && lardon3d_resource_governor_internal_configure_driver_policy()
            == LARDON3D_RESOURCE_DRIVER_POLICY_INHERITED_SAFE);
    return true;
}

static bool
run_capability_governor_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .gpu_available = true,
        .gpu_memory_known = true,
        .gpu_uses_shared_memory = true,
        .gpu_memory_total_bytes = MEBIBYTES(512),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    policy.maximum_cpu_load_ratio = 1.0;
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    CHECK(lardon3d_resource_governor_internal_set_backend_available(
        governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true));
    Lardon3DResourceSnapshot snapshot = {
        .captured_at = {0},
        .memory_available_bytes = GIBIBYTES(12),
        .swap_activity_known = true,
    };
    Lardon3DTaskCapabilityEnvelope envelope = adaptive_orb_envelope();
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;

    /* AUTO starts at the validated minimum and prefers the usable GPU. Two
     * completed healthy sequences, rather than one throughput sample, ramp
     * the next immutable sequence to batch two. */
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.capability.backend ==
        LARDON3D_RESOURCE_BACKEND_ORB_VULKAN);
    CHECK(selection.decision.batch_size == 1 &&
        strcmp(selection.reason, "healthy-slow-start") == 0);
    Lardon3DResourceAvailability uma_charge;
    CHECK(lardon3d_resource_governor_availability(
        governor, &snapshot, &uma_charge));
    CHECK(uma_charge.memory_reserved_bytes == MEBIBYTES(10) + 640 * 1024 &&
        uma_charge.gpu_memory_reserved_bytes == 640 * 1024);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.run", 1, &selection, 1000000000ULL, 1));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 1);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.run", 1, &selection, 1000000000ULL, 1));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    Lardon3DResourceSequenceDiagnostic diagnostic;
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.run", 1, &diagnostic));
    CHECK(diagnostic.backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        diagnostic.actual_backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        diagnostic.helper_limit == 0 &&
        diagnostic.batch_size == 2 && diagnostic.gpu_slots == 1 &&
        diagnostic.gpu_memory_bytes == 640 * 1024);

    /* The higher-resource batch-two trial needs two independently material
     * durable-throughput gains before the next sequence may grow again. */
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.run", 1, &selection, 1000000000ULL, 2));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.run", 1, &diagnostic) &&
        strcmp(diagnostic.reason, "throughput-trial") == 0);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.run", 1, &selection, 1000000000ULL, 2));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.run", 1, &diagnostic) &&
        strcmp(diagnostic.reason, "throughput-improved") == 0);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 4 &&
        strcmp(selection.reason, "throughput-trial") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    /* Establish batch four as the accepted capability before pressure. The
     * next operational trial is eight, so admitting one here would expose a
     * stale pre-pressure adaptive maximum. */
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.run", 1, &selection, 1000000000ULL, 4));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 4);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.run", 1, &selection, 1000000000ULL, 4));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.run", 1, &diagnostic) &&
        diagnostic.batch_size == 4 &&
        strcmp(diagnostic.reason, "throughput-improved") == 0);

    /* Equal throughput at batch two is not a gain: the Governor returns to
     * the last accepted batch and stops growth despite a free GPU slot. */
    for (size_t sample = 0; sample < 2; ++sample) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.no_gain", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.no_gain", 1, &selection, 1000000000ULL, 1));
    }
    for (size_t sample = 0; sample < 2; ++sample) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.no_gain", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 2);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.no_gain", 1, &selection, 2000000000ULL, 2));
    }
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.no_gain", 1, &diagnostic) &&
        strcmp(diagnostic.reason, "throughput-no-gain") == 0);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.no_gain", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 1 &&
        strcmp(selection.reason, "throughput-no-gain") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    /* One high outlier leaves the trial unchanged; a second non-improving
     * observation decides conservatively without oscillating 1->2->1 early. */
    for (size_t sample = 0; sample < 2; ++sample) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.noisy", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.noisy", 1, &selection, 1000000000ULL, 1));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.noisy", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.noisy", 1, &selection, 100000000ULL, 2));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.noisy", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.noisy", 1, &selection, 4000000000ULL, 2));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.noisy", 1, &diagnostic) &&
        strcmp(diagnostic.reason, "throughput-no-gain") == 0);

    /* Complete CPU fallback is scientifically valid, but it cannot train a
     * pure Vulkan trial. The next GPU admission stays at the accepted batch
     * while two fresh pure observations rebuild its baseline. */
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.fallback_hold", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.decision.batch_size == 1);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence_execution(
        governor, "matcher.fallback_hold", 1, &selection, 1000000000ULL, 1,
        LARDON3D_RESOURCE_BACKEND_CPU, "whole-pair-cpu-fallback"));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.fallback_hold", 1, &diagnostic)
        && strcmp(diagnostic.reason, "backend-fallback-hold") == 0);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.fallback_hold", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.capability.backend
            == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
        && selection.decision.batch_size == 1);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    Lardon3DResourceExecutionMetrics starvation_metrics = {
        .vulkan_submits = 1,
        .vulkan_completions = 1,
        .vulkan_starvation_ns = 1000000,
    };
    Lardon3DResourceTelemetryRaw unknown_gpu_raw = {0};
    Lardon3DResourceHostTelemetry unknown_gpu_telemetry;
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &unknown_gpu_raw, &unknown_gpu_telemetry)
        && !unknown_gpu_telemetry.gpu_busy_known);
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.starvation", 1, &envelope,
            &selection, &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence_execution_metrics(
            governor, "matcher.starvation", 1, &selection, 1000000000ULL, 1,
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, "vulkan-completed",
            &starvation_metrics));
    }
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.starvation", 1, &envelope,
            &selection, &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 2
            && selection.capability.inflight_limit == 1
            && selection.capability.helper_limit == 0
            && strcmp(selection.reason,
                "gpu-starvation-throughput-trial") == 0);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence_execution_metrics(
            governor, "matcher.starvation", 1, &selection, 1000000000ULL, 2,
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, "vulkan-completed",
            &starvation_metrics));
    }
    Lardon3DResourceTelemetryRaw busy_raw = {
        .gpu_busy_percent = "95\n",
    };
    Lardon3DResourceHostTelemetry busy_telemetry;
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &busy_raw, &busy_telemetry));
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.starvation", 1, &envelope,
            &selection, &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 4);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence_execution_metrics(
            governor, "matcher.starvation", 1, &selection, 2000000000ULL, 4,
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, "vulkan-completed",
            &starvation_metrics));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.starvation", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.decision.batch_size == 2
        && strcmp(selection.reason, "throughput-no-gain") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.starvation", 1, &diagnostic)
        && diagnostic.host.gpu_busy_known
        && diagnostic.host.gpu_busy_basis_points == 9500
        && diagnostic.execution.vulkan_starvation_ns == 1000000);

    /* The first swap-I/O-only signal changes GREEN to YELLOW inside admission.
     * That same immutable sequence must discard the accepted batch-four
     * history, reserve only batch one, and report the pressure reason. */
    snapshot.swap_pages_out = 1;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.pressure ==
        LARDON3D_RESOURCE_PRESSURE_YELLOW &&
        selection.decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH &&
        selection.decision.batch_size == 1 &&
        strcmp(selection.reason, "pressure-decrease") == 0);
    CHECK(lardon3d_resource_governor_availability(
        governor, &snapshot, &uma_charge));
    CHECK(uma_charge.memory_reserved_bytes == MEBIBYTES(10) + 640 * 1024 &&
        uma_charge.gpu_memory_reserved_bytes == 640 * 1024);
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.run", 1, &diagnostic) &&
        diagnostic.batch_size == 1 &&
        diagnostic.pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW &&
        strcmp(diagnostic.reason, "pressure-decrease") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    /* Three healthy observations clear YELLOW. Two fresh batch-one samples
     * then reopen only batch two. Keeping that limit after the host slow-start
     * reaches four proves the pre-pressure accepted batch-four trial reset. */
    for (size_t observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.run", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
    }
    CHECK(lardon3d_resource_governor_pressure(governor) ==
        LARDON3D_RESOURCE_PRESSURE_GREEN);
    for (size_t sample = 0; sample < 2; ++sample) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.run", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.run", 1, &selection, 1000000000ULL, 1));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2 &&
        strcmp(selection.reason, "throughput-trial") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    for (size_t observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.run", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 2);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
    }

    /* A single PSI observation immediately throttles admission but does not
     * become RED until sustained. Active swap deltas use the same path. */
    snapshot.memory_pressure_known = true;
    snapshot.memory_pressure_avg10 = 100.0;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(!reservation && selection.decision.kind == LARDON3D_RESOURCE_WAIT &&
        selection.pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW);
    snapshot.memory_pressure_known = false;
    snapshot.swap_pages_out = 2;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(!reservation && selection.decision.kind == LARDON3D_RESOURCE_WAIT &&
        selection.pressure == LARDON3D_RESOURCE_PRESSURE_RED);

    /* RED and YELLOW each require three healthy observations. This bounded
     * recovery hysteresis prevents one alternating sample from oscillating. */
    snapshot.swap_pages_out = 2;
    for (size_t observation = 0; observation < 6; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.run", 1, &envelope, &selection,
            &reservation));
        if (reservation)
            CHECK(lardon3d_resource_governor_release(governor, reservation));
    }
    CHECK(lardon3d_resource_governor_pressure(governor) ==
        LARDON3D_RESOURCE_PRESSURE_GREEN);
    /* Pressure reset discarded matcher.run's batch-four trial. After the
     * bounded recovery streak, two new baseline observations reopen only a
     * controlled batch-two trial. */
    for (size_t sample = 0; sample < 2; ++sample) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.run", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.run", 1, &selection, 1000000000ULL, 1));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2 &&
        strcmp(selection.reason, "throughput-trial") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    snapshot.memory_available_bytes = GIBIBYTES(3);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(!reservation && selection.pressure ==
        LARDON3D_RESOURCE_PRESSURE_RED);
    snapshot.memory_available_bytes = GIBIBYTES(2);
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.run", 1, &envelope, &selection,
        &reservation));
    CHECK(!reservation && selection.pressure ==
        LARDON3D_RESOURCE_PRESSURE_RED);
    lardon3d_resource_governor_destroy(governor);

    /* CPU and batch are independent dimensions: CPU can reduce to two while
     * the callback-supported batch remains twelve. */
    profile.gpu_available = false;
    profile.gpu_memory_known = false;
    profile.gpu_uses_shared_memory = false;
    profile.gpu_memory_total_bytes = 0;
    policy = (Lardon3DResourcePolicy) {
        .system_cpu_reserve = 14,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    snapshot.memory_available_bytes = GIBIBYTES(12);
    Lardon3DTaskCapabilityEnvelope cpu_only = {
        .count = 1,
        .capabilities = {{
            .estimate = envelope.capabilities[1].estimate,
            .backend = LARDON3D_RESOURCE_BACKEND_FIXED,
            .inflight_limit = 1,
            .cpu_reducible = true,
        }},
    };
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "candidate_pairs.generate", 1, &cpu_only,
        &selection, &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 1 &&
        selection.decision.batch_size == 12 &&
        strcmp(selection.reason, "healthy-slow-start") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_inflight_feedback_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .gpu_available = true,
        .gpu_memory_known = true,
        .gpu_uses_shared_memory = true,
        .gpu_memory_total_bytes = MEBIBYTES(512),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    policy.maximum_cpu_load_ratio = 1.0;
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    CHECK(lardon3d_resource_governor_internal_set_backend_available(
        governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true));
    Lardon3DTaskCapabilityEnvelope envelope = {
        .count = 1,
        .capabilities = {{
            .estimate = {
                .memory_bytes_per_item = MEBIBYTES(10),
                .minimum_batch_size = 2,
                .maximum_batch_size = 2,
                .desired_cpu_threads = 1,
                .desired_gpu_slots = 1,
                .desired_io_slots = 1,
                .task_class = LARDON3D_RESOURCE_TASK_MIXED,
            },
            .backend = LARDON3D_RESOURCE_BACKEND_ORB_VULKAN,
            .inflight_limit = 2,
            .minimum_inflight_limit = 1,
            .gpu_memory_bytes_per_inflight = 640 * 1024,
            .preferred = true,
            .inflight_adaptive = true,
            .requires_runtime_backend = true,
        }},
    };
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(12),
        .swap_activity_known = true,
    };
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;

    /* Two healthy baseline observations are required before depth two, and
     * the immutable reservation charges one 640 KiB mapped payload per slot
     * exactly once against shared host memory. */
    for (unsigned int observation = 0; observation < 2; ++observation) {
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.inflight_limit == 1);
        Lardon3DResourceAvailability availability;
        CHECK(lardon3d_resource_governor_availability(
            governor, &snapshot, &availability));
        CHECK(availability.memory_reserved_bytes == MEBIBYTES(20) + 640 * 1024
            && availability.gpu_memory_reserved_bytes == 640 * 1024);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.depth", 1, &selection, 2000000000ULL, 2));
    }
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.inflight_limit == 2
        && strcmp(selection.reason, "inflight-throughput-trial") == 0);
    Lardon3DResourceAvailability availability;
    CHECK(lardon3d_resource_governor_availability(
        governor, &snapshot, &availability));
    CHECK(availability.memory_reserved_bytes == MEBIBYTES(20) + 1280 * 1024
        && availability.gpu_memory_reserved_bytes == 1280 * 1024);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.depth", 1, &selection, 1600000000ULL, 2));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.inflight_limit == 2);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "matcher.depth", 1, &selection, 1600000000ULL, 2));
    Lardon3DResourceSequenceDiagnostic diagnostic;
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.depth", 1, &diagnostic)
        && diagnostic.inflight_limit == 2
        && strcmp(diagnostic.reason, "inflight-throughput-improved") == 0);

    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.depth.no_gain", 1, &envelope,
            &selection, &reservation));
        CHECK(reservation);
        CHECK(selection.inflight_limit == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.depth.no_gain", 1, &selection,
            2000000000ULL, 2));
    }
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.depth.no_gain", 1, &envelope,
            &selection, &reservation));
        CHECK(reservation && selection.inflight_limit == 2);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.depth.no_gain", 1, &selection,
            2000000000ULL, 2));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.depth.no_gain", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.inflight_limit == 1
        && strcmp(selection.reason, "inflight-throughput-no-gain") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    /* The first active swap-I/O delta clamps the very same admission to depth
     * one and its exact reservation, then discards the accepted trial history. */
    snapshot.swap_pages_out = 1;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW
        && selection.decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH
        && selection.inflight_limit == 1
        && strcmp(selection.reason, "pressure-decrease") == 0);
    CHECK(lardon3d_resource_governor_availability(
        governor, &snapshot, &availability));
    CHECK(availability.memory_reserved_bytes == MEBIBYTES(20) + 640 * 1024
        && availability.gpu_memory_reserved_bytes == 640 * 1024);
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.depth", 1, &diagnostic)
        && diagnostic.inflight_limit == 1
        && strcmp(diagnostic.reason, "pressure-decrease") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    /* Recovery requires the existing healthy pressure streak and then a fresh
     * two-observation baseline before depth two can reopen. */
    for (unsigned int observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
            &reservation));
        if (!reservation) {
            CHECK(selection.decision.kind == LARDON3D_RESOURCE_WAIT);
        } else {
            CHECK(selection.inflight_limit == 1);
            CHECK(lardon3d_resource_governor_release(governor, reservation));
        }
    }
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_GREEN);

    /* Gate G's generic slow-start limit also rebuilds after pressure. This
     * fixture has a fixed batch minimum of two so its first two healthy
     * attempts remain WAIT; the third reopens that batch at depth one. */
    for (unsigned int observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
            &reservation));
        if (observation < 2) {
            CHECK(!reservation
                && selection.decision.kind == LARDON3D_RESOURCE_WAIT);
        } else {
            CHECK(reservation && selection.inflight_limit == 1);
            CHECK(lardon3d_resource_governor_release(governor, reservation));
        }
    }

    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.inflight_limit == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "matcher.depth", 1, &selection, 2000000000ULL, 2));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.depth", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.inflight_limit == 2
        && strcmp(selection.reason, "inflight-throughput-trial") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_private_telemetry_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    policy.maximum_cpu_load_ratio = 1.0;
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    Lardon3DResourceCpuTopologyInput topology = {
        .affinity_available = true,
        .allowed_cpu_count = 2,
        .allowed_cpu_ids = {0, 2},
    };
    CHECK(lardon3d_resource_governor_internal_configure_cpu_topology(
        governor, &topology));

    const char *stat_first =
        "cpu  0 0 0 0 0 0 0 0\n"
        "cpu0 100 0 100 800 0 0 0 0\n"
        "cpu1 9 0 9 9 0 0 0 0\n"
        "cpu2 200 0 100 700 0 0 0 0\n";
    const char *stat_second =
        "cpu  0 0 0 0 0 0 0 0\n"
        "cpu0 130 0 130 840 0 0 0 0\n"
        "cpu1 9 0 9 9 0 0 0 0\n"
        "cpu2 220 0 120 760 0 0 0 0\n";
    Lardon3DResourceTelemetryRaw raw = {
        .proc_stat = stat_first,
        .meminfo = "MemTotal: 16777216 kB\nMemAvailable: 4194304 kB\n",
        .memory_psi =
            "some avg10=1.25 avg60=0.20 avg300=0.10 total=9\n"
            "full avg10=0.50 avg60=0.10 avg300=0.01 total=2\n",
        .io_psi =
            "some avg10=2.75 avg60=0.20 avg300=0.10 total=9\n"
            "full avg10=0.25 avg60=0.10 avg300=0.01 total=2\n",
        .vmstat = "pgpgin 1\npswpin 10\npswpout 20\n",
        .process_status = "Name:\ttest\nVmRSS:\t1024 kB\nVmHWM:\t2048 kB\n",
        .gpu_busy_percent = "42\n",
    };
    Lardon3DResourceHostTelemetry telemetry;
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry));
    CHECK(!telemetry.compute_pool_utilization_known
        && !telemetry.swap_delta_known
        && telemetry.memory_available_known
        && telemetry.memory_available_bytes == GIBIBYTES(4)
        && telemetry.memory_psi_some_known
        && telemetry.memory_psi_some_basis_points == 125
        && telemetry.memory_psi_full_basis_points == 50
        && telemetry.io_psi_some_basis_points == 275
        && telemetry.io_psi_full_basis_points == 25
        && telemetry.gpu_busy_known
        && telemetry.gpu_busy_basis_points == 4200
        && telemetry.process_rss_bytes == MEBIBYTES(1)
        && telemetry.process_peak_rss_bytes == MEBIBYTES(2));

    raw.proc_stat = stat_second;
    raw.vmstat = "pswpin 13\npswpout 27\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry));
    CHECK(telemetry.compute_pool_utilization_known
        && telemetry.compute_pool_utilization_basis_points == 5000
        && telemetry.swap_delta_known
        && telemetry.swap_pages_in_delta == 3
        && telemetry.swap_pages_out_delta == 7);

    /* Zero deltas, counter regression/wrap, malformed selected CPU fields,
     * signs, and trailing GPU text all degrade to unknown without reusing a
     * stale low-pressure value. */
    raw.vmstat = "pswpin 1\npswpout 2\n";
    raw.gpu_busy_percent = "-1\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry));
    CHECK(!telemetry.compute_pool_utilization_known
        && !telemetry.swap_delta_known && !telemetry.gpu_busy_known);
    raw.proc_stat = "cpu0 1 2\ncpu2 1 2 3 4\n";
    raw.gpu_busy_percent = "42 percent\n";
    raw.memory_psi =
        "some avg10=-1.00 avg60=0.00 avg300=0.00 total=1\n"
        "full avg10=0.10tail avg60=0.00 avg300=0.00 total=1\n";
    raw.process_status = NULL;
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry));
    CHECK(!telemetry.compute_pool_utilization_known
        && !telemetry.gpu_busy_known
        && !telemetry.memory_psi_some_known
        && !telemetry.memory_psi_full_known
        && !telemetry.process_rss_known);

    /* PSI tokens are exact fields, not substring searches or rounded input. */
    raw = (Lardon3DResourceTelemetryRaw) {
        .memory_psi =
            "some xavg10=1.25 avg60=0.00 total=1\n"
            "full avg10=0.10 avg60=0.00 total=1\n",
    };
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.memory_psi_some_known
        && telemetry.memory_psi_full_known);
    raw.memory_psi =
        "some avg10=1.234 avg60=0.00 total=1\n"
        "full avg10=0.10 avg10=0.20 total=1\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.memory_psi_some_known
        && !telemetry.memory_psi_full_known);
    raw.memory_psi =
        "some avg10=+1.00 avg60=0.00 total=1\n"
        "full avg10=.10 avg60=0.00 total=1\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.memory_psi_some_known
        && !telemetry.memory_psi_full_known);

    /* Every selected CPU must occur exactly once. A duplicate cpu0 cannot
     * substitute for the missing cpu2, and per-CPU regression cannot be
     * hidden by growth on its sibling. */
    raw = (Lardon3DResourceTelemetryRaw) {
        .proc_stat = stat_first,
    };
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.compute_pool_utilization_known);
    raw.proc_stat =
        "cpu0 130 0 130 840 0 0 0 0\n"
        "cpu0 140 0 140 850 0 0 0 0\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.compute_pool_utilization_known);
    raw.proc_stat = stat_first;
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry));
    raw.proc_stat =
        "cpu0 99 0 100 800 0 0 0 0\n"
        "cpu2 400 0 200 900 0 0 0 0\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.compute_pool_utilization_known);
    raw.proc_stat =
        "cpu0 18446744073709551615 1 0 0\n"
        "cpu2 1 0 0 0\n";
    CHECK(lardon3d_resource_governor_internal_sample_telemetry_raw(
        governor, &raw, &telemetry)
        && !telemetry.compute_pool_utilization_known);

    Lardon3DTaskCapabilityEnvelope envelope = {
        .count = 1,
        .capabilities = {{
            .estimate = {
                .minimum_batch_size = 1,
                .maximum_batch_size = 1,
                .desired_cpu_threads = 1,
            },
            .backend = LARDON3D_RESOURCE_BACKEND_FIXED,
            .inflight_limit = 1,
        }},
    };
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(8),
    };
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.telemetry", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && lardon3d_resource_governor_release(
        governor, reservation));
    Lardon3DResourceExecutionMetrics metrics = {
        .vulkan_submits = 2,
        .vulkan_completions = 2,
        .vulkan_submit_cpu_ns = 10,
        .vulkan_fence_wait_ns = 11,
        .vulkan_gpu_ns = 12,
        .vulkan_starvation_ns = 13,
        .matcher_cpu_ns = 14,
        .publication_ns = 15,
    };
    CHECK(lardon3d_resource_governor_internal_record_sequence_execution_metrics(
        governor, "test.telemetry", 1, &selection, 1000, 1,
        LARDON3D_RESOURCE_BACKEND_FIXED, "fixed-completed", &metrics));
    Lardon3DResourceSequenceDiagnostic diagnostic;
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "test.telemetry", 1, &diagnostic)
        && diagnostic.serial > 0
        && diagnostic.execution.vulkan_submits == 2
        && diagnostic.execution.publication_ns == 15
        && diagnostic.execution.local_ineligible_fallback_items == 0
        && diagnostic.execution.backend_failure_fallback_items == 0
        && diagnostic.execution.backend_other_fallback_items == 0
        && !lardon3d_resource_governor_internal_diagnostic_since(
            governor, "test.telemetry", 1, diagnostic.serial, &diagnostic));
    char formatted[4096];
    CHECK(lardon3d_resource_governor_internal_format_diagnostic(
        &diagnostic, formatted, sizeof(formatted))
        && strstr(formatted, "vk_submit=2")
        && strstr(formatted, "fallback_items=0/0/0")
        && strstr(formatted, "fallback_items_saturated=0")
        && strstr(formatted, "reason=fixed-envelope")
        && !lardon3d_resource_governor_internal_format_diagnostic(
            &diagnostic, formatted, 8));

    /* Serial saturation intentionally stops since-cursor notification, but
     * last_diagnostic must still select the most recently updated backend. */
    Lardon3DResourceCapabilitySelection fixed_selection = selection;
    CHECK(lardon3d_resource_governor_internal_set_diagnostic_serial(
        governor, UINT64_MAX - 1)
        && lardon3d_resource_governor_internal_record_sequence(
            governor, "test.telemetry", 1, &fixed_selection, 1000, 1));
    Lardon3DTaskCapabilityEnvelope cpu_envelope = envelope;
    cpu_envelope.capabilities[0].backend = LARDON3D_RESOURCE_BACKEND_CPU;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.telemetry", 1, &cpu_envelope, &selection,
        &reservation));
    CHECK(reservation && lardon3d_resource_governor_release(
        governor, reservation)
        && lardon3d_resource_governor_internal_last_diagnostic(
            governor, "test.telemetry", 1, &diagnostic)
        && diagnostic.serial == UINT64_MAX
        && diagnostic.backend == LARDON3D_RESOURCE_BACKEND_CPU);
    CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "test.telemetry", 1, &fixed_selection, 1000, 1)
        && lardon3d_resource_governor_internal_last_diagnostic(
            governor, "test.telemetry", 1, &diagnostic)
        && diagnostic.serial == UINT64_MAX
        && diagnostic.backend == LARDON3D_RESOURCE_BACKEND_FIXED);
    /* Polling may coalesce serial changes, but the fixed-size aggregate must
     * retain every admission/completed sequence and must not double count the
     * CPU admission that has not yet produced durable work. */
    Lardon3DResourceSequenceAggregate aggregate;
    CHECK(lardon3d_resource_governor_internal_sequence_aggregate(
            governor, "test.telemetry", 1, &aggregate)
        && !aggregate.saturated
        && aggregate.admission_count == 2
        && aggregate.sequence_count == 3
        && aggregate.durable_items == 3
        && aggregate.total_wall_time_ns == 3000
        && aggregate.contract_change_count == 1
        && aggregate.selected_backend_admissions[
               LARDON3D_RESOURCE_BACKEND_FIXED] == 1
        && aggregate.selected_backend_admissions[
               LARDON3D_RESOURCE_BACKEND_CPU] == 1
        && aggregate.actual_backend_sequences[
               LARDON3D_RESOURCE_BACKEND_FIXED] == 3
        && aggregate.vulkan_submits == 2
        && aggregate.vulkan_completions == 2
        && aggregate.vulkan_submit_cpu_ns == 10
        && aggregate.vulkan_fence_wait_ns == 11
        && aggregate.vulkan_gpu_known_sequences == 0
        && aggregate.vulkan_gpu_ns == 0
        && aggregate.vulkan_starvation_ns == 13
        && aggregate.matcher_cpu_ns == 14
        && aggregate.publication_ns == 15
        && aggregate.local_ineligible_fallback_items == 0
        && aggregate.backend_failure_fallback_items == 0
        && aggregate.backend_other_fallback_items == 0);

    /* Private telemetry has a bounded lifetime, but its fixed counters still
     * fail closed under injected saturation: they never wrap to a plausible
     * small comparator value. */
    Lardon3DResourceCapabilitySelection saturation_selection = selection;
    saturation_selection.capability.backend =
        LARDON3D_RESOURCE_BACKEND_ORB_VULKAN;
    CHECK(lardon3d_resource_governor_internal_record_fallback_items(
        governor, "test.fallback-saturation", 1, &saturation_selection,
        LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE, UINT64_MAX));
    CHECK(lardon3d_resource_governor_internal_record_fallback_items(
        governor, "test.fallback-saturation", 1, &saturation_selection,
        LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE, 1));
    CHECK(lardon3d_resource_governor_internal_sequence_aggregate(
            governor, "test.fallback-saturation", 1, &aggregate)
        && aggregate.saturated
        && aggregate.local_ineligible_fallback_items == UINT64_MAX
        && aggregate.sequence_count == 0
        && aggregate.durable_items == 0);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
record_gpu_feedback_sample(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DTaskCapabilityEnvelope *envelope,
    const char *task_kind,
    size_t expected_batch,
    uint64_t wall_time_ns,
    size_t items_completed,
    Lardon3DResourceBackend actual_backend
)
{
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;
    bool ok = lardon3d_resource_governor_internal_reserve_capability(
            governor, snapshot, task_kind, 1, envelope, &selection,
            &reservation)
        && reservation
        && selection.capability.backend
            == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
        && selection.decision.batch_size == expected_batch;
    if (reservation) {
        bool released = lardon3d_resource_governor_release(
            governor, reservation);
        reservation = NULL;
        ok = ok && released;
    }
    if (!ok) {
        return false;
    }
    return lardon3d_resource_governor_internal_record_sequence_execution(
        governor, task_kind, 1, &selection, wall_time_ns, items_completed,
        actual_backend,
        actual_backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
            ? "vulkan-completed" : "whole-pair-cpu-fallback");
}

static bool
run_sustained_gpu_batch_feedback_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .gpu_available = true,
        .gpu_memory_known = true,
        .gpu_uses_shared_memory = true,
        .gpu_memory_total_bytes = MEBIBYTES(512),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    policy.maximum_cpu_load_ratio = 1.0;
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor && use_fixed_test_clock(governor));
    CHECK(lardon3d_resource_governor_internal_set_backend_available(
        governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true));
    Lardon3DTaskCapabilityEnvelope envelope = adaptive_orb_envelope();
    envelope.count = 1;
    envelope.capabilities[0].estimate.maximum_batch_size = 8;
    envelope.capabilities[0].sustained_gpu_batch_feedback = true;
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(12),
        .swap_activity_known = true,
    };
    Lardon3DResourceSequenceDiagnostic diagnostic;
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;

    /* Eight pure observations establish the baseline. The eighth opens the
     * batch-two trial; neither one nor two samples can decide a GPU batch. */
    for (unsigned int sample = 0; sample < 8; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.ramp", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
        CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            governor, "matcher.gpu.ramp", 1, &diagnostic));
        CHECK(strcmp(diagnostic.reason,
            sample == 7 ? "gpu-batch-throughput-trial"
                        : "gpu-batch-baseline") == 0);
    }
    const size_t trials[] = {2, 4, 8};
    for (size_t step = 0; step < sizeof(trials) / sizeof(trials[0]); ++step) {
        for (unsigned int sample = 0; sample < 8; ++sample) {
            CHECK(record_gpu_feedback_sample(
                governor, &snapshot, &envelope, "matcher.gpu.ramp",
                trials[step], 1000000000ULL, trials[step],
                LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
            CHECK(lardon3d_resource_governor_internal_last_diagnostic(
                governor, "matcher.gpu.ramp", 1, &diagnostic));
            CHECK(strcmp(diagnostic.reason,
                sample == 7 ? "gpu-batch-throughput-improved"
                            : "gpu-batch-throughput-trial") == 0);
        }
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.gpu.ramp", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 8
        && selection.capability.estimate.maximum_batch_size == 8
        && strcmp(selection.reason, "gpu-first") == 0
        && lardon3d_resource_governor_release(governor, reservation));

    /* Eight equal-rate trial observations reject growth. One high and one low
     * sample in an independent trial remain undecided and cannot oscillate. */
    for (unsigned int sample = 0; sample < 8; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.no-gain", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    for (unsigned int sample = 0; sample < 8; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.no-gain", 2,
            2000000000ULL, 2, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.gpu.no-gain", 1, &diagnostic)
        && strcmp(diagnostic.reason, "gpu-batch-throughput-no-gain") == 0);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.gpu.no-gain", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.decision.batch_size == 1
        && strcmp(selection.reason, "gpu-batch-throughput-no-gain") == 0
        && lardon3d_resource_governor_release(governor, reservation));

    for (unsigned int sample = 0; sample < 8; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.noisy", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    CHECK(record_gpu_feedback_sample(
        governor, &snapshot, &envelope, "matcher.gpu.noisy", 2,
        100000000ULL, 2, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    CHECK(record_gpu_feedback_sample(
        governor, &snapshot, &envelope, "matcher.gpu.noisy", 2,
        4000000000ULL, 2, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "matcher.gpu.noisy", 1, &diagnostic)
        && strcmp(diagnostic.reason, "gpu-batch-throughput-trial") == 0);
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.gpu.noisy", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.batch_size == 2
        && strcmp(selection.reason, "gpu-batch-throughput-trial") == 0
        && lardon3d_resource_governor_release(governor, reservation));

    /* Pressure during a trial clamps the same admission to batch one and
     * clears every sample. Recovery, zero-work and fallback each require a
     * new consecutive pure eight-observation baseline. */
    for (unsigned int sample = 0; sample < 8; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    for (unsigned int sample = 0; sample < 3; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.pressure", 2,
            1000000000ULL, 2, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    snapshot.swap_pages_out = 1;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.gpu.pressure", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.pressure
            == LARDON3D_RESOURCE_PRESSURE_YELLOW
        && selection.decision.batch_size == 1
        && strcmp(selection.reason, "pressure-decrease") == 0
        && lardon3d_resource_governor_release(governor, reservation));
    for (unsigned int sample = 0; sample < 3; ++sample) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "matcher.gpu.pressure", 1, &envelope,
            &selection, &reservation));
        CHECK(reservation && selection.decision.batch_size == 1
            && lardon3d_resource_governor_release(governor, reservation));
    }
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_GREEN);
    for (unsigned int sample = 0; sample < 2; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    CHECK(record_gpu_feedback_sample(
        governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
        1000000000ULL, 0, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    for (unsigned int sample = 0; sample < 2; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    CHECK(record_gpu_feedback_sample(
        governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
        1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_CPU));
    for (unsigned int sample = 0; sample < 7; ++sample) {
        CHECK(record_gpu_feedback_sample(
            governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
            1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.gpu.pressure", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.decision.batch_size == 1
        && lardon3d_resource_governor_release(governor, reservation));
    CHECK(record_gpu_feedback_sample(
        governor, &snapshot, &envelope, "matcher.gpu.pressure", 1,
        1000000000ULL, 1, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "matcher.gpu.pressure", 1, &envelope,
        &selection, &reservation));
    CHECK(reservation && selection.decision.batch_size == 2
        && strcmp(selection.reason, "gpu-batch-throughput-trial") == 0
        && lardon3d_resource_governor_release(governor, reservation));
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_cpu_feedback_progression_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 32,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    policy.maximum_cpu_load_ratio = 1.0;
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    Lardon3DResourceCpuTopologyInput count_only = {0};
    CHECK(governor
        && lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &count_only)
        && use_fixed_test_clock(governor));
    Lardon3DTaskCapabilityEnvelope envelope = {
        .count = 1,
        .capabilities = {{
            .estimate = {
                .memory_bytes_per_item = MEBIBYTES(1),
                .minimum_batch_size = 1,
                .maximum_batch_size = 4,
                .desired_cpu_threads = 28,
                .task_class = LARDON3D_RESOURCE_TASK_CPU,
            },
            .backend = LARDON3D_RESOURCE_BACKEND_CPU,
            .inflight_limit = 1,
            .cpu_reducible = true,
            .batch_adaptive = true,
        }},
    };
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(12),
        .swap_activity_known = true,
    };
    Lardon3DResourceCapabilitySelection selection;
    Lardon3DResourceReservation *reservation = NULL;
    const unsigned int expected_cpu[] = {1, 2, 4, 8, 16, 28};
    for (size_t step = 0;
         step < sizeof(expected_cpu) / sizeof(expected_cpu[0]); ++step) {
        for (unsigned int observation = 0; observation < 2; ++observation) {
            reservation = NULL;
            CHECK(lardon3d_resource_governor_internal_reserve_capability(
                governor, &snapshot, "test.cpu.ramp", 1, &envelope,
                &selection, &reservation));
            CHECK(reservation
                && selection.decision.cpu_threads == expected_cpu[step]
                && selection.decision.batch_size == 1);
            CHECK(lardon3d_resource_governor_release(governor, reservation));
            CHECK(lardon3d_resource_governor_internal_record_sequence(
                governor, "test.cpu.ramp", 1, &selection, 1000000000ULL,
                expected_cpu[step]));
        }
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 28
        && selection.decision.batch_size == 2
        && strcmp(selection.reason, "throughput-trial") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    Lardon3DResourceSequenceDiagnostic ramp_diagnostic;
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "test.cpu.ramp", 1, &ramp_diagnostic)
        && ramp_diagnostic.previous_wall_time_ns == 1000000000ULL
        && ramp_diagnostic.items_completed == 28
        && ramp_diagnostic.durable_items_per_second_milli == 28000);

    /* The first active swap-I/O delta is detected after the maximum-CPU/batch2
     * operational trial was copied. This same admission must replace both
     * dimensions with CPU1/batch1 before it creates the reservation. */
    snapshot.swap_pages_out = 1;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
        &reservation));
    Lardon3DResourceReservationInfo pressure_reservation;
    CHECK(reservation);
    CHECK(selection.pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW);
    CHECK(selection.decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(selection.decision.cpu_threads == 1);
    CHECK(selection.decision.batch_size == 1);
    CHECK(strcmp(selection.reason, "pressure-decrease") == 0);
    CHECK(lardon3d_resource_reservation_get_active(
        governor, reservation, &pressure_reservation));
    CHECK(pressure_reservation.cpu_threads == 1);
    CHECK(pressure_reservation.batch_size == 1);
    CHECK(pressure_reservation.memory_bytes == MEBIBYTES(1));
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "test.cpu.ramp", 1, &ramp_diagnostic)
        && ramp_diagnostic.cpu_threads == 1
        && ramp_diagnostic.batch_size == 1
        && ramp_diagnostic.pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW
        && strcmp(ramp_diagnostic.reason, "pressure-decrease") == 0
        && lardon3d_resource_governor_release(governor, reservation));

    /* Recovery cannot reuse the old maximum-CPU baseline. Three healthy admissions
     * clear YELLOW, then two fresh CPU1 samples reopen only a CPU2 trial. */
    for (unsigned int observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 1
            && lardon3d_resource_governor_release(governor, reservation));
    }
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_GREEN);
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 1
            && lardon3d_resource_governor_release(governor, reservation)
            && lardon3d_resource_governor_internal_record_sequence(
                governor, "test.cpu.ramp", 1, &selection, 1000000000ULL, 1));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 2
        && selection.decision.batch_size == 1
        && strcmp(selection.reason, "cpu-throughput-trial") == 0
        && lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_internal_record_sequence(
        governor, "test.cpu.ramp", 1, &selection, 1000000000ULL, 2));
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 2
        && lardon3d_resource_governor_release(governor, reservation)
        && lardon3d_resource_governor_internal_record_sequence(
            governor, "test.cpu.ramp", 1, &selection, 1000000000ULL, 2));

    /* Gate G's PSI threshold still yields WAIT rather than manufacturing a
     * current reservation; the adaptive history is nevertheless abandoned. */
    snapshot.memory_pressure_known = true;
    snapshot.memory_pressure_avg10 = 100.0;
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
        &reservation));
    CHECK(!reservation && selection.decision.kind == LARDON3D_RESOURCE_WAIT
        && selection.pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW);
    snapshot.memory_pressure_known = false;
    for (unsigned int observation = 0; observation < 3; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "test.cpu.ramp", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 1
            && lardon3d_resource_governor_release(governor, reservation));
    }
    CHECK(lardon3d_resource_governor_pressure(governor)
        == LARDON3D_RESOURCE_PRESSURE_GREEN);

    /* Equal durable rate at CPU2 rejects that dimension after two samples,
     * retains accepted CPU1, and only then opens the independent batch trial. */
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "test.cpu.no_gain", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 1
            && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "test.cpu.no_gain", 1, &selection, 1000000000ULL, 1));
    }
    for (unsigned int observation = 0; observation < 2; ++observation) {
        reservation = NULL;
        CHECK(lardon3d_resource_governor_internal_reserve_capability(
            governor, &snapshot, "test.cpu.no_gain", 1, &envelope, &selection,
            &reservation));
        CHECK(reservation && selection.decision.cpu_threads == 2
            && selection.decision.batch_size == 1);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_internal_record_sequence(
            governor, "test.cpu.no_gain", 1, &selection, 2000000000ULL, 2));
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.no_gain", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 1
        && selection.decision.batch_size == 2
        && strcmp(selection.reason, "throughput-trial") == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));

    Lardon3DResourceSequenceDiagnostic diagnostic;
    CHECK(lardon3d_resource_governor_internal_last_diagnostic(
        governor, "test.cpu.no_gain", 1, &diagnostic)
        && diagnostic.cpu_threads == 1
        && diagnostic.batch_size == 2);
    lardon3d_resource_governor_destroy(governor);

    profile.logical_cpu_count = 12;
    policy.system_cpu_reserve = 4;
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor
        && lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &count_only)
        && use_fixed_test_clock(governor));
    envelope.capabilities[0].estimate.maximum_batch_size = 1;
    envelope.capabilities[0].batch_adaptive = false;
    const unsigned int capped_cpu[] = {1, 2, 4, 8};
    for (size_t step = 0;
         step < sizeof(capped_cpu) / sizeof(capped_cpu[0]); ++step) {
        for (unsigned int observation = 0; observation < 2; ++observation) {
            reservation = NULL;
            CHECK(lardon3d_resource_governor_internal_reserve_capability(
                governor, &snapshot, "test.cpu.cap", 1, &envelope, &selection,
                &reservation));
            CHECK(reservation
                && selection.decision.cpu_threads == capped_cpu[step]);
            CHECK(lardon3d_resource_governor_release(governor, reservation));
            CHECK(lardon3d_resource_governor_internal_record_sequence(
                governor, "test.cpu.cap", 1, &selection, 1000000000ULL,
                capped_cpu[step]));
        }
    }
    reservation = NULL;
    CHECK(lardon3d_resource_governor_internal_reserve_capability(
        governor, &snapshot, "test.cpu.cap", 1, &envelope, &selection,
        &reservation));
    CHECK(reservation && selection.decision.cpu_threads == 8);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_resource_governor_destroy(governor);
    return true;
}

int
main(void)
{
    return (run_driver_runtime_policy_test()
            && run_portable_default_policy_test()
            && run_default_memory_band_test()
            && run_test() && run_generation_test() && run_adaptive_batch_test()
            && run_gate_g_boundary_test() && run_topology_value_reader_test()
            && run_gpu_busy_identity_reader_test()
            && run_cpu_topology_policy_test()
            && run_capability_governor_test()
            && run_inflight_feedback_test()
            && run_private_telemetry_test()
            && run_sustained_gpu_batch_feedback_test()
            && run_cpu_feedback_progression_test())
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
