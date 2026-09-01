#include <ncurses.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/layout.h>

static attr_t
semantic_attribute(
    const Lardon3DTuiPalette *palette,
    Lardon3DTuiSemantic semantic
)
{
    if (!palette || semantic < LARDON3D_TUI_SEMANTIC_NORMAL
        || semantic >= LARDON3D_TUI_SEMANTIC_COUNT) {
        return A_NORMAL;
    }
    attr_t attribute = A_NORMAL;
    if (palette->color_enabled && palette->color_pair[semantic] > 0) {
        attribute |= COLOR_PAIR(palette->color_pair[semantic]);
    }
    if ((palette->attributes[semantic] & LARDON3D_TUI_STYLE_BOLD) != 0) {
        attribute |= A_BOLD;
    }
    if ((palette->attributes[semantic] & LARDON3D_TUI_STYLE_DIM) != 0) {
        attribute |= A_DIM;
    }
    return attribute;
}

static void
draw_text_style(
    int row,
    int column,
    int available,
    const char *text,
    Lardon3DTuiSemantic semantic,
    const Lardon3DTuiPalette *palette
)
{
    if (available <= 0 || row < 0 || column < 0 || !text) {
        return;
    }
    size_t length = strnlen(text, LARDON3D_TUI_TEXT_CAPACITY * 4U);
    int displayed = length < (size_t)available ? (int)length : available;
    attr_t attribute = semantic_attribute(palette, semantic);
    (void)attron(attribute);
    (void)mvaddnstr(row, column, text, displayed);
    (void)attroff(attribute);
}

static void
draw_text(int row, int column, int available, const char *text)
{
    draw_text_style(row, column, available, text,
        LARDON3D_TUI_SEMANTIC_NORMAL, NULL);
}

static void
draw_too_small(int rows, int columns)
{
    static const char message[] = "Terminal trop petit";
    int row = rows > 0 ? rows / 2 : 0;
    int column = columns > (int)(sizeof(message) - 1)
        ? (columns - (int)(sizeof(message) - 1)) / 2
        : 0;
    draw_text(row, column, columns, message);
}

static void
draw_frame(int rows, int columns)
{
    int status_separator = rows - 4;
    (void)box(stdscr, 0, 0);
    (void)mvhline(2, 1, ACS_HLINE, columns - 2);
    (void)mvhline(status_separator, 1, ACS_HLINE, columns - 2);
    (void)mvaddch(2, 0, ACS_LTEE);
    (void)mvaddch(2, columns - 1, ACS_RTEE);
    (void)mvaddch(status_separator, 0, ACS_LTEE);
    (void)mvaddch(status_separator, columns - 1, ACS_RTEE);
}

static const char *
screen_title(Lardon3DScreen screen)
{
    switch (screen) {
    case LARDON3D_SCREEN_PROJECTS:
        return "Projets";
    case LARDON3D_SCREEN_IMPORT:
        return "Import";
    case LARDON3D_SCREEN_VIEWER:
        return "Viewer";
    case LARDON3D_SCREEN_HELP:
        return "Aide / contrats runtime";
    case LARDON3D_SCREEN_TASKS:
        return "Tâches";
    case LARDON3D_SCREEN_RESOURCES:
        return "Ressources / Governor";
    case LARDON3D_SCREEN_OPTICS:
        return "Profils optiques immuables";
    case LARDON3D_SCREEN_SSD:
        return "SSD externe";
    case LARDON3D_SCREEN_HOME:
    default:
        return "Observatoire Lardon3D";
    }
}

static const char *
screen_footer(
    Lardon3DScreen screen,
    Lardon3DTuiInteractionMode interaction_mode
)
{
    Lardon3DTuiKeyContract keys = lardon3d_tui_key_contract(
        interaction_mode);
    if (keys.enter && keys.escape && keys.f10) {
        return "F10 SSD | Enter valider | ESC annuler";
    }
    if (keys.cancel_import && keys.f10) {
        return "F10 SSD | X annuler l'import | Q/ESC désactivés";
    }
    switch (screen) {
    case LARDON3D_SCREEN_PROJECTS:
        return "F10 SSD | N Nouveau O Ouvrir C Fermer | ESC Accueil F7 Optique Q";
    case LARDON3D_SCREEN_IMPORT:
        return "F10 SSD | I Importer R Recharger S Tri/Filtre X Effacer | ESC Q";
    case LARDON3D_SCREEN_TASKS:
        return "F10 SSD | ↑/↓ P pause R reprise C annuler | ESC F6 Ressources Q";
    case LARDON3D_SCREEN_RESOURCES:
        return "F10 SSD | Observation seule: CPU/GPU/batch par Governor | ESC Q";
    case LARDON3D_SCREEN_OPTICS:
        return "F10 SSD | TAB ↑/↓ [ première ] suivante B/L/C V/A/G/K/E R retry ESC Q";
    case LARDON3D_SCREEN_SSD:
        return "F10 SSD | activer/drainer/annuler drain (asynchrone) | ESC Q";
    default:
        return "F10 SSD | F1 Aide F2 Projets F3 Import F4 Viewer F5 Tâches F6 Ressources Q";
    }
}

static void
draw_input_field(
    const char *input_text,
    const char *input_label,
    int row,
    int columns
)
{
    if (!input_text || row + 1 >= LINES - 4) {
        return;
    }
    draw_text(row, 3, columns - 6, input_label);
    (void)mvaddch(row + 1, 2, '[');
    (void)mvaddch(row + 1, columns - 3, ']');
    int available = columns - 8;
    size_t length = strlen(input_text);
    const char *visible = input_text;
    if (length > (size_t)available) {
        visible += length - (size_t)available;
        length = (size_t)available;
    }
    draw_text(row + 1, 4, available, visible);
    (void)move(row + 1, 4 + (int)length);
}

static void
format_bytes(uint64_t bytes, char output[64])
{
    lardon3d_image_catalog_format_size(bytes, output, 64);
}

static void
format_duration(uint64_t seconds, char output[64])
{
    if (seconds < 60) {
        (void)snprintf(output, 64, "%llus", (unsigned long long)seconds);
    } else if (seconds < 3600) {
        (void)snprintf(output, 64, "%llum",
            (unsigned long long)(seconds / 60
                + (seconds % 60 >= 30 ? 1U : 0U)));
    } else {
        uint64_t hours = seconds / 3600;
        uint64_t minutes = (seconds % 3600) / 60;
        (void)snprintf(output, 64, "%lluh%02llu",
            (unsigned long long)hours, (unsigned long long)minutes);
    }
}

static void
draw_progress_bar(
    int row,
    int column,
    int width,
    unsigned int percentage,
    const Lardon3DTuiPalette *palette
)
{
    if (width < 8) return;
    if (width > 52) width = 52;
    int bar_width = width - 7;
    int filled = (int)((unsigned int)bar_width * percentage / 100U);
    char line[64];
    line[0] = '[';
    for (int index = 0; index < bar_width; ++index) {
        line[index + 1] = index < filled ? '#' : '-';
    }
    (void)snprintf(line + bar_width + 1,
        sizeof(line) - (size_t)bar_width - 1, "] %3u%%", percentage);
    draw_text_style(row, column, width, line,
        percentage == 100 ? LARDON3D_TUI_SEMANTIC_HEALTHY
                          : LARDON3D_TUI_SEMANTIC_CPU,
        palette);
}

static void
format_compact_progress_bar(
    unsigned int percentage,
    char output[11]
)
{
    unsigned int bounded = percentage > 100 ? 100 : percentage;
    unsigned int filled = bounded * 8U / 100U;
    output[0] = '[';
    for (unsigned int index = 0; index < 8; ++index) {
        output[index + 1] = index < filled ? '#' : '-';
    }
    output[9] = ']';
    output[10] = '\0';
}

static void
draw_project_line(const Lardon3DAppState *state, int row, int columns)
{
    char line[512];
    (void)snprintf(line, sizeof(line), "Projet: %.120s%s%.370s",
        state->project_loaded ? state->project_name : "aucun",
        state->project_loaded ? "  " : "",
        state->project_loaded ? state->project_path : "");
    draw_text(row, 2, columns - 4, line);
}

static void
draw_home(
    const Lardon3DRuntimeSnapshot *runtime,
    Lardon3DTuiViewport viewport,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    int start = 4;
    int stage_column_width = (columns - 6) / 2;
    int stage_name_width = viewport == LARDON3D_TUI_VIEWPORT_COMPACT ? 11 : 14;
    for (size_t index = 0; index < LARDON3D_TUI_STAGE_COUNT; ++index) {
        int column_group = (int)(index / 6);
        int row = start + (int)(index % 6);
        int column = 2 + column_group * stage_column_width;
        char line[128];
        (void)snprintf(line, sizeof(line), "%-*.*s %-14s",
            stage_name_width, stage_name_width,
            lardon3d_tui_stage_name(runtime->stages[index].stage),
            lardon3d_tui_stage_state_name(runtime->stages[index].state));
        draw_text_style(row, column, stage_column_width - 1, line,
            lardon3d_tui_stage_semantic(runtime->stages[index].state), palette);
    }

    int active_row = start + 6;
    if (!runtime->active_task_known) {
        draw_text_style(active_row, 2, columns - 4,
            "Tâche active: aucune", LARDON3D_TUI_SEMANTIC_DIM, palette);
        return;
    }
    const Lardon3DTaskObservation *task =
        &runtime->tasks[runtime->active_task_index];
    char line[512];
    if (viewport == LARDON3D_TUI_VIEWPORT_COMPACT
        && runtime->active_progress.durable_counts_known) {
        char bar[11];
        format_compact_progress_bar(runtime->active_progress.percentage, bar);
        (void)snprintf(line, sizeof(line),
            "%s %3u%% | %llu/%llu durable | #%llu %.12s", bar,
            runtime->active_progress.percentage_known
                ? runtime->active_progress.percentage : 0U,
            (unsigned long long)runtime->active_progress.completed,
            (unsigned long long)runtime->active_progress.total,
            (unsigned long long)task->id, task->name);
    } else if (runtime->active_progress.durable_counts_known) {
        (void)snprintf(line, sizeof(line),
            "Active #%llu %s [%s] seq=%u | %llu/%llu durable",
            (unsigned long long)task->id, task->name,
            lardon3d_task_state_name(task->state), task->sequence_count,
            (unsigned long long)runtime->active_progress.completed,
            (unsigned long long)runtime->active_progress.total);
    } else if (viewport == LARDON3D_TUI_VIEWPORT_COMPACT) {
        if (runtime->active_progress.percentage_known) {
            char bar[11];
            format_compact_progress_bar(
                runtime->active_progress.percentage, bar);
            (void)snprintf(line, sizeof(line),
                "%s runtime %3u%% | #%llu %.18s", bar,
                runtime->active_progress.percentage,
                (unsigned long long)task->id, task->name);
        } else {
            (void)snprintf(line, sizeof(line),
                "scientifique indéterminé | #%llu %.18s",
                (unsigned long long)task->id, task->name);
        }
    } else {
        (void)snprintf(line, sizeof(line),
            "Active #%llu %s [%s] seq=%u",
            (unsigned long long)task->id, task->name,
            lardon3d_task_state_name(task->state), task->sequence_count);
    }
    draw_text_style(active_row, 2, columns - 4, line,
        task->state == TASK_RUNNING ? LARDON3D_TUI_SEMANTIC_CPU
                                    : LARDON3D_TUI_SEMANTIC_WARNING,
        palette);
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL
        && runtime->active_progress.percentage_known) {
        draw_progress_bar(active_row + 1, 2,
            viewport == LARDON3D_TUI_VIEWPORT_FULL ? 52 : columns - 4,
            runtime->active_progress.percentage, palette);
    }
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        char elapsed[64] = "UNKNOWN";
        char eta[64] = "UNKNOWN";
        if (runtime->active_progress.elapsed_known) {
            format_duration(runtime->active_progress.elapsed_seconds, elapsed);
        }
        if (runtime->active_progress.eta_state == LARDON3D_TUI_ETA_KNOWN) {
            format_duration(runtime->active_progress.eta_seconds, eta);
        } else {
            (void)snprintf(eta, sizeof(eta), "%s",
                lardon3d_tui_eta_state_name(runtime->active_progress.eta_state));
        }
        char throughput[64];
        if (runtime->active_progress.throughput_known) {
            (void)snprintf(throughput, sizeof(throughput),
                runtime->active_progress.runtime_percentage
                    ? "%.1f%%/s" : "%.1f unité/s",
                runtime->active_progress.units_per_second);
        } else {
            (void)snprintf(throughput, sizeof(throughput), "UNKNOWN");
        }
        (void)snprintf(line, sizeof(line),
            "Durée %s | ETA %s | débit %s%s", elapsed, eta, throughput,
            runtime->active_progress.resumed_prefix_excluded
                ? " | préfixe repris exclu" : "");
        draw_text(active_row + 2, 2, columns - 4, line);
    }
}

static void
draw_projects(const char *input_text, const char *input_label, int columns)
{
    draw_text(5, 4, columns - 6, "N : Nouveau projet");
    draw_text(6, 4, columns - 6, "O : Ouvrir un projet");
    draw_text(7, 4, columns - 6, "C : Fermer le projet");
    draw_input_field(input_text, input_label, 9, columns);
}

static void
draw_catalog(const Lardon3DAppState *state, int rows, int columns)
{
    if (!state->project_loaded || !state->image_view || !state->image_catalog) {
        draw_text(5, 4, columns - 6, "Aucun projet chargé.");
        return;
    }
    size_t count = lardon3d_image_view_count(state->image_view);
    size_t total = lardon3d_image_catalog_count(state->image_catalog);
    char line[512];
    (void)snprintf(line, sizeof(line), "Images visibles: %zu / %zu | Tri: %s",
        count, total, lardon3d_image_view_sort_name(
            lardon3d_image_view_sort(state->image_view)));
    draw_text(4, 2, columns - 4, line);
    const char *filter = lardon3d_image_view_filter(state->image_view);
    (void)snprintf(line, sizeof(line), "Filtre: %s",
        filter[0] ? filter : "aucun");
    draw_text(5, 2, columns - 4, line);
    size_t visible = rows > 12 ? (size_t)(rows - 12) : 1;
    size_t offset = lardon3d_image_view_offset(state->image_view);
    size_t selection = lardon3d_image_view_selection(state->image_view);
    for (size_t row = 0; row < visible; ++row) {
        size_t index = offset + row;
        const Lardon3DImageEntry *entry = lardon3d_image_view_get(
            state->image_view, index);
        if (!entry) break;
        char size_text[64];
        format_bytes(entry->size_bytes, size_text);
        (void)snprintf(line, sizeof(line), "%c %-40.40s %s",
            index == selection ? '>' : ' ', entry->filename, size_text);
        draw_text(7 + (int)row, 2, columns - 4, line);
    }
}

static void
draw_import(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *snapshot,
    int rows,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    if (snapshot && snapshot->status == LARDON3D_IMPORT_TASK_RUNNING) {
        char line[256];
        (void)snprintf(line, sizeof(line),
            "Import: %zu/%zu | copiés %zu | présents %zu | ignorés %zu",
            snapshot->processed, snapshot->total, snapshot->copied,
            snapshot->already_present, snapshot->ignored);
        draw_text_style(5, 2, columns - 4, line,
            LARDON3D_TUI_SEMANTIC_CPU, palette);
        unsigned int percent = 0;
        if (snapshot->total > 0) {
            size_t quotient = snapshot->total / 100U;
            size_t remainder = snapshot->total % 100U;
            for (unsigned int candidate = 100; candidate > 0; --candidate) {
                size_t threshold = quotient * candidate
                    + (remainder * candidate + 99U) / 100U;
                if (snapshot->processed >= threshold) {
                    percent = candidate;
                    break;
                }
            }
        }
        if (percent > 100) percent = 100;
        draw_progress_bar(7, 2, columns - 4, percent, palette);
        draw_text(9, 2, columns - 4, "X : annuler l'import");
    } else if (input_text) {
        draw_input_field(input_text, input_label, 5, columns);
    } else {
        draw_catalog(state, rows, columns);
    }
}

static void
draw_tasks(
    const Lardon3DRuntimeSnapshot *runtime,
    size_t selected,
    int rows,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    char line[512];
    (void)snprintf(line, sizeof(line),
        "Total %zu | running %zu | pending %zu | terminal cumulées %zu",
        runtime->task_summary.total, runtime->task_summary.running,
        runtime->task_summary.pending, runtime->task_summary.completed);
    draw_text(4, 2, columns - 4, line);
    if (runtime->task_count == 0) {
        draw_text_style(6, 4, columns - 6, "Aucune tâche retenue.",
            LARDON3D_TUI_SEMANTIC_DIM, palette);
        return;
    }
    if (selected >= runtime->task_count) selected = runtime->task_count - 1;
    const Lardon3DTaskObservation *chosen = &runtime->tasks[selected];
    (void)snprintf(line, sizeof(line), "Sélection #%llu %s | %s | %s",
        (unsigned long long)chosen->id, chosen->name,
        chosen->has_task_kind ? chosen->task_kind : "untyped",
        lardon3d_task_state_name(chosen->state));
    draw_text_style(6, 2, columns - 4, line,
        chosen->state == TASK_FAILED
                || (chosen->state == TASK_COMPLETED
                    && chosen->durable_progress_known
                    && chosen->durable_completed < chosen->durable_total)
            ? LARDON3D_TUI_SEMANTIC_ERROR
        : (chosen->state == TASK_COMPLETED
            ? LARDON3D_TUI_SEMANTIC_HEALTHY
            : LARDON3D_TUI_SEMANTIC_CPU), palette);
    if (chosen->durable_progress_known) {
        (void)snprintf(line, sizeof(line),
            "Progression durable: %llu/%llu%s",
            (unsigned long long)chosen->durable_completed,
            (unsigned long long)chosen->durable_total,
            chosen->state == TASK_COMPLETED
                    && chosen->durable_completed < chosen->durable_total
                ? " — INTEGRITY ERROR" : "");
        draw_text_style(7, 2, columns - 4, line,
            chosen->state == TASK_COMPLETED
                    && chosen->durable_completed < chosen->durable_total
                ? LARDON3D_TUI_SEMANTIC_ERROR
                : LARDON3D_TUI_SEMANTIC_NORMAL, palette);
    } else if (chosen->has_task_kind) {
        draw_text_style(7, 2, columns - 4,
            "Progression scientifique: indéterminée",
            LARDON3D_TUI_SEMANTIC_WARNING, palette);
    } else {
        (void)snprintf(line, sizeof(line), "Progression runtime: %u%%",
            chosen->progress);
        draw_text(7, 2, columns - 4, line);
    }
    int list_start = 9;
    int list_end = rows - 5;
    for (size_t index = 0; index < runtime->task_count
        && list_start + (int)index <= list_end; ++index) {
        const Lardon3DTaskObservation *task = &runtime->tasks[index];
        if (task->durable_progress_known) {
            (void)snprintf(line, sizeof(line),
                "%c #%llu %-18.18s %-10.10s %llu/%llu durable %.42s",
                index == selected ? '>' : ' ',
                (unsigned long long)task->id, task->name,
                lardon3d_task_state_name(task->state),
                (unsigned long long)task->durable_completed,
                (unsigned long long)task->durable_total, task->message);
        } else {
            (void)snprintf(line, sizeof(line),
                "%c #%llu %-18.18s %-10.10s %s %.52s",
                index == selected ? '>' : ' ',
                (unsigned long long)task->id, task->name,
                lardon3d_task_state_name(task->state),
                task->has_task_kind ? "science ?" : "runtime",
                task->message);
        }
        draw_text(list_start + (int)index, 2, columns - 4, line);
    }
}

static void
external_storage_status_text(
    Lardon3DResourceExternalStorageStatus status,
    char output[16]
)
{
    const char *name = "UNKNOWN";
    switch (status) {
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_ABSENT:
        name = "ABSENT";
        break;
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_DETECTED:
        name = "DETECTED";
        break;
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE:
        name = "AVAILABLE";
        break;
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE:
        name = "IN_USE";
        break;
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_DRAINING:
        name = "DRAINING";
        break;
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_SAFE:
        name = "SAFE";
        break;
    case LARDON3D_RESOURCE_EXTERNAL_STORAGE_ERROR:
        name = "ERROR";
        break;
    }
    (void)snprintf(output, 16, "%s", name);
}

static void
format_external_storage_summary(
    const Lardon3DTuiResourceView *resource,
    char *line,
    size_t capacity
)
{
    if (!resource->external_storage_registered) {
        (void)snprintf(line, capacity,
            "Governor SSD: UNREGISTERED | scratch/swap UNKNOWN");
        return;
    }
    char scratch_total[64] = "UNKNOWN";
    char scratch_free[64] = "UNKNOWN";
    if (resource->scratch_total_known) {
        format_bytes(resource->scratch_total_bytes, scratch_total);
    }
    if (resource->scratch_free_known) {
        format_bytes(resource->scratch_free_bytes, scratch_free);
    }
    char status[16];
    external_storage_status_text(resource->external_storage_status, status);
    (void)snprintf(line, capacity,
        "Governor SSD %s | alloc %s | scratch total/free %s/%s | leases %zu",
        status,
        resource->scratch_new_allocations_allowed ? "oui" : "non",
        scratch_total, scratch_free, resource->scratch_leases);
}

static void
draw_resources(
    const Lardon3DTuiResourceView *resource,
    Lardon3DTuiViewport viewport,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    if (!resource->valid) {
        draw_text_style(5, 4, columns - 6,
            "Ressources système indisponibles (UNKNOWN).",
            LARDON3D_TUI_SEMANTIC_WARNING, palette);
        char external[512];
        format_external_storage_summary(
            resource, external, sizeof(external));
        draw_text(7, 2, columns - 4, external);
        return;
    }
    char line[512];
    Lardon3DTuiSemantic health = resource->governor_pressure
            == LARDON3D_RESOURCE_PRESSURE_GREEN
        ? LARDON3D_TUI_SEMANTIC_HEALTHY
        : (resource->governor_pressure == LARDON3D_RESOURCE_PRESSURE_YELLOW
            ? LARDON3D_TUI_SEMANTIC_WARNING
            : LARDON3D_TUI_SEMANTIC_ERROR);
    (void)snprintf(line, sizeof(line), "Governor %s — %s",
        lardon3d_tui_pressure_name(resource->governor_pressure),
        resource->governor_reason);
    draw_text_style(4, 2, columns - 4, line, health, palette);
    char admitted[32] = "UNKNOWN";
    if (resource->cpu_admitted_known) {
        (void)snprintf(admitted, sizeof(admitted), "%u",
            resource->cpu_admitted);
    }
    (void)snprintf(line, sizeof(line),
        "CPU active/admis/disponible: %u/%s/%u (hôte %u) | utilisation %s",
        resource->cpu_active, admitted, resource->cpu_available,
        resource->cpu_logical_total,
        resource->cpu_utilization_known ? "connue" : "UNKNOWN");
    draw_text_style(6, 2, columns - 4, line,
        LARDON3D_TUI_SEMANTIC_CPU, palette);
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        if (resource->cpu_utilization_known) {
            (void)snprintf(line, sizeof(line),
                "CPU raison: %s | utilisation %u.%02u%%",
                resource->cpu_reason,
                resource->cpu_utilization_basis_points / 100U,
                resource->cpu_utilization_basis_points % 100U);
        } else {
            (void)snprintf(line, sizeof(line),
                "CPU raison: %s | utilisation UNKNOWN",
                resource->cpu_reason);
        }
        draw_text(7, 4, columns - 6, line);
    }
    char gpu_busy[64];
    if (resource->gpu_busy_known) {
        (void)snprintf(gpu_busy, sizeof(gpu_busy), "%u.%02u%%",
            resource->gpu_busy_basis_points / 100U,
            resource->gpu_busy_basis_points % 100U);
    } else {
        (void)snprintf(gpu_busy, sizeof(gpu_busy), "UNKNOWN");
    }
    (void)snprintf(line, sizeof(line),
        "GPU %s | slots active/dispo %u/%u | busy %s | backend %s",
        resource->gpu_present ? "présent" : "absent",
        resource->gpu_slots_active, resource->gpu_slots_available,
        gpu_busy,
        lardon3d_tui_gpu_backend_name(resource->gpu_backend));
    draw_text_style(viewport == LARDON3D_TUI_VIEWPORT_FULL ? 9 : 7,
        2, columns - 4, line, LARDON3D_TUI_SEMANTIC_GPU, palette);
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        (void)snprintf(line, sizeof(line), "GPU raison: %s",
            resource->gpu_backend_reason);
        draw_text(10, 4, columns - 6, line);
    }
    char total[64], available[64], reserve[64], reserved[64];
    format_bytes(resource->ram_total_bytes, total);
    format_bytes(resource->ram_available_bytes, available);
    format_bytes(resource->ram_reserve_bytes, reserve);
    format_bytes(resource->ram_reserved_bytes, reserved);
    (void)snprintf(line, sizeof(line),
        "RAM total %s | MemAvailable %s | réserve %s | réservée Task %s",
        total, available, reserve, reserved);
    draw_text(viewport == LARDON3D_TUI_VIEWPORT_FULL ? 11 : 8,
        2, columns - 4, line);
    if (resource->swap_total_known && resource->swap_used_known) {
        char swap_total[64], swap_used[64];
        format_bytes(resource->swap_total_bytes, swap_total);
        format_bytes(resource->swap_used_bytes, swap_used);
        if (resource->swap_delta_known) {
            (void)snprintf(line, sizeof(line),
                "Swap total %s | utilisé %s | delta in/out %llu/%llu pages",
                swap_total, swap_used,
                (unsigned long long)resource->swap_pages_in_delta,
                (unsigned long long)resource->swap_pages_out_delta);
        } else {
            (void)snprintf(line, sizeof(line),
                "Swap total %s | utilisé %s | delta in/out UNKNOWN",
                swap_total, swap_used);
        }
    } else {
        (void)snprintf(line, sizeof(line), "Swap: UNKNOWN");
    }
    draw_text(viewport == LARDON3D_TUI_VIEWPORT_FULL ? 12 : 9,
        2, columns - 4, line);
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        char batch[32] = "UNKNOWN";
        char inflight[32] = "UNKNOWN";
        char helpers[32] = "UNKNOWN";
        if (resource->batch_known) {
            (void)snprintf(batch, sizeof(batch), "%zu", resource->batch_size);
        }
        if (resource->inflight_known) {
            (void)snprintf(inflight, sizeof(inflight), "%zu",
                resource->inflight_limit);
        }
        if (resource->helpers_known) {
            (void)snprintf(helpers, sizeof(helpers), "%u",
                resource->helper_limit);
        }
        (void)snprintf(line, sizeof(line),
            "Contrat: batch %s | inflight %s | helpers %s | I/O active/dispo %u/%u",
            batch, inflight, helpers, resource->io_active,
            resource->io_available);
        draw_text(14, 2, columns - 4, line);
        char gpu_reserved[64] = "UNKNOWN";
        char gpu_available[64] = "UNKNOWN";
        if (resource->gpu_memory_known) {
            format_bytes(resource->gpu_memory_reserved_bytes, gpu_reserved);
            format_bytes(resource->gpu_memory_available_bytes, gpu_available);
        }
        const char *gpu_memory_kind = !resource->gpu_present
            ? "no GPU"
            : (resource->gpu_uses_shared_memory
                ? "UMA once in RAM" : "dedicated");
        (void)snprintf(line, sizeof(line),
            "GPU memory (%s): reserved/available %s/%s",
            gpu_memory_kind, gpu_reserved, gpu_available);
        draw_text(15, 2, columns - 4, line);
    }

    format_external_storage_summary(resource, line, sizeof(line));
    draw_text(viewport == LARDON3D_TUI_VIEWPORT_FULL ? 16 : 10,
        2, columns - 4, line);
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        char external_swap_total[64] = "UNKNOWN";
        char external_swap_used[64] = "UNKNOWN";
        if (resource->external_swap_total_known) {
            format_bytes(resource->external_swap_total_bytes,
                external_swap_total);
        }
        if (resource->external_swap_used_known) {
            format_bytes(resource->external_swap_used_bytes,
                external_swap_used);
        }
        (void)snprintf(line, sizeof(line),
            "Governor SSD swap total/used %s/%s | identité %.120s",
            external_swap_total, external_swap_used,
            resource->external_storage_registered
                ? resource->external_storage_identity : "UNKNOWN");
        draw_text(17, 2, columns - 4, line);
        (void)snprintf(line, sizeof(line), "SSD raison: %.220s",
            resource->external_storage_registered
                ? resource->external_storage_reason : "UNREGISTERED");
        draw_text(18, 2, columns - 4, line);
        draw_text(20, 2, columns - 4,
            "Les choix CPU/GPU/batch sont observés; aucun réglage utilisateur normal.");
    }
}

static Lardon3DTuiSemantic
ssd_semantic(Lardon3DSsdState state)
{
    if (state == LARDON3D_SSD_ERROR) return LARDON3D_TUI_SEMANTIC_ERROR;
    if (state == LARDON3D_SSD_DRAINING || state == LARDON3D_SSD_ENABLING)
        return LARDON3D_TUI_SEMANTIC_WARNING;
    if (state == LARDON3D_SSD_ENABLED || state == LARDON3D_SSD_IN_USE
        || state == LARDON3D_SSD_SAFE_TO_UNPLUG)
        return LARDON3D_TUI_SEMANTIC_HEALTHY;
    return LARDON3D_TUI_SEMANTIC_SSD;
}

static void
draw_ssd(
    const Lardon3DRuntimeSnapshot *runtime,
    const Lardon3DTuiSsdAsyncSnapshot *operation,
    Lardon3DTuiViewport viewport,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    const Lardon3DSsdSnapshot *ssd = &runtime->ssd;
    char line[640];
    if (!runtime->ssd_controller_available) {
        draw_text_style(4, 2, columns - 4, "Etat: UNKNOWN",
            LARDON3D_TUI_SEMANTIC_WARNING, palette);
        if (operation && operation->running) {
            (void)snprintf(line, sizeof(line),
                "Opération asynchrone: %s (ncurses reste réactif)",
                lardon3d_tui_ssd_action_name(operation->action));
            draw_text_style(5, 2, columns - 4, line,
                LARDON3D_TUI_SEMANTIC_WARNING, palette);
        }
        draw_text(7, 2, columns - 4,
            "Contrôleur/télémétrie SSD indisponible; identité, swap, scratch et usage UNKNOWN.");
        return;
    }
    bool telemetry_actionable = !operation
        || !operation->controller_snapshot_known
        || operation->controller_snapshot_actionable;
    Lardon3DSsdState displayed_state = ssd->state;
    if (operation && operation->running
        && operation->action == LARDON3D_TUI_SSD_ACTION_ENABLE) {
        displayed_state = LARDON3D_SSD_ENABLING;
    } else if (operation && operation->running
        && operation->action == LARDON3D_TUI_SSD_ACTION_DRAIN) {
        displayed_state = LARDON3D_SSD_DRAINING;
    }
    /* The async owner is exact operation state, not inferred device state. It
     * is the only way ENABLING can remain visible while the synchronous
     * controller holds its mutex through bounded side-effect verification. */
    (void)snprintf(line, sizeof(line), "Etat: %s%s",
        lardon3d_ssd_state_name(displayed_state),
        displayed_state == LARDON3D_SSD_SAFE_TO_UNPLUG
            ? " — SAFE TO UNPLUG" : "");
    draw_text_style(4, 2, columns - 4, line,
        ssd_semantic(displayed_state), palette);
    if (operation && operation->running) {
        (void)snprintf(line, sizeof(line),
            "Opération asynchrone: %s (ncurses reste réactif)",
            lardon3d_tui_ssd_action_name(operation->action));
        draw_text_style(5, 2, columns - 4, line,
            LARDON3D_TUI_SEMANTIC_WARNING, palette);
    } else if (operation && operation->result_known) {
        draw_text(5, 2, columns - 4, operation->reason);
    }
    if (!telemetry_actionable) {
        /* A synthetic ERROR is deliberately visible, but none of its cleared
         * booleans are observations. Rendering them as inactive/unmounted
         * would turn validation failure into guessed physical state. */
        draw_text_style(7, 2, columns - 4,
            "UNKNOWN — télémétrie invalide: identité, lien, swap, scratch, mount et usage.",
            LARDON3D_TUI_SEMANTIC_ERROR, palette);
        draw_text(9, 2, columns - 4,
            "Contrôle F10 désactivé jusqu'à une observation bornée valide.");
        return;
    }
    char link_speed[64];
    if (ssd->connection_speed_known) {
        (void)snprintf(link_speed, sizeof(link_speed), "%llu Mb/s",
            (unsigned long long)ssd->connection_speed_mbps);
    } else {
        (void)snprintf(link_speed, sizeof(link_speed), "UNKNOWN");
    }
    (void)snprintf(line, sizeof(line),
        "Modèle: %s | série: %s | lien: %s",
        ssd->model_known ? ssd->model : "UNKNOWN",
        ssd->serial_known ? ssd->serial : "UNKNOWN",
        link_speed);
    draw_text_style(7, 2, columns - 4, line,
        LARDON3D_TUI_SEMANTIC_SSD, palette);
    (void)snprintf(line, sizeof(line), "Identité stable Drive: %s",
        ssd->drive_identity[0] ? ssd->drive_identity : "UNKNOWN");
    draw_text(8, 2, columns - 4, line);
    (void)snprintf(line, sizeof(line),
        "Paire exacte: %s | swap UUID %s | scratch UUID %s",
        ssd->pairing_valid ? "VALID" : "INVALID/UNKNOWN",
        ssd->swap_uuid[0] ? ssd->swap_uuid : "UNKNOWN",
        ssd->scratch_uuid[0] ? ssd->scratch_uuid : "UNKNOWN");
    draw_text(9, 2, columns - 4, line);
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        char swap_total[64] = "UNKNOWN";
        char swap_used[64] = "UNKNOWN";
        if (ssd->swap_total_known) {
            format_bytes(ssd->swap_total_bytes, swap_total);
        }
        if (ssd->swap_used_known) {
            format_bytes(ssd->swap_used_bytes, swap_used);
        }
        (void)snprintf(line, sizeof(line),
            "Swap: %s | total %s | utilisé %s",
            ssd->swap_active ? "ACTIVE" : "INACTIVE",
            swap_total, swap_used);
        draw_text(10, 2, columns - 4, line);
        char scratch_total[64] = "UNKNOWN";
        char scratch_free[64] = "UNKNOWN";
        if (ssd->scratch_total_known) {
            format_bytes(ssd->scratch_total_bytes, scratch_total);
        }
        if (ssd->scratch_free_known) {
            format_bytes(ssd->scratch_free_bytes, scratch_free);
        }
        (void)snprintf(line, sizeof(line),
            "Scratch: %s | mount %s | total/free %s/%s | leases %zu/%zu",
            ssd->scratch_mounted ? "MOUNTED" : "UNMOUNTED",
            ssd->scratch_mount_path[0] ? ssd->scratch_mount_path : "UNKNOWN",
            scratch_total, scratch_free,
            ssd->scratch_lease_count, ssd->scratch_lease_capacity);
        draw_text(11, 2, columns - 4, line);
        (void)snprintf(line, sizeof(line),
            "Drain demandé: %s | raison: %s",
            ssd->drain_requested ? "oui" : "non",
            ssd->reason[0] ? ssd->reason : "UNKNOWN");
        draw_text_style(12, 2, columns - 4, line,
            ssd->state == LARDON3D_SSD_ERROR
                ? LARDON3D_TUI_SEMANTIC_ERROR
                : LARDON3D_TUI_SEMANTIC_NORMAL, palette);
        draw_text(14, 2, columns - 4,
            "F10 agit uniquement sur la paire Drive/UUID validée; jamais de format/repair/poweroff.");
        draw_text(15, 2, columns - 4,
            "Le swap/SSD reste une sécurité/scratch physique, jamais de la RAM scientifique.");
    } else {
        (void)snprintf(line, sizeof(line), "Raison: %s",
            ssd->reason[0] ? ssd->reason : "UNKNOWN");
        draw_text_style(6, 2, columns - 4, line,
            ssd->state == LARDON3D_SSD_ERROR
                ? LARDON3D_TUI_SEMANTIC_ERROR
                : LARDON3D_TUI_SEMANTIC_NORMAL, palette);
        (void)snprintf(line, sizeof(line),
            "Swap %s | scratch %s | leases %zu | drain %s",
            ssd->swap_active ? "ACTIVE" : "INACTIVE",
            ssd->scratch_mounted ? "MOUNTED" : "UNMOUNTED",
            ssd->scratch_lease_count,
            ssd->drain_requested ? "oui" : "non");
        draw_text(10, 2, columns - 4, line);
    }
}

static const char *
lens_interface_name(Lardon3DOpticalLensInterface interface_kind)
{
    switch (interface_kind) {
    case LARDON3D_OPTICAL_LENS_MANUAL:
        return "manual/no-EXIF";
    case LARDON3D_OPTICAL_LENS_ELECTRONIC:
        return "electronic";
    case LARDON3D_OPTICAL_LENS_INTEGRATED:
        return "integrated";
    }
    return "UNKNOWN";
}

static const char *
optics_pane_name(Lardon3DTuiOpticsPane pane)
{
    switch (pane) {
    case LARDON3D_TUI_OPTICS_PANE_BODY:
        return "BODY";
    case LARDON3D_TUI_OPTICS_PANE_LENS:
        return "LENS";
    case LARDON3D_TUI_OPTICS_PANE_CONFIGURATION:
        return "CONFIGURATION";
    case LARDON3D_TUI_OPTICS_PANE_CALIBRATION:
        return "CALIBRATION";
    }
    return "UNKNOWN";
}

static void
draw_optics(
    const Lardon3DTuiOpticsSnapshot *optics,
    Lardon3DTuiViewport viewport,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    if (!optics || !optics->project_bound) {
        const char *message = optics && optics->message[0]
            ? optics->message
            : "Aucun Project DB lié; aucun profil ou assignation n'est deviné.";
        draw_text_style(5, 4, columns - 6, message,
            optics && optics->message[0]
                ? LARDON3D_TUI_SEMANTIC_ERROR
                : LARDON3D_TUI_SEMANTIC_WARNING, palette);
        if (optics && optics->message[0]) {
            draw_text(7, 4, columns - 6,
                "R : réessayer explicitement la liaison Project DB.");
        }
        return;
    }
    char line[512];
    size_t body = optics->body_count > 0
        ? (optics->selected_body < optics->body_count
            ? optics->selected_body : 0) : 0;
    size_t lens = optics->lens_count > 0
        ? (optics->selected_lens < optics->lens_count
            ? optics->selected_lens : 0) : 0;
    size_t configuration = optics->configuration_count > 0
        ? (optics->selected_configuration < optics->configuration_count
            ? optics->selected_configuration : 0) : 0;
    (void)snprintf(line, sizeof(line),
        "Body [%zu/%zu affichés%s]: %s %s — %s",
        optics->body_count ? body + 1 : 0, optics->body_count,
        optics->bodies_have_next ? ", suite" : "",
        optics->body_count ? optics->bodies[body].manufacturer : "UNKNOWN",
        optics->body_count ? optics->bodies[body].model : "",
        optics->body_count ? optics->bodies[body].name : "aucun");
    draw_text(4, 2, columns - 4, line);
    (void)snprintf(line, sizeof(line),
        "Lens [%zu/%zu affichés%s]: %s %s — %s (%s)",
        optics->lens_count ? lens + 1 : 0, optics->lens_count,
        optics->lenses_have_next ? ", suite" : "",
        optics->lens_count ? optics->lenses[lens].manufacturer : "UNKNOWN",
        optics->lens_count ? optics->lenses[lens].model : "",
        optics->lens_count ? optics->lenses[lens].name : "aucun",
        optics->lens_count
            ? lens_interface_name(optics->lenses[lens].interface_kind)
            : "UNKNOWN");
    draw_text_style(5, 2, columns - 4, line,
        optics->lens_count && optics->lenses[lens].interface_kind
                == LARDON3D_OPTICAL_LENS_MANUAL
            ? LARDON3D_TUI_SEMANTIC_HEALTHY
            : LARDON3D_TUI_SEMANTIC_NORMAL, palette);
    if (optics->configuration_count > 0) {
        const Lardon3DOpticalConfiguration *selected =
            &optics->configurations[configuration];
        if (selected->has_focal_length) {
            (void)snprintf(line, sizeof(line),
                "Config [%zu/%zu affichés%s] #%llu body #%llu lens #%llu focal %u µm",
                configuration + 1, optics->configuration_count,
                optics->configurations_have_next ? ", suite" : "",
                (unsigned long long)selected->optical_configuration_id,
                (unsigned long long)selected->camera_body_profile_id,
                (unsigned long long)selected->lens_profile_id,
                selected->focal_length_um);
        } else {
            (void)snprintf(line, sizeof(line),
                "Config [%zu/%zu affichés%s] #%llu body #%llu lens #%llu focal ABSENT",
                configuration + 1, optics->configuration_count,
                optics->configurations_have_next ? ", suite" : "",
                (unsigned long long)selected->optical_configuration_id,
                (unsigned long long)selected->camera_body_profile_id,
                (unsigned long long)selected->lens_profile_id);
        }
    } else {
        (void)snprintf(line, sizeof(line), "Config: aucune");
    }
    draw_text(6, 2, columns - 4, line);
    if (optics->calibration_count > 0) {
        size_t selected = optics->selected_calibration
                < optics->calibration_count
            ? optics->selected_calibration : 0;
        const Lardon3DOpticalCalibrationProfile *calibration =
            &optics->calibrations[selected];
        (void)snprintf(line, sizeof(line),
            "Pane %s | calibration [%zu/%zu affichées%s] #%llu %.28s v%u",
            optics_pane_name(optics->active_pane), selected + 1,
            optics->calibration_count,
            optics->calibrations_have_next ? ", suite" : "",
            (unsigned long long)calibration->calibration_profile_id,
            calibration->name, calibration->profile_version);
    } else {
        (void)snprintf(line, sizeof(line),
            "Pane %s | calibration candidate: aucune",
            optics_pane_name(optics->active_pane));
    }
    draw_text(7, 2, columns - 4, line);
    if (optics->capture_inspected) {
        (void)snprintf(line, sizeof(line), "Capture #%llu: %s — %s",
            (unsigned long long)optics->capture_id,
            lardon3d_tui_optics_status_name(optics->capture_status),
            lardon3d_tui_optics_status_explanation(optics->capture_status,
                optics->capture_lens_found
                    && optics->capture_lens.interface_kind
                        == LARDON3D_OPTICAL_LENS_MANUAL));
        Lardon3DTuiSemantic semantic = optics->capture_status
                == LARDON3D_TUI_OPTICS_SELECTED
            ? LARDON3D_TUI_SEMANTIC_HEALTHY
            : (optics->capture_status == LARDON3D_TUI_OPTICS_CORRUPT
                    || optics->capture_status == LARDON3D_TUI_OPTICS_INCOMPATIBLE
                ? LARDON3D_TUI_SEMANTIC_ERROR
                : LARDON3D_TUI_SEMANTIC_WARNING);
        draw_text_style(8, 2, columns - 4, line, semantic, palette);
    } else {
        draw_text_style(8, 2, columns - 4,
            "Capture: non inspecté (V); unresolved reste absence d'assignation.",
            LARDON3D_TUI_SEMANTIC_DIM, palette);
    }
    (void)snprintf(line, sizeof(line),
        "Calibrations compatibles: %zu | sélection: %s",
        optics->calibration_count,
        optics->capture_selection_found ? "explicite" : "aucune/ambiguë");
    draw_text(9, 2, columns - 4, line);
    if (optics->metadata_lookup_performed) {
        (void)snprintf(line, sizeof(line),
            "Métadonnées exactes: body %s | lens %s",
            optics->metadata_body_found ? "MATCH" : "UNRESOLVED",
            optics->metadata_lens_found ? "MATCH" : "UNRESOLVED");
        draw_text(10, 2, columns - 4, line);
    }
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        draw_text(12, 2, columns - 4,
            "B body: manufacturer|model|name   L lens: interface|range|min|max|maker|model|name");
        draw_text(13, 2, columns - 4,
            "C config focal mm/?   V inspect capture   A assign capture   G task:group");
        draw_text(14, 2, columns - 4,
            "K sélection calibration   E metadata exact   [ première page   ] page suivante   R retry");
        draw_text_style(16, 2, columns - 4,
            "Immutable: modifier = créer une nouvelle version/configuration.",
            LARDON3D_TUI_SEMANTIC_WARNING, palette);
    }
}

static void
draw_help(
    Lardon3DTuiViewport viewport,
    int columns,
    const Lardon3DTuiPalette *palette
)
{
    draw_text(4, 2, columns - 4,
        "F1 aide, F2 projets, F3 import, F4 viewer futur, F5 tâches, F6 ressources,");
    draw_text(5, 2, columns - 4,
        "F7 profils optiques, F10 SSD; ESC accueil; Q quitter.");
    draw_text_style(7, 2, columns - 4,
        "Vert=healthy, jaune=warning/throttled, rouge=error, cyan=GPU, bleu=CPU, magenta=SSD.",
        LARDON3D_TUI_SEMANTIC_HEALTHY, palette);
    draw_text(8, 2, columns - 4,
        "Sans couleur/peu de paires, les libellés et bold/dim conservent le sens.");
    if (viewport == LARDON3D_TUI_VIEWPORT_FULL) {
        draw_text(10, 2, columns - 4,
            "La TUI observe des snapshots bornés >=1s; aucun worker ne touche ncurses.");
        draw_text(11, 2, columns - 4,
            "Le Governor choisit CPU/GPU/batch. La TUI ne modifie ni admission ni science.");
        draw_text(12, 2, columns - 4,
            "Dense (future) reste NOT_APPLICABLE; aucune étape future n'est RUNNING.");
    } else {
        draw_text(10, 2, columns - 4,
            "Governor choisit les ressources; Dense future reste NOT_APPLICABLE.");
    }
}

void
lardon3d_layout_draw_runtime(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    const Lardon3DRuntimeSnapshot *runtime,
    const Lardon3DTuiSsdAsyncSnapshot *ssd_operation,
    const Lardon3DTuiOpticsSnapshot *optics,
    size_t selected_task,
    const Lardon3DTuiPalette *palette,
    Lardon3DTuiInteractionMode interaction_mode,
    int rows,
    int columns
)
{
    (void)erase();
    Lardon3DTuiViewport viewport = lardon3d_tui_viewport_classify(rows, columns);
    if (!state || !runtime || viewport == LARDON3D_TUI_VIEWPORT_TOO_SMALL) {
        draw_too_small(rows, columns);
        (void)refresh();
        return;
    }
    draw_frame(rows, columns);
    const char *title = screen_title(state->screen);
    int title_length = (int)strlen(title);
    draw_text_style(1, (columns - title_length) / 2, title_length, title,
        LARDON3D_TUI_SEMANTIC_NORMAL, palette);
    draw_project_line(state, 3, columns);
    switch (state->screen) {
    case LARDON3D_SCREEN_PROJECTS:
        draw_projects(input_text, input_label, columns);
        break;
    case LARDON3D_SCREEN_IMPORT:
        draw_import(state, input_text, input_label, import_snapshot,
            rows, columns, palette);
        break;
    case LARDON3D_SCREEN_TASKS:
        draw_tasks(runtime, selected_task, rows, columns, palette);
        break;
    case LARDON3D_SCREEN_RESOURCES:
        draw_resources(&runtime->resources, viewport, columns, palette);
        break;
    case LARDON3D_SCREEN_OPTICS:
        draw_optics(optics, viewport, columns, palette);
        if (input_text) {
            draw_input_field(input_text, input_label, rows - 7, columns);
        }
        break;
    case LARDON3D_SCREEN_SSD:
        draw_ssd(runtime, ssd_operation, viewport, columns, palette);
        break;
    case LARDON3D_SCREEN_HELP:
        draw_help(viewport, columns, palette);
        break;
    case LARDON3D_SCREEN_VIEWER:
        draw_text_style(6, 4, columns - 8,
            "Viewer Vulkan: PLANNED, aucun travail scientifique actif.",
            LARDON3D_TUI_SEMANTIC_DIM, palette);
        break;
    case LARDON3D_SCREEN_HOME:
    default:
        draw_home(runtime, viewport, columns, palette);
        break;
    }
    const char *status = import_snapshot
            && import_snapshot->status == LARDON3D_IMPORT_TASK_RUNNING
        ? import_snapshot->message
        : state->status_message;
    draw_text(rows - 3, 2, columns - 4, status);
    draw_text(rows - 2, 2, columns - 4,
        screen_footer(state->screen, interaction_mode));
    (void)refresh();
}

void
lardon3d_layout_draw(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    const Lardon3DTaskSnapshot *task_snapshots,
    size_t task_count,
    const Lardon3DTaskQueueSummary *task_summary,
    const Lardon3DResourceAvailability *resource_availability,
    int rows,
    int columns
)
{
    Lardon3DRuntimeSnapshot runtime = {0};
    runtime.task_count = task_snapshots
        ? (task_count < LARDON3D_TUI_TASK_CAPACITY
            ? task_count : LARDON3D_TUI_TASK_CAPACITY)
        : 0;
    if (task_summary) {
        runtime.task_summary = *task_summary;
    }
    for (size_t index = 0; index < runtime.task_count; ++index) {
        const Lardon3DTaskSnapshot *source = &task_snapshots[index];
        Lardon3DTaskObservation *destination = &runtime.tasks[index];
        destination->id = source->id;
        destination->progress = source->progress;
        destination->state = source->state;
        destination->started_at = source->started_at;
        destination->finished_at = source->finished_at;
        (void)snprintf(destination->name, sizeof(destination->name), "%s",
            source->name);
        (void)snprintf(destination->message,
            sizeof(destination->message), "%s", source->message);
    }
    if (resource_availability) {
        runtime.resources.valid = true;
        runtime.resources.cpu_active = resource_availability->cpu_reserved;
        runtime.resources.cpu_available = resource_availability->cpu_available;
        runtime.resources.ram_reserved_bytes =
            resource_availability->memory_reserved_bytes;
        runtime.resources.gpu_memory_known =
            resource_availability->gpu_memory_known;
        runtime.resources.gpu_memory_reserved_bytes =
            resource_availability->gpu_memory_reserved_bytes;
        runtime.resources.gpu_memory_available_bytes =
            resource_availability->gpu_memory_available_bytes;
        runtime.resources.gpu_slots_active =
            resource_availability->gpu_slots_reserved;
        runtime.resources.gpu_slots_available =
            resource_availability->gpu_slots_available;
        runtime.resources.io_active =
            resource_availability->io_slots_reserved;
        runtime.resources.io_available =
            resource_availability->io_slots_available;
    }
    Lardon3DTuiPalette palette;
    lardon3d_tui_palette_plan(false, 0, &palette);
    lardon3d_layout_draw_runtime(state, input_text, input_label,
        import_snapshot, &runtime, NULL, NULL, 0, &palette,
        input_text ? LARDON3D_TUI_INTERACTION_TEXT_INPUT
                   : LARDON3D_TUI_INTERACTION_IDLE,
        rows, columns);
}
