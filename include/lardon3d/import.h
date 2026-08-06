#ifndef LARDON3D_IMPORT_H
#define LARDON3D_IMPORT_H

#include <stdbool.h>
#include <stddef.h>

#include <lardon3d/app_state.h>

typedef struct {
    size_t admissible_found;
    size_t copied;
    size_t already_present;
    size_t ignored;
} Lardon3DImportResult;

bool lardon3d_import_directory(
    Lardon3DAppState *state,
    const char *source_directory,
    Lardon3DImportResult *result
);

#endif
