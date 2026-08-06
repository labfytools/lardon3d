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
        *footer = "N Nouveau projet   C Fermer le projet   ESC Accueil   Q Quit";
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
draw_project_screen(const char *project_name_input, int columns)
{
    draw_text(6, 4, columns - 6, "N : Nouveau projet");
    draw_text(7, 4, columns - 6, "C : Fermer le projet");
    draw_text(8, 4, columns - 6, "ESC : Accueil");
    draw_text(9, 4, columns - 6, "Q : Quitter");

    if (!project_name_input) {
        return;
    }

    draw_text(10, 4, columns - 6, "Nom du nouveau projet :");
    (void)mvaddch(11, 2, '[');
    (void)mvaddch(11, columns - 3, ']');

    int available = columns - 8;
    size_t length = strlen(project_name_input);
    const char *visible = project_name_input;
    if (length > (size_t)available) {
        visible += length - (size_t)available;
        length = (size_t)available;
    }
    draw_text(11, 4, available, visible);
    (void)move(11, 4 + (int)length);
}

static void
draw_content(
    const Lardon3DAppState *state,
    const char *project_name_input,
    int rows,
    int columns
)
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
    if (state->screen == LARDON3D_SCREEN_PROJECTS) {
        draw_project_screen(project_name_input, columns);
    } else {
        draw_text(
            (3 + journal_row) / 2,
            (columns - content_length) / 2,
            content_length,
            content
        );
    }
    draw_text(journal_row + 1, 2, columns - 4, "Journal");
    draw_text(journal_row + 2, 4, columns - 6, state->status_message);
    draw_text(rows - 2, 2, columns - 4, footer);
}

void
lardon3d_layout_draw(
    const Lardon3DAppState *state,
    const char *project_name_input,
    int rows,
    int columns
)
{
    (void)erase();

    if (rows < MINIMUM_ROWS || columns < MINIMUM_COLUMNS) {
        draw_too_small(rows, columns);
    } else {
        draw_frame(rows, columns);
        draw_content(state, project_name_input, rows, columns);
    }

    (void)refresh();
}
