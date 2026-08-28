#ifndef LARDON3D_ACQUISITION_CAMPAIGN_TASK_H
#define LARDON3D_ACQUISITION_CAMPAIGN_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/acquisition_campaign.h>
#include <lardon3d/task_kind_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND "acquisition_campaign.run"
enum {
  LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION = 1,
  LARDON3D_ACQUISITION_CAMPAIGN_REQUEST_VERSION = 1,
};

/* Derived solely from the frozen source, path, confirmation and group bounds.
 */
#define LARDON3D_ACQUISITION_CAMPAIGN_TASK_REQUEST_MAX_BYTES                   \
  ((size_t)LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES *                         \
       (LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY + 700u) +                  \
   (size_t)LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES *                         \
       (4u + 4u * LARDON3D_ACQUISITION_INGEST_MAX_SOURCES) +                   \
   128u)

typedef struct {
  const Lardon3DAcquisitionCampaignSource *sources;
  size_t source_count;
  const Lardon3DAcquisitionCampaignConfirmation *confirmations;
  size_t confirmation_count;
  Lardon3DAcquisitionIngestOptions ingest_options;
} Lardon3DAcquisitionCampaignTaskRequest;

Lardon3DTask *lardon3d_project_create_acquisition_campaign_task(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionCampaignTaskRequest *request, uint64_t *task_id);
bool lardon3d_project_enqueue_acquisition_campaign(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionCampaignTaskRequest *request, uint64_t *task_id);
bool lardon3d_acquisition_campaign_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

/* Exposed for deterministic codec validation without executing ingestion. */
bool lardon3d_acquisition_campaign_request_encode(
    const Lardon3DAcquisitionCampaignTaskRequest *request,
    unsigned char *output, size_t capacity, size_t *size);
bool lardon3d_acquisition_campaign_request_decode(
    const unsigned char *input, size_t size,
    Lardon3DAcquisitionCampaignSource *sources, size_t source_capacity,
    Lardon3DAcquisitionCampaignConfirmation *confirmations,
    size_t confirmation_capacity,
    Lardon3DAcquisitionCampaignTaskRequest *request);

#ifdef __cplusplus
}
#endif

#endif
