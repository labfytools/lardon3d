#ifndef LARDON3D_CANDIDATE_PAIR_TASK_H
#define LARDON3D_CANDIDATE_PAIR_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/task_kind_registry.h>
#include <lardon3d/visual_index.h>

#define LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND "candidate_pair.generate"
enum { LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND_VERSION = 1 };

Lardon3DTask *lardon3d_project_create_candidate_pair_generate_task(
    Lardon3DAppState *state, uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options, uint64_t *task_id);
bool lardon3d_project_enqueue_candidate_pair_generate(Lardon3DAppState *state,
                                                      uint64_t visual_index_id,
                                                      const Lardon3DVisualIndexQueryOptions
                                                          *query_options,
                                                      uint64_t *task_id);
bool lardon3d_candidate_pair_generate_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
                                                   void *context,
                                                   Lardon3DTaskKindBinding *binding);

#endif
