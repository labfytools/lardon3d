#ifndef LARDON3D_ACQUISITION_CAMPAIGN_H
#define LARDON3D_ACQUISITION_CAMPAIGN_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/acquisition_ingest.h>
#include <lardon3d/acquisition_pairing.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS 64u
#define LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES 4096u
#define LARDON3D_ACQUISITION_CAMPAIGN_MAX_GROUPS 4096u
#define LARDON3D_ACQUISITION_CAMPAIGN_MAX_PROPOSALS 4096u
#define LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY LARDON3D_APP_STATE_PATH_CAPACITY

typedef enum {
  LARDON3D_ACQUISITION_CAMPAIGN_OK = 0,
  LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT,
  LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR,
  LARDON3D_ACQUISITION_CAMPAIGN_DUPLICATE,
  LARDON3D_ACQUISITION_CAMPAIGN_PATH_TOO_LONG,
  LARDON3D_ACQUISITION_CAMPAIGN_LIMIT_EXCEEDED,
  LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT,
  LARDON3D_ACQUISITION_CAMPAIGN_INGEST_ERROR,
  LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR
} Lardon3DAcquisitionCampaignResult;

typedef struct {
  char path[LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY];
} Lardon3DAcquisitionCampaignRoot;

typedef struct {
  char path[LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY];
  Lardon3DAcquisitionSourceKind source_kind;
  Lardon3DAcquisitionResult metadata_result;
  Lardon3DAcquisitionMetadata metadata;
} Lardon3DAcquisitionCampaignSource;

typedef struct {
  size_t discovered_entry_count;
  size_t supported_source_count;
  size_t unsupported_entry_count;
  size_t metadata_ok_count;
  size_t metadata_error_count;
} Lardon3DAcquisitionCampaignDiscoverySummary;

typedef struct {
  size_t source_count;
  Lardon3DAcquisitionCampaignSource sources[LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES];
  Lardon3DAcquisitionCampaignDiscoverySummary summary;
} Lardon3DAcquisitionCampaignDiscovery;

typedef struct {
  size_t source_count;
  size_t source_indices[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES];
} Lardon3DAcquisitionCampaignConfirmation;

typedef enum {
  LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_CANDIDATE = 1,
  LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_AMBIGUOUS = 2
} Lardon3DAcquisitionCampaignProposalKind;

typedef struct {
  size_t left_source_index;
  size_t right_source_index;
  Lardon3DAcquisitionCampaignProposalKind kind;
  Lardon3DAcquisitionPairResult pair;
} Lardon3DAcquisitionCampaignProposal;

typedef struct {
  uint32_t group_id;
  Lardon3DAcquisitionGroupBasis basis;
  size_t source_count;
  size_t source_indices[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES];
} Lardon3DAcquisitionCampaignGroup;

typedef struct {
  size_t source_count;
  size_t metadata_ok_count;
  size_t metadata_error_count;
  size_t strong_group_count;
  size_t explicit_group_count;
  size_t unresolved_source_count;
  size_t ambiguous_pair_count;
  size_t contradictory_pair_count;
  size_t insufficient_pair_count;
  size_t candidate_pair_count;
} Lardon3DAcquisitionCampaignPlanSummary;

typedef struct {
  size_t group_count;
  Lardon3DAcquisitionCampaignGroup groups[LARDON3D_ACQUISITION_CAMPAIGN_MAX_GROUPS];
  size_t proposal_count;
  Lardon3DAcquisitionCampaignProposal proposals[LARDON3D_ACQUISITION_CAMPAIGN_MAX_PROPOSALS];
  Lardon3DAcquisitionCampaignPlanSummary summary;
} Lardon3DAcquisitionCampaignPlan;

/*
 * Roots must be absolute and lexically normalized. Only their immediate regular-file
 * entries are considered. Symlinks are never followed. Extensions .arw, .jpg and .jpeg
 * are supported case-insensitively; all other immediate entries are counted as unsupported.
 */
Lardon3DAcquisitionCampaignResult lardon3d_acquisition_campaign_discover(
    const Lardon3DAcquisitionCampaignRoot *roots, size_t root_count,
    Lardon3DAcquisitionCampaignDiscovery *discovery);

/* Pure planning over caller-supplied normalized source/evidence records. */
Lardon3DAcquisitionCampaignResult lardon3d_acquisition_campaign_plan(
    const Lardon3DAcquisitionCampaignSource *sources, size_t source_count,
    const Lardon3DAcquisitionCampaignConfirmation *confirmations, size_t confirmation_count,
    Lardon3DAcquisitionCampaignPlan *plan);

/* Materializes exactly one stable plan group. Retain capture_id before retrying with it. */
Lardon3DAcquisitionCampaignResult lardon3d_acquisition_campaign_materialize_group(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionCampaignSource *sources, size_t source_count,
    const Lardon3DAcquisitionCampaignPlan *plan, uint32_t group_id,
    const Lardon3DAcquisitionIngestOptions *options,
    Lardon3DAcquisitionIngestOutput *output,
    Lardon3DAcquisitionIngestResult *ingest_result);

#ifdef __cplusplus
}
#endif

#endif
