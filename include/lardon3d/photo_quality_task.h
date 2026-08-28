#ifndef LARDON3D_PHOTO_QUALITY_TASK_H
#define LARDON3D_PHOTO_QUALITY_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/acquisition_campaign.h>
#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/task_kind_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_PHOTO_QUALITY_TASK_KIND "photo_quality.triage"
enum { LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION = 1,
       LARDON3D_PHOTO_QUALITY_REQUEST_VERSION = 1 };

/* The durable request stores discovered acquisition evidence, never Capture,
 * Asset, or image identities. Grouping is reproducible before materialization
 * without guessing scientific identity. */
typedef struct {
  const Lardon3DAcquisitionCampaignSource *sources;
  size_t source_count;
  const Lardon3DAcquisitionCampaignConfirmation *confirmations;
  size_t confirmation_count;
} Lardon3DPhotoQualityTaskRequest;

#define LARDON3D_PHOTO_QUALITY_TASK_REQUEST_MAX_BYTES \
  LARDON3D_ACQUISITION_CAMPAIGN_TASK_REQUEST_MAX_BYTES

/* Create one durable, initially unqueued triage Task for a valid existing
 * ScanSet. request arrays are borrowed only for the call and copied into the
 * bounded deterministic codec; task_id is required and receives the durable
 * identity on success. The caller owns the returned Task. */
Lardon3DTask *lardon3d_project_create_photo_quality_task(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DPhotoQualityTaskRequest *request, uint64_t *task_id);
/* Create and transfer the durable Task to state's Queue. Inputs follow the
 * create contract; success transfers Task ownership to the Queue. */
bool lardon3d_project_enqueue_photo_quality(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DPhotoQualityTaskRequest *request, uint64_t *task_id);
/* Rebuild bounded callback context from an immutable durable request. snapshot,
 * reconstruction context, and binding are required and caller-owned; temporary
 * maximum-capacity codec arrays are released before Queue admission. */
bool lardon3d_photo_quality_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);
/* Encode the request deterministically using the acquisition-campaign v1 wire
 * format. output may be NULL only for a size probe; size receives required or
 * written bytes. No input storage is retained. */
bool lardon3d_photo_quality_request_encode(
    const Lardon3DPhotoQualityTaskRequest *request, unsigned char *output,
    size_t capacity, size_t *size);
/* Strictly decode a bounded immutable request into caller-owned source and
 * confirmation arrays. Their capacities must cover encoded counts; request
 * borrows those arrays for their caller-controlled lifetime. */
bool lardon3d_photo_quality_request_decode(
    const unsigned char *input, size_t size,
    Lardon3DAcquisitionCampaignSource *sources, size_t source_capacity,
    Lardon3DAcquisitionCampaignConfirmation *confirmations,
    size_t confirmation_capacity, Lardon3DPhotoQualityTaskRequest *request);

#ifdef __cplusplus
}
#endif
#endif
