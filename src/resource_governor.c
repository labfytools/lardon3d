#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <lardon3d/resource_governor.h>

struct Lardon3DResourceGovernor {
    pthread_mutex_t mutex;
    Lardon3DHardwareProfile profile;
    Lardon3DResourcePolicy policy;
};

static bool
valid_profile(const Lardon3DHardwareProfile *profile)
{
    return profile && profile->logical_cpu_count > 0
        && profile->memory_total_bytes > 0
        && (!profile->gpu_memory_known
            || (profile->gpu_available
                && profile->gpu_memory_total_bytes > 0));
}

static bool
valid_policy(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy
)
{
    return valid_profile(profile) && policy
        && policy->system_memory_reserve_bytes < profile->memory_total_bytes
        && policy->system_cpu_reserve < profile->logical_cpu_count
        && policy->maximum_cpu_load_ratio > 0.0
        && policy->maximum_cpu_load_ratio <= 1.0
        && policy->maximum_io_pressure_avg10 >= 0.0
        && policy->maximum_io_pressure_avg10 <= 100.0
        && (!profile->gpu_memory_known
            || policy->gpu_memory_reserve_bytes
                < profile->gpu_memory_total_bytes);
}

static void
set_decision(
    Lardon3DResourceDecision *decision,
    Lardon3DResourceDecisionKind kind,
    size_t batch_size,
    unsigned int cpu_threads,
    const char *reason
)
{
    *decision = (Lardon3DResourceDecision) {
        .kind = kind,
        .batch_size = batch_size,
        .cpu_threads = cpu_threads,
    };
    (void)snprintf(decision->reason, sizeof(decision->reason), "%s", reason);
}

bool
lardon3d_resource_policy_default(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourcePolicy *policy
)
{
    if (!valid_profile(profile) || !policy) {
        return false;
    }
    *policy = (Lardon3DResourcePolicy) {
        .system_memory_reserve_bytes = profile->memory_total_bytes / 8,
        .gpu_memory_reserve_bytes = profile->gpu_memory_known
            ? profile->gpu_memory_total_bytes / 8
            : 0,
        .system_cpu_reserve = profile->logical_cpu_count > 2 ? 1 : 0,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_io_pressure_avg10 = 80.0,
    };
    return valid_policy(profile, policy);
}

Lardon3DResourceGovernor *
lardon3d_resource_governor_create(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourcePolicy *policy
)
{
    if (!valid_policy(profile, policy)) {
        return NULL;
    }
    Lardon3DResourceGovernor *governor = calloc(1, sizeof(*governor));
    if (!governor) {
        return NULL;
    }
    if (pthread_mutex_init(&governor->mutex, NULL) != 0) {
        free(governor);
        return NULL;
    }
    governor->profile = *profile;
    governor->policy = *policy;
    return governor;
}

void
lardon3d_resource_governor_destroy(Lardon3DResourceGovernor *governor)
{
    if (!governor) {
        return;
    }
    (void)pthread_mutex_destroy(&governor->mutex);
    free(governor);
}

bool
lardon3d_resource_governor_set_policy(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourcePolicy *policy
)
{
    if (!governor || !policy) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool accepted = valid_policy(&governor->profile, policy);
    if (accepted) {
        governor->policy = *policy;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return accepted;
}

static size_t
capacity_for(uint64_t available, uint64_t reserve, uint64_t per_item)
{
    if (per_item == 0) {
        return SIZE_MAX;
    }
    if (available <= reserve) {
        return 0;
    }
    uint64_t capacity = (available - reserve) / per_item;
    return capacity > SIZE_MAX ? SIZE_MAX : (size_t)capacity;
}

static size_t
minimum_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

bool
lardon3d_resource_governor_decide(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceRequest *request,
    Lardon3DResourceDecision *decision
)
{
    if (!governor || !snapshot || !request || !decision) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    const Lardon3DHardwareProfile profile = governor->profile;
    const Lardon3DResourcePolicy policy = governor->policy;
    (void)pthread_mutex_unlock(&governor->mutex);

    if (!(snapshot->cpu_load_1m >= 0.0)
        || (snapshot->io_pressure_known
            && (!(snapshot->io_pressure_avg10 >= 0.0)
                || snapshot->io_pressure_avg10 > 100.0))
        || (snapshot->gpu_memory_available_known
            && profile.gpu_memory_known
            && !profile.gpu_uses_shared_memory
            && snapshot->gpu_memory_available_bytes
                > profile.gpu_memory_total_bytes)) {
        return false;
    }

    if (request->minimum_batch_size == 0
        || request->preferred_batch_size < request->minimum_batch_size
        || request->requested_cpu_threads == 0) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, "Demande de ressources invalide.");
        return true;
    }
    unsigned int available_threads = profile.logical_cpu_count
        - policy.system_cpu_reserve;
    unsigned int cpu_threads = request->requested_cpu_threads < available_threads
        ? request->requested_cpu_threads
        : available_threads;

    uint64_t system_memory_per_item = request->memory_bytes_per_item;
    if (request->gpu_memory_bytes_per_item > 0
        && profile.gpu_uses_shared_memory) {
        if (system_memory_per_item
            > UINT64_MAX - request->gpu_memory_bytes_per_item) {
            set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, cpu_threads, "Besoin mémoire trop grand.");
            return true;
        }
        system_memory_per_item += request->gpu_memory_bytes_per_item;
    }
    size_t theoretical_batch = capacity_for(
        profile.memory_total_bytes,
        policy.system_memory_reserve_bytes,
        system_memory_per_item
    );
    if (request->gpu_memory_bytes_per_item > 0) {
        if (!profile.gpu_available) {
            set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, cpu_threads, "Aucun GPU disponible.");
            return true;
        }
        if (profile.gpu_memory_known && !profile.gpu_uses_shared_memory) {
            theoretical_batch = minimum_size(
                theoretical_batch,
                capacity_for(
                    profile.gpu_memory_total_bytes,
                    policy.gpu_memory_reserve_bytes,
                    request->gpu_memory_bytes_per_item
                )
            );
        }
    }
    if (theoretical_batch < request->minimum_batch_size) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, cpu_threads, "Tâche impossible sur cette machine.");
        return true;
    }

    double load_limit = (double)profile.logical_cpu_count
        * policy.maximum_cpu_load_ratio;
    if (snapshot->cpu_load_1m >= load_limit) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, cpu_threads, "Charge CPU trop élevée.");
        return true;
    }
    if (request->io_intensive && snapshot->io_pressure_known
        && snapshot->io_pressure_avg10 >= policy.maximum_io_pressure_avg10) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, cpu_threads, "Pression d'entrée-sortie trop élevée.");
        return true;
    }

    size_t current_batch = capacity_for(
        snapshot->memory_available_bytes,
        policy.system_memory_reserve_bytes,
        system_memory_per_item
    );
    if (request->gpu_memory_bytes_per_item > 0
        && !profile.gpu_uses_shared_memory) {
        if (!snapshot->gpu_memory_available_known) {
            set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, cpu_threads, "Mémoire GPU disponible inconnue.");
            return true;
        }
        current_batch = minimum_size(
            current_batch,
            capacity_for(
                snapshot->gpu_memory_available_bytes,
                policy.gpu_memory_reserve_bytes,
                request->gpu_memory_bytes_per_item
            )
        );
    }
    if (current_batch < request->minimum_batch_size) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, cpu_threads, "Ressources temporairement insuffisantes.");
        return true;
    }
    size_t batch_size = minimum_size(
        current_batch,
        request->preferred_batch_size
    );
    if (batch_size < request->preferred_batch_size) {
        set_decision(decision, LARDON3D_RESOURCE_REDUCE_BATCH, batch_size, cpu_threads, "Taille de lot réduite.");
        return true;
    }
    set_decision(decision, LARDON3D_RESOURCE_START, batch_size, cpu_threads, "Ressources disponibles.");
    return true;
}

const char *
lardon3d_resource_decision_name(Lardon3DResourceDecisionKind kind)
{
    switch (kind) {
    case LARDON3D_RESOURCE_START:
        return "Démarrer";
    case LARDON3D_RESOURCE_WAIT:
        return "Attendre";
    case LARDON3D_RESOURCE_REDUCE_BATCH:
        return "Réduire le lot";
    case LARDON3D_RESOURCE_REJECT:
        return "Refuser";
    default:
        return "Inconnue";
    }
}
