#ifndef LARDON3D_RESOURCE_SNAPSHOT_TEST_UTILS_H
#define LARDON3D_RESOURCE_SNAPSHOT_TEST_UTILS_H

#include <stdbool.h>
#include <time.h>

#include <lardon3d/resource_snapshot.h>

static inline bool
lardon3d_test_resource_snapshot_make_fresh(
    Lardon3DResourceSnapshot *snapshot
)
{
    return snapshot
        && clock_gettime(CLOCK_MONOTONIC, &snapshot->captured_at) == 0;
}

#endif
