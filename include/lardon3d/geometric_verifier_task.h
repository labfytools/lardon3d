#ifndef LARDON3D_GEOMETRIC_VERIFIER_TASK_H
#define LARDON3D_GEOMETRIC_VERIFIER_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/geometric_verifier.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND "geometric_verifier.run"

enum {
  LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION = 1,
  LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH = 1,
  /* Sixteen is the independently safe preparation/participant window, not a
   * scientific dataset limit. Eight is the largest width with a material gain
   * at the durable Task boundary; Governor admits only within that lower
   * operational range. */
  LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH = 16,
  LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_SAFE_CPU_THREADS = 16,
  LARDON3D_GEOMETRIC_VERIFIER_TASK_VALIDATED_USEFUL_CPU_THREADS = 8,
};

typedef struct {
  Lardon3DGeometricVerifierParameters verifier;
} Lardon3DGeometricVerifierTaskConfiguration;

/* Creates and durably pre-creates one recoverable GV Task. `state` retains
 * ownership of Project DB/Governor; the returned Task owns its private
 * execution context and must be destroyed or transferred to Queue. A non-NULL
 * `task_id` is initialized to zero and retains the durable ID only after Task
 * creation succeeds. Each GVR identity remains the exact parent, verifier kind
 * and version, and parameter fingerprint; operational CPU/batch choices do not
 * alter its scientific payload. */
Lardon3DTask *lardon3d_project_create_geometric_verifier_task(
    Lardon3DAppState *state,
    const Lardon3DGeometricVerifierTaskConfiguration *configuration,
    uint64_t *task_id);
/* Transfers the created Task to Queue on success. If durable creation succeeds
 * but transfer fails, the returned false result retains its nonzero `task_id`
 * so recovery can find the pending durable work; no GVR identity is guessed. */
bool lardon3d_project_enqueue_geometric_verifier_task(
    Lardon3DAppState *state,
    const Lardon3DGeometricVerifierTaskConfiguration *configuration,
    uint64_t *task_id);
/* Registry reconstruction borrows snapshot/runtime inputs and returns one
 * binding whose userdata is owned by the restored Task on success. Exact
 * historical resource envelopes may be normalized in memory by Registry;
 * durable scientific fingerprints and cursors remain unchanged. */
bool lardon3d_geometric_verifier_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
/* Test-only observation of real callback contracts; one bit per admitted CPU
 * width. It changes neither Governor selection nor production execution. */
void lardon3d_geometric_verifier_task_test_reset_cpu_contracts(void);
unsigned int lardon3d_geometric_verifier_task_test_cpu_contracts(void);
/* Deterministic test acknowledgement immediately before sequence_break. Arm
 * before enqueue, wait for the callback, change Governor policy, then release;
 * no production reservation or scheduling rule is bypassed. */
void lardon3d_geometric_verifier_task_test_arm_sequence_barrier(void);
bool lardon3d_geometric_verifier_task_test_wait_sequence_barrier(void);
void lardon3d_geometric_verifier_task_test_release_sequence_barrier(void);
/* Test-only acknowledgement after every preparation participant is joined and
 * the complete batch has passed status/ownership preflight, but before the
 * callback owner publishes its first row. An external control request made
 * while this barrier is held must be observed only after this already-engaged
 * batch is published and checkpointed. */
void lardon3d_geometric_verifier_task_test_arm_prepublication_barrier(void);
bool lardon3d_geometric_verifier_task_test_wait_prepublication_barrier(void);
void lardon3d_geometric_verifier_task_test_release_prepublication_barrier(void);
#endif

#endif
