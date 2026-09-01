#ifndef LARDON3D_CANDIDATE_PAIR_GEN_H
#define LARDON3D_CANDIDATE_PAIR_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>
#include <lardon3d/visual_index.h>

typedef struct {
  uint32_t generated_count;    // paires nouvellement créées
  uint32_t skipped_count;      // paires déjà existantes (idempotence)
  uint32_t queried_count;      // candidats retournés par le Visual Index
} Lardon3DCandidatePairGenStats;

Lardon3DVisualIndexResult lardon3d_candidate_pair_generate(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *stats);

Lardon3DVisualIndexResult lardon3d_candidate_pair_generate_batch(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t after_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *total_stats,
    uint64_t *last_feature_set_id);

/* Produce the frozen Candidate-generation v1 SHA-256 fingerprint in caller-owned
 * 32-byte storage. Integer inputs are encoded at fixed width, little-endian;
 * the filter is one u32 and exclude_same_asset is exactly one 0/1 byte. This
 * preserves acquired little-endian fingerprints while making identity portable.
 * query_options may be NULL for the historical ID-only form; fingerprint must
 * be non-NULL. */
void lardon3d_candidate_pair_generation_fingerprint(
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    unsigned char fingerprint[32]);

#endif
