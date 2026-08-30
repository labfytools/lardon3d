#ifndef LARDON3D_TESTS_VULKAN_PROCESS_STARTUP_H
#define LARDON3D_TESTS_VULKAN_PROCESS_STARTUP_H

#include <cstdlib>
#include <cstring>

/* Standalone Vulkan evidence tools own this process-start action. Call it as
 * the first statement of main, before OpenCV or any other library may create a
 * pthread. An absent value gets the safe default; an explicit value is never
 * overwritten and must already be exact true/1. The production backend itself
 * remains a non-mutating late boundary. */
static inline bool lardon3d_vulkan_evidence_process_startup() {
  const char *value = std::getenv("MESA_SHADER_CACHE_DISABLE");
  if (!value) {
    if (setenv("MESA_SHADER_CACHE_DISABLE", "true", 0) != 0) return false;
    value = std::getenv("MESA_SHADER_CACHE_DISABLE");
  }
  return value &&
         (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0);
}

#endif
