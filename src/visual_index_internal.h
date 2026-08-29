#ifndef LARDON3D_VISUAL_INDEX_INTERNAL_H
#define LARDON3D_VISUAL_INDEX_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>
#include <lardon3d/visual_index.h>

/* Task-internal entry point: cpu_threads includes the Queue callback. Child
 * threads only read immutable Feature Files into disjoint staging slices; the
 * callback retains sole ownership of canonical reduction and publication. */
Lardon3DVisualIndexResult lardon3d_visual_index_update_once_parallel(
    const char *project_path, Lardon3DProjectDb *database, uint64_t visual_index_id,
    uint64_t producer_task_id, uint64_t after_feature_set_id, size_t maximum_feature_sets,
    unsigned int cpu_threads, uint64_t *last_feature_set_id, size_t *indexed_count);

#endif
