#include <ncurses.h>
#include <string.h>

#include <lardon3d/layout.h>

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
        break;
    case LARDON3D_SCREEN_IMPORT:
        *title = "Import";
        *content = "Import des images";
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
draw_content(const Lardon3DAppState *state, int rows, int columns)
{
    const char *title;
    const char *content;
    const char *footer;
    screen_texts(state->screen, &title, &content, &footer);
    int title_length = (int)strlen(title);
    int content_length = (int)strlen(content);
    int journal_row = rows - 7;
    const char *project = state->project_loaded
        ? state->project_name
        : "Aucun projet chargé.";

    draw_text(1, (columns - title_length) / 2, title_length, title);
    draw_text(3, 2, columns - 4, "Projet");
    draw_text(4, 4, columns - 6, project);
    draw_text(
        (3 + journal_row) / 2,
        (columns - content_length) / 2,
        content_length,
        content
    );
    draw_text(journal_row + 1, 2, columns - 4, "Journal");
    draw_text(journal_row + 2, 4, columns - 6, state->status_message);
    draw_text(rows - 2, 2, columns - 4, footer);
}

void
lardon3d_layout_draw(
    const Lardon3DAppState *state,
    int rows,
    int columns
)
{
    (void)erase();

    if (rows < MINIMUM_ROWS || columns < MINIMUM_COLUMNS) {
        draw_too_small(rows, columns);
    } else {
        draw_frame(rows, columns);
        draw_content(state, rows, columns);
    }

    (void)refresh();
}
