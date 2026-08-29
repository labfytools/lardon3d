#ifndef LARDON3D_GEOMETRIC_VERIFIER_H
#define LARDON3D_GEOMETRIC_VERIFIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>

enum {
  LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1 = 1,
  LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2 = 2,
  LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3 = 3,
  LARDON3D_GEOMETRIC_VERIFIER_VERSION =
      LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
  LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE = 84,
  LARDON3D_GEOMETRIC_VERIFIER_MINIMUM_MATCHES = 7,
};

typedef enum {
  LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC = 1,
  LARDON3D_GEOMETRIC_ALGORITHM_USAC_DEFAULT = 2,
} Lardon3DGeometricVerifierAlgorithm;

typedef struct {
  double threshold_pixels;
  double confidence;
  uint32_t max_iterations;
  uint32_t min_inlier_count;
  double min_inlier_ratio;
  uint32_t seed_policy_version;
  uint32_t canonicalization_version;
} Lardon3DGeometricVerifierParameters;

typedef enum {
  LARDON3D_GEOMETRIC_VERIFIER_OK = 0,
  LARDON3D_GEOMETRIC_VERIFIER_INVALID_ARGUMENT,
  LARDON3D_GEOMETRIC_VERIFIER_NOT_FOUND,
  LARDON3D_GEOMETRIC_VERIFIER_CORRUPT,
  LARDON3D_GEOMETRIC_VERIFIER_OUT_OF_MEMORY,
  LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR,
  LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR,
} Lardon3DGeometricVerifierResult;

Lardon3DGeometricVerifierParameters
lardon3d_geometric_verifier_default_parameters(void);
bool lardon3d_geometric_verifier_parameters_valid(
    const Lardon3DGeometricVerifierParameters *parameters);
bool lardon3d_geometric_verifier_fingerprint_bytes(
    const Lardon3DGeometricVerifierParameters *parameters,
    Lardon3DGeometricVerifierAlgorithm algorithm, uint32_t verifier_version,
    unsigned char bytes[LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE]);
/* Computes the identity fingerprint for one supported scientific policy.
 * Versions 1 through 3 are retained so durable historical tasks and GVRs
 * remain reconstructable; new callers should select VERSION_V3. Returns false
 * and writes a zero fingerprint for invalid parameters or versions. */
bool lardon3d_geometric_verifier_fingerprint_for_version(
    const Lardon3DGeometricVerifierParameters *parameters,
    uint32_t verifier_version, unsigned char fingerprint[32]);
/* Convenience form for the current production policy (v3). */
void lardon3d_geometric_verifier_fingerprint(
    const Lardon3DGeometricVerifierParameters *parameters,
    unsigned char fingerprint[32]);
bool lardon3d_geometric_verifier_canonicalize(double model[9]);
uint32_t lardon3d_geometric_verifier_seed(const unsigned char match_sha256[32],
                                          const unsigned char fingerprint[32]);
Lardon3DGeometricVerifierResult lardon3d_geometric_verifier_verify_and_publish(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t match_result_id,
    const Lardon3DGeometricVerifierParameters *parameters,
    Lardon3DProjectDbGeometricVerificationResult *result, bool *reused);
/* Verifies and atomically publishes or exactly reuses the requested v1/v2/v3
 * identity. Output is caller-owned. No scientific row is published for I/O,
 * corruption, allocation, estimator, or database failure. C++ exceptions are
 * contained by the implementation and never cross this C17 ABI. */
Lardon3DGeometricVerifierResult
lardon3d_geometric_verifier_verify_and_publish_version(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t match_result_id,
    const Lardon3DGeometricVerifierParameters *parameters,
    uint32_t verifier_version,
    Lardon3DProjectDbGeometricVerificationResult *result, bool *reused);

#ifdef LARDON3D_GEOMETRIC_VERIFIER_TESTING
void lardon3d_geometric_verifier_test_reset_estimator_calls(void);
uint32_t lardon3d_geometric_verifier_test_estimator_calls(void);
bool lardon3d_geometric_verifier_test_has_minimal_support(
    const uint32_t *indices_a, const uint32_t *indices_b, size_t count,
    uint32_t feature_count_a, uint32_t feature_count_b);
#endif

#endif
