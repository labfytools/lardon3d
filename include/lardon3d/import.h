#ifndef LARDON3D_IMPORT_H
#define LARDON3D_IMPORT_H

#include <stdbool.h>
#include <stddef.h>

#include <lardon3d/app_state.h>

typedef struct {
    size_t admissible_found;
    size_t processed;
    size_t newly_manifested;
    size_t copied;
    size_t already_present;
    size_t ignored;
} Lardon3DImportResult;

typedef struct {
    size_t total;
    size_t processed;
    size_t copied;
    size_t already_present;
    size_t ignored;
    const char *message;
} Lardon3DImportProgress;

typedef bool (*Lardon3DImportCancelled)(void *context);
typedef void (*Lardon3DImportProgressed)(
    void *context,
    const Lardon3DImportProgress *progress
);

typedef struct {
    void *context;
    Lardon3DImportCancelled is_cancelled;
    Lardon3DImportProgressed progressed;
} Lardon3DImportControl;

typedef enum {
    LARDON3D_IMPORT_FAILED = 0,
    LARDON3D_IMPORT_SUCCEEDED,
    LARDON3D_IMPORT_CANCELLED
} Lardon3DImportOutcome;

bool lardon3d_import_directory(
    Lardon3DAppState *state,
    const char *source_directory,
    Lardon3DImportResult *result
);

Lardon3DImportOutcome lardon3d_import_directory_controlled(
    Lardon3DAppState *state,
    const char *source_directory,
    Lardon3DImportResult *result,
    const Lardon3DImportControl *control
);

Lardon3DImportOutcome lardon3d_import_directory_batch(
    Lardon3DAppState *state,
    const char *source_directory,
    size_t batch_size,
    Lardon3DImportResult *result,
    const Lardon3DImportControl *control,
    bool *complete
);

Lardon3DImportOutcome lardon3d_import_directory_batch_to_scanset(
    Lardon3DAppState *state,
    uint64_t scanset_id,
    uint64_t producer_task_id,
    const char *source_directory,
    size_t batch_size,
    Lardon3DImportResult *result,
    const Lardon3DImportControl *control,
    bool *complete
);

#endif
