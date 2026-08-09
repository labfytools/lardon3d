#ifndef LARDON3D_MATCHER_H
#define LARDON3D_MATCHER_H

#include <lardon3d/feature_store.h>
#include <lardon3d/match_file.h>
#include <lardon3d/project_db.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LARDON3D_MATCHER_VERSION = 1,
    LARDON3D_MATCHER_KNN_K = 2,
};

typedef enum {
    LARDON3D_MATCHER_ORB_BF = 0,
    LARDON3D_MATCHER_SIFT_BF = 1,
    LARDON3D_MATCHER_ROOTSIFT_BF = 2,
} Lardon3DMatcherKind;

typedef struct {
    Lardon3DMatcherKind kind;
    float ratio_threshold;
} Lardon3DMatcherParams;

typedef struct {
    uint32_t knn_query_count;
    uint32_t match_count;
    uint32_t feature_count_a;
    uint32_t feature_count_b;
    uint64_t feature_open_ns;
    uint64_t descriptor_read_ns;
    uint64_t knn_ns;
    uint64_t filter_ns;
    uint64_t canonicalize_ns;
    uint64_t serialize_ns;
    uint64_t sha256_ns;
    uint64_t publication_ns;
    uint64_t database_ns;
    uint64_t total_ns;
} Lardon3DMatcherStats;

typedef enum {
    LARDON3D_MATCHER_OK = 0,
    LARDON3D_MATCHER_INVALID_ARGUMENT,
    LARDON3D_MATCHER_IO_ERROR,
    LARDON3D_MATCHER_TYPE_MISMATCH,
    LARDON3D_MATCHER_FAILED,
} Lardon3DMatcherResult;

const char *lardon3d_matcher_kind_string(Lardon3DMatcherKind kind);

float lardon3d_matcher_default_ratio(Lardon3DMatcherKind kind);

void lardon3d_matcher_fingerprint(const Lardon3DMatcherParams *params,
                                   unsigned char fingerprint[32]);

Lardon3DMatcherResult lardon3d_matcher_run(
    const char *project_path,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    const char *match_file_path,
    Lardon3DMatcherStats *stats);

Lardon3DMatcherResult lardon3d_matcher_match_and_publish(
    const char *project_path,
    Lardon3DProjectDb *database,
    const Lardon3DProjectDbCandidatePair *pair,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    Lardon3DProjectDbMatchResult *result);

Lardon3DMatcherResult lardon3d_matcher_match_and_publish_profiled(
    const char *project_path,
    Lardon3DProjectDb *database,
    const Lardon3DProjectDbCandidatePair *pair,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    Lardon3DProjectDbMatchResult *result,
    Lardon3DMatcherStats *stats);

#ifdef __cplusplus
}
#endif

#endif
