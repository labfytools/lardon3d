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

/* Request wire payload is a deterministic codec, not a memcpy of in-memory structs:
   - fixed magic + version prefix
   - fixed-width integer fields
   - explicit little-endian byte order
   - bounded text fields and counts
   - strict decode-side validation rejecting malformed inputs.
   No native struct layout is used for persistence compatibility. */
#define LARDON3D_ACQUISITION_CAMPAIGN_TASK_REQUEST_MAX_BYTES                   \
  ((size_t)LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES *                         \
       (LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY + 700u) +                  \
   (size_t)LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES *                         \
       (4u + 4u * LARDON3D_ACQUISITION_INGEST_MAX_SOURCES) +                   \
   128u)

typedef struct {
  /* Arrays are caller-owned and borrowed only for encode/create/enqueue. */
  const Lardon3DAcquisitionCampaignSource *sources;
  size_t source_count;
  const Lardon3DAcquisitionCampaignConfirmation *confirmations;
  size_t confirmation_count;
  Lardon3DAcquisitionIngestOptions ingest_options;
} Lardon3DAcquisitionCampaignTaskRequest;

/* Create one durable, initially unqueued campaign Task for an existing
 * ScanSet. request arrays are copied into the bounded deterministic codec and
 * are not retained. task_id is required and receives zero on failure. The
 * caller owns the returned Task and must destroy it or transfer it to a Queue.
 * Exact durable checkpoint retry is idempotent; changed request bytes conflict. */
Lardon3DTask *lardon3d_project_create_acquisition_campaign_task(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionCampaignTaskRequest *request, uint64_t *task_id);
/* Create and transfer a durable campaign Task to state's Queue. Inputs follow
 * the create contract; success transfers Task ownership to the Queue. task_id
 * is required and is initialized to zero before validation. If transfer
 * fails after durable creation, false is returned while task_id keeps only that
 * recoverable durable identity; the transient Task object is destroyed. */
bool lardon3d_project_enqueue_acquisition_campaign(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionCampaignTaskRequest *request, uint64_t *task_id);
/* Rebuild bounded callback context from the immutable durable request.
 * snapshot, the Lardon3DTaskReconstructionContext passed through context, and
 * binding are required and caller-owned. On success binding owns newly
 * allocated userdata through userdata_destroy. Group count/cursor and the
 * exact generic kind/version are validated; no Capture identity is guessed. */
bool lardon3d_acquisition_campaign_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

/* Encode a deterministic v1 payload without retaining input storage. output
 * may be NULL only with capacity==0 for a size probe. size is required: after a
 * valid request it receives the exact required byte count even when capacity is
 * insufficient and false is returned; invalid input or an encoding exception
 * sets it to zero. On success that same value is the written byte count. */
bool lardon3d_acquisition_campaign_request_encode(
    const Lardon3DAcquisitionCampaignTaskRequest *request,
    unsigned char *output, size_t capacity, size_t *size);
/* Strictly decode one bounded immutable payload into caller-owned arrays.
 * Their capacities must cover the encoded counts. On success request borrows
 * those arrays for the caller-controlled lifetime; malformed/truncated input,
 * trailing bytes, invalid enums/counts, or insufficient capacity fail. */
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
