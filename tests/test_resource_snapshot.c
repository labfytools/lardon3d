#include <stdbool.h>
#include <fcntl.h>
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
create_card(const char *root, unsigned int index, const char *total, const char *used)
{
    char card[256];
    char device[256];
    char path[256];
    if (snprintf(card, sizeof(card), "%s/card%u", root, index) < 0
        || snprintf(device, sizeof(device), "%s/device", card) < 0
        || mkdir(card, 0700) != 0 || mkdir(device, 0700) != 0) {
        return false;
    }
    (void)snprintf(path, sizeof(path), "%s/vendor", device);
    if (!write_text(path, "0x1002\n")) {
        return false;
    }
    (void)snprintf(path, sizeof(path), "%s/mem_info_vram_total", device);
    if (!write_text(path, total)) {
        return false;
    }
    (void)snprintf(path, sizeof(path), "%s/mem_info_vram_used", device);
    return write_text(path, used);
}

static void
remove_card(const char *root, unsigned int index)
{
    char path[256];
    const char *files[] = {"vendor", "mem_info_vram_total", "mem_info_vram_used"};
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
    CHECK(create_card(root, 0, "1000\n", "100\n"));
    CHECK(create_card(root, 1, "4000\n", "3000\n"));
    Lardon3DHardwareProfile profile = {0};
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_available);
    CHECK(profile.gpu_drm_card_index == 0);
    CHECK(profile.gpu_memory_total_bytes == 1000);
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
    CHECK(unlink(selected_usage) == 0);
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
