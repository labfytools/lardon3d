#include <ncurses.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/layout.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>

enum {
    MINIMUM_ROWS = 20,
    MINIMUM_COLUMNS = 72,
};

static void
draw_text(int row, int column, int available, const char *text)
{
    if (available <= 0) {
        return;
    }

    size_t length = strlen(text);
    int displayed = length < (size_t)available ? (int)length : available;
    (void)mvaddnstr(row, column, text, displayed);
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
    int journal_row = rows - 7;
    int footer_row = rows - 3;

    (void)box(stdscr, 0, 0);
    (void)mvhline(2, 1, ACS_HLINE, columns - 2);
    (void)mvhline(journal_row, 1, ACS_HLINE, columns - 2);
    (void)mvhline(footer_row, 1, ACS_HLINE, columns - 2);
    (void)mvaddch(2, 0, ACS_LTEE);
    (void)mvaddch(2, columns - 1, ACS_RTEE);
    (void)mvaddch(journal_row, 0, ACS_LTEE);
    (void)mvaddch(journal_row, columns - 1, ACS_RTEE);
    (void)mvaddch(footer_row, 0, ACS_LTEE);
    (void)mvaddch(footer_row, columns - 1, ACS_RTEE);
}

static void
screen_texts(
    Lardon3DScreen screen,
    const char **title,
    const char **content,
    const char **footer
)
{
    *footer = "ESC Accueil   F1 Aide   F2 Projets   F3 Import   F4 Viewer   Q Quit";

    switch (screen) {
    case LARDON3D_SCREEN_PROJECTS:
        *title = "Projets";
        *content = "Gestion des projets";
        *footer = "N Nouveau   O Ouvrir   C Fermer   ESC Accueil   Q Quit";
        break;
    case LARDON3D_SCREEN_IMPORT:
        *title = "Import";
        *content = "Import des images";
        *footer = "I Importer  R Recharger  S Tri  / Filtre  X Effacer  ↑/↓  ESC  Q";
        break;
    case LARDON3D_SCREEN_VIEWER:
        *title = "Viewer";
        *content = "Le viewer Vulkan n'est pas encore disponible.";
        break;
    case LARDON3D_SCREEN_HELP:
        *title = "Aide";
        *content = "Raccourcis clavier.";
        break;
    case LARDON3D_SCREEN_HOME:
    default:
        *title = "Accueil";
        *content = "Bienvenue dans Lardon3D";
        *footer = "F1 Aide   F2 Projets   F3 Import   F4 Viewer   Q Quit";
        break;
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
    if (!input_text) {
        return;
    }

    draw_text(row, 4, columns - 6, input_label);
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
draw_project_screen(
    const char *input_text,
    const char *input_label,
    int columns
)
{
    draw_text(6, 4, columns - 6, "N : Nouveau projet");
    draw_text(7, 4, columns - 6, "O : Ouvrir un projet");
    draw_text(8, 4, columns - 6, "C : Fermer le projet");
    draw_text(9, 4, columns - 6, "ESC : Accueil");
    draw_text(10, 4, columns - 6, "Q : Quitter");
    draw_input_field(input_text, input_label, 11, columns);
}

static void
draw_catalog(
    const Lardon3DAppState *state,
    int rows,
    int columns
)
{
    if (!state->project_loaded) {
        draw_text(7, 4, columns - 6, "Aucun projet chargé.");
        return;
    }
    size_t count = lardon3d_image_view_count(state->image_view);
    size_t total = lardon3d_image_catalog_count(state->image_catalog);
    char line[512];
    (void)snprintf(
        line,
        sizeof(line),
        "Images visibles : %zu / %zu",
        count,
        total
    );
    draw_text(6, 4, columns - 6, line);
    char size_text[64];
    lardon3d_image_catalog_format_size(
        lardon3d_image_view_total_size(state->image_view),
        size_text,
        sizeof(size_text)
    );
    (void)snprintf(
        line,
        sizeof(line),
        "Taille totale visible : %s",
        size_text
    );
    draw_text(7, 4, columns - 6, line);
    (void)snprintf(
        line,
        sizeof(line),
        "Tri : %s",
        lardon3d_image_view_sort_name(
            lardon3d_image_view_sort(state->image_view)
        )
    );
    draw_text(8, 4, columns - 6, line);
    const char *filter = lardon3d_image_view_filter(state->image_view);
    (void)snprintf(
        line,
        sizeof(line),
        "Filtre : %s",
        filter[0] ? filter : "Aucun"
    );
    draw_text(9, 4, columns - 6, line);
    if (count == 0) {
        draw_text(
            11,
            4,
            columns - 6,
            filter[0]
                ? "Aucune image ne correspond au filtre."
                : "Aucune image importée."
        );
        return;
    }

    int journal_row = rows - 7;
    size_t visible = journal_row > 11 ? (size_t)(journal_row - 11) : 0;
    size_t offset = lardon3d_image_view_offset(state->image_view);
    size_t selection = lardon3d_image_view_selection(state->image_view);
    for (size_t row = 0; row < visible; ++row) {
        size_t index = offset + row;
        const Lardon3DImageEntry *entry = lardon3d_image_view_get(
            state->image_view,
            index
        );
        if (!entry) {
            break;
        }
        lardon3d_image_catalog_format_size(
            entry->size_bytes,
            size_text,
            sizeof(size_text)
        );
        (void)snprintf(
            line,
            sizeof(line),
            "%c %s    %s",
            index == selection ? '>' : ' ',
            entry->filename,
            size_text
        );
        draw_text(11 + (int)row, 4, columns - 6, line);
    }
}

static void
draw_import_screen(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *snapshot,
    int rows,
    int columns
)
{
    if (snapshot && snapshot->status == LARDON3D_IMPORT_TASK_RUNNING) {
        draw_text(6, 4, columns - 6, "Import en cours");
        char line[128];
        (void)snprintf(
            line,
            sizeof(line),
            "Fichiers : %zu / %zu",
            snapshot->processed,
            snapshot->total
        );
        draw_text(7, 4, columns - 6, line);
        (void)snprintf(line, sizeof(line), "Copiés : %zu", snapshot->copied);
        draw_text(8, 4, columns - 6, line);
        (void)snprintf(
            line,
            sizeof(line),
            "Déjà présents : %zu",
            snapshot->already_present
        );
        draw_text(9, 4, columns - 6, line);
        (void)snprintf(line, sizeof(line), "Ignorés : %zu", snapshot->ignored);
        draw_text(10, 4, columns - 6, line);

        int percent = snapshot->total == 0
            ? 0
            : (int)((snapshot->processed * 100) / snapshot->total);
        if (percent > 100) {
            percent = 100;
        }
        int bar_width = columns - 18;
        if (bar_width > 40) {
            bar_width = 40;
        }
        if (bar_width < 1) {
            bar_width = 1;
        }
        int filled = (bar_width * percent) / 100;
        char bar[64];
        bar[0] = '[';
        for (int index = 0; index < bar_width; ++index) {
            bar[index + 1] = index < filled ? '#' : '-';
        }
        (void)snprintf(
            bar + bar_width + 1,
            sizeof(bar) - (size_t)bar_width - 1,
            "] %d %%",
            percent
        );
        draw_text(11, 4, columns - 6, bar);
        draw_text(12, 4, columns - 6, "C : Annuler l'import");
        return;
    }
    if (input_text) {
        draw_input_field(input_text, input_label, 7, columns);
        return;
    }
    draw_catalog(state, rows, columns);
}

static void
draw_content(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    int rows,
    int columns
)
{
    const char *title;
    const char *content;
    const char *footer;
    screen_texts(state->screen, &title, &content, &footer);
    bool import_running = import_snapshot
        && import_snapshot->status == LARDON3D_IMPORT_TASK_RUNNING;
    if (import_running) {
        footer = "C Annuler l'import   Q désactivé";
    }
    int title_length = (int)strlen(title);
    int content_length = (int)strlen(content);
    int journal_row = rows - 7;
    const char *project = state->project_loaded
        ? state->project_name
        : "Aucun projet chargé.";

    draw_text(1, (columns - title_length) / 2, title_length, title);
    draw_text(3, 2, columns - 4, "Projet");
    draw_text(4, 4, columns - 6, project);
    if (state->project_loaded) {
        draw_text(5, 4, columns - 6, state->project_path);
    }
    if (state->screen == LARDON3D_SCREEN_PROJECTS) {
        draw_project_screen(
            input_text,
            input_label,
            columns
        );
    } else if (state->screen == LARDON3D_SCREEN_IMPORT) {
        draw_import_screen(
            state,
            input_text,
            input_label,
            import_snapshot,
            rows,
            columns
        );
    } else {
        draw_text(
            (3 + journal_row) / 2,
            (columns - content_length) / 2,
            content_length,
            content
        );
    }
    draw_text(journal_row + 1, 2, columns - 4, "Journal");
    draw_text(
        journal_row + 2,
        4,
        columns - 6,
        import_running ? import_snapshot->message : state->status_message
    );
    draw_text(rows - 2, 2, columns - 4, footer);
}

void
lardon3d_layout_draw(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    int rows,
    int columns
)
{
    (void)erase();

    if (rows < MINIMUM_ROWS || columns < MINIMUM_COLUMNS) {
        draw_too_small(rows, columns);
    } else {
        draw_frame(rows, columns);
        draw_content(
            state,
            input_text,
            input_label,
            import_snapshot,
            rows,
            columns
        );
    }

    (void)refresh();
}
