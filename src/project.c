#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/import_task.h>
#include <lardon3d/project.h>
#include <lardon3d/project_db.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>

enum {
  MAX_CREATED_DIRECTORIES = 16,
  INI_LINE_CAPACITY = 512,
};

typedef struct {
  char paths[MAX_CREATED_DIRECTORIES][PATH_MAX];
  size_t count;
} CreatedDirectories;

typedef struct {
  char name[128];
  char stable_id[LARDON3D_PROJECT_DB_ID_CAPACITY];
  unsigned int version;
} ProjectMetadata;

static void set_status(Lardon3DAppState *state, const char *message) {
  (void)snprintf(state->status_message, sizeof(state->status_message), "%s", message);
}

static void store_recovery_summary(Lardon3DAppState *state,
                                   const Lardon3DProjectRecoverySummary *summary) {
  state->recovery_inspected = summary ? summary->inspected : 0;
  state->recovery_resumed = summary ? summary->resumed : 0;
  state->recovery_skipped = summary ? summary->skipped : 0;
  state->recovery_failed = summary ? summary->failed : 0;
  state->recovery_published_not_durable = summary ? summary->published_not_durable : 0;
  state->recovery_queue_full = summary && summary->queue_full;
}

static void set_database_status(Lardon3DAppState *state, Lardon3DProjectDb *database,
                                const char *open_error, const char *fallback) {
  char detail[LARDON3D_PROJECT_DB_ERROR_CAPACITY] = "";
  if (database) {
    (void)lardon3d_project_db_last_error(database, detail);
  } else if (open_error) {
    (void)snprintf(detail, sizeof(detail), "%s", open_error);
  }
  (void)snprintf(state->status_message, sizeof(state->status_message), "project.db error: %.220s",
                 detail[0] ? detail : fallback);
}

static void clear_catalog(Lardon3DAppState *state) {
  lardon3d_image_view_destroy(state->image_view);
  state->image_view = NULL;
  lardon3d_image_catalog_destroy(state->image_catalog);
  state->image_catalog = NULL;
}

static bool normalize_name(Lardon3DAppState *state, const char *input, char *output,
                           size_t output_size) {
  if (!input) {
    set_status(state, "Error: project name is empty.");
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
    set_status(state, "Error: project name is empty.");
    return false;
  }
  if (length >= output_size) {
    set_status(state, "Error: project name is too long.");
    return false;
  }
  if ((length == 1 && start[0] == '.') || (length == 2 && start[0] == '.' && start[1] == '.')) {
    set_status(state, "Error: forbidden project name.");
    return false;
  }

  for (const char *character = start; character < end; ++character) {
    if (*character == '/' || *character == '\\' || iscntrl((unsigned char)*character)) {
      set_status(state, "Error: forbidden project name.");
      return false;
    }
  }

  (void)memcpy(output, start, length);
  output[length] = '\0';
  return true;
}

static bool copy_path(char *destination, size_t size, const char *source) {
  int written = snprintf(destination, size, "%s", source);
  return written >= 0 && (size_t)written < size;
}

static bool join_path(char *destination, size_t size, const char *parent, const char *child) {
  int written = snprintf(destination, size, "%s/%s", parent, child);
  return written >= 0 && (size_t)written < size;
}

static bool generate_stable_id(char output[LARDON3D_PROJECT_DB_ID_CAPACITY]) {
  unsigned char bytes[16];
  int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  size_t used = 0;
  while (used < sizeof(bytes)) {
    ssize_t amount = read(descriptor, bytes + used, sizeof(bytes) - used);
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    if (amount <= 0) {
      (void)close(descriptor);
      return false;
    }
    used += (size_t)amount;
  }
  if (close(descriptor) != 0) {
    return false;
  }
  for (size_t index = 0; index < sizeof(bytes); ++index) {
    (void)snprintf(output + index * 2, 3, "%02x", bytes[index]);
  }
  return true;
}

static bool resolve_projects_root(Lardon3DAppState *state, char root[PATH_MAX]) {
  const char *configured = getenv("LARDON3D_PROJECTS_ROOT");
  if (configured && configured[0]) {
    if (configured[0] != '/' || !copy_path(root, PATH_MAX, configured)) {
      set_status(state, "Error: invalid project root directory.");
      return false;
    }
  } else {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') {
      set_status(state, "Error: HOME is missing or invalid.");
      return false;
    }

    int written = snprintf(root, PATH_MAX, "%s/Documents/Lardon/Projets3D", home);
    if (written < 0 || (size_t)written >= PATH_MAX) {
      set_status(state, "Error: root path is too long.");
      return false;
    }
  }

  size_t length = strlen(root);
  while (length > 1 && root[length - 1] == '/') {
    root[--length] = '\0';
  }
  return true;
}

static void cleanup_directories(CreatedDirectories *created) {
  while (created->count > 0) {
    --created->count;
    (void)rmdir(created->paths[created->count]);
  }
}

static bool ensure_directory(Lardon3DAppState *state, const char *path,
                             CreatedDirectories *created) {
  struct stat info;
  if (lstat(path, &info) == 0) {
    if (!S_ISDIR(info.st_mode)) {
      set_status(state, "Error: a path component is not a directory.");
      return false;
    }
    return true;
  }
  if (errno != ENOENT || created->count >= MAX_CREATED_DIRECTORIES) {
    set_status(state, "Error: unable to prepare the root directory.");
    return false;
  }
  if (mkdir(path, 0755) != 0) {
    set_status(state, "Error: unable to create a directory.");
    return false;
  }
  if (!copy_path(created->paths[created->count], sizeof(created->paths[created->count]), path)) {
    (void)rmdir(path);
    set_status(state, "Error: directory path is too long.");
    return false;
  }
  ++created->count;
  return true;
}

static bool ensure_directory_tree(Lardon3DAppState *state, const char *path,
                                  CreatedDirectories *created) {
  char partial[PATH_MAX];
  if (!copy_path(partial, sizeof(partial), path)) {
    set_status(state, "Error: root path is too long.");
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

static bool write_project_ini(Lardon3DAppState *state, const char *project_path,
                              const char *project_name, const char *stable_id) {
  char temporary_path[PATH_MAX];
  char final_path[PATH_MAX];
  if (!join_path(final_path, sizeof(final_path), project_path, "project.ini") ||
      !join_path(temporary_path, sizeof(temporary_path), project_path, ".project.ini.tmp.XXXXXX")) {
    set_status(state, "Error: project.ini path is too long.");
    return false;
  }

  int descriptor = mkstemp(temporary_path);
  if (descriptor < 0) {
    set_status(state, "Error: unable to create project.ini.");
    return false;
  }

  FILE *file = fdopen(descriptor, "w");
  if (!file) {
    (void)close(descriptor);
    (void)unlink(temporary_path);
    set_status(state, "Error: unable to write project.ini.");
    return false;
  }

  bool success =
      fprintf(file, "[project]\nname=%s\nstable_id=%s\nversion=2\n", project_name, stable_id) >= 0;
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
    set_status(state, "Error: unable to write project.ini.");
  }
  return success;
}

bool lardon3d_project_create(Lardon3DAppState *state, const char *name) {
  if (!state) {
    return false;
  }
  if (state->project_loaded || state->project_db) {
    set_status(state, "Error: a project is already open.");
    return false;
  }

  char normalized_name[sizeof(state->project_name)];
  char root[PATH_MAX];
  char project_path[PATH_MAX];
  char stable_id[LARDON3D_PROJECT_DB_ID_CAPACITY];
  if (!normalize_name(state, name, normalized_name, sizeof(normalized_name))) {
    return false;
  }
  if (!resolve_projects_root(state, root)) {
    return false;
  }
  if (!generate_stable_id(stable_id)) {
    set_status(state, "Error: unable to create project identity.");
    return false;
  }
  if (!join_path(project_path, sizeof(project_path), root, normalized_name)) {
    set_status(state, "Error: project path is too long.");
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
    set_status(state, "Error: this project already exists.");
    return false;
  }
  if (!ensure_directory(state, project_path, &created)) {
    cleanup_directories(&created);
    return false;
  }

  const char *subdirectories[] = {
      "images", "reconstruction", "exports", "logs", ".lardon3d", ".lardon3d/checkpoints",
  };
  for (size_t index = 0; index < sizeof(subdirectories) / sizeof(subdirectories[0]); ++index) {
    char path[PATH_MAX];
    if (!join_path(path, sizeof(path), project_path, subdirectories[index])) {
      set_status(state, "Error: directory path is too long.");
      cleanup_directories(&created);
      return false;
    }
    if (!ensure_directory(state, path, &created)) {
      cleanup_directories(&created);
      return false;
    }
  }

  if (!write_project_ini(state, project_path, normalized_name, stable_id)) {
    cleanup_directories(&created);
    return false;
  }

  char database_path[PATH_MAX];
  char ini_path[PATH_MAX];
  if (!join_path(database_path, sizeof(database_path), project_path, "project.db") ||
      !join_path(ini_path, sizeof(ini_path), project_path, "project.ini")) {
    cleanup_directories(&created);
    set_status(state, "Error: project database path is too long.");
    return false;
  }
  Lardon3DProjectDb *database = NULL;
  char database_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDbResult database_result =
      lardon3d_project_db_open(database_path, &database, database_error);
  struct timespec now;
  if (database_result == LARDON3D_PROJECT_DB_OK && clock_gettime(CLOCK_REALTIME, &now) == 0) {
    Lardon3DProjectDbProject project = {
        .created_at = now.tv_sec,
        .updated_at = now.tv_sec,
    };
    (void)snprintf(project.stable_id, sizeof(project.stable_id), "%s", stable_id);
    (void)snprintf(project.name, sizeof(project.name), "%s", normalized_name);
    database_result = lardon3d_project_db_set_project(database, &project);
  } else if (database_result == LARDON3D_PROJECT_DB_OK) {
    database_result = LARDON3D_PROJECT_DB_IO_ERROR;
  }
  if (database_result != LARDON3D_PROJECT_DB_OK) {
    set_database_status(state, database, database_error, "initialisation impossible");
    lardon3d_project_db_close(database);
    (void)unlink(database_path);
    (void)unlink(ini_path);
    cleanup_directories(&created);
    return false;
  }

  clear_catalog(state);
  state->project_loaded = true;
  store_recovery_summary(state, NULL);
  state->project_db = database;
  (void)copy_path(state->project_name, sizeof(state->project_name), normalized_name);
  (void)copy_path(state->project_path, sizeof(state->project_path), project_path);
  (void)snprintf(state->project_stable_id, sizeof(state->project_stable_id), "%s", stable_id);
  (void)snprintf(state->status_message, sizeof(state->status_message), "Project created: %s",
                 state->project_name);
  return true;
}

static bool read_project_ini(Lardon3DAppState *state, const char *path, ProjectMetadata *metadata) {
  int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
  if (descriptor < 0) {
    set_status(state, "Error: project.ini is missing or inaccessible.");
    return false;
  }

  struct stat info;
  if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
    (void)close(descriptor);
    set_status(state, "Error: project.ini is not a regular file.");
    return false;
  }

  FILE *file = fdopen(descriptor, "r");
  if (!file) {
    (void)close(descriptor);
    set_status(state, "Error: unable to read project.ini.");
    return false;
  }

  bool in_project_section = false;
  bool section_found = false;
  bool name_found = false;
  bool stable_id_found = false;
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
      if (name_found ||
          !normalize_name(state, line + 5, metadata->name, sizeof(state->project_name))) {
        valid = false;
      }
      name_found = true;
    } else if (in_project_section && strncmp(line, "stable_id=", 10) == 0) {
      size_t id_length = strnlen(line + 10, sizeof(metadata->stable_id));
      if (stable_id_found || id_length != 32) {
        valid = false;
      } else {
        for (size_t index = 0; index < id_length; ++index) {
          char character = line[10 + index];
          if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            valid = false;
          }
        }
        if (valid) {
          /* The v2 project identity is exactly 128 lowercase hexadecimal bits. Copying the
           * validated width makes the no-truncation contract explicit to both readers and
           * compiler diagnostics; project identity must never be accepted by prefix. */
          memcpy(metadata->stable_id, line + 10, id_length);
          metadata->stable_id[id_length] = '\0';
        }
      }
      stable_id_found = true;
    } else if (in_project_section && strncmp(line, "version=", 8) == 0) {
      if (version_found) {
        valid = false;
      } else if (strcmp(line, "version=1") == 0) {
        metadata->version = 1;
      } else if (strcmp(line, "version=2") == 0) {
        metadata->version = 2;
      } else {
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
  if (!valid || !section_found || !name_found || !version_found ||
      (metadata->version == 2 && !stable_id_found) || (metadata->version == 1 && stable_id_found)) {
    set_status(state, "Error: invalid project.ini.");
    return false;
  }
  return true;
}

bool lardon3d_project_open(Lardon3DAppState *state, const char *directory_name) {
  if (!state) {
    return false;
  }
  if (state->project_loaded || state->project_db) {
    set_status(state, "Error: a project is already open.");
    return false;
  }

  char normalized_directory[sizeof(state->project_name)];
  char root[PATH_MAX];
  char project_path[PATH_MAX];
  char ini_path[PATH_MAX];
  char database_path[PATH_MAX];
  if (!normalize_name(state, directory_name, normalized_directory, sizeof(normalized_directory))) {
    return false;
  }
  if (!resolve_projects_root(state, root)) {
    return false;
  }
  if (!join_path(project_path, sizeof(project_path), root, normalized_directory) ||
      !join_path(ini_path, sizeof(ini_path), project_path, "project.ini") ||
      !join_path(database_path, sizeof(database_path), project_path, "project.db")) {
    set_status(state, "Error: project path is too long.");
    return false;
  }

  struct stat info;
  if (lstat(project_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
    set_status(state, "Error: project directory is missing or invalid.");
    return false;
  }

  ProjectMetadata metadata = {0};
  if (!read_project_ini(state, ini_path, &metadata)) {
    return false;
  }

  bool database_existed = false;
  if (lstat(database_path, &info) == 0) {
    if (!S_ISREG(info.st_mode)) {
      set_status(state, "Error: project.db is not a regular file.");
      return false;
    }
    database_existed = true;
  } else if (errno != ENOENT) {
    set_status(state, "Error: project.db is inaccessible.");
    return false;
  }

  CreatedDirectories created = {0};
  char internal_path[PATH_MAX];
  char checkpoint_path[PATH_MAX];
  if (!join_path(internal_path, sizeof(internal_path), project_path, ".lardon3d") ||
      !join_path(checkpoint_path, sizeof(checkpoint_path), internal_path, "checkpoints") ||
      !ensure_directory(state, internal_path, &created) ||
      !ensure_directory(state, checkpoint_path, &created)) {
    cleanup_directories(&created);
    return false;
  }

  Lardon3DProjectDb *database = NULL;
  char database_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDbResult database_result =
      lardon3d_project_db_open(database_path, &database, database_error);
  if (database_result != LARDON3D_PROJECT_DB_OK) {
    cleanup_directories(&created);
    set_database_status(state, NULL, database_error, "ouverture impossible");
    return false;
  }

  Lardon3DProjectDbProject db_project;
  database_result = lardon3d_project_db_get_project(database, &db_project);
  bool rewrite_metadata = metadata.version == 1;
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    database_result = LARDON3D_PROJECT_DB_IO_ERROR;
  } else if (database_result == LARDON3D_PROJECT_DB_OK) {
    if (metadata.stable_id[0] && strcmp(metadata.stable_id, db_project.stable_id) != 0) {
      database_result = LARDON3D_PROJECT_DB_CONSTRAINT;
    } else if (!metadata.stable_id[0]) {
      (void)snprintf(metadata.stable_id, sizeof(metadata.stable_id), "%s", db_project.stable_id);
      rewrite_metadata = true;
    }
    if (database_result == LARDON3D_PROJECT_DB_OK) {
      db_project.updated_at = now.tv_sec;
      (void)snprintf(db_project.name, sizeof(db_project.name), "%s", metadata.name);
      database_result = lardon3d_project_db_set_project(database, &db_project);
    }
  } else if (database_result == LARDON3D_PROJECT_DB_NOT_FOUND) {
    if (database_existed && !metadata.stable_id[0]) {
      database_result = LARDON3D_PROJECT_DB_CONSTRAINT;
    } else {
      if (!metadata.stable_id[0]) {
        if (!generate_stable_id(metadata.stable_id)) {
          database_result = LARDON3D_PROJECT_DB_IO_ERROR;
        }
        rewrite_metadata = true;
      }
      if (database_result != LARDON3D_PROJECT_DB_IO_ERROR) {
        Lardon3DProjectDbProject new_project = {
            .created_at = now.tv_sec,
            .updated_at = now.tv_sec,
        };
        (void)snprintf(new_project.stable_id, sizeof(new_project.stable_id), "%s",
                       metadata.stable_id);
        (void)snprintf(new_project.name, sizeof(new_project.name), "%s", metadata.name);
        database_result = lardon3d_project_db_set_project(database, &new_project);
      }
    }
  }
  if (database_result == LARDON3D_PROJECT_DB_OK && rewrite_metadata &&
      !write_project_ini(state, project_path, metadata.name, metadata.stable_id)) {
    database_result = LARDON3D_PROJECT_DB_IO_ERROR;
  }
  if (database_result != LARDON3D_PROJECT_DB_OK) {
    if (database_result == LARDON3D_PROJECT_DB_CONSTRAINT) {
      set_status(state, "Error: inconsistent project.ini/project.db identity.");
    } else {
      set_database_status(state, database, database_error, "initialisation impossible");
    }
    lardon3d_project_db_close(database);
    if (!database_existed) {
      (void)unlink(database_path);
    }
    cleanup_directories(&created);
    return false;
  }

  clear_catalog(state);
  state->project_loaded = true;
  state->project_db = database;
  (void)copy_path(state->project_name, sizeof(state->project_name), metadata.name);
  (void)snprintf(state->project_stable_id, sizeof(state->project_stable_id), "%s",
                 metadata.stable_id);
  (void)copy_path(state->project_path, sizeof(state->project_path), project_path);
  store_recovery_summary(state, NULL);
  if (state->task_queue && state->resource_governor) {
    Lardon3DProjectRecoverySummary summary;
    (void)lardon3d_project_resume_recoverable_tasks(state, lardon3d_task_kind_registry_production(),
                                                    &summary);
    (void)snprintf(state->status_message, sizeof(state->status_message),
                   "Project opened — %zu task(s) resumed, %zu skipped, %zu failed%s.",
                   summary.resumed, summary.skipped, summary.failed,
                   summary.queue_full ? ", recovery window saturated" : "");
  } else {
    (void)snprintf(state->status_message, sizeof(state->status_message), "Project opened: %s",
                   state->project_name);
  }
  return true;
}

void lardon3d_project_close(Lardon3DAppState *state) {
  if (!state) {
    return;
  }

  if (!state->project_loaded) {
    set_status(state, "No project to close.");
    return;
  }

  clear_catalog(state);
  lardon3d_project_db_close(state->project_db);
  state->project_db = NULL;
  state->project_loaded = false;
  state->project_name[0] = '\0';
  state->project_path[0] = '\0';
  state->project_stable_id[0] = '\0';
  store_recovery_summary(state, NULL);
  set_status(state, "Project closed.");
}

static bool checkpoint_paths(const Lardon3DAppState *state, uint64_t task_id,
                             char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY],
                             char absolute[PATH_MAX]) {
  int written = snprintf(relative, LARDON3D_PROJECT_DB_PATH_CAPACITY,
                         ".lardon3d/checkpoints/%" PRIu64 ".chk", task_id);
  return written > 0 && (size_t)written < LARDON3D_PROJECT_DB_PATH_CAPACITY &&
         join_path(absolute, PATH_MAX, state->project_path, relative);
}

static int checkpoint_lock_acquire(const char *checkpoint_path) {
  char lock_path[PATH_MAX];
  int written = snprintf(lock_path, sizeof(lock_path), "%s.lock", checkpoint_path);
  if (written < 0 || (size_t)written >= sizeof(lock_path)) {
    return -1;
  }
  int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX) != 0) {
    if (lock >= 0) {
      (void)close(lock);
    }
    return -1;
  }
  return lock;
}

#ifdef LARDON3D_CHECKPOINT_TESTING
static void checkpoint_test_rendezvous(const char *variable) {
  const char *value = getenv(variable);
  if (!value || !value[0]) {
    return;
  }
  char *end = NULL;
  errno = 0;
  long descriptor = strtol(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || descriptor < 0 || descriptor > INT_MAX) {
    return;
  }
  unsigned char token = 1;
  if (write((int)descriptor, &token, sizeof(token)) == (ssize_t)sizeof(token)) {
    (void)read((int)descriptor, &token, sizeof(token));
  }
}
#endif

static Lardon3DProjectTaskCheckpointResult
checkpoint_task_internal(Lardon3DAppState *state, const Lardon3DTask *task,
                         const char *image_import_source, uint64_t image_import_scanset_id,
                         const Lardon3DProjectDbFeatureExtractTask *feature_parameters,
                         const Lardon3DProjectDbSiftExtractTask *sift_parameters,
                         const Lardon3DProjectDbVisualIndexUpdateTask *visual_parameters,
                         const Lardon3DProjectDbCandidatePairGenerateTask *candidate_parameters,
                         const Lardon3DProjectDbMatcherTask *matcher_parameters,
                         const Lardon3DProjectDbGeometricVerifierTask *geometric_parameters,
                         const Lardon3DProjectDbAcquisitionCampaignTask *campaign_parameters,
                         const Lardon3DProjectDbPhotoQualityTask *quality_parameters,
                         const Lardon3DProjectDbRawDevelopmentTask *raw_parameters,
                         const Lardon3DProjectDbRawDevelopmentBatchTask *raw_batch_parameters,
                         const Lardon3DProjectDbFeatureExtractBatchTask *feature_batch_parameters) {
  if (!state || !state->project_loaded || !state->project_db) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_NO_PROJECT;
  }
  Lardon3DTaskDurableSnapshot snapshot;
  if (!lardon3d_task_durable_snapshot(task, &snapshot) || snapshot.id == 0) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  char task_kind[LARDON3D_TASK_KIND_CAPACITY];
  uint32_t task_kind_version = 0;
  if (!lardon3d_task_kind(task, task_kind, &task_kind_version)) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  char absolute[PATH_MAX];
  if (!checkpoint_paths(state, snapshot.id, relative, absolute)) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_IO_ERROR;
  }
  /* Serialize the fixed .next slot per task through stage, DB commit, and
   * promotion.  The advisory lock is process-owned and is released by kernel
   * close on crash, so it cannot become a durable recovery dependency. */
  int checkpoint_lock = checkpoint_lock_acquire(absolute);
  if (checkpoint_lock < 0) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_IO_ERROR;
  }
#define CHECKPOINT_RETURN(value) do { (void)close(checkpoint_lock); return (value); } while (0)
  /* DB and filesystem durability are independent: changing their write order
   * cannot make S1 atomic.  Keep .next durable across the DB commit window so
   * recovery always has an explicitly published representation to compare. */
  Lardon3DTaskCheckpointResult saved = lardon3d_task_checkpoint_stage(absolute, &snapshot);
  if (saved == LARDON3D_TASK_CHECKPOINT_INVALID) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK);
  }
  /* S1 must be directory-durable before SQLite may publish S1.  A staged
   * namespace with an uncertain parent-directory sync is not a safe recovery
   * representation, even when its file descriptor itself was synced. */
  if (saved == LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE);
  }
  if (saved != LARDON3D_TASK_CHECKPOINT_OK) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_IO_ERROR);
  }
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_DB_ERROR);
  }
  Lardon3DProjectDbCheckpoint checkpoint = {
      .format_version = LARDON3D_TASK_CHECKPOINT_VERSION,
      /* A fully synced .next is the durable checkpoint representation during
       * the DB-to-canonical handoff, even though checkpoint.path names the
       * legacy canonical location for v22 compatibility. */
      .durability = LARDON3D_DB_CHECKPOINT_DURABLE,
      .updated_at = now.tv_sec,
  };
  (void)snprintf(checkpoint.path, sizeof(checkpoint.path), "%s", relative);
  Lardon3DProjectDbResult recorded =
      image_import_source
          ? lardon3d_project_db_record_image_import_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                image_import_source, image_import_scanset_id, now.tv_sec)
      : feature_parameters
          ? lardon3d_project_db_record_feature_extract_task(state->project_db, &snapshot, task_kind,
                                                            task_kind_version, &checkpoint,
                                                            feature_parameters, now.tv_sec)
      : sift_parameters
          ? lardon3d_project_db_record_sift_extract_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                sift_parameters, now.tv_sec)
      : visual_parameters
          ? lardon3d_project_db_record_visual_index_update_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                visual_parameters, now.tv_sec)
      : candidate_parameters
          ? lardon3d_project_db_record_candidate_pair_generate_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                candidate_parameters, now.tv_sec)
      : matcher_parameters
          ? lardon3d_project_db_record_matcher_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                matcher_parameters, now.tv_sec)
      : geometric_parameters
          ? lardon3d_project_db_record_geometric_verifier_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                geometric_parameters, now.tv_sec)
      : campaign_parameters
          ? lardon3d_project_db_record_acquisition_campaign_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                campaign_parameters, now.tv_sec)
      : quality_parameters
          ? lardon3d_project_db_record_photo_quality_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                quality_parameters, now.tv_sec)
      : raw_parameters
          ? lardon3d_project_db_record_raw_development_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                raw_parameters, now.tv_sec)
      : raw_batch_parameters
          ? lardon3d_project_db_record_raw_development_batch_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                raw_batch_parameters, now.tv_sec)
      : feature_batch_parameters
          ? lardon3d_project_db_record_feature_extract_batch_task(
                state->project_db, &snapshot, task_kind, task_kind_version, &checkpoint,
                feature_batch_parameters, now.tv_sec)
          : lardon3d_project_db_record_task(state->project_db, &snapshot, task_kind,
                                            task_kind_version, &checkpoint, now.tv_sec);
  if (recorded == LARDON3D_PROJECT_DB_BUSY) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_DB_BUSY);
  }
  if (recorded != LARDON3D_PROJECT_DB_OK) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_DB_ERROR);
  }
#ifdef LARDON3D_CHECKPOINT_TESTING
  /* Deterministic crash boundary: production recovery must select .next after
   * the SQLite FULL transaction is durable but before canonical promotion. */
  const char *stop_after_db_commit = getenv("LARDON3D_TEST_PROJECT_CHECKPOINT_AFTER_DB_COMMIT");
  if (stop_after_db_commit && strcmp(stop_after_db_commit, "1") == 0) {
    CHECKPOINT_RETURN(LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE);
  }
#endif
  Lardon3DTaskCheckpointResult promoted = lardon3d_task_checkpoint_promote_staged(absolute);
  Lardon3DProjectTaskCheckpointResult result = promoted == LARDON3D_TASK_CHECKPOINT_OK
             ? LARDON3D_PROJECT_TASK_CHECKPOINT_OK
             : LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE;
#undef CHECKPOINT_RETURN
  (void)close(checkpoint_lock);
  return result;
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_task(Lardon3DAppState *state,
                                                                     const Lardon3DTask *task) {
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult
lardon3d_project_checkpoint_image_import_task(Lardon3DAppState *state, const Lardon3DTask *task,
                                              const char *source_path, uint64_t scanset_id) {
  if (!source_path || !source_path[0] || scanset_id == 0) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  return checkpoint_task_internal(
      state, task, source_path, scanset_id, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_acquisition_campaign_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbAcquisitionCampaignTask *parameters) {
  if (!parameters) return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL,
                                  parameters, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_photo_quality_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbPhotoQualityTask *parameters) {
  if (!parameters) return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                  parameters, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_raw_development_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbRawDevelopmentTask *parameters) {
  if (!parameters) return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                  NULL, parameters, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_feature_extract_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbFeatureExtractTask *parameters) {
  if (!parameters) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  return checkpoint_task_internal(state, task, NULL, 0, parameters, NULL, NULL, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_sift_extract_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbSiftExtractTask *parameters) {
  if (!parameters) return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  return checkpoint_task_internal(state, task, NULL, 0, NULL, parameters, NULL, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_visual_index_update_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbVisualIndexUpdateTask *parameters) {
  if (!parameters) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, parameters, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_candidate_pair_generate_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbCandidatePairGenerateTask *parameters) {
  if (!parameters) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, parameters, NULL, NULL,
                                  NULL, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_matcher_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbMatcherTask *parameters) {
  if (!parameters) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, parameters, NULL,
                                  NULL, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_geometric_verifier_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbGeometricVerifierTask *parameters) {
  if (!parameters) {
    return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  }
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL,
                                  parameters, NULL, NULL, NULL, NULL, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_raw_development_batch_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbRawDevelopmentBatchTask *parameters) {
  if (!parameters) return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                  NULL, NULL, parameters, NULL);
}

Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_feature_extract_batch_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbFeatureExtractBatchTask *parameters) {
  if (!parameters) return LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK;
  return checkpoint_task_internal(state, task, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                  NULL, NULL, NULL, parameters);
}

static bool coherent_recovery(const Lardon3DProjectDbTask *database_task,
                              const Lardon3DTaskDurableSnapshot *snapshot) {
  return snapshot->id == database_task->task_id &&
         strcmp(snapshot->name, database_task->name) == 0 &&
         snapshot->saved_state == database_task->saved_state &&
         snapshot->recovery_state == database_task->recovery_state &&
         snapshot->progress == database_task->progress &&
         snapshot->sequence_count == database_task->sequence_count;
}

Lardon3DProjectDbResult lardon3d_project_list_recoverable(Lardon3DAppState *state,
                                                          const Lardon3DTaskKindRegistry *registry,
                                                          uint64_t after_task_id,
                                                          Lardon3DProjectRecoveryEntry *entries,
                                                          size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!state || !state->project_loaded || !state->project_db || !registry || !entries || !count ||
      capacity == 0 || capacity > LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  enum { RECOVERY_CHUNK = 8 };
  Lardon3DProjectDbTask tasks[RECOVERY_CHUNK];
  uint64_t cursor = after_task_id;
  while (*count < capacity) {
    size_t requested = capacity - *count;
    if (requested > RECOVERY_CHUNK) {
      requested = RECOVERY_CHUNK;
    }
    size_t task_count = 0;
    Lardon3DProjectDbResult result = lardon3d_project_db_list_recoverable(
        state->project_db, cursor, tasks, requested, &task_count);
    if (result != LARDON3D_PROJECT_DB_OK) {
      return result;
    }
    for (size_t index = 0; index < task_count; ++index) {
      Lardon3DProjectRecoveryEntry *entry = &entries[*count];
      memset(entry, 0, sizeof(*entry));
      entry->task_id = tasks[index].task_id;
      entry->durability = tasks[index].checkpoint.durability;
      (void)snprintf(entry->name, sizeof(entry->name), "%s", tasks[index].name);
      if (tasks[index].has_task_kind) {
        (void)snprintf(entry->task_kind, sizeof(entry->task_kind), "%s", tasks[index].task_kind);
        entry->task_kind_version = tasks[index].task_kind_version;
      }
      char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
      char absolute[PATH_MAX];
      const Lardon3DTaskKindDescriptor *descriptor = NULL;
      Lardon3DTaskKindResult kind_result =
          tasks[index].has_task_kind
              ? lardon3d_task_kind_registry_lookup(registry, tasks[index].task_kind,
                                                   tasks[index].task_kind_version, &descriptor)
              : LARDON3D_TASK_KIND_UNKNOWN;
      if (!tasks[index].has_task_kind) {
        entry->status = LARDON3D_PROJECT_RECOVERY_LEGACY_UNTYPED;
      } else if (kind_result == LARDON3D_TASK_KIND_UNKNOWN) {
        entry->status = LARDON3D_PROJECT_RECOVERY_UNKNOWN_TASK_KIND;
      } else if (kind_result == LARDON3D_TASK_KIND_UNSUPPORTED_VERSION) {
        entry->status = LARDON3D_PROJECT_RECOVERY_UNSUPPORTED_TASK_KIND_VERSION;
      } else if (kind_result != LARDON3D_TASK_KIND_OK) {
        entry->status = LARDON3D_PROJECT_RECOVERY_UNKNOWN_TASK_KIND;
      } else if (!checkpoint_paths(state, entry->task_id, relative, absolute) ||
                 strcmp(relative, tasks[index].checkpoint.path) != 0) {
        entry->status = LARDON3D_PROJECT_RECOVERY_INVALID_CHECKPOINT;
      } else {
        /* Writer and recovery take this lock before touching SQLite.  Recovery
         * must hold it across staged validation and promotion: otherwise a
         * writer could replace .next after it matched the DB-stored task
         * summary fields.
         * Reload the DB row only after acquiring the lock, because the paged
         * row above may have become stale while recovery waited for a writer. */
#ifdef LARDON3D_CHECKPOINT_TESTING
        /* Test-only rendezvous proves that the paged row is never used after
         * another writer advances it before recovery obtains .chk.lock. */
        checkpoint_test_rendezvous("LARDON3D_TEST_RECOVERY_BEFORE_CHECKPOINT_LOCK_FD");
#endif
        int checkpoint_lock = checkpoint_lock_acquire(absolute);
        if (checkpoint_lock < 0) {
          entry->status = LARDON3D_PROJECT_RECOVERY_CHECKPOINT_IO_ERROR;
          cursor = entry->task_id;
          ++*count;
          continue;
        }
        Lardon3DProjectDbTask durable_task;
        Lardon3DProjectDbResult loaded_task =
            lardon3d_project_db_load_task(state->project_db, entry->task_id, &durable_task);
        if (loaded_task != LARDON3D_PROJECT_DB_OK) {
          (void)close(checkpoint_lock);
          return loaded_task;
        }
        /* Pagination is only a candidate discovery mechanism. A writer may
         * have made this task terminal while recovery waited, so do not emit a
         * recoverable entry unless the locked, authoritative row is pending. */
        if (durable_task.recovery_state != TASK_PENDING || !durable_task.has_checkpoint ||
            !durable_task.has_task_kind || strcmp(relative, durable_task.checkpoint.path) != 0) {
          (void)close(checkpoint_lock);
          cursor = entry->task_id;
          continue;
        }
        entry->durability = durable_task.checkpoint.durability;
        (void)snprintf(entry->name, sizeof(entry->name), "%s", durable_task.name);
        if (durable_task.has_task_kind) {
          (void)snprintf(entry->task_kind, sizeof(entry->task_kind), "%s", durable_task.task_kind);
          entry->task_kind_version = durable_task.task_kind_version;
        }
        uint32_t version = 0;
        Lardon3DTaskCheckpointResult promotion = LARDON3D_TASK_CHECKPOINT_OK;
        /* A codec-valid canonical checkpoint whose DB-stored task-summary
         * fields match wins. A stale or corrupt .next is ignored in that
         * case; the DB deliberately does not duplicate the full snapshot. */
        Lardon3DTaskCheckpointResult loaded =
            lardon3d_task_checkpoint_load(absolute, &entry->snapshot, &version);
        bool canonical_match = loaded == LARDON3D_TASK_CHECKPOINT_OK
            && version == durable_task.checkpoint.format_version
            && coherent_recovery(&durable_task, &entry->snapshot);
        if (!canonical_match) {
          Lardon3DTaskDurableSnapshot staged_snapshot;
          uint32_t staged_version = 0;
          Lardon3DTaskCheckpointResult staged = lardon3d_task_checkpoint_load_staged(
              absolute, &staged_snapshot, &staged_version);
          if (staged == LARDON3D_TASK_CHECKPOINT_OK
              && staged_version == durable_task.checkpoint.format_version
              && coherent_recovery(&durable_task, &staged_snapshot)) {
            entry->snapshot = staged_snapshot;
#ifdef LARDON3D_CHECKPOINT_TESTING
            checkpoint_test_rendezvous("LARDON3D_TEST_RECOVERY_AFTER_STAGED_MATCH_FD");
#endif
            /* The same lock protects this re-read/promote helper from a
             * replacement of .next between DB-summary validation and rename. */
            promotion = lardon3d_task_checkpoint_promote_staged(absolute);
            if (promotion == LARDON3D_TASK_CHECKPOINT_OK ||
                promotion == LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE) {
              loaded = LARDON3D_TASK_CHECKPOINT_OK;
              version = staged_version;
              canonical_match = true;
            }
          }
        }
        if (!canonical_match && loaded == LARDON3D_TASK_CHECKPOINT_NOT_FOUND) {
          entry->status = LARDON3D_PROJECT_RECOVERY_MISSING_CHECKPOINT;
        } else if (!canonical_match && loaded == LARDON3D_TASK_CHECKPOINT_UNSUPPORTED_VERSION) {
          entry->status = LARDON3D_PROJECT_RECOVERY_UNSUPPORTED_CHECKPOINT;
        } else if (!canonical_match && loaded == LARDON3D_TASK_CHECKPOINT_INVALID) {
          entry->status = LARDON3D_PROJECT_RECOVERY_INVALID_CHECKPOINT;
        } else if (!canonical_match) {
          entry->status = loaded == LARDON3D_TASK_CHECKPOINT_IO_ERROR
                              ? LARDON3D_PROJECT_RECOVERY_CHECKPOINT_IO_ERROR
                              : LARDON3D_PROJECT_RECOVERY_INVALID_CHECKPOINT;
        } else {
          entry->status = promotion == LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE ||
                                  entry->durability == LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE
                              ? LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE
                              : LARDON3D_PROJECT_RECOVERABLE;
        }
        (void)close(checkpoint_lock);
      }
      cursor = entry->task_id;
      ++*count;
    }
    if (task_count < requested) {
      break;
    }
  }
  return LARDON3D_PROJECT_DB_OK;
}

Lardon3DProjectDbResult
lardon3d_project_resume_recoverable_tasks(Lardon3DAppState *state,
                                          const Lardon3DTaskKindRegistry *registry,
                                          Lardon3DProjectRecoverySummary *summary) {
  if (summary) {
    memset(summary, 0, sizeof(*summary));
  }
  if (!state || !state->project_loaded || !state->project_db || !state->task_queue ||
      !state->resource_governor || !registry || !summary) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  enum { RECOVERY_PAGE_SIZE = 8 };
  uint64_t cursor = 0;
  Lardon3DProjectDbResult result = LARDON3D_PROJECT_DB_OK;
  bool stop = false;
  do {
    Lardon3DProjectRecoveryEntry entries[RECOVERY_PAGE_SIZE];
    size_t count = 0;
    result = lardon3d_project_list_recoverable(state, registry, cursor, entries, RECOVERY_PAGE_SIZE,
                                               &count);
    if (result != LARDON3D_PROJECT_DB_OK) {
      ++summary->failed;
      break;
    }
    for (size_t index = 0; index < count; ++index) {
      Lardon3DProjectRecoveryEntry *entry = &entries[index];
      cursor = entry->task_id;
      ++summary->inspected;
      bool recoverable = entry->status == LARDON3D_PROJECT_RECOVERABLE ||
                         entry->status == LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE;
      if (!recoverable || entry->snapshot.recovery_state != TASK_PENDING) {
        ++summary->skipped;
        continue;
      }
      if (entry->status == LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE) {
        ++summary->published_not_durable;
      }
      Lardon3DTaskSnapshot existing;
      if (lardon3d_task_queue_get(state->task_queue, entry->task_id, &existing)) {
        ++summary->skipped;
        continue;
      }
      Lardon3DImageImportReconstructionContext context = {
          .project_path = state->project_path,
          .project_db = state->project_db,
          .resource_governor = state->resource_governor,
          .orb_vulkan_backend = state->orb_vulkan_backend,
      };
      Lardon3DTask *task = NULL;
      Lardon3DTaskKindResult restored = lardon3d_task_kind_registry_restore(
          registry, entry->task_kind, entry->task_kind_version, &entry->snapshot, &context, &task);
      if (restored != LARDON3D_TASK_KIND_OK || !task) {
        ++summary->failed;
        continue;
      }
      Lardon3DTaskQueueAddResult added =
          lardon3d_task_queue_try_add_ex(state->task_queue, task, NULL);
      if (added == LARDON3D_TASK_QUEUE_ADD_OK) {
        ++summary->resumed;
        continue;
      }
      lardon3d_task_destroy(task);
      if (added == LARDON3D_TASK_QUEUE_ADD_DUPLICATE_ID) {
        ++summary->skipped;
      } else if (added == LARDON3D_TASK_QUEUE_ADD_FULL) {
        ++summary->skipped;
      } else {
        ++summary->failed;
      }
      if (added == LARDON3D_TASK_QUEUE_ADD_FULL || added == LARDON3D_TASK_QUEUE_ADD_STOPPING) {
        summary->queue_full = added == LARDON3D_TASK_QUEUE_ADD_FULL;
        stop = true;
        break;
      }
    }
    if (stop || count < RECOVERY_PAGE_SIZE) {
      break;
    }
  } while (cursor > 0);
  store_recovery_summary(state, summary);
  return result;
}

bool lardon3d_project_last_recovery_summary(const Lardon3DAppState *state,
                                            Lardon3DProjectRecoverySummary *summary) {
  if (!state || !summary) {
    return false;
  }
  *summary = (Lardon3DProjectRecoverySummary){
      .inspected = state->recovery_inspected,
      .resumed = state->recovery_resumed,
      .skipped = state->recovery_skipped,
      .failed = state->recovery_failed,
      .published_not_durable = state->recovery_published_not_durable,
      .queue_full = state->recovery_queue_full,
  };
  return true;
}
