#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <lardon3d/project.h>

enum {
    MAX_CREATED_DIRECTORIES = 16,
    INI_LINE_CAPACITY = 512,
};

typedef struct {
    char paths[MAX_CREATED_DIRECTORIES][PATH_MAX];
    size_t count;
} CreatedDirectories;

static void
set_status(Lardon3DAppState *state, const char *message)
{
    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "%s",
        message
    );
}

static bool
normalize_name(
    Lardon3DAppState *state,
    const char *input,
    char *output,
    size_t output_size
)
{
    if (!input) {
        set_status(state, "Erreur : le nom du projet est vide.");
        return false;
    }

    const char *start = input;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }

    const char *end = input + strlen(input);
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }

    size_t length = (size_t)(end - start);
    if (length == 0) {
        set_status(state, "Erreur : le nom du projet est vide.");
        return false;
    }
    if (length >= output_size) {
        set_status(state, "Erreur : le nom du projet est trop long.");
        return false;
    }
    if ((length == 1 && start[0] == '.')
        || (length == 2 && start[0] == '.' && start[1] == '.')) {
        set_status(state, "Erreur : nom de projet interdit.");
        return false;
    }

    for (const char *character = start; character < end; ++character) {
        if (*character == '/' || *character == '\\'
            || iscntrl((unsigned char)*character)) {
            set_status(state, "Erreur : nom de projet interdit.");
            return false;
        }
    }

    (void)memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool
copy_path(char *destination, size_t size, const char *source)
{
    int written = snprintf(destination, size, "%s", source);
    return written >= 0 && (size_t)written < size;
}

static bool
join_path(
    char *destination,
    size_t size,
    const char *parent,
    const char *child
)
{
    int written = snprintf(destination, size, "%s/%s", parent, child);
    return written >= 0 && (size_t)written < size;
}

static bool
resolve_projects_root(Lardon3DAppState *state, char root[PATH_MAX])
{
    const char *configured = getenv("LARDON3D_PROJECTS_ROOT");
    if (configured && configured[0]) {
        if (configured[0] != '/' || !copy_path(root, PATH_MAX, configured)) {
            set_status(state, "Erreur : répertoire racine invalide.");
            return false;
        }
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            set_status(state, "Erreur : HOME est absent ou invalide.");
            return false;
        }

        int written = snprintf(
            root,
            PATH_MAX,
            "%s/Documents/Lardon/Projets3D",
            home
        );
        if (written < 0 || (size_t)written >= PATH_MAX) {
            set_status(state, "Erreur : chemin racine trop long.");
            return false;
        }
    }

    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/') {
        root[--length] = '\0';
    }
    return true;
}

static void
cleanup_directories(CreatedDirectories *created)
{
    while (created->count > 0) {
        --created->count;
        (void)rmdir(created->paths[created->count]);
    }
}

static bool
ensure_directory(
    Lardon3DAppState *state,
    const char *path,
    CreatedDirectories *created
)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            set_status(state, "Erreur : un élément du chemin n'est pas un dossier.");
            return false;
        }
        return true;
    }
    if (errno != ENOENT || created->count >= MAX_CREATED_DIRECTORIES) {
        set_status(state, "Erreur : impossible de préparer le répertoire racine.");
        return false;
    }
    if (mkdir(path, 0755) != 0) {
        set_status(state, "Erreur : impossible de créer un dossier.");
        return false;
    }
    if (!copy_path(
            created->paths[created->count],
            sizeof(created->paths[created->count]),
            path
        )) {
        (void)rmdir(path);
        set_status(state, "Erreur : chemin de dossier trop long.");
        return false;
    }
    ++created->count;
    return true;
}

static bool
ensure_directory_tree(
    Lardon3DAppState *state,
    const char *path,
    CreatedDirectories *created
)
{
    char partial[PATH_MAX];
    if (!copy_path(partial, sizeof(partial), path)) {
        set_status(state, "Erreur : chemin racine trop long.");
        return false;
    }

    for (char *separator = partial + 1; *separator; ++separator) {
        if (*separator != '/') {
            continue;
        }
        *separator = '\0';
        bool success = ensure_directory(state, partial, created);
        *separator = '/';
        if (!success) {
            return false;
        }
    }
    return ensure_directory(state, partial, created);
}

static bool
write_project_ini(
    Lardon3DAppState *state,
    const char *project_path,
    const char *project_name
)
{
    char temporary_path[PATH_MAX];
    char final_path[PATH_MAX];
    if (!join_path(final_path, sizeof(final_path), project_path, "project.ini")
        || !join_path(
            temporary_path,
            sizeof(temporary_path),
            project_path,
            ".project.ini.tmp.XXXXXX"
        )) {
        set_status(state, "Erreur : chemin de project.ini trop long.");
        return false;
    }

    int descriptor = mkstemp(temporary_path);
    if (descriptor < 0) {
        set_status(state, "Erreur : impossible de créer project.ini.");
        return false;
    }

    FILE *file = fdopen(descriptor, "w");
    if (!file) {
        (void)close(descriptor);
        (void)unlink(temporary_path);
        set_status(state, "Erreur : impossible d'écrire project.ini.");
        return false;
    }

    bool success = fprintf(
        file,
        "[project]\nname=%s\nversion=1\n",
        project_name
    ) >= 0;
    if (success) {
        success = fflush(file) == 0;
    }
    if (success) {
        success = fsync(fileno(file)) == 0;
    }
    if (fclose(file) != 0) {
        success = false;
    }
    if (success) {
        success = rename(temporary_path, final_path) == 0;
    }
    if (!success) {
        (void)unlink(temporary_path);
        set_status(state, "Erreur : impossible d'écrire project.ini.");
    }
    return success;
}

bool
lardon3d_project_create(Lardon3DAppState *state, const char *name)
{
    if (!state) {
        return false;
    }

    char normalized_name[sizeof(state->project_name)];
    char root[PATH_MAX];
    char project_path[PATH_MAX];
    if (!normalize_name(
            state,
            name,
            normalized_name,
            sizeof(normalized_name)
        )) {
        return false;
    }
    if (!resolve_projects_root(state, root)) {
        return false;
    }
    if (!join_path(
            project_path,
            sizeof(project_path),
            root,
            normalized_name
        )) {
        set_status(state, "Erreur : chemin du projet trop long.");
        return false;
    }

    CreatedDirectories created = {0};
    if (!ensure_directory_tree(state, root, &created)) {
        cleanup_directories(&created);
        return false;
    }

    struct stat info;
    if (lstat(project_path, &info) == 0 || errno != ENOENT) {
        cleanup_directories(&created);
        set_status(state, "Erreur : ce projet existe déjà.");
        return false;
    }
    if (!ensure_directory(state, project_path, &created)) {
        cleanup_directories(&created);
        return false;
    }

    const char *subdirectories[] = {
        "images",
        "reconstruction",
        "exports",
        "logs",
    };
    for (size_t index = 0;
         index < sizeof(subdirectories) / sizeof(subdirectories[0]);
         ++index) {
        char path[PATH_MAX];
        if (!join_path(
                path,
                sizeof(path),
                project_path,
                subdirectories[index]
            )) {
            set_status(state, "Erreur : chemin de dossier trop long.");
            cleanup_directories(&created);
            return false;
        }
        if (!ensure_directory(state, path, &created)) {
            cleanup_directories(&created);
            return false;
        }
    }

    if (!write_project_ini(state, project_path, normalized_name)) {
        cleanup_directories(&created);
        return false;
    }

    state->project_loaded = true;
    (void)copy_path(
        state->project_name,
        sizeof(state->project_name),
        normalized_name
    );
    (void)copy_path(
        state->project_path,
        sizeof(state->project_path),
        project_path
    );
    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "Projet créé : %s",
        state->project_name
    );
    return true;
}

static bool
read_project_ini(
    Lardon3DAppState *state,
    const char *path,
    char project_name[128]
)
{
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        set_status(state, "Erreur : project.ini est absent ou inaccessible.");
        return false;
    }

    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        (void)close(descriptor);
        set_status(state, "Erreur : project.ini n'est pas un fichier régulier.");
        return false;
    }

    FILE *file = fdopen(descriptor, "r");
    if (!file) {
        (void)close(descriptor);
        set_status(state, "Erreur : impossible de lire project.ini.");
        return false;
    }

    bool in_project_section = false;
    bool section_found = false;
    bool name_found = false;
    bool version_found = false;
    bool valid = true;
    char line[INI_LINE_CAPACITY];

    while (valid && fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\n') {
            line[--length] = '\0';
        } else if (!feof(file)) {
            valid = false;
            break;
        }
        if (length > 0 && line[length - 1] == '\r') {
            line[--length] = '\0';
        }

        if (strcmp(line, "[project]") == 0) {
            in_project_section = true;
            section_found = true;
        } else if (line[0] == '[') {
            in_project_section = false;
        } else if (in_project_section && strncmp(line, "name=", 5) == 0) {
            if (name_found || !normalize_name(
                    state,
                    line + 5,
                    project_name,
                    sizeof(state->project_name)
                )) {
                valid = false;
            }
            name_found = true;
        } else if (in_project_section && strncmp(line, "version=", 8) == 0) {
            if (version_found || strcmp(line, "version=1") != 0) {
                valid = false;
            }
            version_found = true;
        }
    }

    if (ferror(file)) {
        valid = false;
    }
    if (fclose(file) != 0) {
        valid = false;
    }
    if (!valid || !section_found || !name_found || !version_found) {
        set_status(state, "Erreur : project.ini invalide.");
        return false;
    }
    return true;
}

bool
lardon3d_project_open(
    Lardon3DAppState *state,
    const char *directory_name
)
{
    if (!state) {
        return false;
    }

    char normalized_directory[sizeof(state->project_name)];
    char root[PATH_MAX];
    char project_path[PATH_MAX];
    char ini_path[PATH_MAX];
    if (!normalize_name(
            state,
            directory_name,
            normalized_directory,
            sizeof(normalized_directory)
        )) {
        return false;
    }
    if (!resolve_projects_root(state, root)) {
        return false;
    }
    if (!join_path(
            project_path,
            sizeof(project_path),
            root,
            normalized_directory
        )
        || !join_path(ini_path, sizeof(ini_path), project_path, "project.ini")) {
        set_status(state, "Erreur : chemin du projet trop long.");
        return false;
    }

    struct stat info;
    if (lstat(project_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        set_status(state, "Erreur : dossier projet absent ou invalide.");
        return false;
    }

    char project_name[sizeof(state->project_name)];
    if (!read_project_ini(state, ini_path, project_name)) {
        return false;
    }

    state->project_loaded = true;
    (void)copy_path(
        state->project_name,
        sizeof(state->project_name),
        project_name
    );
    (void)copy_path(
        state->project_path,
        sizeof(state->project_path),
        project_path
    );
    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "Projet ouvert : %s",
        state->project_name
    );
    return true;
}

void
lardon3d_project_close(Lardon3DAppState *state)
{
    if (!state) {
        return;
    }

    if (!state->project_loaded) {
        set_status(state, "Aucun projet à fermer.");
        return;
    }

    state->project_loaded = false;
    state->project_name[0] = '\0';
    state->project_path[0] = '\0';
    set_status(state, "Projet fermé.");
}
