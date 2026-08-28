#ifndef LARDON3D_RAW_DEVELOPMENT_H
#define LARDON3D_RAW_DEVELOPMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <lardon3d/app_state.h>
#include <lardon3d/project_db.h>

typedef enum {
    LARDON3D_RAW_DEVELOPMENT_OK = 0,
    LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT,
    LARDON3D_RAW_DEVELOPMENT_SOURCE_NOT_FOUND,
    LARDON3D_RAW_DEVELOPMENT_SOURCE_CHANGED,
    LARDON3D_RAW_DEVELOPMENT_UNSUPPORTED_RAW,
    LARDON3D_RAW_DEVELOPMENT_CORRUPT_RAW,
    LARDON3D_RAW_DEVELOPMENT_DECODER_FAILURE,
    LARDON3D_RAW_DEVELOPMENT_OUT_OF_MEMORY,
    LARDON3D_RAW_DEVELOPMENT_OUTPUT_LIMIT_EXCEEDED,
    LARDON3D_RAW_DEVELOPMENT_IO_ERROR,
    LARDON3D_RAW_DEVELOPMENT_DB_ERROR,
    LARDON3D_RAW_DEVELOPMENT_CONSTRAINT,
    LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR
} Lardon3DRawDevelopmentResult;

typedef struct {
    Lardon3DProjectDbImageAsset source_asset;
    Lardon3DProjectDbImageAsset derived_asset;
    Lardon3DProjectDbImage image;
    unsigned char parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
    float resolved_camera_wb[4];
    uint32_t width;
    uint32_t height;
    char libraw_version[64];
    char png_encoder_version[64];
} Lardon3DRawDevelopmentOutput;

/* Computes the fixed RAW policy v1 fingerprint for explicitly resolved camera WB. */
Lardon3DRawDevelopmentResult lardon3d_raw_development_policy_fingerprint(
    const float resolved_camera_wb[4],
    unsigned char fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE]
);

/* Publishes a managed ARW source and a derived PNG into an existing Capture. */
Lardon3DRawDevelopmentResult lardon3d_raw_develop_to_capture(
    Lardon3DAppState *state, uint64_t capture_id, const char *source_arw_path,
    uint64_t producer_task_id, int64_t created_at, Lardon3DRawDevelopmentOutput *output
);

/* Develop the immutable managed RAW identified explicitly by source_asset_id.
 * The asset must have a durable RAW source-kind relation to capture_id. This
 * API validates the managed bytes against their stored SHA-256 and never finds
 * source identity from path, name, hash, attachment order, or image identity.
 * Output storage is caller-owned; published assets/images remain DB-owned. */
Lardon3DRawDevelopmentResult lardon3d_raw_develop_asset_to_capture(
    Lardon3DAppState *state, uint64_t capture_id, uint64_t source_asset_id,
    uint64_t producer_task_id, int64_t created_at, Lardon3DRawDevelopmentOutput *output
);

#ifdef __cplusplus
}
#endif

#endif
