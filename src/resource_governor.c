#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <lardon3d/resource_governor.h>

struct Lardon3DResourceReservation {
    Lardon3DResourceReservationInfo information;
    uint64_t charged_memory_bytes;
    struct Lardon3DResourceReservation *next;
};

enum {
    LARDON3D_BATCH_METRICS_CAPACITY = 8,
};

typedef struct {
    size_t batch_size;
    uint64_t duration_ns;
    size_t peak_memory_bytes;
} Lardon3DBatchMetrics;

struct Lardon3DResourceGovernor {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint64_t generation;
    Lardon3DHardwareProfile profile;
    Lardon3DResourcePolicy policy;
    uint64_t next_reservation_id;
    uint64_t memory_reserved_bytes;
    uint64_t gpu_memory_reserved_bytes;
    unsigned int cpu_reserved;
    unsigned int gpu_slots_reserved;
    unsigned int io_slots_reserved;
    Lardon3DBatchMetrics batch_metrics[LARDON3D_RESOURCE_TASK_MIXED + 1][LARDON3D_BATCH_METRICS_CAPACITY];
    size_t batch_metrics_count[LARDON3D_RESOURCE_TASK_MIXED + 1];
    size_t batch_metrics_head[LARDON3D_RESOURCE_TASK_MIXED + 1];
    size_t active_count;
    Lardon3DResourceReservation *active;
    Lardon3DResourceReservation *released;
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
        && policy->io_slot_capacity > 0
        && (!profile->gpu_available || policy->gpu_slot_capacity > 0)
        && (!profile->gpu_memory_known
            || policy->gpu_memory_reserve_bytes
                < profile->gpu_memory_total_bytes);
}

static bool
valid_snapshot(
    const Lardon3DHardwareProfile *profile,
    const Lardon3DResourceSnapshot *snapshot
)
{
    return snapshot && snapshot->cpu_load_1m >= 0.0
        && (!snapshot->io_pressure_known
            || (snapshot->io_pressure_avg10 >= 0.0
                && snapshot->io_pressure_avg10 <= 100.0))
        && (!snapshot->gpu_memory_available_known
            || !profile->gpu_memory_known
            || profile->gpu_uses_shared_memory
            || snapshot->gpu_memory_available_bytes
                <= profile->gpu_memory_total_bytes);
}

static bool
valid_estimate(const Lardon3DResourceEstimate *estimate)
{
    return estimate && estimate->minimum_batch_size > 0
        && estimate->maximum_batch_size >= estimate->minimum_batch_size
        && estimate->desired_cpu_threads > 0
        && estimate->task_class >= LARDON3D_RESOURCE_TASK_GENERAL
        && estimate->task_class <= LARDON3D_RESOURCE_TASK_MIXED
        && ((estimate->gpu_memory_fixed_bytes == 0
                && estimate->gpu_memory_bytes_per_item == 0)
            || estimate->desired_gpu_slots > 0);
}

static uint64_t
subtract_floor(uint64_t value, uint64_t amount)
{
    return value > amount ? value - amount : 0;
}

static unsigned int
subtract_unsigned(unsigned int value, unsigned int amount)
{
    return value > amount ? value - amount : 0;
}

static uint64_t
minimum_uint64(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static size_t
minimum_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

static bool
resource_size(
    uint64_t fixed,
    uint64_t per_item,
    size_t batch,
    uint64_t *total
)
{
    if (per_item > 0 && batch > UINT64_MAX / per_item) {
        return false;
    }
    uint64_t variable = per_item * (uint64_t)batch;
    if (fixed > UINT64_MAX - variable) {
        return false;
    }
    *total = fixed + variable;
    return true;
}

static size_t
batch_capacity(uint64_t available, uint64_t fixed, uint64_t per_item)
{
    if (available < fixed) {
        return 0;
    }
    if (per_item == 0) {
        return SIZE_MAX;
    }
    uint64_t capacity = (available - fixed) / per_item;
    return capacity > SIZE_MAX ? SIZE_MAX : (size_t)capacity;
}

static bool
record_batch_locked(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t batch_size,
    uint64_t duration_ns,
    size_t peak_memory_bytes
)
{
    if (batch_size == 0) {
        return false;
    }
    size_t class_index = (size_t)task_class;
    if (class_index > (size_t)LARDON3D_RESOURCE_TASK_MIXED) {
        return false;
    }
    Lardon3DBatchMetrics *metrics = governor->batch_metrics[class_index];
    size_t count = governor->batch_metrics_count[class_index];
    size_t head = governor->batch_metrics_head[class_index];
    metrics[head] = (Lardon3DBatchMetrics) {
        .batch_size = batch_size,
        .duration_ns = duration_ns,
        /* Zéro est le marqueur persistant « mesure inconnue ». La boucle
         * d'adaptation ignore explicitement ces échantillons. */
        .peak_memory_bytes = peak_memory_bytes,
    };
    head = (head + 1) % LARDON3D_BATCH_METRICS_CAPACITY;
    governor->batch_metrics_head[class_index] = head;
    if (count < LARDON3D_BATCH_METRICS_CAPACITY) {
        governor->batch_metrics_count[class_index] = count + 1;
    }
    return true;
}

static size_t
adaptive_batch_limit(
    const Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t static_batch,
    uint64_t memory_bytes_per_item
)
{
    if (memory_bytes_per_item == 0 || static_batch == 0) {
        return static_batch;
    }
    size_t class_index = (size_t)task_class;
    if (class_index > (size_t)LARDON3D_RESOURCE_TASK_MIXED) {
        return static_batch;
    }
    size_t count = governor->batch_metrics_count[class_index];
    if (count == 0) {
        return static_batch;
    }
    size_t head = governor->batch_metrics_head[class_index];
    uint64_t measured_per_item = 0;
    size_t samples = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (head + LARDON3D_BATCH_METRICS_CAPACITY - count + i)
            % LARDON3D_BATCH_METRICS_CAPACITY;
        const Lardon3DBatchMetrics *m = &governor->batch_metrics[class_index][idx];
        if (m->batch_size > 0 && m->peak_memory_bytes > 0) {
            /* Coût par élément le plus défavorable observé : une moyenne
             * sous-estimerait le pic et laisserait un lot dépasser son
             * budget. La stabilité de l'hôte prime sur le débit. */
            uint64_t per_item = m->peak_memory_bytes / m->batch_size
                + (m->peak_memory_bytes % m->batch_size != 0);
            if (per_item > measured_per_item) {
                measured_per_item = per_item;
            }
            ++samples;
        }
    }
    if (samples == 0) {
        return static_batch;
    }
    if (measured_per_item <= memory_bytes_per_item) {
        return static_batch;
    }
    /* Éviter l'overflow de la multiplication : si static_batch est trop
     * grand pour être multiplié sans débordement, on retourne 1 (le lot le
     * plus conservateur possible) plutôt que de saturer à SIZE_MAX. */
    if (static_batch > UINT64_MAX / memory_bytes_per_item) {
        return 1;
    }
    uint64_t corrected = (uint64_t)static_batch * memory_bytes_per_item
        / measured_per_item;
    size_t result = corrected > SIZE_MAX ? SIZE_MAX : (size_t)corrected;
    return result > 0 ? result : 1;
}

static void
set_decision(
    Lardon3DResourceDecision *decision,
    Lardon3DResourceDecisionKind kind,
    size_t batch_size,
    unsigned int cpu_threads,
    unsigned int gpu_slots,
    unsigned int io_slots,
    const char *reason
)
{
    *decision = (Lardon3DResourceDecision) {
        .kind = kind,
        .batch_size = batch_size,
        .cpu_threads = cpu_threads,
        .gpu_slots = gpu_slots,
        .io_slots = io_slots,
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
        .gpu_slot_capacity = profile->gpu_available ? 1 : 0,
        .io_slot_capacity = 1,
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
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        (void)pthread_mutex_destroy(&governor->mutex);
        free(governor);
        return NULL;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&governor->mutex);
        free(governor);
        return NULL;
    }
    if (pthread_cond_init(&governor->cond, &attr) != 0) {
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&governor->mutex);
        free(governor);
        return NULL;
    }
    (void)pthread_condattr_destroy(&attr);
    governor->generation = 0;
    governor->profile = *profile;
    governor->policy = *policy;
    governor->next_reservation_id = 1;
    return governor;
}

void
lardon3d_resource_governor_destroy(Lardon3DResourceGovernor *governor)
{
    if (!governor) {
        return;
    }
    Lardon3DResourceReservation *reservation = governor->active;
    while (reservation) {
        Lardon3DResourceReservation *next = reservation->next;
        free(reservation);
        reservation = next;
    }
    reservation = governor->released;
    while (reservation) {
        Lardon3DResourceReservation *next = reservation->next;
        free(reservation);
        reservation = next;
    }
    (void)pthread_cond_destroy(&governor->cond);
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
    bool policy_valid = valid_policy(&governor->profile, policy);
    uint64_t memory_budget = policy_valid
        ? governor->profile.memory_total_bytes
            - policy->system_memory_reserve_bytes
        : 0;
    uint64_t gpu_budget = policy_valid && governor->profile.gpu_memory_known
        && !governor->profile.gpu_uses_shared_memory
        ? governor->profile.gpu_memory_total_bytes
            - policy->gpu_memory_reserve_bytes
        : UINT64_MAX;
    bool accepted = policy_valid
        && memory_budget >= governor->memory_reserved_bytes
        && gpu_budget >= governor->gpu_memory_reserved_bytes
        && governor->profile.logical_cpu_count - policy->system_cpu_reserve
            >= governor->cpu_reserved
        && policy->gpu_slot_capacity >= governor->gpu_slots_reserved
        && policy->io_slot_capacity >= governor->io_slots_reserved;
    if (accepted) {
        governor->policy = *policy;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return accepted;
}

static bool
availability_locked(
    const Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    Lardon3DResourceAvailability *availability
)
{
    if (!valid_snapshot(&governor->profile, snapshot)) {
        return false;
    }
    uint64_t detected_memory = minimum_uint64(
        snapshot->memory_available_bytes,
        governor->profile.memory_total_bytes
    );
    uint64_t memory_budget = subtract_floor(
        detected_memory,
        governor->policy.system_memory_reserve_bytes
    );
    uint64_t gpu_budget = 0;
    bool gpu_known = false;
    if (governor->profile.gpu_uses_shared_memory) {
        gpu_known = true;
        gpu_budget = memory_budget;
    } else if (governor->profile.gpu_memory_known
        && snapshot->gpu_memory_available_known) {
        gpu_known = true;
        gpu_budget = subtract_floor(
            snapshot->gpu_memory_available_bytes,
            governor->policy.gpu_memory_reserve_bytes
        );
    }
    unsigned int cpu_budget = governor->profile.logical_cpu_count
        - governor->policy.system_cpu_reserve;
    *availability = (Lardon3DResourceAvailability) {
        .memory_budget_bytes = memory_budget,
        .memory_reserved_bytes = governor->memory_reserved_bytes,
        .memory_available_bytes = subtract_floor(
            memory_budget,
            governor->memory_reserved_bytes
        ),
        .gpu_memory_known = gpu_known,
        .gpu_memory_budget_bytes = gpu_budget,
        .gpu_memory_reserved_bytes = governor->gpu_memory_reserved_bytes,
        .gpu_memory_available_bytes = governor->profile.gpu_uses_shared_memory
            ? subtract_floor(memory_budget, governor->memory_reserved_bytes)
            : subtract_floor(gpu_budget, governor->gpu_memory_reserved_bytes),
        .cpu_budget = cpu_budget,
        .cpu_reserved = governor->cpu_reserved,
        .cpu_available = subtract_unsigned(cpu_budget, governor->cpu_reserved),
        .gpu_slot_budget = governor->policy.gpu_slot_capacity,
        .gpu_slots_reserved = governor->gpu_slots_reserved,
        .gpu_slots_available = subtract_unsigned(
            governor->policy.gpu_slot_capacity,
            governor->gpu_slots_reserved
        ),
        .io_slot_budget = governor->policy.io_slot_capacity,
        .io_slots_reserved = governor->io_slots_reserved,
        .io_slots_available = subtract_unsigned(
            governor->policy.io_slot_capacity,
            governor->io_slots_reserved
        ),
        .active_reservations = governor->active_count,
    };
    return true;
}

bool
lardon3d_resource_governor_availability(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    Lardon3DResourceAvailability *availability
)
{
    if (!governor || !snapshot || !availability) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool success = availability_locked(governor, snapshot, availability);
    (void)pthread_mutex_unlock(&governor->mutex);
    return success;
}

uint64_t
lardon3d_resource_governor_generation(
    Lardon3DResourceGovernor *governor
)
{
    if (!governor) {
        return 0;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    uint64_t generation = governor->generation;
    (void)pthread_mutex_unlock(&governor->mutex);
    return generation;
}

bool
lardon3d_resource_governor_wait_for_change(
    Lardon3DResourceGovernor *governor,
    uint64_t observed_generation,
    uint64_t timeout_ns
)
{
    if (!governor) {
        return false;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    uint64_t remainder = timeout_ns % 1000000000ULL;
    deadline.tv_nsec += (long)remainder;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool changed = false;
    while (governor->generation == observed_generation) {
        int result = pthread_cond_timedwait(
            &governor->cond,
            &governor->mutex,
            &deadline
        );
        if (result == ETIMEDOUT) {
            break;
        }
        if (result != 0) {
            (void)pthread_mutex_unlock(&governor->mutex);
            return false;
        }
    }
    changed = governor->generation != observed_generation;
    (void)pthread_mutex_unlock(&governor->mutex);
    return changed;
}

static void
evaluate_locked(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision
)
{
    if (!valid_estimate(estimate)) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Estimation de ressources invalide.");
        return;
    }
    if ((estimate->desired_gpu_slots > 0
            || estimate->gpu_memory_fixed_bytes > 0
            || estimate->gpu_memory_bytes_per_item > 0)
        && !governor->profile.gpu_available) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Aucun GPU disponible.");
        return;
    }

    uint64_t memory_fixed = estimate->memory_fixed_bytes;
    uint64_t memory_per_item = estimate->memory_bytes_per_item;
    if (governor->profile.gpu_uses_shared_memory) {
        if (memory_fixed > UINT64_MAX - estimate->gpu_memory_fixed_bytes
            || memory_per_item
                > UINT64_MAX - estimate->gpu_memory_bytes_per_item) {
            set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Estimation mémoire trop grande.");
            return;
        }
        memory_fixed += estimate->gpu_memory_fixed_bytes;
        memory_per_item += estimate->gpu_memory_bytes_per_item;
    }

    size_t theoretical_batch = batch_capacity(
        governor->profile.memory_total_bytes
            - governor->policy.system_memory_reserve_bytes,
        memory_fixed,
        memory_per_item
    );
    if (!governor->profile.gpu_uses_shared_memory
        && (estimate->gpu_memory_fixed_bytes > 0
            || estimate->gpu_memory_bytes_per_item > 0)
        && governor->profile.gpu_memory_known) {
        theoretical_batch = minimum_size(
            theoretical_batch,
            batch_capacity(
                governor->profile.gpu_memory_total_bytes
                    - governor->policy.gpu_memory_reserve_bytes,
                estimate->gpu_memory_fixed_bytes,
                estimate->gpu_memory_bytes_per_item
            )
        );
    }
    if (theoretical_batch < estimate->minimum_batch_size) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Tâche impossible sur cette machine.");
        return;
    }

    double load_limit = (double)governor->profile.logical_cpu_count
        * governor->policy.maximum_cpu_load_ratio;
    if (snapshot->cpu_load_1m >= load_limit) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Charge CPU trop élevée.");
        return;
    }
    if (estimate->desired_io_slots > 0 && snapshot->io_pressure_known
        && snapshot->io_pressure_avg10
            >= governor->policy.maximum_io_pressure_avg10) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Pression d'entrée-sortie trop élevée.");
        return;
    }

    Lardon3DResourceAvailability available;
    if (!availability_locked(governor, snapshot, &available)) {
        set_decision(decision, LARDON3D_RESOURCE_REJECT, 0, 0, 0, 0, "Instantané de ressources invalide.");
        return;
    }
    size_t batch = batch_capacity(
        available.memory_available_bytes,
        memory_fixed,
        memory_per_item
    );
    if (!governor->profile.gpu_uses_shared_memory
        && (estimate->gpu_memory_fixed_bytes > 0
            || estimate->gpu_memory_bytes_per_item > 0)) {
        if (!available.gpu_memory_known) {
            set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Mémoire GPU disponible inconnue.");
            return;
        }
        batch = minimum_size(
            batch,
            batch_capacity(
                available.gpu_memory_available_bytes,
                estimate->gpu_memory_fixed_bytes,
                estimate->gpu_memory_bytes_per_item
            )
        );
    }
    /* Le lot maximal visé est corrigé par les métriques mesurées : c'est la
     * nouvelle cible du contrat, pas une réduction faute de ressources. La
     * correction ne descend jamais sous minimum_batch_size pour éviter un
     * WAIT persistant. */
    size_t adapted_maximum = adaptive_batch_limit(
        governor,
        estimate->task_class,
        estimate->maximum_batch_size,
        memory_per_item
    );
    if (adapted_maximum < estimate->minimum_batch_size) {
        adapted_maximum = estimate->minimum_batch_size;
    }
    batch = minimum_size(batch, adapted_maximum);
    if (batch < estimate->minimum_batch_size) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Ressources déjà réservées ou temporairement insuffisantes.");
        return;
    }
    if (available.cpu_available == 0
        || (estimate->desired_gpu_slots > 0
            && available.gpu_slots_available == 0)
        || (estimate->desired_io_slots > 0
            && available.io_slots_available == 0)) {
        set_decision(decision, LARDON3D_RESOURCE_WAIT, 0, 0, 0, 0, "Slots de calcul déjà réservés.");
        return;
    }
    unsigned int cpu = estimate->desired_cpu_threads < available.cpu_available
        ? estimate->desired_cpu_threads
        : available.cpu_available;
    unsigned int gpu = estimate->desired_gpu_slots
        < available.gpu_slots_available
        ? estimate->desired_gpu_slots
        : available.gpu_slots_available;
    unsigned int io = estimate->desired_io_slots < available.io_slots_available
        ? estimate->desired_io_slots
        : available.io_slots_available;
    bool reduced = batch < adapted_maximum
        || cpu < estimate->desired_cpu_threads
        || gpu < estimate->desired_gpu_slots
        || io < estimate->desired_io_slots;
    set_decision(
        decision,
        reduced ? LARDON3D_RESOURCE_REDUCE_BATCH : LARDON3D_RESOURCE_START,
        batch,
        cpu,
        gpu,
        io,
        reduced ? "Contrat réduit aux ressources disponibles."
            : "Ressources disponibles."
    );
}

bool
lardon3d_resource_governor_reserve(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
)
{
    if (!governor || !snapshot || !estimate || !decision || !reservation) {
        return false;
    }
    *reservation = NULL;
    Lardon3DResourceReservation *created = calloc(1, sizeof(*created));
    if (!created) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    if (!valid_snapshot(&governor->profile, snapshot)) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return false;
    }
    evaluate_locked(governor, snapshot, estimate, decision);
    if ((decision->kind != LARDON3D_RESOURCE_START
            && decision->kind != LARDON3D_RESOURCE_REDUCE_BATCH)
        || governor->next_reservation_id == 0) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return true;
    }
    uint64_t memory_bytes;
    uint64_t gpu_memory_bytes;
    if (!resource_size(
            estimate->memory_fixed_bytes,
            estimate->memory_bytes_per_item,
            decision->batch_size,
            &memory_bytes
        )
        || !resource_size(
            estimate->gpu_memory_fixed_bytes,
            estimate->gpu_memory_bytes_per_item,
            decision->batch_size,
            &gpu_memory_bytes
        )
        || clock_gettime(CLOCK_REALTIME, &created->information.created_at) != 0) {
        (void)pthread_mutex_unlock(&governor->mutex);
        free(created);
        return false;
    }
    created->information.id = governor->next_reservation_id++;
    created->information.memory_bytes = memory_bytes;
    created->information.gpu_memory_bytes = gpu_memory_bytes;
    created->information.cpu_threads = decision->cpu_threads;
    created->information.gpu_slots = decision->gpu_slots;
    created->information.io_slots = decision->io_slots;
    created->information.batch_size = decision->batch_size;
    created->information.task_class = estimate->task_class;
    created->information.state = LARDON3D_RESERVATION_ACTIVE;
    created->charged_memory_bytes = governor->profile.gpu_uses_shared_memory
        ? memory_bytes + gpu_memory_bytes
        : memory_bytes;
    created->next = governor->active;
    governor->active = created;
    governor->memory_reserved_bytes += created->charged_memory_bytes;
    governor->gpu_memory_reserved_bytes += gpu_memory_bytes;
    governor->cpu_reserved += decision->cpu_threads;
    governor->gpu_slots_reserved += decision->gpu_slots;
    governor->io_slots_reserved += decision->io_slots;
    ++governor->active_count;
    *reservation = created;
    ++governor->generation;
    (void)pthread_cond_broadcast(&governor->cond);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_reserve_available(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DResourceDecision *decision,
    Lardon3DResourceReservation **reservation
)
{
    if (!governor || !estimate || !decision || !reservation) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DHardwareProfile profile = governor->profile;
    (void)pthread_mutex_unlock(&governor->mutex);
    Lardon3DResourceSnapshot snapshot;
    if (!lardon3d_resource_snapshot_capture(&profile, &snapshot, NULL, 0)) {
        return false;
    }
    return lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        estimate,
        decision,
        reservation
    );
}

static Lardon3DResourceReservation *
find_reservation(
    Lardon3DResourceReservation *head,
    const Lardon3DResourceReservation *wanted
)
{
    for (Lardon3DResourceReservation *current = head;
         current;
         current = current->next) {
        if (current == wanted) {
            return current;
        }
    }
    return NULL;
}

bool
lardon3d_resource_governor_release(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation *reservation
)
{
    if (!governor || !reservation) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceReservation *previous = NULL;
    Lardon3DResourceReservation *current = governor->active;
    while (current && current != reservation) {
        previous = current;
        current = current->next;
    }
    if (!current) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    if (previous) {
        previous->next = current->next;
    } else {
        governor->active = current->next;
    }
    governor->memory_reserved_bytes -= current->charged_memory_bytes;
    governor->gpu_memory_reserved_bytes -= current->information.gpu_memory_bytes;
    governor->cpu_reserved -= current->information.cpu_threads;
    governor->gpu_slots_reserved -= current->information.gpu_slots;
    governor->io_slots_reserved -= current->information.io_slots;
    --governor->active_count;
    current->information.state = LARDON3D_RESERVATION_RELEASED;
    current->next = governor->released;
    governor->released = current;
    ++governor->generation;
    (void)pthread_cond_broadcast(&governor->cond);
    (void)pthread_mutex_unlock(&governor->mutex);
    return true;
}

bool
lardon3d_resource_governor_reservation_is_valid(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation
)
{
    if (!governor || !reservation) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool valid = find_reservation(governor->active, reservation) != NULL;
    (void)pthread_mutex_unlock(&governor->mutex);
    return valid;
}

bool
lardon3d_resource_reservation_get(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation,
    Lardon3DResourceReservationInfo *information
)
{
    if (!governor || !reservation || !information) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceReservation *found = find_reservation(
        governor->active,
        reservation
    );
    if (!found) {
        found = find_reservation(governor->released, reservation);
    }
    if (found) {
        *information = found->information;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return found != NULL;
}

bool
lardon3d_resource_reservation_get_active(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation,
    Lardon3DResourceReservationInfo *information
)
{
    if (!governor || !reservation || !information) {
        return false;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    Lardon3DResourceReservation *found = find_reservation(
        governor->active,
        reservation
    );
    if (found) {
        *information = found->information;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return found != NULL;
}

size_t
lardon3d_resource_governor_list_reservations(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservationInfo *reservations,
    size_t capacity
)
{
    if (!governor || (!reservations && capacity > 0)) {
        return 0;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    size_t count = 0;
    for (Lardon3DResourceReservation *current = governor->active;
         current && count < capacity;
         current = current->next) {
        reservations[count++] = current->information;
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return count;
}

size_t
lardon3d_resource_governor_reservation_count(
    Lardon3DResourceGovernor *governor
)
{
    if (!governor) {
        return 0;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    size_t count = governor->active_count;
    (void)pthread_mutex_unlock(&governor->mutex);
    return count;
}

bool
lardon3d_resource_governor_record_batch(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t batch_size,
    uint64_t duration_ns,
    size_t peak_memory_bytes
)
{
    if (!governor) {
        return false;
    }
    if (batch_size == 0) {
        /* No-op réussi : aucune métrique, aucun réveil inutile. */
        return true;
    }
    (void)pthread_mutex_lock(&governor->mutex);
    bool recorded = record_batch_locked(
        governor,
        task_class,
        batch_size,
        duration_ns,
        peak_memory_bytes
    );
    if (recorded) {
        ++governor->generation;
        (void)pthread_cond_broadcast(&governor->cond);
    }
    (void)pthread_mutex_unlock(&governor->mutex);
    return recorded;
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
    Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = request->memory_bytes_per_item,
        .gpu_memory_bytes_per_item = request->gpu_memory_bytes_per_item,
        .minimum_batch_size = request->minimum_batch_size,
        .maximum_batch_size = request->preferred_batch_size,
        .desired_cpu_threads = request->requested_cpu_threads,
        .desired_gpu_slots = request->gpu_memory_bytes_per_item > 0 ? 1 : 0,
        .desired_io_slots = request->io_intensive ? 1 : 0,
        .task_class = request->io_intensive
            ? LARDON3D_RESOURCE_TASK_IO
            : LARDON3D_RESOURCE_TASK_GENERAL,
    };
    (void)pthread_mutex_lock(&governor->mutex);
    if (!valid_snapshot(&governor->profile, snapshot)) {
        (void)pthread_mutex_unlock(&governor->mutex);
        return false;
    }
    evaluate_locked(governor, snapshot, &estimate, decision);
    (void)pthread_mutex_unlock(&governor->mutex);
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
