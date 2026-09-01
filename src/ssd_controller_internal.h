#ifndef LARDON3D_SSD_CONTROLLER_INTERNAL_H
#define LARDON3D_SSD_CONTROLLER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/ssd_controller.h>

#ifdef LARDON3D_SSD_CONTROLLER_TESTING
#include <glib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LARDON3D_SSD_OBJECT_PATH_CAPACITY = 256,
};

typedef struct {
    bool present;
    bool unit_ready;
    bool interface_available;
    uint64_t size_bytes;
    char label[LARDON3D_SSD_TEXT_CAPACITY];
    char uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char drive_identity[LARDON3D_SSD_IDENTITY_CAPACITY];
    char object_path[LARDON3D_SSD_OBJECT_PATH_CAPACITY];
    char device[LARDON3D_SSD_PATH_CAPACITY];

    /* Required UDisks Swapspace.Active telemetry. `active` has no meaning
     * unless active_known is true; missing/wrong D-Bus storage must never be
     * narrowed to the unsafe default "inactive". */
    bool active_known;
    bool active;
    bool total_known;
    bool used_known;
    uint64_t total_bytes;
    uint64_t used_bytes;
} Lardon3DSsdProviderSwap;

typedef struct {
    bool present;
    bool unit_ready;
    bool interface_available;
    uint64_t size_bytes;
    char label[LARDON3D_SSD_TEXT_CAPACITY];
    char uuid[LARDON3D_SSD_TEXT_CAPACITY];
    char drive_identity[LARDON3D_SSD_IDENTITY_CAPACITY];
    char object_path[LARDON3D_SSD_OBJECT_PATH_CAPACITY];
    char device[LARDON3D_SSD_PATH_CAPACITY];

    bool mounted;
    bool total_known;
    bool free_known;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char mount_path[LARDON3D_SSD_PATH_CAPACITY];
} Lardon3DSsdProviderScratch;

typedef struct {
    bool model_known;
    bool serial_known;
    bool connection_speed_known;
    char model[LARDON3D_SSD_TEXT_CAPACITY];
    char serial[LARDON3D_SSD_TEXT_CAPACITY];
    uint64_t connection_speed_mbps;

    bool ambiguous_labels;
    bool invalid_observation;
    char invalid_reason[LARDON3D_SSD_REASON_CAPACITY];
    Lardon3DSsdProviderSwap swap;
    Lardon3DSsdProviderScratch scratch;

    bool memory_available_known;
    uint64_t memory_available_bytes;
    bool memory_pressure_known;
    bool memory_pressure_elevated;
    bool io_pressure_known;
    bool io_pressure_elevated;
    bool swap_activity_known;
    uint64_t swap_pages_in_delta;
    uint64_t swap_pages_out_delta;
} Lardon3DSsdProviderSnapshot;

typedef struct {
    bool (*monotonic_now_ns)(void *context, uint64_t *now_ns);
    bool (*refresh)(
        void *context,
        Lardon3DSsdProviderSnapshot *snapshot,
        char reason[LARDON3D_SSD_REASON_CAPACITY]
    );
    bool (*start_swap)(
        void *context,
        const char *object_path,
        char reason[LARDON3D_SSD_REASON_CAPACITY]
    );
    bool (*stop_swap)(
        void *context,
        const char *object_path,
        char reason[LARDON3D_SSD_REASON_CAPACITY]
    );
    bool (*mount_scratch)(
        void *context,
        const char *object_path,
        char mount_path[LARDON3D_SSD_PATH_CAPACITY],
        char reason[LARDON3D_SSD_REASON_CAPACITY]
    );
    bool (*unmount_scratch)(
        void *context,
        const char *object_path,
        char reason[LARDON3D_SSD_REASON_CAPACITY]
    );
    void (*destroy)(void *context);
} Lardon3DSsdProviderOps;

typedef struct {
    const Lardon3DSsdProviderOps *ops;
    void *context;
} Lardon3DSsdProvider;

/* Private dependency-injection seam. The controller takes ownership of
 * `provider.context` on success only. Tests use fixed in-memory providers;
 * production constructs the GDBus provider below. This never extends the
 * public ABI or permits provider replacement after creation. The interval is
 * at least one second; tests use force-refresh instead of weakening caching. */
Lardon3DSsdController *lardon3d_ssd_controller_create_with_provider(
    Lardon3DSsdProvider provider,
    uint64_t minimum_poll_interval_ns
);

bool lardon3d_ssd_production_provider_create(Lardon3DSsdProvider *provider);

#ifdef LARDON3D_SSD_CONTROLLER_TESTING
/* Parses a caller-owned GetManagedObjects fixture without touching D-Bus,
 * /proc, mounts, or swap. This exists solely to validate exact GLib storage
 * types and bounded UDisks discovery; production uses the same parser. */
bool lardon3d_ssd_parse_managed_objects_for_test(
    GVariant *objects,
    Lardon3DSsdProviderSnapshot *snapshot,
    char reason[LARDON3D_SSD_REASON_CAPACITY]
);
/* Deterministic saturation seam. It changes only the private source watermark
 * and cached copy; later production operations still use the real saturating
 * bump path and otherwise retain identical controller behavior. */
bool lardon3d_ssd_controller_set_generation_for_test(
    Lardon3DSsdController *controller,
    uint64_t generation
);
/* Forces only the cached copy to fail bounded-string validation. Physical
 * lease bookkeeping still completes through the production release path. */
bool lardon3d_ssd_controller_corrupt_cached_snapshot_for_test(
    Lardon3DSsdController *controller
);
#endif

#ifdef __cplusplus
}
#endif

#endif
