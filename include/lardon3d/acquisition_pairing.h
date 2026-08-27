#ifndef LARDON3D_ACQUISITION_PAIRING_H
#define LARDON3D_ACQUISITION_PAIRING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION 1u
#define LARDON3D_ACQUISITION_TEXT_CAPACITY 128u
#define LARDON3D_ACQUISITION_DATETIME_CAPACITY 32u
#define LARDON3D_ACQUISITION_MAX_CANDIDATES 64u

typedef enum {
    LARDON3D_ACQUISITION_SOURCE_UNSUPPORTED = 0,
    LARDON3D_ACQUISITION_SOURCE_RAW = 1,
    LARDON3D_ACQUISITION_SOURCE_JPEG = 2
} Lardon3DAcquisitionSourceKind;

typedef enum {
    LARDON3D_ACQUISITION_OK = 0,
    LARDON3D_ACQUISITION_INVALID_ARGUMENT,
    LARDON3D_ACQUISITION_UNSUPPORTED_FORMAT,
    LARDON3D_ACQUISITION_CORRUPT_SOURCE,
    LARDON3D_ACQUISITION_IO_ERROR,
    LARDON3D_ACQUISITION_METADATA_UNAVAILABLE,
    LARDON3D_ACQUISITION_LIMIT_EXCEEDED,
    LARDON3D_ACQUISITION_INTERNAL_ERROR
} Lardon3DAcquisitionResult;

enum {
    LARDON3D_ACQUISITION_FIELD_MAKE = UINT32_C(1) << 0,
    LARDON3D_ACQUISITION_FIELD_MODEL = UINT32_C(1) << 1,
    LARDON3D_ACQUISITION_FIELD_BODY_SERIAL = UINT32_C(1) << 2,
    LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL = UINT32_C(1) << 3,
    LARDON3D_ACQUISITION_FIELD_SUBSEC_ORIGINAL = UINT32_C(1) << 4,
    LARDON3D_ACQUISITION_FIELD_OFFSET_ORIGINAL = UINT32_C(1) << 5,
    LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID = UINT32_C(1) << 6,
    LARDON3D_ACQUISITION_FIELD_DIMENSIONS = UINT32_C(1) << 7,
    LARDON3D_ACQUISITION_FIELD_ORIENTATION = UINT32_C(1) << 8
};

typedef struct {
    uint32_t policy_version;
    Lardon3DAcquisitionSourceKind source_kind;
    uint32_t present_fields;
    char make[LARDON3D_ACQUISITION_TEXT_CAPACITY];
    char model[LARDON3D_ACQUISITION_TEXT_CAPACITY];
    char body_serial[LARDON3D_ACQUISITION_TEXT_CAPACITY];
    char datetime_original[LARDON3D_ACQUISITION_DATETIME_CAPACITY];
    char subsec_original[LARDON3D_ACQUISITION_DATETIME_CAPACITY];
    char offset_original[LARDON3D_ACQUISITION_DATETIME_CAPACITY];
    char image_unique_id[LARDON3D_ACQUISITION_TEXT_CAPACITY];
    uint32_t width;
    uint32_t height;
    uint16_t orientation;
    uint16_t reserved;
} Lardon3DAcquisitionMetadata;

typedef enum {
    LARDON3D_ACQUISITION_INSUFFICIENT = 0,
    LARDON3D_ACQUISITION_SAME_ACQUISITION_CANDIDATE = 1,
    LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG = 2,
    LARDON3D_ACQUISITION_DIFFERENT = 3,
    LARDON3D_ACQUISITION_AMBIGUOUS = 4
} Lardon3DAcquisitionDecision;

enum {
    LARDON3D_ACQUISITION_EVIDENCE_IMAGE_UNIQUE_ID = UINT32_C(1) << 0,
    LARDON3D_ACQUISITION_EVIDENCE_BODY_SERIAL = UINT32_C(1) << 1,
    LARDON3D_ACQUISITION_EVIDENCE_MAKE = UINT32_C(1) << 2,
    LARDON3D_ACQUISITION_EVIDENCE_MODEL = UINT32_C(1) << 3,
    LARDON3D_ACQUISITION_EVIDENCE_DATETIME_SECOND = UINT32_C(1) << 4,
    LARDON3D_ACQUISITION_EVIDENCE_DIMENSIONS = UINT32_C(1) << 5,
    LARDON3D_ACQUISITION_EVIDENCE_BASENAME_HINT = UINT32_C(1) << 6
};

enum {
    LARDON3D_ACQUISITION_CONTRADICTION_IMAGE_UNIQUE_ID = UINT32_C(1) << 0,
    LARDON3D_ACQUISITION_CONTRADICTION_BODY_SERIAL = UINT32_C(1) << 1
};

typedef struct {
    Lardon3DAcquisitionDecision decision;
    uint32_t evidence;
    uint32_t contradictions;
    uint32_t strength;
} Lardon3DAcquisitionPairResult;

typedef struct {
    uint64_t candidate_id;
    const Lardon3DAcquisitionMetadata *metadata;
    uint8_t same_asset;
    uint8_t basename_hint;
    uint8_t reserved[6];
} Lardon3DAcquisitionCandidate;

typedef struct {
    Lardon3DAcquisitionDecision decision;
    uint64_t selected_candidate_id;
    size_t top_candidate_count;
    Lardon3DAcquisitionPairResult selected_pair;
} Lardon3DAcquisitionSelection;

Lardon3DAcquisitionResult lardon3d_acquisition_extract_metadata(
    const char *path, Lardon3DAcquisitionMetadata *metadata
);

Lardon3DAcquisitionResult lardon3d_acquisition_compare(
    const Lardon3DAcquisitionMetadata *left, const Lardon3DAcquisitionMetadata *right,
    int same_asset, int basename_hint, Lardon3DAcquisitionPairResult *result
);

Lardon3DAcquisitionResult lardon3d_acquisition_select_candidate(
    const Lardon3DAcquisitionMetadata *source,
    const Lardon3DAcquisitionCandidate *candidates, size_t candidate_count,
    Lardon3DAcquisitionSelection *selection
);

#ifdef __cplusplus
}
#endif

#endif
