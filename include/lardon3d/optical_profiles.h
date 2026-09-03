#ifndef LARDON3D_OPTICAL_PROFILES_H
#define LARDON3D_OPTICAL_PROFILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_OPTICAL_TEXT_CAPACITY = 128,
  LARDON3D_OPTICAL_PROVENANCE_CAPACITY = 256,
  LARDON3D_OPTICAL_PAGE_MAX = 128,
  LARDON3D_OPTICAL_FOCUS_DOMAIN_DIGEST_SIZE = 32,
  /* This is an operational persistence/API bound, not a scientific focus
   * range. Every member remains one opaque exact observation token. */
  LARDON3D_OPTICAL_FOCUS_DOMAIN_TOKEN_MAX = 64,
};

typedef enum {
  /* No electronic identity is required. A manual lens legitimately has no
   * metadata alias and is selected only through an explicit profile/config. */
  LARDON3D_OPTICAL_LENS_MANUAL = 1,
  LARDON3D_OPTICAL_LENS_ELECTRONIC = 2,
  LARDON3D_OPTICAL_LENS_INTEGRATED = 3,
} Lardon3DOpticalLensInterface;

typedef enum {
  LARDON3D_OPTICAL_FOCAL_RANGE_UNKNOWN = 1,
  LARDON3D_OPTICAL_FOCAL_RANGE_PRIME = 2,
  LARDON3D_OPTICAL_FOCAL_RANGE_ZOOM = 3,
} Lardon3DOpticalFocalRangeKind;

typedef enum {
  LARDON3D_OPTICAL_ASSIGNMENT_CAMPAIGN = 1,
  LARDON3D_OPTICAL_ASSIGNMENT_CALLER_EXPLICIT = 2,
} Lardon3DOpticalAssignmentProvenance;

typedef enum {
  /* The profile applies only to its exact optical_configuration_id. It never
   * authorizes interpolation or borrowing across a body, lens, or focal setup. */
  LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION = 1,
} Lardon3DOpticalCalibrationApplicability;

typedef struct {
  uint64_t camera_body_profile_id;
  char manufacturer[LARDON3D_OPTICAL_TEXT_CAPACITY];
  char model[LARDON3D_OPTICAL_TEXT_CAPACITY];
  char name[LARDON3D_OPTICAL_TEXT_CAPACITY];
} Lardon3DOpticalCameraBodyProfile;

typedef struct {
  uint64_t alias_id;
  uint64_t camera_body_profile_id;
  char metadata_make[LARDON3D_OPTICAL_TEXT_CAPACITY];
  char metadata_model[LARDON3D_OPTICAL_TEXT_CAPACITY];
} Lardon3DOpticalCameraBodyAlias;

typedef struct {
  uint64_t lens_profile_id;
  char manufacturer[LARDON3D_OPTICAL_TEXT_CAPACITY];
  char model[LARDON3D_OPTICAL_TEXT_CAPACITY];
  char name[LARDON3D_OPTICAL_TEXT_CAPACITY];
  Lardon3DOpticalLensInterface interface_kind;
  Lardon3DOpticalFocalRangeKind focal_range_kind;
  /* Micrometres make profile identity locale-independent and exact. Unknown
   * range uses 0/0; a prime uses equal positive values; a zoom uses min<max. */
  uint32_t minimum_focal_um;
  uint32_t maximum_focal_um;
} Lardon3DOpticalLensProfile;

typedef struct {
  uint64_t alias_id;
  uint64_t lens_profile_id;
  char metadata_make[LARDON3D_OPTICAL_TEXT_CAPACITY];
  char metadata_model[LARDON3D_OPTICAL_TEXT_CAPACITY];
} Lardon3DOpticalLensAlias;

typedef struct {
  uint64_t optical_configuration_id;
  uint64_t camera_body_profile_id;
  uint64_t lens_profile_id;
  bool has_focal_length;
  uint32_t focal_length_um;
} Lardon3DOpticalConfiguration;

typedef struct {
  uint64_t campaign_task_id;
  uint32_t group_id;
  uint64_t optical_configuration_id;
} Lardon3DOpticalCampaignGroupAssignment;

typedef struct {
  uint64_t capture_id;
  uint64_t optical_configuration_id;
  Lardon3DOpticalAssignmentProvenance provenance;
  bool has_campaign_origin;
  uint64_t campaign_task_id;
  uint32_t campaign_group_id;
} Lardon3DOpticalCaptureAssignment;

typedef struct {
  uint64_t calibration_profile_id;
  uint64_t optical_configuration_id;
  /* Existing immutable sparse calibration; numerical coefficients, model,
   * dimensions and scientific provenance remain owned by that v22 object. */
  uint64_t sparse_calibration_id;
  char name[LARDON3D_OPTICAL_TEXT_CAPACITY];
  uint32_t profile_version;
  char provenance[LARDON3D_OPTICAL_PROVENANCE_CAPACITY];
  Lardon3DOpticalCalibrationApplicability applicability;
  int64_t created_at;
} Lardon3DOpticalCalibrationProfile;

typedef struct {
  uint64_t capture_id;
  uint64_t calibration_profile_id;
  uint64_t optical_configuration_id;
  uint64_t sparse_calibration_id;
} Lardon3DOpticalCaptureCalibrationSelection;

typedef enum {
  LARDON3D_OPTICAL_OBSERVATION_UNKNOWN = 1,
  LARDON3D_OPTICAL_OBSERVATION_OBSERVED = 2,
} Lardon3DOpticalObservationState;

typedef enum {
  LARDON3D_OPTICAL_STABILIZATION_UNKNOWN = 1,
  LARDON3D_OPTICAL_STABILIZATION_OFF = 2,
  LARDON3D_OPTICAL_STABILIZATION_ON = 3,
} Lardon3DOpticalStabilizationState;

typedef enum {
  LARDON3D_OPTICAL_GEOMETRIC_STATE_METADATA = 1,
  LARDON3D_OPTICAL_GEOMETRIC_STATE_CALLER_EXPLICIT = 2,
} Lardon3DOpticalGeometricStateProvenance;

typedef enum {
  LARDON3D_OPTICAL_CALIBRATION_REQUIRED = 1,
  LARDON3D_OPTICAL_CALIBRATION_RESOLVED = 2,
  LARDON3D_OPTICAL_CALIBRATION_SELECTION_REQUIRED = 3,
} Lardon3DOpticalCalibrationResolutionKind;

typedef struct {
  uint64_t capture_id;
  uint64_t optical_configuration_id;
  uint32_t state_version;
  Lardon3DOpticalGeometricStateProvenance provenance;
  Lardon3DOpticalObservationState focus_state;
  /* Focus is an exact, bounded observation token, not a physical-distance or
   * autofocus domain. Empty is required when focus is explicitly unknown. */
  char focus_observation[LARDON3D_OPTICAL_TEXT_CAPACITY];
  Lardon3DOpticalObservationState aperture_state;
  uint32_t aperture_x1000;
  Lardon3DOpticalStabilizationState stabilization;
  Lardon3DOpticalObservationState crop_state;
  char crop_observation[LARDON3D_OPTICAL_TEXT_CAPACITY];
  Lardon3DOpticalObservationState pipeline_state;
  char pipeline_observation[LARDON3D_OPTICAL_TEXT_CAPACITY];
  Lardon3DOpticalObservationState representation_state;
  char representation_observation[LARDON3D_OPTICAL_TEXT_CAPACITY];
  Lardon3DOpticalObservationState decoded_geometry_state;
  uint32_t decoded_width;
  uint32_t decoded_height;
} Lardon3DOpticalCaptureGeometricState;

typedef struct {
  uint64_t applicability_id;
  uint64_t calibration_profile_id;
  uint64_t optical_configuration_id;
  uint64_t exemplar_capture_id;
} Lardon3DOpticalCalibrationApplicabilityV2;

typedef struct {
  uint64_t focus_domain_id;
  /* The v26 applicability retains calibration/configuration identity and its
   * exemplar retains the exact complete non-focus geometric tuple. */
  uint64_t applicability_id;
  uint64_t calibration_profile_id;
  uint64_t optical_configuration_id;
  uint64_t exemplar_capture_id;
  uint32_t domain_version;
  unsigned char evidence_sha256[LARDON3D_OPTICAL_FOCUS_DOMAIN_DIGEST_SIZE];
  uint32_t token_count;
} Lardon3DOpticalFocusDomainV2;

typedef struct {
  Lardon3DOpticalCalibrationResolutionKind kind;
  uint64_t applicability_id;
  uint64_t calibration_profile_id;
  uint64_t sparse_calibration_id;
} Lardon3DOpticalCalibrationResolutionV2;

typedef struct {
  uint64_t capture_id;
  uint64_t applicability_id;
  uint64_t calibration_profile_id;
  uint64_t optical_configuration_id;
  uint64_t sparse_calibration_id;
} Lardon3DOpticalCaptureCalibrationSelectionV2;

/* All profile/config creation calls borrow input only for the call and return a
 * caller-owned, NUL-terminated copy; input and output storage must not overlap.
 * The generated row-ID field in a create input must be zero; referenced IDs
 * must fit positive SQLite INTEGER values.
 * For otherwise valid arguments, every non-OK scalar-load/create result zeroes
 * its output. List capacity must be 1..OPTICAL_PAGE_MAX; failures clear the
 * caller-owned item array/count and restore next_after_id to the supplied
 * cursor. Successful pages are ascending. Exact natural-identity retries return
 * the existing row; the same user-visible identity with conflicting properties
 * is CONSTRAINT. A stored row whose own SQLite types or property bounds are
 * malformed is CORRUPT rather than a valid property conflict. */
Lardon3DProjectDbResult lardon3d_optical_camera_body_create(
    Lardon3DProjectDb *database, const Lardon3DOpticalCameraBodyProfile *input,
    Lardon3DOpticalCameraBodyProfile *output);
Lardon3DProjectDbResult lardon3d_optical_camera_body_load(
    Lardon3DProjectDb *database, uint64_t camera_body_profile_id,
    Lardon3DOpticalCameraBodyProfile *output);
Lardon3DProjectDbResult lardon3d_optical_camera_body_list(
    Lardon3DProjectDb *database, uint64_t after_profile_id,
    Lardon3DOpticalCameraBodyProfile *items, size_t capacity, size_t *count,
    uint64_t *next_after_profile_id);

/* Aliases are exact BINARY metadata make/model pairs: no case folding, fuzzy
 * lookup, EXIF inference, path lookup, or default device branch is performed.
 * Text inputs are borrowed for the call and must not overlap output storage. A
 * pair maps to at most one profile. Alias lists are ordered by alias_id. */
Lardon3DProjectDbResult lardon3d_optical_camera_body_alias_add(
    Lardon3DProjectDb *database, uint64_t camera_body_profile_id,
    const char *metadata_make, const char *metadata_model,
    Lardon3DOpticalCameraBodyAlias *output);
Lardon3DProjectDbResult lardon3d_optical_camera_body_find_exact_alias(
    Lardon3DProjectDb *database, const char *metadata_make,
    const char *metadata_model, Lardon3DOpticalCameraBodyProfile *output);
Lardon3DProjectDbResult lardon3d_optical_camera_body_alias_list(
    Lardon3DProjectDb *database, uint64_t camera_body_profile_id,
    uint64_t after_alias_id, Lardon3DOpticalCameraBodyAlias *items,
    size_t capacity, size_t *count, uint64_t *next_after_alias_id);

Lardon3DProjectDbResult lardon3d_optical_lens_create(
    Lardon3DProjectDb *database, const Lardon3DOpticalLensProfile *input,
    Lardon3DOpticalLensProfile *output);
Lardon3DProjectDbResult lardon3d_optical_lens_load(
    Lardon3DProjectDb *database, uint64_t lens_profile_id,
    Lardon3DOpticalLensProfile *output);
Lardon3DProjectDbResult lardon3d_optical_lens_list(
    Lardon3DProjectDb *database, uint64_t after_profile_id,
    Lardon3DOpticalLensProfile *items, size_t capacity, size_t *count,
    uint64_t *next_after_profile_id);
Lardon3DProjectDbResult lardon3d_optical_lens_alias_add(
    Lardon3DProjectDb *database, uint64_t lens_profile_id,
    const char *metadata_make, const char *metadata_model,
    Lardon3DOpticalLensAlias *output);
Lardon3DProjectDbResult lardon3d_optical_lens_find_exact_alias(
    Lardon3DProjectDb *database, const char *metadata_make,
    const char *metadata_model, Lardon3DOpticalLensProfile *output);
Lardon3DProjectDbResult lardon3d_optical_lens_alias_list(
    Lardon3DProjectDb *database, uint64_t lens_profile_id,
    uint64_t after_alias_id, Lardon3DOpticalLensAlias *items, size_t capacity,
    size_t *count, uint64_t *next_after_alias_id);

/* A configuration is the exact body+lens context plus an optional focal value
 * encoded as integer micrometres. Known prime/zoom bounds are enforced. A
 * missing focal value is not an inferred value and distinct 16/24/50 mm
 * configurations remain distinct rows. */
Lardon3DProjectDbResult lardon3d_optical_configuration_create(
    Lardon3DProjectDb *database, const Lardon3DOpticalConfiguration *input,
    Lardon3DOpticalConfiguration *output);
Lardon3DProjectDbResult lardon3d_optical_configuration_load(
    Lardon3DProjectDb *database, uint64_t optical_configuration_id,
    Lardon3DOpticalConfiguration *output);
Lardon3DProjectDbResult lardon3d_optical_configuration_list(
    Lardon3DProjectDb *database, uint64_t after_configuration_id,
    Lardon3DOpticalConfiguration *items, size_t capacity, size_t *count,
    uint64_t *next_after_configuration_id);

/* A one-based campaign group can be assigned only while its durable cursor is
 * zero and no Capture mapping exists. Exact retries remain idempotent; another
 * configuration is CONSTRAINT. Unassigned groups are valid and unresolved.
 * Task/group IDs remain operational provenance and never become Capture
 * identity. */
Lardon3DProjectDbResult lardon3d_optical_campaign_group_assign(
    Lardon3DProjectDb *database, uint64_t campaign_task_id, uint32_t group_id,
    uint64_t optical_configuration_id);
Lardon3DProjectDbResult lardon3d_optical_campaign_group_load(
    Lardon3DProjectDb *database, uint64_t campaign_task_id, uint32_t group_id,
    Lardon3DOpticalCampaignGroupAssignment *output);

/* Explicit post-import assignment is allowed only when the Capture has no
 * assignment. Its exact retry is idempotent and any other configuration or
 * campaign provenance conflicts. Absence (NOT_FOUND on load) is the sole
 * unresolved representation; no fabricated "unknown" profile is created. */
Lardon3DProjectDbResult lardon3d_optical_capture_assign_explicit(
    Lardon3DProjectDb *database, uint64_t capture_id,
    uint64_t optical_configuration_id);
Lardon3DProjectDbResult lardon3d_optical_capture_assignment_load(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureAssignment *output);

/* Calibration profiles bind metadata to an existing immutable sparse
 * calibration and exactly one optical configuration. Creation never computes,
 * copies, interpolates, or fabricates calibration coefficients. Exact creation
 * retry is idempotent and a valid immutable-property conflict is CONSTRAINT;
 * malformed durable profile state or its sparse-calibration dependency is
 * CORRUPT and takes precedence over caller-property comparison. Compatible lists
 * are bounded and ordered by calibration_profile_id; zero rows is valid. */
Lardon3DProjectDbResult lardon3d_optical_calibration_profile_create(
    Lardon3DProjectDb *database,
    const Lardon3DOpticalCalibrationProfile *input,
    Lardon3DOpticalCalibrationProfile *output);
Lardon3DProjectDbResult lardon3d_optical_calibration_profile_load(
    Lardon3DProjectDb *database, uint64_t calibration_profile_id,
    Lardon3DOpticalCalibrationProfile *output);
Lardon3DProjectDbResult lardon3d_optical_calibration_profile_list_compatible(
    Lardon3DProjectDb *database, uint64_t optical_configuration_id,
    uint64_t after_profile_id, Lardon3DOpticalCalibrationProfile *items,
    size_t capacity, size_t *count, uint64_t *next_after_profile_id);

/* Selection is always explicit. The Capture assignment and profile must name
 * exactly the same configuration; incompatibility is CONSTRAINT. Exact retry
 * is idempotent, conflict is rejected, and multiple compatible profiles remain
 * ambiguous/NOT_FOUND until one is selected. */
Lardon3DProjectDbResult lardon3d_optical_capture_calibration_select(
    Lardon3DProjectDb *database, uint64_t capture_id,
    uint64_t calibration_profile_id);
Lardon3DProjectDbResult lardon3d_optical_capture_calibration_selection_load(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureCalibrationSelection *output);

/* v26 state is Capture-owned and separate from both Capture identity and the
 * v23 body/lens/focal configuration. Unknown values remain explicit. Create is
 * immutable: an exact retry is idempotent and any differing tuple conflicts. */
Lardon3DProjectDbResult lardon3d_optical_capture_geometric_state_create(
    Lardon3DProjectDb *database,
    const Lardon3DOpticalCaptureGeometricState *input,
    Lardon3DOpticalCaptureGeometricState *output);
Lardon3DProjectDbResult lardon3d_optical_capture_geometric_state_load(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureGeometricState *output);

/* Applicability binds an existing v1 calibration profile to the exemplar's
 * exact configuration and complete observed-state tuple. It authorizes no
 * body/lens/focal substitution, unknown default, interpolation or extrapolation. */
Lardon3DProjectDbResult lardon3d_optical_calibration_applicability_v2_create(
    Lardon3DProjectDb *database, uint64_t calibration_profile_id,
    uint64_t exemplar_capture_id,
    Lardon3DOpticalCalibrationApplicabilityV2 *output);

/* Attach one physically validated discrete focus domain to an existing exact
 * v26 applicability. evidence_sha256 must be a nonzero retained-evidence or
 * provenance digest. focus_tokens contains 1..FOCUS_DOMAIN_TOKEN_MAX distinct,
 * nonempty, NUL-terminated opaque tokens; the call borrows the array and
 * strings only for its duration. Members are stored as a deterministic exact
 * set: order has no meaning, exact retry is idempotent, and conflicting reuse
 * of the applicability is CONSTRAINT. The API performs no numeric conversion,
 * EXIF interpretation, interpolation, extrapolation, or physical validation.
 * output is mandatory. On OK, it receives the durable focus domain and its
 * attached applicability identity; for valid calls it is zeroed on non-OK. */
Lardon3DProjectDbResult lardon3d_optical_focus_domain_v2_create(
    Lardon3DProjectDb *database, uint64_t applicability_id,
    uint32_t domain_version,
    const unsigned char evidence_sha256[LARDON3D_OPTICAL_FOCUS_DOMAIN_DIGEST_SIZE],
    const char *const *focus_tokens, size_t token_count,
    Lardon3DOpticalFocusDomainV2 *output);

/* Resolution counts distinct eligible applicability rows: exact v26 state
 * matches plus v27 domains whose non-focus geometry is exact and whose focus
 * token is an exact member. NONE is CALIBRATION_REQUIRED, ONE is RESOLVED, and
 * MANY is SELECTION_REQUIRED. All are successful outcomes. */
Lardon3DProjectDbResult lardon3d_optical_capture_calibration_resolve_v2(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCalibrationResolutionV2 *output);
/* Selection requires a complete observed geometric-state tuple and an
 * applicability currently eligible by exact v26 match or exact v27 domain
 * membership; UNKNOWN in any geometry-relevant field is a CONSTRAINT. The
 * first selection is immutable, exact retry is idempotent, and a conflicting
 * applicability is rejected without changing the selection. */
Lardon3DProjectDbResult lardon3d_optical_capture_calibration_select_v2(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t applicability_id);
/* Loads the Capture's durable v2 selection without creating or changing it.
 * NOT_FOUND means no selection exists. OK returns the complete stored tuple
 * only when its applicability remains exactly compatible with the Capture's
 * complete observed geometric state; broken dependencies are CORRUPT. */
Lardon3DProjectDbResult lardon3d_optical_capture_calibration_selection_load_v2(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureCalibrationSelectionV2 *output);

#ifdef __cplusplus
}
#endif

#endif
