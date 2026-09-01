#ifndef LARDON3D_GEOMETRIC_VERIFIER_INTERNAL_H
#define LARDON3D_GEOMETRIC_VERIFIER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/geometric_verifier.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lardon3DGeometricVerifierPrepared
    Lardon3DGeometricVerifierPrepared;

/* Worker-owned preparation validates every immutable input and computes the
 * exact frozen v1/v2/v3 scientific payload, but never mutates Project DB. If
 * the exact identity already exists, `prepared` remains NULL and the
 * caller-owned `result` is filled with that durable row. Otherwise the caller
 * owns one bounded opaque preparation until destroy, including on publication
 * failure. Independent preparations may run concurrently against the shared,
 * internally serialized Project DB handle. Every Match/Feature file handle is
 * owned by one preparation call and closed before that call returns. */
Lardon3DGeometricVerifierResult
lardon3d_geometric_verifier_internal_prepare_version(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t match_result_id,
    const Lardon3DGeometricVerifierParameters *parameters,
    uint32_t verifier_version, Lardon3DGeometricVerifierPrepared **prepared,
    Lardon3DProjectDbGeometricVerificationResult *result, bool *reused);

/* The sequence owner alone publishes prepared payloads in canonical parent
 * order. Retry is exact: a concurrent/existing identical row is returned as
 * reuse, while no different identity is inferred. The function does not
 * consume `prepared`; destroy remains mandatory after every return. */
Lardon3DGeometricVerifierResult
lardon3d_geometric_verifier_internal_publish_prepared(
    Lardon3DProjectDb *database,
    const Lardon3DGeometricVerifierPrepared *prepared,
    Lardon3DProjectDbGeometricVerificationResult *result, bool *reused);

/* Releases one preparation. NULL is accepted so a batch cleanup sweep remains
 * safe after clearing owned slots; every non-NULL object must be released
 * exactly once on partial thread creation, cancellation, success, or failure. */
void lardon3d_geometric_verifier_internal_prepared_destroy(
    Lardon3DGeometricVerifierPrepared *prepared);

#ifdef __cplusplus
}
#endif

#endif
