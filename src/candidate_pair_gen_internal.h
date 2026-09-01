#ifndef LARDON3D_CANDIDATE_PAIR_GEN_INTERNAL_H
#define LARDON3D_CANDIDATE_PAIR_GEN_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/candidate_pair_gen.h>

typedef struct {
  uint64_t image_id_a;
  uint64_t image_id_b;
} Lardon3DCandidatePairProposal;

typedef struct {
  uint64_t source_feature_set_id;
  uint32_t queried_count;
  size_t proposal_count;
  Lardon3DCandidatePairProposal proposals[LARDON3D_VISUAL_INDEX_TOP_K_MAX];
} Lardon3DCandidatePairComputation;

/* Computation is read-only and owns no storage beyond `computed`. This split
 * lets task-local workers use private DB handles while the task owner retains
 * the only Candidate Pair publication path and its canonical source order. */
Lardon3DVisualIndexResult lardon3d_candidate_pair_compute(
    const char *project_path, Lardon3DProjectDb *database, uint64_t visual_index_id,
    uint64_t source_feature_set_id, const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairComputation *computed);

Lardon3DVisualIndexResult lardon3d_candidate_pair_publish(
    Lardon3DProjectDb *database, const Lardon3DCandidatePairComputation *computed,
    Lardon3DCandidatePairGenStats *stats);

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
/* Test-only counters observe the production callback after Registry recovery.
 * They prove that a multi-CPU execution contract distributes source work to
 * more than the Queue owner; production state and scientific output are not
 * instrumented or altered. */
void lardon3d_candidate_pair_task_test_reset_parallel_counters(void);
size_t lardon3d_candidate_pair_task_test_started_participants(void);
size_t lardon3d_candidate_pair_task_test_computed_work_items(void);
/* Deterministic failure/ownership seam for the production partial-create
 * cleanup path. SIZE_MAX disables failure; the active-handle count must return
 * to zero before the callback exits on every result. */
void lardon3d_candidate_pair_task_test_fail_thread_create_after(
    size_t successful_children);
size_t lardon3d_candidate_pair_task_test_active_private_databases(void);
bool lardon3d_candidate_pair_task_test_compute_window(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    const uint64_t *source_ids, size_t source_count,
    unsigned int admitted_threads,
    Lardon3DCandidatePairComputation *computations,
    Lardon3DVisualIndexResult *results);
#endif

#endif
