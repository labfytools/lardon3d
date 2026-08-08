#ifndef LARDON3D_PRECISION_FEATURES_H
#define LARDON3D_PRECISION_FEATURES_H

#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/project_db.h>

typedef struct {
  double association_radius_pixels;
} Lardon3DFeatureConsolidationParameters;

Lardon3DFeatureConsolidationParameters lardon3d_feature_consolidation_v1(void);
bool lardon3d_feature_consolidation_parameters_valid(
    const Lardon3DFeatureConsolidationParameters *parameters);
void lardon3d_feature_consolidation_fingerprint(
    const Lardon3DFeatureConsolidationParameters *parameters, unsigned char fingerprint[32]);
bool lardon3d_feature_support_higher_quality(
    const Lardon3DProjectDbFeatureSupportGroup *candidate,
    const Lardon3DProjectDbFeatureSupportGroup *reference);
Lardon3DProjectDbResult lardon3d_consolidate_orb_sift(
    Lardon3DAppState *state, uint64_t orb_feature_set_id, uint64_t sift_feature_set_id,
    const Lardon3DFeatureConsolidationParameters *parameters,
    Lardon3DProjectDbFeatureSupportSet *support_set);

#endif
