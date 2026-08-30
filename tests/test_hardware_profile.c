#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/hardware_profile.h>

#include "../src/hardware_profile_internal.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

#define GIBIBYTES(value) ((uint64_t)(value) * 1024 * 1024 * 1024)

static bool
write_text(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600);
    if (descriptor < 0) return false;
    size_t length = strlen(text);
    ssize_t written = write(descriptor, text, length);
    return close(descriptor) == 0 && written == (ssize_t)length;
}

static bool
read_exact_text(const char *path, const char *expected)
{
    char buffer[64];
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    ssize_t count = read(descriptor, buffer, sizeof(buffer));
    bool eof = count >= 0 && count < (ssize_t)sizeof(buffer)
        && read(descriptor, buffer + count, 1) == 0;
    bool equal = eof && (size_t)count == strlen(expected)
        && memcmp(buffer, expected, (size_t)count) == 0;
    return close(descriptor) == 0 && equal;
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
create_card(const char *root, unsigned int index, const char *vram,
    const char *gtt)
{
    char card[PATH_MAX];
    char device[PATH_MAX];
    int card_written = snprintf(card, sizeof(card), "%s/card%u", root, index);
    int device_written = snprintf(device, sizeof(device), "%s/device", card);
    if (card_written <= 0 || (size_t)card_written >= sizeof(card)
        || device_written <= 0 || (size_t)device_written >= sizeof(device)
        || mkdir(card, 0700) != 0 || mkdir(device, 0700) != 0) {
        return false;
    }
    char path[PATH_MAX];
    return card_path(path, root, index, "vendor")
        && write_text(path, "0x1002\n")
        && card_path(path, root, index, "mem_info_vram_total")
        && write_text(path, vram)
        && (!gtt || (card_path(path, root, index, "mem_info_gtt_total")
            && write_text(path, gtt)));
}

static bool
remove_card(const char *root, unsigned int index)
{
    char path[PATH_MAX];
    const char *files[] = {
        "vendor", "mem_info_vram_total", "mem_info_gtt_total",
    };
    for (size_t file = 0; file < sizeof(files) / sizeof(files[0]); ++file) {
        if (!card_path(path, root, index, files[file])) return false;
        if (unlink(path) != 0 && errno != ENOENT) return false;
    }
    int written = snprintf(path, sizeof(path), "%s/card%u/device", root,
        index);
    if (written <= 0 || (size_t)written >= sizeof(path) || rmdir(path) != 0)
        return false;
    written = snprintf(path, sizeof(path), "%s/card%u", root, index);
    return written > 0 && (size_t)written < sizeof(path) && rmdir(path) == 0;
}

static bool
run_fake_gpu_test(void)
{
    char root[] = "/tmp/lardon3d-hardware-profile-XXXXXX";
    CHECK(mkdtemp(root));
    Lardon3DHardwareProfile profile = {
        .memory_total_bytes = GIBIBYTES(16),
    };

    /* Exact current-host evidence: the small 512 MiB aperture and system-scale
     * GTT are UMA, while the payload capacity remains observable. */
    CHECK(create_card(root, 1, "536870912\n", "7986020352\n"));
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_available && profile.gpu_drm_card_index == 1
        && profile.gpu_memory_known
        && profile.gpu_memory_total_bytes == UINT64_C(536870912)
        && profile.gpu_uses_shared_memory);
    CHECK(remove_card(root, 1));

    CHECK(create_card(root, 0, "8589934592\n", "8589934592\n"));
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_available && profile.gpu_memory_known
        && profile.gpu_memory_total_bytes == GIBIBYTES(8)
        && !profile.gpu_uses_shared_memory);
    CHECK(remove_card(root, 0));

    /* Missing GTT with a low aperture is intentionally conservative; malformed
     * GTT cannot turn a large known VRAM device into UMA. */
    CHECK(create_card(root, 2, "536870912\n", NULL));
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_memory_known && profile.gpu_uses_shared_memory);
    CHECK(remove_card(root, 2));
    CHECK(create_card(root, 3, "8589934592\n", "-1\n"));
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_memory_known && !profile.gpu_uses_shared_memory);
    CHECK(remove_card(root, 3));

    CHECK(create_card(root, 4, "+536870912\n", "7986020352\n"));
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(profile.gpu_available && !profile.gpu_memory_known
        && profile.gpu_uses_shared_memory);
    CHECK(remove_card(root, 4));

    profile.gpu_available = true;
    profile.gpu_memory_known = true;
    profile.gpu_uses_shared_memory = true;
    profile.gpu_memory_total_bytes = 1;
    lardon3d_hardware_profile_detect_gpu_at_root(&profile, root);
    CHECK(!profile.gpu_available && !profile.gpu_memory_known
        && !profile.gpu_uses_shared_memory
        && profile.gpu_memory_total_bytes == 0 && profile.gpu_name[0] == '\0');
    CHECK(rmdir(root) == 0);
    return true;
}

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
        CHECK(profile.gpu_drm_card_index < 64);
    }
    /* Skip-safe current-host integration evidence. Exact sysfs values identify
     * the validated 780M configuration without making its PCI device ID part
     * of production policy or a portable test requirement. */
    if (read_exact_text("/sys/class/drm/card1/device/vendor", "0x1002\n")
        && read_exact_text("/sys/class/drm/card1/device/device", "0x1900\n")
        && read_exact_text("/sys/class/drm/card1/device/mem_info_vram_total",
            "536870912\n")
        && read_exact_text("/sys/class/drm/card1/device/mem_info_gtt_total",
            "7986020352\n")) {
        CHECK(profile.gpu_available && profile.gpu_drm_card_index == 1
            && profile.gpu_memory_known
            && profile.gpu_memory_total_bytes == UINT64_C(536870912)
            && profile.gpu_uses_shared_memory);
    }
    Lardon3DHardwareProfile second;
    CHECK(lardon3d_hardware_profile_detect(&second, NULL, 0));
    CHECK(second.logical_cpu_count == profile.logical_cpu_count);
    CHECK(second.memory_total_bytes == profile.memory_total_bytes);
    CHECK(run_fake_gpu_test());
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
