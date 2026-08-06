#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/hardware_profile.h>

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
    CHECK(!lardon3d_hardware_profile_detect(NULL, error, sizeof(error)));
    CHECK(error[0]);
    Lardon3DHardwareProfile profile;
    CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
    CHECK(error[0] == '\0');
    CHECK(profile.logical_cpu_count > 0);
    CHECK(profile.page_size_bytes > 0);
    CHECK(profile.memory_total_bytes >= profile.page_size_bytes);
    CHECK(profile.cpu_architecture[0]);
    if (profile.gpu_memory_known) {
        CHECK(profile.gpu_available);
        CHECK(profile.gpu_memory_total_bytes > 0);
    }
    if (profile.gpu_available) {
        CHECK(profile.gpu_name[0]);
    }
    Lardon3DHardwareProfile second;
    CHECK(lardon3d_hardware_profile_detect(&second, NULL, 0));
    CHECK(second.logical_cpu_count == profile.logical_cpu_count);
    CHECK(second.memory_total_bytes == profile.memory_total_bytes);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
