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
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    ssize_t count;
    do {
        count = read(descriptor, text, capacity - 1);
    } while (count < 0 && errno == EINTR);
    bool success = count >= 0 && close(descriptor) == 0;
    if (!success) {
        return false;
    }
    text[(size_t)count] = '\0';
    while (count > 0 && (text[(size_t)count - 1] == '\n'
            || text[(size_t)count - 1] == '\r')) {
        text[--count] = '\0';
    }
    return true;
}

static bool
parse_uint64(const char *text, uint64_t *value)
{
    if (!text || !text[0] || !value || text[0] == '-') {
        return false;
    }
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        ++end;
    }
    if (*end) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
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

static void
detect_gpu(Lardon3DHardwareProfile *profile)
{
    for (unsigned int index = 0; index < 64; ++index) {
        char vendor_path[PATH_MAX];
        char memory_path[PATH_MAX];
        int vendor_written = snprintf(
            vendor_path,
            sizeof(vendor_path),
            "/sys/class/drm/card%u/device/vendor",
            index
        );
        int memory_written = snprintf(
            memory_path,
            sizeof(memory_path),
            "/sys/class/drm/card%u/device/mem_info_vram_total",
            index
        );
        if (vendor_written < 0 || (size_t)vendor_written >= sizeof(vendor_path)
            || memory_written < 0
            || (size_t)memory_written >= sizeof(memory_path)) {
            continue;
        }
        char vendor[32];
        if (!read_text(vendor_path, vendor, sizeof(vendor))) {
            continue;
        }
        profile->gpu_available = true;
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
        } else {
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
    detect_gpu(profile);
    return true;
}
