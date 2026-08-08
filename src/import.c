#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <lardon3d/import.h>
#include <lardon3d/image_catalog.h>

enum {
    MANIFEST_LINE_CAPACITY = PATH_MAX + 512,
};

typedef struct {
    FILE *file;
    char temporary_path[PATH_MAX];
    char final_path[PATH_MAX];
    bool previous_exists;
} ManifestWriter;

typedef struct {
    char filename[NAME_MAX + 1];
    bool created;
} ImportCandidate;

typedef struct {
    ImportCandidate *items;
    size_t count;
    size_t capacity;
} CandidateList;

typedef enum {
    COPY_IMAGE_ERROR = -1,
    COPY_IMAGE_COLLISION = 0,
    COPY_IMAGE_CREATED = 1,
    COPY_IMAGE_CANCELLED = 2,
} CopyImageOutcome;

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
import_is_cancelled(const Lardon3DImportControl *control)
{
    return control && control->is_cancelled
        && control->is_cancelled(control->context);
}

static void
publish_progress(
    const Lardon3DImportControl *control,
    const Lardon3DImportResult *result,
    size_t processed,
    const char *message
)
{
    if (!control || !control->progressed) {
        return;
    }
    const Lardon3DImportProgress progress = {
        .total = result->admissible_found,
        .processed = processed <= result->admissible_found
            ? processed
            : result->admissible_found,
        .copied = result->copied,
        .already_present = result->already_present,
        .ignored = result->ignored,
        .message = message,
    };
    control->progressed(control->context, &progress);
}

static bool
join_path(
    char destination[PATH_MAX],
    const char *parent,
    const char *child
)
{
    int written = snprintf(destination, PATH_MAX, "%s/%s", parent, child);
    return written >= 0 && (size_t)written < PATH_MAX;
}

static bool
trim_source_path(
    Lardon3DAppState *state,
    const char *input,
    char output[PATH_MAX]
)
{
    if (!input) {
        set_status(state, "Erreur : dossier source vide.");
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
        set_status(state, "Erreur : dossier source vide.");
        return false;
    }
    if (length >= PATH_MAX) {
        set_status(state, "Erreur : chemin source trop long.");
        return false;
    }
    (void)memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool
resolve_source_path(
    Lardon3DAppState *state,
    const char *source,
    char absolute_source[PATH_MAX]
)
{
    if (source[0] == '/') {
        int written = snprintf(absolute_source, PATH_MAX, "%s", source);
        if (written >= 0 && (size_t)written < PATH_MAX) {
            return true;
        }
    } else {
        char current_directory[PATH_MAX];
        if (getcwd(current_directory, sizeof(current_directory))
            && join_path(absolute_source, current_directory, source)) {
            return true;
        }
    }

    set_status(state, "Erreur : chemin source trop long ou inaccessible.");
    return false;
}

static bool
has_supported_extension(const char *filename)
{
    const char *extension = strrchr(filename, '.');
    if (!extension) {
        return false;
    }

    return strcasecmp(extension, ".jpg") == 0
        || strcasecmp(extension, ".jpeg") == 0
        || strcasecmp(extension, ".png") == 0
        || strcasecmp(extension, ".tif") == 0
        || strcasecmp(extension, ".tiff") == 0
        || strcasecmp(extension, ".heic") == 0;
}

static bool
has_forbidden_manifest_character(const char *text)
{
    return strchr(text, '\t') || strchr(text, '\n') || strchr(text, '\r');
}

static bool
candidate_list_append(
    Lardon3DAppState *state,
    CandidateList *candidates,
    const char *filename
)
{
    if (candidates->count == candidates->capacity) {
        size_t capacity = candidates->capacity == 0
            ? 16
            : candidates->capacity * 2;
        if (capacity < candidates->capacity
            || capacity > SIZE_MAX / sizeof(*candidates->items)) {
            set_status(state, "Erreur : trop de fichiers à importer.");
            return false;
        }
        void *items = realloc(
            candidates->items,
            capacity * sizeof(*candidates->items)
        );
        if (!items) {
            set_status(state, "Erreur : mémoire insuffisante pour l'import.");
            return false;
        }
        candidates->items = items;
        candidates->capacity = capacity;
    }

    ImportCandidate *candidate = &candidates->items[candidates->count];
    int written = snprintf(
        candidate->filename,
        sizeof(candidate->filename),
        "%s",
        filename
    );
    if (written < 0 || (size_t)written >= sizeof(candidate->filename)) {
        set_status(state, "Erreur : nom de fichier trop long.");
        return false;
    }
    candidate->created = false;
    ++candidates->count;
    return true;
}

static bool
ensure_originals_directory(
    Lardon3DAppState *state,
    char images_path[PATH_MAX],
    char originals_path[PATH_MAX]
)
{
    if (!join_path(images_path, state->project_path, "images")
        || !join_path(originals_path, images_path, "originals")) {
        set_status(state, "Erreur : chemin du projet trop long.");
        return false;
    }

    struct stat info;
    if (lstat(images_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        set_status(state, "Erreur : dossier images absent ou invalide.");
        return false;
    }
    if (lstat(originals_path, &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            set_status(state, "Erreur : images/originals n'est pas un dossier.");
            return false;
        }
        return true;
    }
    if (errno != ENOENT || mkdir(originals_path, 0755) != 0) {
        set_status(state, "Erreur : impossible de créer images/originals.");
        return false;
    }
    return true;
}

static bool
manifest_line_is_valid(const char *line)
{
    const char *first_tab = strchr(line, '\t');
    if (!first_tab || first_tab == line) {
        return false;
    }
    const char *second_tab = strchr(first_tab + 1, '\t');
    if (!second_tab || second_tab == first_tab + 1) {
        return false;
    }
    if (strchr(second_tab + 1, '\t') || !strchr(second_tab + 1, '\n')) {
        return false;
    }
    for (const char *digit = first_tab + 1; digit < second_tab; ++digit) {
        if (!isdigit((unsigned char)*digit)) {
            return false;
        }
    }
    return true;
}

static void
manifest_abort(ManifestWriter *writer)
{
    if (writer->file) {
        (void)fclose(writer->file);
        writer->file = NULL;
    }
    if (writer->temporary_path[0]) {
        (void)unlink(writer->temporary_path);
    }
}

static bool
manifest_begin(
    Lardon3DAppState *state,
    const char *images_path,
    ManifestWriter *writer
)
{
    *writer = (ManifestWriter) {0};
    if (!join_path(writer->final_path, images_path, "manifest.tsv")
        || !join_path(
            writer->temporary_path,
            images_path,
            ".manifest.tsv.tmp.XXXXXX"
        )) {
        set_status(state, "Erreur : chemin du manifeste trop long.");
        return false;
    }

    FILE *previous = NULL;
    int previous_descriptor = open(
        writer->final_path,
        O_RDONLY | O_NOFOLLOW
    );
    if (previous_descriptor >= 0) {
        struct stat info;
        if (fstat(previous_descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
            (void)close(previous_descriptor);
            set_status(state, "Erreur : manifest.tsv invalide.");
            return false;
        }
        previous = fdopen(previous_descriptor, "r");
        if (!previous) {
            (void)close(previous_descriptor);
            set_status(state, "Erreur : impossible de lire manifest.tsv.");
            return false;
        }
        writer->previous_exists = true;
    } else if (errno != ENOENT) {
        set_status(state, "Erreur : impossible de lire manifest.tsv.");
        return false;
    }

    int descriptor = mkstemp(writer->temporary_path);
    if (descriptor < 0) {
        if (previous) {
            (void)fclose(previous);
        }
        set_status(state, "Erreur : impossible de préparer manifest.tsv.");
        return false;
    }
    writer->file = fdopen(descriptor, "w");
    if (!writer->file) {
        (void)close(descriptor);
        if (previous) {
            (void)fclose(previous);
        }
        manifest_abort(writer);
        set_status(state, "Erreur : impossible d'écrire manifest.tsv.");
        return false;
    }

    bool success = fputs(
        "filename\tsize_bytes\tsource_path\n",
        writer->file
    ) >= 0;
    if (previous) {
        char line[MANIFEST_LINE_CAPACITY];
        if (!fgets(line, sizeof(line), previous)
            || strcmp(line, "filename\tsize_bytes\tsource_path\n") != 0) {
            success = false;
        }
        while (success && fgets(line, sizeof(line), previous)) {
            if (!manifest_line_is_valid(line)
                || fputs(line, writer->file) < 0) {
                success = false;
            }
        }
        if (ferror(previous) || fclose(previous) != 0) {
            success = false;
        }
    }

    if (!success) {
        manifest_abort(writer);
        set_status(state, "Erreur : manifest.tsv invalide ou illisible.");
    }
    return success;
}

static int
manifest_contains(const ManifestWriter *writer, const char *filename)
{
    if (!writer->previous_exists) {
        return 0;
    }

    int descriptor = open(writer->final_path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        return -1;
    }
    FILE *file = fdopen(descriptor, "r");
    if (!file) {
        (void)close(descriptor);
        return -1;
    }

    int found = 0;
    char line[MANIFEST_LINE_CAPACITY];
    (void)fgets(line, sizeof(line), file);
    size_t filename_length = strlen(filename);
    while (fgets(line, sizeof(line), file)) {
        const char *tab = strchr(line, '\t');
        if (tab && (size_t)(tab - line) == filename_length
            && memcmp(line, filename, filename_length) == 0) {
            found = 1;
            break;
        }
    }
    if (ferror(file) || fclose(file) != 0) {
        return -1;
    }
    return found;
}

static bool
manifest_append(
    ManifestWriter *writer,
    const char *filename,
    off_t size,
    const char *source_path
)
{
    return fprintf(
        writer->file,
        "%s\t%" PRIdMAX "\t%s\n",
        filename,
        (intmax_t)size,
        source_path
    ) >= 0;
}

static bool
manifest_commit(Lardon3DAppState *state, ManifestWriter *writer)
{
    bool success = fflush(writer->file) == 0;
    if (success) {
        success = fsync(fileno(writer->file)) == 0;
    }
    if (fclose(writer->file) != 0) {
        success = false;
    }
    writer->file = NULL;
    if (success) {
        success = rename(writer->temporary_path, writer->final_path) == 0;
    }
    if (!success) {
        (void)unlink(writer->temporary_path);
        set_status(state, "Erreur : impossible de mettre à jour manifest.tsv.");
    }
    return success;
}

static ssize_t
read_retry(int descriptor, void *buffer, size_t capacity)
{
    ssize_t count;
    do {
        count = read(descriptor, buffer, capacity);
    } while (count < 0 && errno == EINTR);
    return count;
}

static int
copy_image(
    const char *source_path,
    const char *destination_path,
    off_t *destination_size,
    const Lardon3DImportControl *control
)
{
    int source = open(source_path, O_RDONLY | O_NOFOLLOW);
    if (source < 0) {
        return -1;
    }

    struct stat source_info;
    if (fstat(source, &source_info) != 0 || !S_ISREG(source_info.st_mode)) {
        (void)close(source);
        return -1;
    }

    mode_t mode = source_info.st_mode & 0666;
    if (mode == 0) {
        mode = 0644;
    }
    int destination = open(
        destination_path,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
        mode
    );
    if (destination < 0) {
        if (errno != EEXIST) {
            (void)close(source);
            return COPY_IMAGE_ERROR;
        }
        destination = open(destination_path, O_RDONLY | O_NOFOLLOW);
        struct stat destination_info;
        bool identical = destination >= 0
            && fstat(destination, &destination_info) == 0
            && S_ISREG(destination_info.st_mode)
            && destination_info.st_size == source_info.st_size;
        char source_buffer[64 * 1024], destination_buffer[64 * 1024];
        while (identical) {
            ssize_t source_count = read_retry(source, source_buffer,
                sizeof(source_buffer));
            ssize_t destination_count = read_retry(destination,
                destination_buffer, sizeof(destination_buffer));
            if (source_count < 0 || destination_count < 0
                || source_count != destination_count) {
                identical = false;
                break;
            }
            if (source_count == 0) break;
            if (memcmp(source_buffer, destination_buffer,
                    (size_t)source_count) != 0) {
                identical = false;
            }
        }
        if (destination >= 0 && close(destination) != 0) identical = false;
        if (close(source) != 0) identical = false;
        if (!identical) return COPY_IMAGE_ERROR;
        *destination_size = source_info.st_size;
        return COPY_IMAGE_COLLISION;
    }

    bool success = true;
    bool cancelled = false;
    char buffer[64 * 1024];
    for (;;) {
        if (import_is_cancelled(control)) {
            cancelled = true;
            success = false;
            break;
        }
        ssize_t read_count = read(source, buffer, sizeof(buffer));
        if (read_count == 0) {
            break;
        }
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            success = false;
            break;
        }

        ssize_t written_total = 0;
        while (written_total < read_count) {
            if (import_is_cancelled(control)) {
                cancelled = true;
                success = false;
                break;
            }
            ssize_t written = write(
                destination,
                buffer + written_total,
                (size_t)(read_count - written_total)
            );
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                success = false;
                break;
            }
            written_total += written;
        }
        if (!success) {
            break;
        }
    }

    if (success) {
        success = fsync(destination) == 0;
    }
    if (close(source) != 0) {
        success = false;
    }
    if (close(destination) != 0) {
        success = false;
    }
    if (!success) {
        (void)unlink(destination_path);
        return cancelled ? COPY_IMAGE_CANCELLED : COPY_IMAGE_ERROR;
    }

    *destination_size = source_info.st_size;
    return COPY_IMAGE_CREATED;
}

static bool
analyze_source_directory(
    Lardon3DAppState *state,
    const char *absolute_source,
    const char *originals_path,
    Lardon3DImportResult *result,
    CandidateList *candidates,
    const Lardon3DImportControl *control,
    bool *cancelled
)
{
    DIR *directory = opendir(absolute_source);
    if (!directory) {
        set_status(state, "Erreur : impossible d'ouvrir le dossier source.");
        return false;
    }

    bool success = true;
    for (;;) {
        if (import_is_cancelled(control)) {
            *cancelled = true;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno != 0) {
                set_status(state, "Erreur : lecture du dossier source impossible.");
                success = false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char source_path[PATH_MAX];
        if (!join_path(source_path, absolute_source, entry->d_name)) {
            set_status(state, "Erreur : chemin source trop long.");
            success = false;
            break;
        }

        struct stat source_info;
        if (lstat(source_path, &source_info) != 0
            || !S_ISREG(source_info.st_mode)
            || !has_supported_extension(entry->d_name)) {
            ++result->ignored;
            continue;
        }
        ++result->admissible_found;

        if (has_forbidden_manifest_character(entry->d_name)) {
            set_status(
                state,
                "Erreur : nom de fichier incompatible avec le manifeste."
            );
            success = false;
            break;
        }

        char destination_path[PATH_MAX];
        if (!join_path(destination_path, originals_path, entry->d_name)) {
            set_status(state, "Erreur : chemin destination trop long.");
            success = false;
            break;
        }
        if (!candidate_list_append(state, candidates, entry->d_name)) {
            success = false;
            break;
        }
    }

    if (closedir(directory) != 0) {
        set_status(state, "Erreur : fermeture du dossier source impossible.");
        success = false;
    }
    return success;
}

static size_t
rollback_created_files(
    CandidateList *candidates,
    const char *originals_path
)
{
    size_t removed = 0;
    for (size_t index = candidates->count; index > 0; --index) {
        ImportCandidate *candidate = &candidates->items[index - 1];
        if (!candidate->created) {
            continue;
        }

        char destination_path[PATH_MAX];
        if (join_path(
                destination_path,
                originals_path,
                candidate->filename
            )
            && unlink(destination_path) == 0) {
            candidate->created = false;
            ++removed;
        }
    }
    return removed;
}

static void
fail_import(
    Lardon3DAppState *state,
    Lardon3DImportResult *result,
    size_t removed,
    const char *reason
)
{
    size_t copied_before_rollback = result->copied;
    result->copied -= removed;
    if (copied_before_rollback > 0 && result->copied == 0) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Import annulé : %zu copie%s retirée%s après erreur (%s).",
            removed,
            removed == 1 ? "" : "s",
            removed == 1 ? "" : "s",
            reason
        );
    } else if (result->copied > 0) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur critique : %zu copie%s conservée%s sans manifeste (%s).",
            result->copied,
            result->copied == 1 ? "" : "s",
            result->copied == 1 ? "" : "s",
            reason
        );
    } else {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur d'import : %s.",
            reason
        );
    }
}

static Lardon3DImportOutcome
cancel_import(
    Lardon3DAppState *state,
    Lardon3DImportResult *result,
    size_t processed,
    ManifestWriter *manifest,
    CandidateList *candidates,
    const char *originals_path,
    const Lardon3DImportControl *control
)
{
    manifest_abort(manifest);
    size_t removed = rollback_created_files(candidates, originals_path);
    result->copied -= removed;
    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "Import annulé : %zu sur %zu fichiers traités.",
        processed,
        result->admissible_found
    );
    publish_progress(control, result, processed, state->status_message);
    free(candidates->items);
    return LARDON3D_IMPORT_CANCELLED;
}

Lardon3DImportOutcome
lardon3d_import_directory_batch(
    Lardon3DAppState *state,
    const char *source_directory,
    size_t batch_size,
    Lardon3DImportResult *result,
    const Lardon3DImportControl *control,
    bool *complete
)
{
    if (complete) *complete = false;
    if (!state || !result || !complete || batch_size == 0
        || !state->project_loaded) {
        return LARDON3D_IMPORT_FAILED;
    }
    *result = (Lardon3DImportResult) {0};
    char trimmed_source[PATH_MAX], absolute_source[PATH_MAX];
    if (!trim_source_path(state, source_directory, trimmed_source)
        || !resolve_source_path(state, trimmed_source, absolute_source)
        || has_forbidden_manifest_character(absolute_source)) {
        return LARDON3D_IMPORT_FAILED;
    }
    struct stat source_info;
    if (lstat(absolute_source, &source_info) != 0
        || !S_ISDIR(source_info.st_mode)) {
        set_status(state, "Erreur : dossier source absent ou invalide.");
        return LARDON3D_IMPORT_FAILED;
    }
    char images_path[PATH_MAX], originals_path[PATH_MAX];
    if (!join_path(images_path, state->project_path, "images")
        || !join_path(originals_path, images_path, "originals")
        || !ensure_originals_directory(state, images_path, originals_path)) {
        return LARDON3D_IMPORT_FAILED;
    }
    ManifestWriter manifest;
    if (!manifest_begin(state, images_path, &manifest)) {
        return LARDON3D_IMPORT_FAILED;
    }
    DIR *directory = opendir(absolute_source);
    if (!directory) {
        manifest_abort(&manifest);
        set_status(state, "Erreur : impossible d'ouvrir le dossier source.");
        return LARDON3D_IMPORT_FAILED;
    }
    CandidateList created = {0};
    bool success = true, cancelled = false;
    size_t added = 0;
    for (;;) {
        if (import_is_cancelled(control)) {
            cancelled = true;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno != 0) success = false;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) continue;
        char source_path[PATH_MAX], destination_path[PATH_MAX];
        struct stat item_info;
        if (!join_path(source_path, absolute_source, entry->d_name)) {
            success = false; break;
        }
        if (lstat(source_path, &item_info) != 0
            || !S_ISREG(item_info.st_mode)
            || !has_supported_extension(entry->d_name)) {
            ++result->ignored;
            continue;
        }
        ++result->admissible_found;
        if (has_forbidden_manifest_character(entry->d_name)) {
            success = false; break;
        }
        int listed = manifest_contains(&manifest, entry->d_name);
        if (listed < 0) {
            success = false; break;
        }
        if (listed > 0) {
            ++result->processed;
            ++result->already_present;
            continue;
        }
        if (added >= batch_size
            || !join_path(destination_path, originals_path, entry->d_name)) {
            if (added < batch_size) success = false;
            continue;
        }
        off_t size = 0;
        int copied = copy_image(source_path, destination_path, &size, control);
        if (copied == COPY_IMAGE_CANCELLED) {
            cancelled = true; break;
        }
        if (copied == COPY_IMAGE_ERROR) {
            success = false; break;
        }
        if (copied == COPY_IMAGE_COLLISION) {
            struct stat destination_info;
            if (lstat(destination_path, &destination_info) != 0
                || !S_ISREG(destination_info.st_mode)) {
                success = false; break;
            }
            size = destination_info.st_size;
            ++result->already_present;
        } else {
            if (!candidate_list_append(state, &created, entry->d_name)) {
                (void)unlink(destination_path);
                success = false; break;
            }
            created.items[created.count - 1].created = true;
            ++result->copied;
        }
        if (!manifest_append(&manifest, entry->d_name, size, source_path)) {
            success = false; break;
        }
        ++added;
        ++result->newly_manifested;
        ++result->processed;
    }
    if (closedir(directory) != 0) success = false;
    if (!success || cancelled) {
        manifest_abort(&manifest);
        (void)rollback_created_files(&created, originals_path);
        free(created.items);
        set_status(state, cancelled ? "Import annulé à une frontière sûre."
                                    : "Erreur pendant un lot d'import.");
        return cancelled ? LARDON3D_IMPORT_CANCELLED : LARDON3D_IMPORT_FAILED;
    }
    if (!manifest_commit(state, &manifest)) {
        (void)rollback_created_files(&created, originals_path);
        free(created.items);
        return LARDON3D_IMPORT_FAILED;
    }
    free(created.items);
    *complete = result->processed == result->admissible_found;
    set_status(state, *complete ? "Import terminé." : "Lot d'import publié.");
    publish_progress(control, result, result->processed, state->status_message);
    return LARDON3D_IMPORT_SUCCEEDED;
}

Lardon3DImportOutcome
lardon3d_import_directory_controlled(
    Lardon3DAppState *state,
    const char *source_directory,
    Lardon3DImportResult *result,
    const Lardon3DImportControl *control
)
{
    if (!state || !result) {
        return LARDON3D_IMPORT_FAILED;
    }
    *result = (Lardon3DImportResult) {0};
    publish_progress(control, result, 0, "Analyse du dossier source...");

    if (!state->project_loaded) {
        set_status(state, "Aucun projet chargé.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }

    char trimmed_source[PATH_MAX];
    char absolute_source[PATH_MAX];
    if (!trim_source_path(state, source_directory, trimmed_source)) {
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }
    if (!resolve_source_path(state, trimmed_source, absolute_source)) {
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }
    if (has_forbidden_manifest_character(absolute_source)) {
        set_status(state, "Erreur : chemin source incompatible avec le manifeste.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }

    struct stat source_directory_info;
    if (lstat(absolute_source, &source_directory_info) != 0) {
        set_status(state, "Erreur : dossier source inexistant.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }
    if (!S_ISDIR(source_directory_info.st_mode)) {
        set_status(state, "Erreur : la source n'est pas un dossier.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }

    char images_path[PATH_MAX];
    char originals_path[PATH_MAX];
    if (!join_path(images_path, state->project_path, "images")
        || !join_path(originals_path, images_path, "originals")) {
        set_status(state, "Erreur : chemin du projet trop long.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }

    CandidateList candidates = {0};
    bool analysis_cancelled = false;
    if (!analyze_source_directory(
            state,
            absolute_source,
            originals_path,
            result,
            &candidates,
            control,
            &analysis_cancelled
        )) {
        free(candidates.items);
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }
    if (analysis_cancelled) {
        free(candidates.items);
        set_status(state, "Import annulé : 0 fichier traité.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_CANCELLED;
    }
    publish_progress(control, result, 0, "Import en cours...");

    if (import_is_cancelled(control)) {
        free(candidates.items);
        set_status(state, "Import annulé : 0 fichier traité.");
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_CANCELLED;
    }

    if (!ensure_originals_directory(state, images_path, originals_path)) {
        free(candidates.items);
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }

    ManifestWriter manifest;
    if (!manifest_begin(state, images_path, &manifest)) {
        free(candidates.items);
        publish_progress(control, result, 0, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }

    bool success = true;
    bool cancelled = false;
    size_t processed = 0;
    const char *failure_reason = NULL;
    for (size_t index = 0; index < candidates.count; ++index) {
        if (import_is_cancelled(control)) {
            cancelled = true;
            break;
        }
        ImportCandidate *candidate = &candidates.items[index];
        char source_path[PATH_MAX];
        char destination_path[PATH_MAX];
        if (!join_path(source_path, absolute_source, candidate->filename)
            || !join_path(
                destination_path,
                originals_path,
                candidate->filename
            )) {
            success = false;
            failure_reason = "chemin de fichier trop long";
            break;
        }

        off_t size = 0;
        int copied = copy_image(
            source_path,
            destination_path,
            &size,
            control
        );
        if (copied == COPY_IMAGE_CANCELLED) {
            cancelled = true;
            break;
        }
        if (copied == COPY_IMAGE_ERROR) {
            success = false;
            failure_reason = "copie d'une image impossible";
            break;
        }
        if (copied == COPY_IMAGE_COLLISION) {
            ++result->already_present;
            struct stat destination_info;
            bool destination_is_regular = false;
            if (lstat(destination_path, &destination_info) == 0
                && S_ISREG(destination_info.st_mode)) {
                size = destination_info.st_size;
                destination_is_regular = true;
            }
            if (!destination_is_regular) {
                ++processed;
                publish_progress(control, result, processed, "Import en cours...");
                continue;
            }
        } else {
            ++result->copied;
            candidate->created = true;
        }

        int already_listed = manifest_contains(
            &manifest,
            candidate->filename
        );
        if (already_listed < 0) {
            success = false;
            failure_reason = "lecture du manifeste impossible";
            break;
        }
        if (!already_listed
            && !manifest_append(
                &manifest,
                candidate->filename,
                size,
                source_path
            )) {
            success = false;
            failure_reason = "écriture du manifeste impossible";
            break;
        }
        ++processed;
        publish_progress(control, result, processed, "Import en cours...");
    }

    if (cancelled) {
        return cancel_import(
            state,
            result,
            processed,
            &manifest,
            &candidates,
            originals_path,
            control
        );
    }
    if (!success) {
        manifest_abort(&manifest);
        size_t removed = rollback_created_files(
            &candidates,
            originals_path
        );
        free(candidates.items);
        (void)fail_import(state, result, removed, failure_reason);
        publish_progress(control, result, processed, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }
    if (import_is_cancelled(control)) {
        return cancel_import(
            state,
            result,
            processed,
            &manifest,
            &candidates,
            originals_path,
            control
        );
    }
    if (!manifest_commit(state, &manifest)) {
        size_t removed = rollback_created_files(
            &candidates,
            originals_path
        );
        free(candidates.items);
        (void)fail_import(
            state,
            result,
            removed,
            "mise à jour du manifeste impossible"
        );
        publish_progress(control, result, processed, state->status_message);
        return LARDON3D_IMPORT_FAILED;
    }
    free(candidates.items);

    (void)snprintf(
        state->status_message,
        sizeof(state->status_message),
        "Import terminé : %zu copiée%s, %zu déjà présente%s.",
        result->copied,
        result->copied == 1 ? "" : "s",
        result->already_present,
        result->already_present == 1 ? "" : "s"
    );
    publish_progress(control, result, processed, "Import terminé.");
    return LARDON3D_IMPORT_SUCCEEDED;
}

bool
lardon3d_import_directory(
    Lardon3DAppState *state,
    const char *source_directory,
    Lardon3DImportResult *result
)
{
    return lardon3d_import_directory_controlled(
        state,
        source_directory,
        result,
        NULL
    ) == LARDON3D_IMPORT_SUCCEEDED;
}

Lardon3DImportOutcome
lardon3d_import_directory_batch_to_scanset(
    Lardon3DAppState *state,
    uint64_t scanset_id,
    uint64_t producer_task_id,
    const char *source_directory,
    size_t batch_size,
    Lardon3DImportResult *result,
    const Lardon3DImportControl *control,
    bool *complete
)
{
    if (!state || !state->project_loaded || !state->project_db || !result
        || !complete || scanset_id == 0 || producer_task_id == 0
        || batch_size == 0) return LARDON3D_IMPORT_FAILED;
    *result = (Lardon3DImportResult){0};
    *complete = false;
    char trimmed[PATH_MAX], source[PATH_MAX];
    if (!trim_source_path(state, source_directory, trimmed)
        || !resolve_source_path(state, trimmed, source)) return LARDON3D_IMPORT_FAILED;
    struct stat directory_info;
    if (lstat(source, &directory_info) != 0 || !S_ISDIR(directory_info.st_mode)
        || S_ISLNK(directory_info.st_mode)) {
        set_status(state, "Erreur : dossier source absent ou invalide.");
        return LARDON3D_IMPORT_FAILED;
    }
    DIR *directory = opendir(source);
    if (!directory) return LARDON3D_IMPORT_FAILED;
    bool success = true, cancelled = false, remaining = false;
    size_t registered = 0;
    ManifestWriter legacy_manifest = {0};
    bool legacy_manifest_active = false;
    char legacy_images[PATH_MAX], legacy_originals[PATH_MAX];
    bool legacy_paths = ensure_originals_directory(state, legacy_images,
        legacy_originals);
    for (;;) {
        if (import_is_cancelled(control)) { cancelled = true; break; }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) success = false; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[PATH_MAX]; struct stat information;
        if (!join_path(path, source, entry->d_name)) { success = false; break; }
        if (lstat(path, &information) != 0 || !S_ISREG(information.st_mode)
            || S_ISLNK(information.st_mode) || !has_supported_extension(entry->d_name)) {
            ++result->ignored; continue;
        }
        ++result->admissible_found;
        if (registered >= batch_size) { remaining = true; continue; }
        Lardon3DProjectDbImage image;
        Lardon3DProjectDbImageAsset asset;
        Lardon3DImageCatalogImportResult imported =
            lardon3d_image_catalog_import_file(state, scanset_id, path,
                producer_task_id, &image, &asset);
        if (imported == LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT) {
            ++result->already_present;
            ++result->processed;
        } else if (imported == LARDON3D_IMAGE_CATALOG_IMPORTED) {
            ++registered;
            ++result->newly_manifested;
            ++result->copied;
            ++result->processed;
            if (legacy_paths && (!legacy_manifest_active
                    ? manifest_begin(state, legacy_images, &legacy_manifest)
                    : true)) {
                legacy_manifest_active = true;
                int listed = manifest_contains(&legacy_manifest, entry->d_name);
                char asset_path[PATH_MAX], legacy_path[PATH_MAX];
                if (listed == 0
                    && join_path(asset_path, state->project_path, asset.path)
                    && join_path(legacy_path, legacy_originals, entry->d_name)
                    && link(asset_path, legacy_path) == 0
                    && !manifest_append(&legacy_manifest, entry->d_name,
                        (off_t)asset.size_bytes, path)) {
                    (void)unlink(legacy_path);
                    manifest_abort(&legacy_manifest);
                    legacy_manifest_active = false;
                    legacy_paths = false;
                }
            }
        } else {
            success = false; break;
        }
    }
    if (closedir(directory) != 0) success = false;
    if (legacy_manifest_active && !manifest_commit(state, &legacy_manifest)) {
        legacy_manifest_active = false;
    }
    if (cancelled) {
        set_status(state, "Import annulé à une frontière sûre.");
        return LARDON3D_IMPORT_CANCELLED;
    }
    if (!success) {
        set_status(state, "Erreur pendant un lot d'import.");
        return LARDON3D_IMPORT_FAILED;
    }
    *complete = !remaining;
    set_status(state, *complete ? "Import terminé." : "Lot d'import publié.");
    publish_progress(control, result, result->processed, state->status_message);
    return LARDON3D_IMPORT_SUCCEEDED;
}
