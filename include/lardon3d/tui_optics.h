#ifndef LARDON3D_TUI_OPTICS_H
#define LARDON3D_TUI_OPTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/optical_profiles.h>
#include <lardon3d/project_db.h>
#include <lardon3d/tui_model.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LARDON3D_TUI_OPTICS_PAGE_CAPACITY = 16,
};

typedef struct Lardon3DTuiOptics Lardon3DTuiOptics;

typedef enum {
    LARDON3D_TUI_OPTICS_PANE_BODY = 0,
    LARDON3D_TUI_OPTICS_PANE_LENS,
    LARDON3D_TUI_OPTICS_PANE_CONFIGURATION,
    LARDON3D_TUI_OPTICS_PANE_CALIBRATION,
} Lardon3DTuiOpticsPane;

typedef struct {
    bool project_bound;
    Lardon3DTuiOpticsPane active_pane;
    Lardon3DOpticalCameraBodyProfile
        bodies[LARDON3D_TUI_OPTICS_PAGE_CAPACITY];
    size_t body_count;
    size_t selected_body;
    bool bodies_have_next;
    Lardon3DOpticalLensProfile
        lenses[LARDON3D_TUI_OPTICS_PAGE_CAPACITY];
    size_t lens_count;
    size_t selected_lens;
    bool lenses_have_next;
    Lardon3DOpticalConfiguration
        configurations[LARDON3D_TUI_OPTICS_PAGE_CAPACITY];
    size_t configuration_count;
    size_t selected_configuration;
    bool configurations_have_next;
    Lardon3DOpticalCalibrationProfile
        calibrations[LARDON3D_TUI_OPTICS_PAGE_CAPACITY];
    size_t calibration_count;
    size_t selected_calibration;
    bool calibrations_have_next;

    bool capture_inspected;
    uint64_t capture_id;
    Lardon3DTuiOpticsStatus capture_status;
    bool capture_assignment_found;
    Lardon3DOpticalCaptureAssignment capture_assignment;
    bool capture_configuration_found;
    Lardon3DOpticalConfiguration capture_configuration;
    bool capture_body_found;
    Lardon3DOpticalCameraBodyProfile capture_body;
    bool capture_lens_found;
    Lardon3DOpticalLensProfile capture_lens;
    bool capture_selection_found;
    Lardon3DOpticalCaptureCalibrationSelection capture_selection;

    bool metadata_lookup_performed;
    bool metadata_body_found;
    Lardon3DOpticalCameraBodyProfile metadata_body;
    bool metadata_lens_found;
    Lardon3DOpticalLensProfile metadata_lens;
    char message[LARDON3D_TUI_TEXT_CAPACITY];
} Lardon3DTuiOpticsSnapshot;

/* One main-thread-only view model caches bounded pages and caller-owned copies;
 * no function is thread-safe. NULL is rejected except destroy/unbind, which
 * accept it. Binding borrows the Project DB until unbind/destroy and performs
 * bounded list calls once; redraw/snapshot never queries SQLite. On bind
 * failure no DB borrow is retained and snapshot.message preserves the exact
 * BUSY/IO/CORRUPT-style reason for explicit retry. Unbind before DB close. */
Lardon3DTuiOptics *lardon3d_tui_optics_create(void);
void lardon3d_tui_optics_destroy(Lardon3DTuiOptics *optics);
bool lardon3d_tui_optics_bind(
    Lardon3DTuiOptics *optics,
    Lardon3DProjectDb *database
);
void lardon3d_tui_optics_unbind(Lardon3DTuiOptics *optics);
/* Initializes a non-NULL output before validation; on success it is a
 * caller-owned value with no internal pointers and remains valid after the
 * next model call, unbind, or destroy. */
bool lardon3d_tui_optics_snapshot(
    const Lardon3DTuiOptics *optics,
    Lardon3DTuiOpticsSnapshot *snapshot
);

bool lardon3d_tui_optics_select_pane(
    Lardon3DTuiOptics *optics,
    Lardon3DTuiOpticsPane pane
);
bool lardon3d_tui_optics_move_selection(
    Lardon3DTuiOptics *optics,
    int direction
);
/* `next=true` loads the next ascending page; false returns to the first page.
 * Calibration pages use the currently inspected Capture's exact optical
 * configuration. There is no unbounded accumulated history. */
bool lardon3d_tui_optics_page(
    Lardon3DTuiOptics *optics,
    Lardon3DTuiOpticsPane pane,
    bool next
);

/* Creation is immutable: editing means creating/selecting a new versioned
 * profile/configuration. Inputs are borrowed for the call and exact retry or
 * conflict semantics remain those of optical_profiles.h. */
bool lardon3d_tui_optics_create_body(
    Lardon3DTuiOptics *optics,
    const Lardon3DOpticalCameraBodyProfile *input
);
bool lardon3d_tui_optics_create_lens(
    Lardon3DTuiOptics *optics,
    const Lardon3DOpticalLensProfile *input
);
bool lardon3d_tui_optics_create_configuration(
    Lardon3DTuiOptics *optics,
    bool has_focal_length,
    uint32_t focal_length_um
);

bool lardon3d_tui_optics_assign_campaign_group(
    Lardon3DTuiOptics *optics,
    uint64_t campaign_task_id,
    uint32_t group_id
);
bool lardon3d_tui_optics_assign_capture(
    Lardon3DTuiOptics *optics,
    uint64_t capture_id
);
bool lardon3d_tui_optics_inspect_capture(
    Lardon3DTuiOptics *optics,
    uint64_t capture_id
);
bool lardon3d_tui_optics_select_capture_calibration(
    Lardon3DTuiOptics *optics
);

/* Exact metadata lookup only. Empty lens make is valid for data-driven manual
 * aliases; no alias match leaves that side unresolved and creates nothing. */
bool lardon3d_tui_optics_lookup_exact_metadata(
    Lardon3DTuiOptics *optics,
    const char *body_make,
    const char *body_model,
    const char *lens_make,
    const char *lens_model
);

#ifdef __cplusplus
}
#endif

#endif
