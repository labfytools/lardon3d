#include <ncurses.h>
#include <stdbool.h>
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
    int middle_column = columns / 2;
    int journal_row = rows / 2;
    int footer_row = rows - 3;

    (void)box(stdscr, 0, 0);
    (void)mvhline(2, 1, ACS_HLINE, columns - 2);
    (void)mvhline(journal_row, 1, ACS_HLINE, columns - 2);
    (void)mvhline(footer_row, 1, ACS_HLINE, columns - 2);
    (void)mvvline(3, middle_column, ACS_VLINE, journal_row - 3);

    (void)mvaddch(2, 0, ACS_LTEE);
    (void)mvaddch(2, middle_column, ACS_TTEE);
    (void)mvaddch(2, columns - 1, ACS_RTEE);
    (void)mvaddch(journal_row, 0, ACS_LTEE);
    (void)mvaddch(journal_row, middle_column, ACS_BTEE);
    (void)mvaddch(journal_row, columns - 1, ACS_RTEE);
    (void)mvaddch(footer_row, 0, ACS_LTEE);
    (void)mvaddch(footer_row, columns - 1, ACS_RTEE);
}

static void
draw_content(int rows, int columns)
{
    int middle_column = columns / 2;
    int journal_row = rows / 2;

    draw_text(1, 2, middle_column - 3, "Lardon3D");
    draw_text(1, columns - 7, 5, "Heure");

    draw_text(3, 2, middle_column - 3, "Projet");
    draw_text(5, 4, middle_column - 5, "Aucun projet chargé.");

    draw_text(3, middle_column + 2, columns - middle_column - 3, "Informations");
    draw_text(5, middle_column + 4, columns - middle_column - 5, "Lardon3D");
    draw_text(6, middle_column + 4, columns - middle_column - 5, "Reconstruction 3D");
    draw_text(7, middle_column + 4, columns - middle_column - 5, "Version 0.1");

    draw_text(journal_row + 1, 2, columns - 4, "Journal");
    draw_text(journal_row + 3, 4, columns - 6, "Bienvenue dans Lardon3D");

    draw_text(
        rows - 2,
        2,
        columns - 4,
        "F1 Aide   F2 Projet   F3 Import   F4 Viewer   Q Quit"
    );
}

bool
lardon3d_layout_draw(void)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);

    if (erase() == ERR) {
        return false;
    }

    if (rows < MINIMUM_ROWS || columns < MINIMUM_COLUMNS) {
        draw_too_small(rows, columns);
    } else {
        draw_frame(rows, columns);
        draw_content(rows, columns);
    }

    return refresh() != ERR;
}
