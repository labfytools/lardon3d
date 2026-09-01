#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <lardon3d/hardware_profile.h>

#include "hardware_profile_internal.h"

static void
set_error(char *message, size_t size, const char *text)
{
    if (message && size > 0) {
        (void)snprintf(message, size, "%s", text);
    }
}

static bool
read_text(const char *path, char *text, size_t capacity)
{
    if (!text || capacity < 2) {
        return false;
    }
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    size_t total = 0;
    bool success = true;
    while (total + 1 < capacity) {
        ssize_t count = read(descriptor, text + total, capacity - total - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            success = false;
            break;
        }
        if (count == 0) break;
        total += (size_t)count;
    }
    if (success && total + 1 == capacity) {
        char extra;
        ssize_t count;
        do {
            count = read(descriptor, &extra, 1);
        } while (count < 0 && errno == EINTR);
        success = count == 0;
    }
    if (close(descriptor) != 0) success = false;
    if (!success) {
        return false;
    }
    text[total] = '\0';
    while (total > 0 && (text[total - 1] == '\n'
            || text[total - 1] == '\r')) {
        text[--total] = '\0';
    }
    return true;
}

static bool
parse_uint64(const char *text, uint64_t *value)
{
    if (!text || !text[0] || !value) {
        return false;
    }
    const unsigned char *cursor = (const unsigned char *)text;
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
shared_memory_evidence(
    const Lardon3DHardwareProfile *profile,
    uint64_t vram_total,
    bool gtt_known,
    uint64_t gtt_total
)
{
    if (profile->memory_total_bytes == 0) return false;
    const uint64_t two_gibibytes = UINT64_C(2) * 1024 * 1024 * 1024;
    bool conservatively_small = vram_total <= two_gibibytes
        && vram_total <= profile->memory_total_bytes / 8;
    bool system_scale_gtt = gtt_known
        && gtt_total >= profile->memory_total_bytes / 4
        && vram_total <= gtt_total / 2;
    /* WHY: amdgpu exposes a small stolen/dedicated VRAM aperture even for an
     * integrated GPU. Treating that positive number as separate free memory
     * undercharges host RAM. A low-VRAM uncertain device may conservatively
     * become UMA; the reverse error could violate the 3 GiB hard reserve and
     * the 3--4 GiB host-caution zone. */
    return conservatively_small || system_scale_gtt;
}

static const char *
vendor_name(const char *vendor)
{
    if (strcmp(vendor, "0x1002") == 0) {
        return "AMD";
    }
    if (strcmp(vendor, "0x8086") == 0) {
        return "Intel";
    }
    if (strcmp(vendor, "0x10de") == 0) {
        return "NVIDIA";
    }
    return vendor;
}

void
lardon3d_hardware_profile_detect_gpu_at_root(
    Lardon3DHardwareProfile *profile,
    const char *drm_root
)
{
    if (!profile || !drm_root) {
        return;
    }
    profile->gpu_available = false;
    profile->gpu_drm_card_index = 0;
    profile->gpu_memory_known = false;
    profile->gpu_uses_shared_memory = false;
    profile->gpu_memory_total_bytes = 0;
    profile->gpu_name[0] = '\0';
    for (unsigned int index = 0; index < 64; ++index) {
        char vendor_path[PATH_MAX];
        char memory_path[PATH_MAX];
        char gtt_path[PATH_MAX];
        int vendor_written = snprintf(
            vendor_path,
            sizeof(vendor_path),
            "%s/card%u/device/vendor",
            drm_root,
            index
        );
        int memory_written = snprintf(
            memory_path,
            sizeof(memory_path),
            "%s/card%u/device/mem_info_vram_total",
            drm_root,
            index
        );
        int gtt_written = snprintf(
            gtt_path,
            sizeof(gtt_path),
            "%s/card%u/device/mem_info_gtt_total",
            drm_root,
            index
        );
        if (vendor_written < 0 || (size_t)vendor_written >= sizeof(vendor_path)
            || memory_written < 0
            || (size_t)memory_written >= sizeof(memory_path)
            || gtt_written < 0 || (size_t)gtt_written >= sizeof(gtt_path)) {
            continue;
        }
        char vendor[32];
        if (!read_text(vendor_path, vendor, sizeof(vendor))) {
            continue;
        }
        profile->gpu_available = true;
        profile->gpu_drm_card_index = index;
        (void)snprintf(
            profile->gpu_name,
            sizeof(profile->gpu_name),
            "%s GPU",
            vendor_name(vendor)
        );
        char memory[64];
        uint64_t bytes;
        if (read_text(memory_path, memory, sizeof(memory))
            && parse_uint64(memory, &bytes) && bytes > 0) {
            profile->gpu_memory_known = true;
            profile->gpu_memory_total_bytes = bytes;
            char gtt[64];
            uint64_t gtt_bytes = 0;
            bool gtt_known = read_text(gtt_path, gtt, sizeof(gtt))
                && parse_uint64(gtt, &gtt_bytes) && gtt_bytes > 0;
            profile->gpu_uses_shared_memory = shared_memory_evidence(
                profile, bytes, gtt_known, gtt_bytes);
        } else {
            /* Unknown payload capacity must never be treated as an independent
             * VRAM budget. Governor can still use the GPU conservatively while
             * charging every admitted byte to MemAvailable. */
            profile->gpu_uses_shared_memory = true;
        }
        return;
    }
}

bool
lardon3d_hardware_profile_detect(
    Lardon3DHardwareProfile *profile,
    char *error_message,
    size_t error_message_size
)
{
    set_error(error_message, error_message_size, "");
    if (!profile) {
        set_error(error_message, error_message_size, "Profil matériel absent.");
        return false;
    }
    *profile = (Lardon3DHardwareProfile) {0};
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    long page_size = sysconf(_SC_PAGESIZE);
    long page_count = sysconf(_SC_PHYS_PAGES);
    if (cpu_count < 1 || cpu_count > UINT_MAX || page_size < 1
        || page_count < 1) {
        set_error(
            error_message,
            error_message_size,
            "Détection CPU ou mémoire impossible."
        );
        return false;
    }
    uint64_t page_size_bytes = (uint64_t)page_size;
    uint64_t pages = (uint64_t)page_count;
    if (pages > UINT64_MAX / page_size_bytes) {
        set_error(error_message, error_message_size, "Mémoire physique trop grande.");
        return false;
    }
    profile->logical_cpu_count = (unsigned int)cpu_count;
    profile->page_size_bytes = page_size_bytes;
    profile->memory_total_bytes = pages * page_size_bytes;
    struct utsname system_name;
    if (uname(&system_name) == 0) {
        (void)snprintf(
            profile->cpu_architecture,
            sizeof(profile->cpu_architecture),
            "%s",
            system_name.machine
        );
    } else {
        (void)snprintf(
            profile->cpu_architecture,
            sizeof(profile->cpu_architecture),
            "Linux"
        );
    }
    lardon3d_hardware_profile_detect_gpu_at_root(profile, "/sys/class/drm");
    return true;
}
