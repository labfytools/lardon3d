#include <stdbool.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_snapshot.h>

#include "../src/hardware_profile_internal.h"
#include "../src/resource_snapshot_internal.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

static bool
write_text(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    size_t length = strlen(text);
    ssize_t written = write(descriptor, text, length);
    bool success = written == (ssize_t)length;
    if (close(descriptor) != 0) {
        success = false;
    }
    return success;
}

static bool
card_path(char path[PATH_MAX], const char *root, unsigned int index,
          const char *name)
{
    int written = snprintf(path, PATH_MAX, "%s/card%u/device/%s", root,
        index, name);
    return written > 0 && written < PATH_MAX;
}

static bool
create_card(const char *root, unsigned int index, const char *total,
            const char *used, const char *gtt)
{
    char card[PATH_MAX];
    char device[PATH_MAX];
    char path[PATH_MAX];
    int card_written = snprintf(card, sizeof(card), "%s/card%u", root, index);
    int device_written = snprintf(device, sizeof(device), "%s/device", card);
    if (card_written <= 0 || (size_t)card_written >= sizeof(card)
        || device_written <= 0 || (size_t)device_written >= sizeof(device)
        || mkdir(card, 0700) != 0 || mkdir(device, 0700) != 0) {
        return false;
    }
    if (!card_path(path, root, index, "vendor")
        || !write_text(path, "0x1002\n")) {
        return false;
    }
    if (!card_path(path, root, index, "mem_info_vram_total")
        || !write_text(path, total)) {
        return false;
    }
    if (!card_path(path, root, index, "mem_info_vram_used")
        || !write_text(path, used)) {
        return false;
    }
    if (!gtt) return true;
    return card_path(path, root, index, "mem_info_gtt_total")
        && write_text(path, gtt);
}

static void
remove_card(const char *root, unsigned int index)
{
    char path[256];
    const char *files[] = {
        "vendor", "mem_info_vram_total", "mem_info_vram_used",
        "mem_info_gtt_total",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        (void)snprintf(path, sizeof(path), "%s/card%u/device/%s", root, index, files[i]);
        (void)unlink(path);
    }
    (void)snprintf(path, sizeof(path), "%s/card%u/device", root, index);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), "%s/card%u", root, index);
    (void)rmdir(path);
}

static bool
test_selected_gpu_pairing(void)
{
    char root[] = "/tmp/lardon3d-drm-XXXXXX";
    CHECK(mkdtemp(root));
    CHECK(create_card(root, 0, "1000\n", "100\n", "8000\n"));
    CHECK(create_card(root, 1, "4000\n", "3000\n", NULL));
    Lardon3DHardwareProfile profile = {
        .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
    };
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_available);
    CHECK(profile.gpu_drm_card_index == 0);
    CHECK(profile.gpu_memory_total_bytes == 1000);
    CHECK(profile.gpu_uses_shared_memory);
    Lardon3DResourceSnapshot snapshot = {.memory_available_bytes = 800};
    lardon3d_resource_snapshot_capture_gpu_at_root(&profile, &snapshot, root);
    CHECK(snapshot.gpu_memory_available_known);
    CHECK(snapshot.gpu_memory_available_bytes == 900);

    char selected_usage[256];
    (void)snprintf(
        selected_usage,
        sizeof(selected_usage),
        "%s/card0/device/mem_info_vram_used",
        root
    );
    CHECK(write_text(selected_usage, "100junk\n"));
    snapshot = (Lardon3DResourceSnapshot) {0};
    lardon3d_resource_snapshot_capture_gpu_at_root(&profile, &snapshot, root);
    CHECK(!snapshot.gpu_memory_available_known);
    remove_card(root, 0);
    remove_card(root, 1);
    CHECK(rmdir(root) == 0);
    return true;
}

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
    struct timespec before;
    struct timespec after;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    CHECK(lardon3d_resource_snapshot_capture(
        &profile,
        &snapshot,
        error,
        sizeof(error)
    ));
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    CHECK(error[0] == '\0');
    CHECK(snapshot.captured_at.tv_sec > 0);
    CHECK(snapshot.captured_at.tv_sec > before.tv_sec
        || (snapshot.captured_at.tv_sec == before.tv_sec
            && snapshot.captured_at.tv_nsec >= before.tv_nsec));
    CHECK(snapshot.captured_at.tv_sec < after.tv_sec
        || (snapshot.captured_at.tv_sec == after.tv_sec
            && snapshot.captured_at.tv_nsec <= after.tv_nsec));
    CHECK(snapshot.memory_available_bytes > 0);
    CHECK(snapshot.memory_free_bytes <= profile.memory_total_bytes);
    Lardon3DResourceObservation observation;
    CHECK(lardon3d_resource_observation_capture(
        &profile, &observation, error, sizeof(error)));
    CHECK(observation.swap_total_known);
    CHECK(observation.snapshot.swap_available_bytes
        <= observation.swap_total_bytes);
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
    if (snapshot.cpu_pressure_known) {
        CHECK(snapshot.cpu_pressure_avg10 >= 0.0);
        CHECK(snapshot.cpu_pressure_avg10 <= 100.0);
    }
    if (snapshot.memory_pressure_known) {
        CHECK(snapshot.memory_pressure_avg10 >= 0.0);
        CHECK(snapshot.memory_pressure_avg10 <= 100.0);
    }
    CHECK(test_selected_gpu_pairing());
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
