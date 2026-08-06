#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_snapshot.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

static bool
run_test(void)
{
    char error[256];
    Lardon3DResourceSnapshot snapshot;
    CHECK(!lardon3d_resource_snapshot_capture(NULL, &snapshot, error, sizeof(error)));
    CHECK(error[0]);
    Lardon3DHardwareProfile invalid = {0};
    CHECK(!lardon3d_resource_snapshot_capture(
        &invalid,
        &snapshot,
        error,
        sizeof(error)
    ));

    Lardon3DHardwareProfile profile;
    CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
    CHECK(lardon3d_resource_snapshot_capture(
        &profile,
        &snapshot,
        error,
        sizeof(error)
    ));
    CHECK(error[0] == '\0');
    CHECK(snapshot.captured_at.tv_sec > 0);
    CHECK(snapshot.memory_available_bytes > 0);
    CHECK(snapshot.memory_free_bytes <= profile.memory_total_bytes);
    CHECK(snapshot.cpu_load_1m >= 0.0);
    CHECK(snapshot.cpu_load_5m >= 0.0);
    CHECK(snapshot.cpu_load_15m >= 0.0);
    if (snapshot.gpu_memory_available_known && profile.gpu_memory_known) {
        CHECK(snapshot.gpu_memory_available_bytes <= profile.gpu_memory_total_bytes);
    }
    if (snapshot.io_pressure_known) {
        CHECK(snapshot.io_pressure_avg10 >= 0.0);
        CHECK(snapshot.io_pressure_avg10 <= 100.0);
    }
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
