#ifndef LARDON3D_PROJECT_DB_INTERNAL_H
#define LARDON3D_PROJECT_DB_INTERNAL_H

#include <pthread.h>
#include <sqlite3.h>

#include <lardon3d/project_db.h>

struct Lardon3DProjectDb {
  sqlite3 *connection;
  pthread_mutex_t mutex;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
};

Lardon3DProjectDbResult sqlite_result(Lardon3DProjectDb *database, int code,
                                      const char *context);
Lardon3DProjectDbResult execute(Lardon3DProjectDb *database, const char *sql,
                                const char *context);
Lardon3DProjectDbResult prepare(Lardon3DProjectDb *database, const char *sql,
                                sqlite3_stmt **statement);
Lardon3DProjectDbResult step_done(Lardon3DProjectDb *database,
                                  sqlite3_stmt *statement, const char *context);

#endif
