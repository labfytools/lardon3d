#ifndef LARDON3D_PROJECT_DB_H
#define LARDON3D_PROJECT_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/task.h>
#include <lardon3d/sparse_sfm_incremental.h>

enum {
  LARDON3D_PROJECT_DB_SCHEMA_VERSION = 19,
  LARDON3D_PROJECT_DB_ID_CAPACITY = 65,
  LARDON3D_PROJECT_DB_KIND_CAPACITY = 65,
  LARDON3D_PROJECT_DB_PATH_CAPACITY = 4096,
  LARDON3D_PROJECT_DB_ERROR_CAPACITY = 256,
  LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX = 256,
  LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX = 256,
  LARDON3D_PROJECT_DB_CANDIDATE_PAIR_PAGE_MAX = 256,
  LARDON3D_PROJECT_DB_MATCH_RESULT_PAGE_MAX = 256,
  LARDON3D_PROJECT_DB_GEOMETRIC_RESULT_PAGE_MAX = 256,
  LARDON3D_PROJECT_DB_TRACK_PAGE_MAX = 64,
  LARDON3D_PROJECT_DB_INLIER_MASK_MAX = 1024,
  LARDON3D_PROJECT_DB_FUNDAMENTAL_COEFFICIENTS = 9,
  LARDON3D_PROJECT_DB_SCANSET_NAME_CAPACITY = 256,
  LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY = 256,
  LARDON3D_PROJECT_DB_SHA256_SIZE = 32,
};

typedef struct Lardon3DProjectDb Lardon3DProjectDb;

typedef enum {
  LARDON3D_PROJECT_DB_OK = 0,
  LARDON3D_PROJECT_DB_INVALID_ARGUMENT,
  LARDON3D_PROJECT_DB_NOT_FOUND,
  LARDON3D_PROJECT_DB_BUSY,
  LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA,
  LARDON3D_PROJECT_DB_CORRUPT,
  LARDON3D_PROJECT_DB_CONSTRAINT,
  LARDON3D_PROJECT_DB_IO_ERROR
} Lardon3DProjectDbResult;

typedef enum {
  LARDON3D_DB_CHECKPOINT_DURABLE = 0,
  LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE
} Lardon3DProjectDbCheckpointDurability;

typedef enum {
  LARDON3D_DB_ARTIFACT_STAGED = 0,
  LARDON3D_DB_ARTIFACT_READY
} Lardon3DProjectDbArtifactState;

typedef struct {
  char stable_id[LARDON3D_PROJECT_DB_ID_CAPACITY];
  char name[LARDON3D_TASK_NAME_CAPACITY];
  int64_t created_at;
  int64_t updated_at;
} Lardon3DProjectDbProject;

typedef struct {
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint32_t format_version;
  Lardon3DProjectDbCheckpointDurability durability;
  int64_t updated_at;
} Lardon3DProjectDbCheckpoint;

typedef struct {
  uint64_t task_id;
  char name[LARDON3D_TASK_NAME_CAPACITY];
  bool has_task_kind;
  char task_kind[LARDON3D_TASK_KIND_CAPACITY];
  uint32_t task_kind_version;
  Lardon3DTaskState saved_state;
  Lardon3DTaskState recovery_state;
  unsigned int progress;
  unsigned int sequence_count;
  struct timespec started_at;
  struct timespec finished_at;
  int64_t updated_at;
  bool has_checkpoint;
  Lardon3DProjectDbCheckpoint checkpoint;
} Lardon3DProjectDbTask;

typedef struct {
  char artifact_id[LARDON3D_PROJECT_DB_ID_CAPACITY];
  char kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  Lardon3DProjectDbArtifactState state;
  uint64_t size_bytes;
  bool has_producer_task;
  uint64_t producer_task_id;
  int64_t created_at;
  int64_t updated_at;
} Lardon3DProjectDbArtifact;

typedef struct {
  uint64_t task_id;
  char source_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint64_t scanset_id;
} Lardon3DProjectDbImageImport;

typedef struct {
  uint64_t scanset_id;
  char name[LARDON3D_PROJECT_DB_SCANSET_NAME_CAPACITY];
  int64_t created_at;
  int64_t updated_at;
} Lardon3DProjectDbScanSet;

typedef enum { LARDON3D_DB_IMAGE_ASSET_READY = 1 } Lardon3DProjectDbImageAssetState;

typedef struct {
  uint64_t asset_id;
  unsigned char sha256[LARDON3D_PROJECT_DB_SHA256_SIZE];
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint64_t size_bytes;
  Lardon3DProjectDbImageAssetState state;
  int64_t created_at;
} Lardon3DProjectDbImageAsset;

typedef struct {
  uint64_t image_id;
  uint64_t scanset_id;
  uint64_t asset_id;
  char original_name[LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY];
  char source_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  bool has_producer_task;
  uint64_t producer_task_id;
  int64_t imported_at;
} Lardon3DProjectDbImage;

typedef struct {
  uint64_t capture_id;
  uint64_t scanset_id;
  int64_t created_at;
} Lardon3DProjectDbCapture;

typedef enum {
  LARDON3D_DB_CAPTURE_ASSET_SOURCE = 1,
  LARDON3D_DB_CAPTURE_ASSET_DERIVED = 2,
} Lardon3DProjectDbCaptureAssetRole;

typedef struct {
  uint64_t capture_id;
  uint64_t asset_id;
  Lardon3DProjectDbCaptureAssetRole role;
} Lardon3DProjectDbCaptureAsset;

typedef enum {
  LARDON3D_DB_ASSET_DERIVATION_GENERIC_VERSIONED = 1,
} Lardon3DProjectDbAssetDerivationKind;

typedef struct {
  uint64_t parent_asset_id;
  uint64_t child_asset_id;
  Lardon3DProjectDbAssetDerivationKind kind;
  uint32_t version;
  unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  bool has_producer_task;
  uint64_t producer_task_id;
  int64_t created_at;
} Lardon3DProjectDbAssetDerivation;

typedef struct {
  uint64_t candidate_pair_id;
  uint64_t image_id_a;
  uint64_t image_id_b;
  int64_t created_at;
} Lardon3DProjectDbCandidatePair;

typedef struct {
  uint64_t task_id;
  uint64_t after_candidate_pair_id;
  char feature_extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t feature_extractor_version;
  unsigned char feature_parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  int matcher_kind;
  float ratio_threshold;
} Lardon3DProjectDbMatcherTask;

typedef struct {
  uint64_t task_id;
  uint64_t after_match_result_id;
  double threshold_pixels;
  double confidence;
  uint32_t max_iterations;
  uint32_t min_inlier_count;
  double min_inlier_ratio;
  uint32_t seed_policy_version;
  uint32_t canonicalization_version;
  unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
} Lardon3DProjectDbGeometricVerifierTask;

typedef struct {
  uint64_t task_id;
  char builder_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t builder_version;
  unsigned char builder_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  int verifier_kind;
  uint32_t verifier_version;
  unsigned char verifier_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  unsigned char input_scope_hash[LARDON3D_PROJECT_DB_SHA256_SIZE];
  uint64_t gvr_count;
  char scope_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint64_t scope_size_bytes;
  unsigned char scope_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE];
  uint32_t scope_format_version;
} Lardon3DProjectDbTrackBuilderTask;

typedef struct {
  uint64_t match_result_id;
  uint64_t candidate_pair_id;
  uint64_t feature_set_id_a;
  uint64_t feature_set_id_b;
  char matcher_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t matcher_version;
  unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  int result_status;
  uint32_t match_count;
  bool has_match_asset;
  unsigned char match_asset_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE];
  char match_asset_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint64_t match_asset_size_bytes;
  int64_t created_at;
} Lardon3DProjectDbMatchResult;

typedef enum {
  LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL = 1,
} Lardon3DGeometricVerifierKind;

typedef enum {
  LARDON3D_GEOMETRIC_REJECTED = 1,
  LARDON3D_GEOMETRIC_VERIFIED = 2,
} Lardon3DGeometricVerificationStatus;

typedef struct {
  uint64_t geometric_verification_result_id;
  uint64_t match_result_id;
  Lardon3DGeometricVerifierKind verifier_kind;
  uint32_t verifier_version;
  unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  Lardon3DGeometricVerificationStatus status;
  uint32_t inlier_count;
  size_t inlier_mask_size;
  unsigned char inlier_mask[LARDON3D_PROJECT_DB_INLIER_MASK_MAX];
  bool has_model;
  double model[LARDON3D_PROJECT_DB_FUNDAMENTAL_COEFFICIENTS];
  int64_t created_at;
} Lardon3DProjectDbGeometricVerificationResult;

typedef enum {
  LARDON3D_PROJECT_DB_IMAGE_REGISTERED = 0,
  LARDON3D_PROJECT_DB_IMAGE_ALREADY_PRESENT
} Lardon3DProjectDbImageRegisterStatus;

enum {
  LARDON3D_MATCH_RESULT_STATUS_NO_MATCH = 0,
  LARDON3D_MATCH_RESULT_STATUS_MATCHED = 1,
};

typedef enum {
  LARDON3D_DB_FEATURE_ASSET_DURABLE = 0,
  LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE = 1
} Lardon3DProjectDbFeatureDurability;

typedef struct {
  uint64_t feature_asset_id;
  unsigned char sha256[32];
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint64_t size_bytes;
  Lardon3DProjectDbFeatureDurability durability;
  int64_t created_at;
} Lardon3DProjectDbFeatureAsset;

typedef struct {
  uint64_t feature_set_id;
  uint64_t image_id;
  uint64_t feature_asset_id;
  char extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t extractor_version;
  unsigned char parameter_fingerprint[32];
  unsigned char source_image_sha256[32];
  uint32_t feature_count;
  uint32_t descriptor_type;
  uint32_t descriptor_dimension;
  uint32_t occupied_cells;
  uint32_t total_cells;
  double coverage_ratio;
  double feature_density_per_megapixel;
  bool has_producer_task;
  uint64_t producer_task_id;
  int64_t created_at;
  Lardon3DProjectDbFeatureAsset asset;
} Lardon3DProjectDbFeatureSet;

typedef struct {
  uint64_t task_id;
  uint64_t image_id;
  char extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t extractor_version;
  uint32_t max_features;
  uint32_t pyramid_levels;
  uint32_t fast_threshold;
  unsigned char parameter_fingerprint[32];
} Lardon3DProjectDbFeatureExtractTask;

typedef struct {
  uint64_t task_id;
  uint64_t image_id;
  char extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t extractor_version;
  uint32_t max_features;
  uint32_t octave_layers;
  double contrast_threshold;
  double edge_threshold;
  double sigma;
  uint32_t grid_rows;
  uint32_t grid_cols;
  uint32_t max_features_per_cell;
  unsigned char parameter_fingerprint[32];
} Lardon3DProjectDbSiftExtractTask;

typedef struct {
  uint64_t feature_support_set_id;
  uint64_t image_id;
  uint64_t first_feature_set_id;
  uint64_t second_feature_set_id;
  double radius_pixels;
  unsigned char parameter_fingerprint[32];
  uint32_t group_count;
  int64_t created_at;
} Lardon3DProjectDbFeatureSupportSet;

typedef struct {
  uint64_t feature_support_group_id;
  uint64_t feature_support_set_id;
  double x;
  double y;
  double distance_pixels;
  uint32_t support_count;
  bool first_member_from_second_set;
  uint32_t first_feature_index;
  bool has_second_feature;
  uint32_t second_feature_index;
} Lardon3DProjectDbFeatureSupportGroup;

typedef enum {
  LARDON3D_DB_VISUAL_INDEX_DURABLE = 0,
  LARDON3D_DB_VISUAL_INDEX_PUBLISHED_NOT_DURABLE = 1
} Lardon3DProjectDbVisualIndexDurability;

typedef struct {
  uint64_t visual_index_id;
  char index_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t index_version;
  uint32_t descriptor_type;
  uint32_t descriptor_dimension;
  char extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t extractor_version;
  unsigned char feature_parameter_fingerprint[32];
  unsigned char index_parameter_fingerprint[32];
  uint32_t table_count;
  uint32_t key_bits;
  uint32_t max_features_per_set;
  uint32_t max_bucket_postings;
  int64_t created_at;
} Lardon3DProjectDbVisualIndex;

typedef struct {
  uint64_t visual_index_segment_id;
  uint64_t visual_index_id;
  uint64_t generation;
  unsigned char sha256[32];
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  uint64_t size_bytes;
  uint64_t posting_count;
  uint32_t feature_set_count;
  Lardon3DProjectDbVisualIndexDurability durability;
  bool has_producer_task;
  uint64_t producer_task_id;
  int64_t created_at;
} Lardon3DProjectDbVisualIndexSegment;

typedef struct {
  uint64_t task_id;
  uint64_t visual_index_id;
  uint64_t after_feature_set_id;
} Lardon3DProjectDbVisualIndexUpdateTask;

typedef struct {
  uint64_t task_id;
  uint64_t visual_index_id;
  uint64_t after_feature_set_id;
  uint32_t top_k;
  uint32_t minimum_evidence_count;
  int scanset_filter;
  bool exclude_same_asset;
} Lardon3DProjectDbCandidatePairGenerateTask;

typedef struct {
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint32_t position_in_track;
} Lardon3DProjectDbTrackObservation;

typedef struct {
  uint64_t track_id;
  uint64_t track_set_id;
  uint32_t observation_count;
  Lardon3DProjectDbTrackObservation *observations;
} Lardon3DProjectDbTrack;

typedef struct {
  uint64_t track_set_id;
  char builder_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t builder_version;
  unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  int verifier_kind;
  uint32_t verifier_version;
  unsigned char verifier_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  unsigned char input_scope_hash[LARDON3D_PROJECT_DB_SHA256_SIZE];
  uint64_t gvr_count;
  uint64_t track_count;
  int64_t created_at;
} Lardon3DProjectDbTrackSet;

typedef struct {
  uint64_t task_id;
  uint64_t track_set_id;
  uint64_t calibration_scope_id;
  uint32_t sfm_kind;
  uint32_t sfm_version;
  Lardon3DSparseIncrementalParameters parameters;
} Lardon3DProjectDbSparseSfmTask;

typedef struct {
  uint64_t task_id;
  uint64_t base_reconstruction_id;
  uint64_t extension_track_set_id;
  uint64_t calibration_scope_id;
  uint32_t incremental_kind;
  uint32_t incremental_version;
  unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
} Lardon3DProjectDbIncrementalReconstructionTask;

void lardon3d_project_db_free_track(Lardon3DProjectDbTrack *track);
Lardon3DProjectDbResult lardon3d_project_db_create_track_set(
    Lardon3DProjectDb *database, const Lardon3DProjectDbTrackSet *configuration,
    const Lardon3DProjectDbTrack *tracks, size_t track_count,
    Lardon3DProjectDbTrackSet *published);
Lardon3DProjectDbResult lardon3d_project_db_load_track_set(
    Lardon3DProjectDb *database, uint64_t track_set_id, Lardon3DProjectDbTrackSet *track_set);
Lardon3DProjectDbResult lardon3d_project_db_find_track_set(
    Lardon3DProjectDb *database, const Lardon3DProjectDbTrackSet *identity,
    Lardon3DProjectDbTrackSet *track_set);
Lardon3DProjectDbResult lardon3d_project_db_list_track_sets(
    Lardon3DProjectDb *database, uint64_t after_track_set_id, Lardon3DProjectDbTrackSet *track_sets,
    size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_load_track(
    Lardon3DProjectDb *database, uint64_t track_id, Lardon3DProjectDbTrack *track);
Lardon3DProjectDbResult lardon3d_project_db_list_tracks(
    Lardon3DProjectDb *database, uint64_t track_set_id, uint64_t after_track_id,
    Lardon3DProjectDbTrack *tracks, size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_find_track_by_observation(
    Lardon3DProjectDb *database, uint64_t track_set_id, uint64_t feature_set_id,
    uint32_t feature_index, Lardon3DProjectDbTrack *track);

Lardon3DProjectDbResult lardon3d_project_db_open(const char *path, Lardon3DProjectDb **database,
                                                 char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]);
void lardon3d_project_db_close(Lardon3DProjectDb *database);
bool lardon3d_project_db_last_error(Lardon3DProjectDb *database,
                                    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]);
Lardon3DProjectDbResult lardon3d_project_db_legacy_catalog_pending(Lardon3DProjectDb *database,
                                                                   bool *pending);
unsigned int lardon3d_project_db_schema_version(Lardon3DProjectDb *database);

Lardon3DProjectDbResult lardon3d_project_db_set_project(Lardon3DProjectDb *database,
                                                        const Lardon3DProjectDbProject *project);
Lardon3DProjectDbResult lardon3d_project_db_get_project(Lardon3DProjectDb *database,
                                                        Lardon3DProjectDbProject *project);
Lardon3DProjectDbResult lardon3d_project_db_record_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot, const char *task_kind,
    uint32_t task_kind_version, const Lardon3DProjectDbCheckpoint *checkpoint, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_record_image_import_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot, const char *task_kind,
    uint32_t task_kind_version, const Lardon3DProjectDbCheckpoint *checkpoint,
    const char *source_path, uint64_t scanset_id, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_create_scanset(Lardon3DProjectDb *database,
                                                           const char *name,
                                                           Lardon3DProjectDbScanSet *scanset);
Lardon3DProjectDbResult lardon3d_project_db_load_scanset(Lardon3DProjectDb *database,
                                                         uint64_t scanset_id,
                                                         Lardon3DProjectDbScanSet *scanset);
Lardon3DProjectDbResult lardon3d_project_db_list_scansets(Lardon3DProjectDb *database,
                                                          uint64_t after_scanset_id,
                                                          Lardon3DProjectDbScanSet *scansets,
                                                          size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_register_image(
    Lardon3DProjectDb *database, uint64_t scanset_id,
    const unsigned char sha256[LARDON3D_PROJECT_DB_SHA256_SIZE], const char *asset_path,
    uint64_t size_bytes, const char *original_name, const char *source_path,
    uint64_t producer_task_id, int64_t imported_at, Lardon3DProjectDbImageRegisterStatus *status,
    Lardon3DProjectDbImage *image);
Lardon3DProjectDbResult lardon3d_project_db_register_image_asset(
    Lardon3DProjectDb *database,
    const unsigned char sha256[LARDON3D_PROJECT_DB_SHA256_SIZE], const char *asset_path,
    uint64_t size_bytes, int64_t created_at, Lardon3DProjectDbImageAsset *asset);
Lardon3DProjectDbResult lardon3d_project_db_load_image(Lardon3DProjectDb *database,
                                                       uint64_t image_id,
                                                       Lardon3DProjectDbImage *image,
                                                       Lardon3DProjectDbImageAsset *asset);
Lardon3DProjectDbResult lardon3d_project_db_list_images(Lardon3DProjectDb *database,
                                                        uint64_t scanset_id,
                                                        uint64_t after_image_id,
                                                        Lardon3DProjectDbImage *images,
                                                        Lardon3DProjectDbImageAsset *assets,
                                                        size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_count_images(Lardon3DProjectDb *database,
                                                         uint64_t scanset_id, uint64_t *count);
Lardon3DProjectDbResult lardon3d_project_db_create_capture(
    Lardon3DProjectDb *database, uint64_t scanset_id, int64_t created_at,
    Lardon3DProjectDbCapture *capture);
Lardon3DProjectDbResult lardon3d_project_db_load_capture(
    Lardon3DProjectDb *database, uint64_t capture_id, Lardon3DProjectDbCapture *capture);
Lardon3DProjectDbResult lardon3d_project_db_list_captures(
    Lardon3DProjectDb *database, uint64_t scanset_id, uint64_t after_capture_id,
    Lardon3DProjectDbCapture *captures, size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_attach_capture_asset(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t asset_id,
    Lardon3DProjectDbCaptureAssetRole role);
Lardon3DProjectDbResult lardon3d_project_db_list_capture_assets(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t after_asset_id,
    Lardon3DProjectDbCaptureAsset *assets, size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_attach_capture_image(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t image_id);
Lardon3DProjectDbResult lardon3d_project_db_find_capture_for_image(
    Lardon3DProjectDb *database, uint64_t image_id, Lardon3DProjectDbCapture *capture);
Lardon3DProjectDbResult lardon3d_project_db_record_asset_derivation(
    Lardon3DProjectDb *database, const Lardon3DProjectDbAssetDerivation *derivation);
Lardon3DProjectDbResult lardon3d_project_db_load_asset_derivation(
    Lardon3DProjectDb *database, uint64_t child_asset_id,
    Lardon3DProjectDbAssetDerivation *derivation);
Lardon3DProjectDbResult lardon3d_project_db_set_selected_capture_image(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t image_id);
Lardon3DProjectDbResult lardon3d_project_db_get_selected_capture_image(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t *image_id);
Lardon3DProjectDbResult lardon3d_project_db_create_candidate_pair(
    Lardon3DProjectDb *database, uint64_t image_id_a, uint64_t image_id_b, int64_t created_at,
    Lardon3DProjectDbCandidatePair *pair);
Lardon3DProjectDbResult lardon3d_project_db_load_candidate_pair(
    Lardon3DProjectDb *database, uint64_t candidate_pair_id, Lardon3DProjectDbCandidatePair *pair);
Lardon3DProjectDbResult lardon3d_project_db_find_candidate_pair(
    Lardon3DProjectDb *database, uint64_t image_id_a, uint64_t image_id_b,
    Lardon3DProjectDbCandidatePair *pair);
Lardon3DProjectDbResult lardon3d_project_db_list_candidate_pairs(
    Lardon3DProjectDb *database, uint64_t after_candidate_pair_id,
    Lardon3DProjectDbCandidatePair *pairs, size_t capacity, size_t *count);
Lardon3DProjectDbResult
lardon3d_project_db_load_image_import(Lardon3DProjectDb *database, uint64_t task_id,
                                      Lardon3DProjectDbImageImport *parameters);
Lardon3DProjectDbResult lardon3d_project_db_allocate_task_id(Lardon3DProjectDb *database,
                                                             uint64_t *task_id);
Lardon3DProjectDbResult lardon3d_project_db_load_task(Lardon3DProjectDb *database, uint64_t task_id,
                                                      Lardon3DProjectDbTask *task);
Lardon3DProjectDbResult lardon3d_project_db_list_recoverable(Lardon3DProjectDb *database,
                                                             uint64_t after_task_id,
                                                             Lardon3DProjectDbTask *tasks,
                                                             size_t capacity, size_t *count);
Lardon3DProjectDbResult
lardon3d_project_db_create_artifact(Lardon3DProjectDb *database,
                                    const Lardon3DProjectDbArtifact *artifact);
Lardon3DProjectDbResult lardon3d_project_db_mark_artifact_ready(Lardon3DProjectDb *database,
                                                                const char *artifact_id,
                                                                int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_artifact(Lardon3DProjectDb *database,
                                                          const char *artifact_id,
                                                          Lardon3DProjectDbArtifact *artifact);
Lardon3DProjectDbResult lardon3d_project_db_record_feature_extract_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot, const char *task_kind,
    uint32_t task_kind_version, const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbFeatureExtractTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult
lardon3d_project_db_load_feature_extract_task(Lardon3DProjectDb *database, uint64_t task_id,
                                              Lardon3DProjectDbFeatureExtractTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_record_sift_extract_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind, uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbSiftExtractTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_sift_extract_task(
    Lardon3DProjectDb *database, uint64_t task_id, Lardon3DProjectDbSiftExtractTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_register_feature_set(
    Lardon3DProjectDb *database, uint64_t image_id, const char *extractor_kind,
    uint32_t extractor_version, const unsigned char parameter_fingerprint[32],
    const unsigned char source_image_sha256[32], uint32_t feature_count, uint32_t descriptor_type,
    uint32_t descriptor_dimension, const unsigned char asset_sha256[32], const char *asset_path,
    uint64_t asset_size_bytes, Lardon3DProjectDbFeatureDurability durability,
    uint64_t producer_task_id, int64_t created_at, Lardon3DProjectDbFeatureSet *feature_set);
Lardon3DProjectDbResult lardon3d_project_db_register_feature_set_quality(
    Lardon3DProjectDb *database, uint64_t image_id, const char *extractor_kind,
    uint32_t extractor_version, const unsigned char parameter_fingerprint[32],
    const unsigned char source_image_sha256[32], uint32_t feature_count, uint32_t descriptor_type,
    uint32_t descriptor_dimension, uint32_t occupied_cells, uint32_t total_cells,
    double coverage_ratio, double feature_density_per_megapixel,
    const unsigned char asset_sha256[32], const char *asset_path, uint64_t asset_size_bytes,
    Lardon3DProjectDbFeatureDurability durability, uint64_t producer_task_id, int64_t created_at,
    Lardon3DProjectDbFeatureSet *feature_set);
Lardon3DProjectDbResult
lardon3d_project_db_find_feature_set(Lardon3DProjectDb *database, uint64_t image_id,
                                     const char *extractor_kind, uint32_t extractor_version,
                                     const unsigned char parameter_fingerprint[32],
                                     Lardon3DProjectDbFeatureSet *feature_set);
Lardon3DProjectDbResult
lardon3d_project_db_load_feature_set(Lardon3DProjectDb *database, uint64_t feature_set_id,
                                     Lardon3DProjectDbFeatureSet *feature_set);
Lardon3DProjectDbResult
lardon3d_project_db_list_feature_sets(Lardon3DProjectDb *database, uint64_t after_feature_set_id,
                                      Lardon3DProjectDbFeatureSet *feature_sets, size_t capacity,
                                      size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_publish_feature_support(
    Lardon3DProjectDb *database, const Lardon3DProjectDbFeatureSupportSet *configuration,
    const Lardon3DProjectDbFeatureSupportGroup *groups, size_t group_count,
    Lardon3DProjectDbFeatureSupportSet *published);
Lardon3DProjectDbResult lardon3d_project_db_load_feature_support(
    Lardon3DProjectDb *database, uint64_t image_id, uint64_t first_feature_set_id,
    uint64_t second_feature_set_id, const unsigned char parameter_fingerprint[32],
    Lardon3DProjectDbFeatureSupportSet *support_set);
Lardon3DProjectDbResult lardon3d_project_db_list_feature_support_groups(
    Lardon3DProjectDb *database, uint64_t feature_support_set_id,
    uint64_t after_feature_support_group_id, Lardon3DProjectDbFeatureSupportGroup *groups,
    size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_create_visual_index(
    Lardon3DProjectDb *database, const Lardon3DProjectDbVisualIndex *configuration,
    Lardon3DProjectDbVisualIndex *visual_index);
Lardon3DProjectDbResult lardon3d_project_db_load_visual_index(
    Lardon3DProjectDb *database, uint64_t visual_index_id,
    Lardon3DProjectDbVisualIndex *visual_index);
Lardon3DProjectDbResult lardon3d_project_db_list_visual_index_segments(
    Lardon3DProjectDb *database, uint64_t visual_index_id, uint64_t after_generation,
    Lardon3DProjectDbVisualIndexSegment *segments, size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_list_visual_index_pending(
    Lardon3DProjectDb *database, uint64_t visual_index_id, uint64_t after_feature_set_id,
    Lardon3DProjectDbFeatureSet *feature_sets, size_t capacity, size_t *count);
Lardon3DProjectDbResult lardon3d_project_db_publish_visual_index_segment(
    Lardon3DProjectDb *database, const Lardon3DProjectDbVisualIndexSegment *segment,
    const uint64_t *feature_set_ids, size_t feature_set_count,
    Lardon3DProjectDbVisualIndexSegment *published);
Lardon3DProjectDbResult lardon3d_project_db_record_visual_index_update_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind, uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbVisualIndexUpdateTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_visual_index_update_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbVisualIndexUpdateTask *parameters);

Lardon3DProjectDbResult lardon3d_project_db_record_candidate_pair_generate_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind, uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbCandidatePairGenerateTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_candidate_pair_generate_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbCandidatePairGenerateTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_record_matcher_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind, uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbMatcherTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_matcher_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbMatcherTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_record_geometric_verifier_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind, uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbGeometricVerifierTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_geometric_verifier_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbGeometricVerifierTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_record_track_builder_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *kind, uint32_t version, const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbTrackBuilderTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_track_builder_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbTrackBuilderTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_record_sparse_sfm_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *kind, uint32_t version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbSparseSfmTask *parameters, int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_sparse_sfm_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbSparseSfmTask *parameters);
Lardon3DProjectDbResult lardon3d_project_db_record_incremental_reconstruction_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
    const char *kind, uint32_t version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbIncrementalReconstructionTask *parameters,
    int64_t updated_at);
Lardon3DProjectDbResult lardon3d_project_db_load_incremental_reconstruction_task(
    Lardon3DProjectDb *database, uint64_t task_id,
    Lardon3DProjectDbIncrementalReconstructionTask *parameters);

Lardon3DProjectDbResult lardon3d_project_db_create_match_result(
    Lardon3DProjectDb *database, uint64_t candidate_pair_id, uint64_t feature_set_id_a,
    uint64_t feature_set_id_b, const char *matcher_kind, uint32_t matcher_version,
    const unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE], int result_status,
    uint32_t match_count,
    const unsigned char *match_asset_sha256, const char *match_asset_path,
    uint64_t match_asset_size_bytes, int64_t created_at,
    Lardon3DProjectDbMatchResult *result);
Lardon3DProjectDbResult lardon3d_project_db_load_match_result(
    Lardon3DProjectDb *database, uint64_t match_result_id, Lardon3DProjectDbMatchResult *result);
Lardon3DProjectDbResult lardon3d_project_db_repair_match_result(
    Lardon3DProjectDb *database, uint64_t match_result_id, int result_status,
    uint32_t match_count, const unsigned char *match_asset_sha256,
    const char *match_asset_path, uint64_t match_asset_size_bytes,
    Lardon3DProjectDbMatchResult *result);
Lardon3DProjectDbResult lardon3d_project_db_find_match_result(
    Lardon3DProjectDb *database, uint64_t candidate_pair_id,
    uint64_t feature_set_id_a, uint64_t feature_set_id_b,
    const char *matcher_kind, uint32_t matcher_version,
    const unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE],
    Lardon3DProjectDbMatchResult *result);
Lardon3DProjectDbResult lardon3d_project_db_list_match_results(
    Lardon3DProjectDb *database, uint64_t after_match_result_id,
    Lardon3DProjectDbMatchResult *results, size_t capacity, size_t *count);

Lardon3DProjectDbResult lardon3d_project_db_create_geometric_verification_result(
    Lardon3DProjectDb *database, uint64_t match_result_id,
    Lardon3DGeometricVerifierKind verifier_kind, uint32_t verifier_version,
    const unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE],
    Lardon3DGeometricVerificationStatus status, uint32_t inlier_count,
    const unsigned char *inlier_mask, size_t inlier_mask_size, const double *model,
    int64_t created_at, Lardon3DProjectDbGeometricVerificationResult *result);
Lardon3DProjectDbResult lardon3d_project_db_load_geometric_verification_result(
    Lardon3DProjectDb *database, uint64_t geometric_verification_result_id,
    Lardon3DProjectDbGeometricVerificationResult *result);
Lardon3DProjectDbResult lardon3d_project_db_find_geometric_verification_result(
    Lardon3DProjectDb *database, uint64_t match_result_id,
    Lardon3DGeometricVerifierKind verifier_kind, uint32_t verifier_version,
    const unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE],
    Lardon3DProjectDbGeometricVerificationResult *result);
Lardon3DProjectDbResult lardon3d_project_db_list_geometric_verification_results(
    Lardon3DProjectDb *database, uint64_t match_result_id,
    uint64_t after_geometric_verification_result_id,
    Lardon3DProjectDbGeometricVerificationResult *results, size_t capacity, size_t *count);

#endif
