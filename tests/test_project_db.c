#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/project_db.h>

#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); return false; \
} } while (0)

Lardon3DProjectDbResult lardon3d_project_db_test_orphan_checkpoint(Lardon3DProjectDb *database);

static Lardon3DTaskDurableSnapshot
task_snapshot(uint64_t id, Lardon3DTaskState saved)
{
    Lardon3DTaskDurableSnapshot snapshot = {
        .id = id,
        .estimate = {.minimum_batch_size = 1, .maximum_batch_size = 1, .desired_cpu_threads = 1},
        .progress = saved == TASK_COMPLETED ? 100 : 25,
        .saved_state = saved,
        .recovery_state = saved == TASK_RUNNING || saved == TASK_PAUSED ? TASK_PENDING : saved,
        .started_at = {.tv_sec = 10, .tv_nsec = 20},
        .finished_at = {.tv_sec = 30, .tv_nsec = 40},
        .sequence_count = 2,
    };
    (void)snprintf(snapshot.name, sizeof(snapshot.name), "Tâche %llu", (unsigned long long)id);
    return snapshot;
}

typedef struct { Lardon3DProjectDb *database; bool success; } ThreadContext;

static void *
read_thread(void *userdata)
{
    ThreadContext *context = userdata;
    context->success = true;
    for (size_t index = 0; index < 100; ++index) {
        Lardon3DProjectDbTask task;
        if (lardon3d_project_db_load_task(context->database, 1, &task) != LARDON3D_PROJECT_DB_OK) {
            context->success = false;
            break;
        }
    }
    return NULL;
}

static bool
create_future_database(const char *path)
{
    sqlite3 *connection = NULL;
    if (sqlite3_open(path, &connection) != SQLITE_OK) return false;
    bool ok = sqlite3_exec(connection, "CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
        "INSERT INTO metadata VALUES('schema_version',3);", NULL, NULL, NULL) == SQLITE_OK;
    return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool
create_v1_database(const char *path)
{
    static const char sql[] =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
        "INSERT INTO metadata VALUES('schema_version',1);"
        "CREATE TABLE project(singleton INTEGER PRIMARY KEY CHECK(singleton=1),stable_id TEXT NOT NULL UNIQUE,name TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
        "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY CHECK(task_id>0),name TEXT NOT NULL,saved_state INTEGER NOT NULL CHECK(saved_state BETWEEN 0 AND 5),recovery_state INTEGER NOT NULL CHECK(recovery_state BETWEEN 0 AND 5),progress INTEGER NOT NULL CHECK(progress BETWEEN 0 AND 100),sequence_count INTEGER NOT NULL CHECK(sequence_count>=0),started_sec INTEGER NOT NULL,started_nsec INTEGER NOT NULL CHECK(started_nsec BETWEEN 0 AND 999999999),finished_sec INTEGER NOT NULL,finished_nsec INTEGER NOT NULL CHECK(finished_nsec BETWEEN 0 AND 999999999),updated_at INTEGER NOT NULL);"
        "CREATE INDEX tasks_recovery_state_idx ON tasks(recovery_state,task_id);"
        "CREATE TABLE checkpoints(task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,path TEXT NOT NULL,format_version INTEGER NOT NULL CHECK(format_version>0),durability INTEGER NOT NULL CHECK(durability BETWEEN 0 AND 1),updated_at INTEGER NOT NULL);"
        "CREATE TABLE artifacts(artifact_id TEXT PRIMARY KEY,kind TEXT NOT NULL,path TEXT NOT NULL,state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 1),size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),producer_task_id INTEGER REFERENCES tasks(task_id),created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
        "CREATE INDEX artifacts_state_idx ON artifacts(state,artifact_id);"
        "CREATE INDEX artifacts_producer_idx ON artifacts(producer_task_id);"
        "INSERT INTO project VALUES(1,'legacy-project','Legacy',1,1);"
        "INSERT INTO tasks VALUES(9,'Legacy task',1,0,12,3,1,0,0,0,2);"
        "INSERT INTO checkpoints VALUES(9,'legacy.chk',1,0,2);"
        "INSERT INTO artifacts VALUES('legacy-artifact','legacy','legacy.bin',0,0,9,2,2);";
    sqlite3 *connection = NULL;
    if (sqlite3_open(path, &connection) != SQLITE_OK) return false;
    bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
    return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool
query_integer(const char *path, const char *sql, sqlite3_int64 expected)
{
    sqlite3 *connection = NULL;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK
        || sqlite3_prepare_v2(connection, sql, -1, &statement, NULL) != SQLITE_OK) {
        if (statement) (void)sqlite3_finalize(statement);
        if (connection) (void)sqlite3_close(connection);
        return false;
    }
    bool matches = sqlite3_step(statement) == SQLITE_ROW
        && sqlite3_column_int64(statement, 0) == expected
        && sqlite3_step(statement) == SQLITE_DONE;
    return sqlite3_finalize(statement) == SQLITE_OK
        && sqlite3_close(connection) == SQLITE_OK && matches;
}

static bool
run_test(void)
{
    char directory[] = "/tmp/lardon3d-project-db-XXXXXX";
    CHECK(mkdtemp(directory));
    char database_path[512], artifact_path[512], future_path[512], corrupt_path[512];
    char legacy_path[512], failed_migration_path[512];
    CHECK(snprintf(database_path, sizeof(database_path), "%s/project.db", directory) > 0);
    CHECK(snprintf(artifact_path, sizeof(artifact_path), "%s/artifact.bin", directory) > 0);
    CHECK(snprintf(future_path, sizeof(future_path), "%s/future.db", directory) > 0);
    CHECK(snprintf(corrupt_path, sizeof(corrupt_path), "%s/corrupt.db", directory) > 0);
    CHECK(snprintf(legacy_path, sizeof(legacy_path), "%s/legacy.db", directory) > 0);
    CHECK(snprintf(failed_migration_path, sizeof(failed_migration_path), "%s/failed-migration.db", directory) > 0);

    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    Lardon3DProjectDb *database = NULL;
    CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
    CHECK(database && lardon3d_project_db_schema_version(database) == 2);

    Lardon3DProjectDbProject project = {.created_at = 100, .updated_at = 100};
    (void)snprintf(project.stable_id, sizeof(project.stable_id), "project-0001");
    (void)snprintf(project.name, sizeof(project.name), "Projet test");
    CHECK(lardon3d_project_db_set_project(database, &project) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbProject loaded_project;
    CHECK(lardon3d_project_db_get_project(database, &loaded_project) == LARDON3D_PROJECT_DB_OK);
    CHECK(strcmp(loaded_project.stable_id, project.stable_id) == 0);
    project.updated_at = 101; (void)snprintf(project.name, sizeof(project.name), "Projet renommé");
    CHECK(lardon3d_project_db_set_project(database, &project) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbProject conflict = project; (void)snprintf(conflict.stable_id, sizeof(conflict.stable_id), "other");
    CHECK(lardon3d_project_db_set_project(database, &conflict) == LARDON3D_PROJECT_DB_CONSTRAINT);
    char copied_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    CHECK(lardon3d_project_db_last_error(database, copied_error));
    CHECK(copied_error[0] != '\0');
    project = loaded_project; memset(project.name, 'x', sizeof(project.name));
    CHECK(lardon3d_project_db_set_project(database, &project) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

    Lardon3DTaskDurableSnapshot running = task_snapshot(1, TASK_RUNNING);
    Lardon3DProjectDbCheckpoint checkpoint = {.format_version = 1, .durability = LARDON3D_DB_CHECKPOINT_DURABLE, .updated_at = 200};
    (void)snprintf(checkpoint.path, sizeof(checkpoint.path), "%s/checkpoints/task-1.chk", directory);
    CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 1, &checkpoint, 200) == LARDON3D_PROJECT_DB_OK);
    running.progress = 30; running.sequence_count = 3;
    CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 1, &checkpoint, 201) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbTask task;
    CHECK(lardon3d_project_db_load_task(database, 1, &task) == LARDON3D_PROJECT_DB_OK);
    CHECK(task.recovery_state == TASK_PENDING && task.progress == 30 && task.sequence_count == 3);
    CHECK(task.has_task_kind && strcmp(task.task_kind, "test.work") == 0
        && task.task_kind_version == 1);
    CHECK(task.has_checkpoint && strcmp(task.checkpoint.path, checkpoint.path) == 0);
    checkpoint.durability = LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE;
    CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 1, &checkpoint, 201) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_load_task(database, 1, &task) == LARDON3D_PROJECT_DB_OK);
    CHECK(task.checkpoint.durability == LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE);
    CHECK(lardon3d_project_db_record_task(database, &running, "test.other", 1,
        &checkpoint, 202) == LARDON3D_PROJECT_DB_CONSTRAINT);
    CHECK(lardon3d_project_db_record_task(database, &running, "Test.invalid", 1,
        &checkpoint, 202) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
    CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 0,
        &checkpoint, 202) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

    Lardon3DTaskDurableSnapshot completed = task_snapshot(2, TASK_COMPLETED);
    CHECK(lardon3d_project_db_record_task(database, &completed, "test.work", 1, NULL, 202) == LARDON3D_PROJECT_DB_OK);
    Lardon3DTaskDurableSnapshot no_checkpoint = task_snapshot(4, TASK_PENDING);
    CHECK(lardon3d_project_db_record_task(database, &no_checkpoint, NULL, 0, NULL, 202) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbTask page[1]; size_t count = 0;
    CHECK(lardon3d_project_db_list_recoverable(database, 0, page, 1, &count) == LARDON3D_PROJECT_DB_OK);
    CHECK(count == 1 && page[0].task_id == 1);
    CHECK(lardon3d_project_db_list_recoverable(database, 1, page, 1, &count) == LARDON3D_PROJECT_DB_OK && count == 0);
    CHECK(lardon3d_project_db_list_recoverable(database, 0, page, LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX + 1, &count) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

    Lardon3DTaskDurableSnapshot rollback_task = task_snapshot(3, TASK_PENDING);
    CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT", "1", 1) == 0);
    CHECK(lardon3d_project_db_record_task(database, &rollback_task, "test.work", 1, &checkpoint, 203) == LARDON3D_PROJECT_DB_IO_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT") == 0);
    CHECK(lardon3d_project_db_load_task(database, 3, &task) == LARDON3D_PROJECT_DB_NOT_FOUND);
    CHECK(lardon3d_project_db_test_orphan_checkpoint(database) == LARDON3D_PROJECT_DB_CONSTRAINT);

    Lardon3DProjectDbArtifact artifact = {.state = LARDON3D_DB_ARTIFACT_STAGED, .has_producer_task = true, .producer_task_id = 1, .created_at = 300, .updated_at = 300};
    (void)snprintf(artifact.artifact_id, sizeof(artifact.artifact_id), "artifact-1");
    (void)snprintf(artifact.kind, sizeof(artifact.kind), "generic-test");
    (void)snprintf(artifact.path, sizeof(artifact.path), "%s", artifact_path);
    CHECK(lardon3d_project_db_create_artifact(database, &artifact) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbArtifact loaded_artifact;
    CHECK(lardon3d_project_db_load_artifact(database, artifact.artifact_id, &loaded_artifact) == LARDON3D_PROJECT_DB_OK);
    CHECK(loaded_artifact.state == LARDON3D_DB_ARTIFACT_STAGED);
    CHECK(lardon3d_project_db_mark_artifact_ready(database, artifact.artifact_id, 301) == LARDON3D_PROJECT_DB_IO_ERROR);
    int descriptor = open(artifact_path, O_WRONLY | O_CREAT | O_TRUNC, 0600); CHECK(descriptor >= 0);
    const char payload[] = "validated"; CHECK(write(descriptor, payload, sizeof(payload)) == (ssize_t)sizeof(payload)); CHECK(close(descriptor) == 0);
    CHECK(lardon3d_project_db_mark_artifact_ready(database, artifact.artifact_id, 301) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_load_artifact(database, artifact.artifact_id, &loaded_artifact) == LARDON3D_PROJECT_DB_OK);
    CHECK(loaded_artifact.state == LARDON3D_DB_ARTIFACT_READY && loaded_artifact.size_bytes == sizeof(payload));
    CHECK(lardon3d_project_db_create_artifact(database, &artifact) == LARDON3D_PROJECT_DB_CONSTRAINT);
    artifact.producer_task_id = 999;
    (void)snprintf(artifact.artifact_id, sizeof(artifact.artifact_id), "orphan-artifact");
    CHECK(lardon3d_project_db_create_artifact(database, &artifact) == LARDON3D_PROJECT_DB_CONSTRAINT);
    artifact = loaded_artifact; artifact.state = LARDON3D_DB_ARTIFACT_STAGED; artifact.size_bytes = 0;
    memset(artifact.path, 'x', sizeof(artifact.path));
    CHECK(lardon3d_project_db_create_artifact(database, &artifact) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

    ThreadContext contexts[2] = {{.database = database}, {.database = database}}; pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, read_thread, &contexts[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, read_thread, &contexts[1]) == 0);
    CHECK(pthread_join(threads[0], NULL) == 0 && pthread_join(threads[1], NULL) == 0);
    CHECK(contexts[0].success && contexts[1].success);

    char too_long[LARDON3D_PROJECT_DB_PATH_CAPACITY + 1]; memset(too_long, 'x', sizeof(too_long)); too_long[sizeof(too_long) - 1] = '\0';
    CHECK(lardon3d_project_db_open(too_long, &database, error) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
    lardon3d_project_db_close(contexts[0].database); database = NULL;
    CHECK(query_integer(database_path, "SELECT value FROM metadata WHERE key='schema_version'", 2));
    CHECK(query_integer(database_path, "SELECT count(*) FROM tasks WHERE task_id=1", 1));
    CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_load_task(database, 1, &task) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_load_artifact(database, "artifact-1", &loaded_artifact) == LARDON3D_PROJECT_DB_OK);
    lardon3d_project_db_close(database);

    CHECK(create_future_database(future_path));
    CHECK(lardon3d_project_db_open(future_path, &database, error) == LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA);
    descriptor = open(corrupt_path, O_WRONLY | O_CREAT | O_TRUNC, 0600); CHECK(descriptor >= 0);
    CHECK(write(descriptor, "not sqlite", 10) == 10 && close(descriptor) == 0);
    CHECK(lardon3d_project_db_open(corrupt_path, &database, error) == LARDON3D_PROJECT_DB_CORRUPT);

    CHECK(create_v1_database(legacy_path));
    CHECK(lardon3d_project_db_open(legacy_path, &database, error) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_schema_version(database) == 2);
    CHECK(lardon3d_project_db_get_project(database, &loaded_project) == LARDON3D_PROJECT_DB_OK
        && strcmp(loaded_project.stable_id, "legacy-project") == 0);
    CHECK(lardon3d_project_db_load_task(database, 9, &task) == LARDON3D_PROJECT_DB_OK);
    CHECK(!task.has_task_kind && task.has_checkpoint
        && strcmp(task.checkpoint.path, "legacy.chk") == 0);
    CHECK(lardon3d_project_db_load_artifact(database, "legacy-artifact",
        &loaded_artifact) == LARDON3D_PROJECT_DB_OK);
    lardon3d_project_db_close(database); database = NULL;
    CHECK(query_integer(legacy_path, "SELECT value FROM metadata WHERE key='schema_version'", 2));

    CHECK(create_v1_database(failed_migration_path));
    CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V2", "1", 1) == 0);
    CHECK(lardon3d_project_db_open(failed_migration_path, &database, error)
        == LARDON3D_PROJECT_DB_IO_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V2") == 0);
    CHECK(query_integer(failed_migration_path,
        "SELECT value FROM metadata WHERE key='schema_version'", 1));
    CHECK(lardon3d_project_db_open(failed_migration_path, &database, error)
        == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_project_db_load_task(database, 9, &task) == LARDON3D_PROJECT_DB_OK
        && !task.has_task_kind);
    lardon3d_project_db_close(database); database = NULL;

    CHECK(unlink(artifact_path) == 0); CHECK(unlink(database_path) == 0); CHECK(unlink(future_path) == 0); CHECK(unlink(corrupt_path) == 0);
    CHECK(unlink(legacy_path) == 0); CHECK(unlink(failed_migration_path) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
