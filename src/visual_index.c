#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/visual_index.h>

#include "visual_index_internal.h"

enum {
  SEGMENT_HEADER_SIZE = 128,
  POSTING_SIZE = 24,
  DESCRIPTOR_SIZE = 32,
  SEGMENT_POSTING_MAX = LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX * 1024 *
                        LARDON3D_VISUAL_INDEX_TABLE_COUNT,
  FEATURE_SET_POSTING_MAX = 1024 * LARDON3D_VISUAL_INDEX_TABLE_COUNT,
  VISUAL_INDEX_CHILD_STACK_BYTES = 512 * 1024,
};

typedef struct {
  uint32_t table;
  uint32_t key;
  uint64_t feature_set_id;
  uint32_t feature_index;
} Posting;

typedef struct {
  uint64_t feature_set_id;
  uint32_t evidence;
  uint32_t last_query_feature;
} Accumulator;

static void put_u32(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)value;
  output[1] = (unsigned char)(value >> 8);
  output[2] = (unsigned char)(value >> 16);
  output[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *output, uint64_t value) {
  put_u32(output, (uint32_t)value);
  put_u32(output + 4, (uint32_t)(value >> 32));
}

static uint32_t get_u32(const unsigned char *input) {
  return (uint32_t)input[0] | (uint32_t)input[1] << 8 | (uint32_t)input[2] << 16 |
         (uint32_t)input[3] << 24;
}

static uint64_t get_u64(const unsigned char *input) {
  return (uint64_t)get_u32(input) | (uint64_t)get_u32(input + 4) << 32;
}

bool lardon3d_visual_index_configuration_valid(
    const Lardon3DVisualIndexConfiguration *configuration) {
  return configuration && configuration->version == LARDON3D_VISUAL_INDEX_VERSION &&
         configuration->max_features_per_set > 0 &&
         configuration->max_features_per_set <= 1024 &&
         configuration->max_bucket_postings > 0 &&
         configuration->max_bucket_postings <= 4096;
}

void lardon3d_visual_index_configuration_fingerprint(
    const Lardon3DVisualIndexConfiguration *configuration, unsigned char fingerprint[32]) {
  unsigned char canonical[32] = {0};
  memcpy(canonical, "L3DVICF1", 8);
  if (lardon3d_visual_index_configuration_valid(configuration)) {
    put_u32(canonical + 8, configuration->version);
    put_u32(canonical + 12, LARDON3D_VISUAL_INDEX_TABLE_COUNT);
    put_u32(canonical + 16, LARDON3D_VISUAL_INDEX_KEY_BITS);
    put_u32(canonical + 20, configuration->max_features_per_set);
    put_u32(canonical + 24, configuration->max_bucket_postings);
    put_u32(canonical + 28, LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX);
  }
  EVP_Digest(canonical, sizeof(canonical), fingerprint, NULL, EVP_sha256(), NULL);
}

static Lardon3DVisualIndexResult db_result(Lardon3DProjectDbResult result) {
  switch (result) {
  case LARDON3D_PROJECT_DB_OK:
    return LARDON3D_VISUAL_INDEX_OK;
  case LARDON3D_PROJECT_DB_NOT_FOUND:
    return LARDON3D_VISUAL_INDEX_NOT_FOUND;
  case LARDON3D_PROJECT_DB_BUSY:
    return LARDON3D_VISUAL_INDEX_DB_BUSY;
  case LARDON3D_PROJECT_DB_INVALID_ARGUMENT:
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  case LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA:
    return LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION;
  case LARDON3D_PROJECT_DB_CORRUPT:
    return LARDON3D_VISUAL_INDEX_CORRUPT;
  default:
    return LARDON3D_VISUAL_INDEX_DB_ERROR;
  }
}

Lardon3DVisualIndexResult lardon3d_visual_index_create(
    Lardon3DProjectDb *database, const Lardon3DProjectDbFeatureSet *prototype,
    const Lardon3DVisualIndexConfiguration *configuration, uint64_t *visual_index_id) {
  if (visual_index_id) {
    *visual_index_id = 0;
  }
  if (!database || !prototype || !visual_index_id ||
      !lardon3d_visual_index_configuration_valid(configuration) ||
      prototype->descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_U8 ||
      prototype->descriptor_dimension != DESCRIPTOR_SIZE) {
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  }
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  Lardon3DProjectDbVisualIndex requested = {
      .index_version = LARDON3D_VISUAL_INDEX_VERSION,
      .descriptor_type = prototype->descriptor_type,
      .descriptor_dimension = prototype->descriptor_dimension,
      .extractor_version = prototype->extractor_version,
      .table_count = LARDON3D_VISUAL_INDEX_TABLE_COUNT,
      .key_bits = LARDON3D_VISUAL_INDEX_KEY_BITS,
      .max_features_per_set = configuration->max_features_per_set,
      .max_bucket_postings = configuration->max_bucket_postings,
      .created_at = now.tv_sec,
  };
  snprintf(requested.index_kind, sizeof(requested.index_kind), "%s", LARDON3D_VISUAL_INDEX_KIND);
  snprintf(requested.extractor_kind, sizeof(requested.extractor_kind), "%s",
           prototype->extractor_kind);
  memcpy(requested.feature_parameter_fingerprint, prototype->parameter_fingerprint, 32);
  lardon3d_visual_index_configuration_fingerprint(configuration,
                                                  requested.index_parameter_fingerprint);
  Lardon3DProjectDbVisualIndex created;
  Lardon3DProjectDbResult result =
      lardon3d_project_db_create_visual_index(database, &requested, &created);
  if (result == LARDON3D_PROJECT_DB_OK) {
    *visual_index_id = created.visual_index_id;
  }
  return db_result(result);
}

static uint32_t lsh_key(const unsigned char descriptor[32], uint32_t table) {
  uint32_t key = 0;
  for (uint32_t bit = 0; bit < LARDON3D_VISUAL_INDEX_KEY_BITS; ++bit) {
    uint32_t source = (table * 41U + bit * 11U) & 255U;
    uint32_t value = (descriptor[source / 8] >> (source % 8)) & 1U;
    key |= value << bit;
  }
  return key;
}

static int compare_posting(const void *left, const void *right) {
  const Posting *a = left;
  const Posting *b = right;
  if (a->table != b->table) {
    return a->table < b->table ? -1 : 1;
  }
  if (a->key != b->key) {
    return a->key < b->key ? -1 : 1;
  }
  if (a->feature_set_id != b->feature_set_id) {
    return a->feature_set_id < b->feature_set_id ? -1 : 1;
  }
  return a->feature_index < b->feature_index ? -1 : a->feature_index > b->feature_index;
}

static Lardon3DVisualIndexResult append_feature_postings(
    const char *project_path, const Lardon3DProjectDbFeatureSet *set, uint32_t maximum,
    Posting *postings, size_t capacity, size_t *count) {
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  Lardon3DFeatureStoreResult opened =
      lardon3d_feature_reader_open(project_path, set, &reader, &metadata);
  if (opened != LARDON3D_FEATURE_STORE_OK) {
    return opened == LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION
               ? LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION
               : opened == LARDON3D_FEATURE_STORE_CORRUPT ? LARDON3D_VISUAL_INDEX_CORRUPT
                                                          : LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  uint32_t selected = metadata.feature_count < maximum ? metadata.feature_count : maximum;
  unsigned char descriptors[LARDON3D_FEATURE_READER_RANGE_MAX * DESCRIPTOR_SIZE];
  for (uint32_t start = 0; start < selected;) {
    uint32_t amount = selected - start;
    if (amount > LARDON3D_FEATURE_READER_RANGE_MAX) {
      amount = LARDON3D_FEATURE_READER_RANGE_MAX;
    }
    if (lardon3d_feature_reader_descriptors(reader, start, descriptors, amount,
                                             sizeof(descriptors)) != LARDON3D_FEATURE_STORE_OK) {
      lardon3d_feature_reader_close(reader);
      return LARDON3D_VISUAL_INDEX_IO_ERROR;
    }
    for (uint32_t i = 0; i < amount; ++i) {
      for (uint32_t table = 0; table < LARDON3D_VISUAL_INDEX_TABLE_COUNT; ++table) {
        if (*count >= capacity) {
          lardon3d_feature_reader_close(reader);
          return LARDON3D_VISUAL_INDEX_LIMIT;
        }
        postings[*count] = (Posting){table, lsh_key(descriptors + i * DESCRIPTOR_SIZE, table),
                                     set->feature_set_id, start + i};
        ++*count;
      }
    }
    start += amount;
  }
  lardon3d_feature_reader_close(reader);
  return LARDON3D_VISUAL_INDEX_OK;
}

typedef struct {
  const char *project_path;
  const Lardon3DProjectDbFeatureSet *sets;
  uint32_t maximum_features;
  Posting *postings;
  size_t set_count;
  size_t first_set;
  size_t stride;
  size_t *posting_counts;
  Lardon3DVisualIndexResult *results;
} PostingBuildJob;

static void *build_posting_slices(void *userdata) {
  PostingBuildJob *job = userdata;
  for (size_t i = job->first_set; i < job->set_count; i += job->stride) {
    size_t count = 0;
    job->results[i] = append_feature_postings(
        job->project_path, &job->sets[i], job->maximum_features,
        job->postings + i * FEATURE_SET_POSTING_MAX, FEATURE_SET_POSTING_MAX, &count);
    job->posting_counts[i] = count;
  }
  return NULL;
}

static Lardon3DVisualIndexResult build_postings(
    const char *project_path, const Lardon3DProjectDbFeatureSet *sets, size_t set_count,
    uint32_t maximum_features, unsigned int cpu_threads, Posting *postings,
    size_t *posting_count) {
  size_t counts[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX] = {0};
  Lardon3DVisualIndexResult results[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX];
  for (size_t i = 0; i < set_count; ++i) {
    results[i] = LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  size_t participants = cpu_threads < set_count ? cpu_threads : set_count;
  PostingBuildJob jobs[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX];
  pthread_t children[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX - 1];
  bool created[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX - 1] = {false};
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  /* RESOURCE: each child only reads one immutable Feature File slice. Bound
   * its stack explicitly so the Task's existing 2 MiB per-member reservation
   * accounts for the complete 16-participant segment on every host. */
  if (pthread_attr_setstacksize(&attributes, VISUAL_INDEX_CHILD_STACK_BYTES) != 0) {
    (void)pthread_attr_destroy(&attributes);
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  for (size_t participant = 0; participant < participants; ++participant) {
    jobs[participant] = (PostingBuildJob){.project_path = project_path,
                                          .sets = sets,
                                          .maximum_features = maximum_features,
                                          .postings = postings,
                                          .set_count = set_count,
                                          .first_set = participant,
                                          .stride = participants,
                                          .posting_counts = counts,
                                          .results = results};
  }
  for (size_t participant = 1; participant < participants; ++participant) {
    created[participant - 1] =
        pthread_create(&children[participant - 1], &attributes, build_posting_slices,
                       &jobs[participant]) == 0;
  }
  bool attributes_released = pthread_attr_destroy(&attributes) == 0;
  (void)build_posting_slices(&jobs[0]);
  bool joined = true;
  for (size_t participant = 1; participant < participants; ++participant) {
    if (created[participant - 1] && pthread_join(children[participant - 1], NULL) != 0) {
      joined = false;
    }
  }
  if (!joined || !attributes_released) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  /* A failed pthread_create does not change results: the owner computes only
   * the missing disjoint slices after all successfully created children join. */
  for (size_t participant = 1; participant < participants; ++participant) {
    if (!created[participant - 1]) {
      (void)build_posting_slices(&jobs[participant]);
    }
  }
  size_t compact_count = 0;
  for (size_t i = 0; i < set_count; ++i) {
    if (results[i] != LARDON3D_VISUAL_INDEX_OK) {
      return results[i];
    }
    /* Completion order is deliberately discarded. Each source owns a fixed
     * private slice, then the owner compacts in feature_set_id selection order;
     * create_segment_file applies the persisted total posting order. */
    memmove(postings + compact_count, postings + i * FEATURE_SET_POSTING_MAX,
            counts[i] * sizeof(*postings));
    compact_count += counts[i];
  }
  *posting_count = compact_count;
  return LARDON3D_VISUAL_INDEX_OK;
}

static bool write_exact(int descriptor, const void *data, size_t size) {
  const unsigned char *bytes = data;
  while (size > 0) {
    ssize_t written = write(descriptor, bytes, size);
    if (written <= 0) {
      return false;
    }
    bytes += (size_t)written;
    size -= (size_t)written;
  }
  return true;
}

static bool read_exact(int descriptor, void *data, size_t size, off_t offset) {
  unsigned char *bytes = data;
  while (size > 0) {
    ssize_t amount = pread(descriptor, bytes, size, offset);
    if (amount <= 0) {
      return false;
    }
    bytes += (size_t)amount;
    size -= (size_t)amount;
    offset += amount;
  }
  return true;
}

static bool hash_fd(int descriptor, unsigned char output[32]) {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (!context || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 || lseek(descriptor, 0, SEEK_SET) < 0) {
    EVP_MD_CTX_free(context);
    return false;
  }
  unsigned char buffer[65536];
  ssize_t amount;
  while ((amount = read(descriptor, buffer, sizeof(buffer))) > 0) {
    if (EVP_DigestUpdate(context, buffer, (size_t)amount) != 1) {
      EVP_MD_CTX_free(context);
      return false;
    }
  }
  unsigned int size = 0;
  bool ok = amount == 0 && EVP_DigestFinal_ex(context, output, &size) == 1 && size == 32;
  EVP_MD_CTX_free(context);
  return ok;
}

static bool make_directory(const char *path) {
  return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool visual_directories(const char *project_path, const char prefix[3], char *directory,
                               size_t capacity) {
  char assets[PATH_MAX];
  char visual[PATH_MAX];
  int a = snprintf(assets, sizeof(assets), "%s/assets", project_path);
  int v = snprintf(visual, sizeof(visual), "%s/visual-index", assets);
  int d = snprintf(directory, capacity, "%s/%s", visual, prefix);
  return a > 0 && (size_t)a < sizeof(assets) && v > 0 && (size_t)v < sizeof(visual) && d > 0 &&
         (size_t)d < capacity && make_directory(assets) && make_directory(visual) &&
         make_directory(directory);
}

static Lardon3DVisualIndexResult publish_file(const char *project_path, const char *temporary,
                                              const unsigned char hash[32], char path[4096],
                                              bool *durable) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < 32; ++i) {
    hex[i * 2] = digits[hash[i] >> 4];
    hex[i * 2 + 1] = digits[hash[i] & 15];
  }
  hex[64] = '\0';
  char prefix[3] = {hex[0], hex[1], '\0'};
  char directory[PATH_MAX];
  if (!visual_directories(project_path, prefix, directory, sizeof(directory))) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  char destination[PATH_MAX];
  int absolute = snprintf(destination, sizeof(destination), "%s/%s", directory, hex);
  int relative = snprintf(path, 4096, "assets/visual-index/%s/%s", prefix, hex);
  if (absolute <= 0 || (size_t)absolute >= sizeof(destination) || relative <= 0 || relative >= 4096) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  if (link(temporary, destination) != 0) {
    if (errno != EEXIST) {
      return LARDON3D_VISUAL_INDEX_IO_ERROR;
    }
    int existing = open(destination, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    unsigned char actual[32];
    struct stat status;
    bool valid = existing >= 0 && fstat(existing, &status) == 0 && S_ISREG(status.st_mode) &&
                 hash_fd(existing, actual) && memcmp(actual, hash, 32) == 0;
    if (existing >= 0) {
      close(existing);
    }
    if (!valid) {
      return LARDON3D_VISUAL_INDEX_CORRUPT;
    }
  }
  int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  *durable = fsync(directory_fd) == 0;
#ifdef LARDON3D_VISUAL_INDEX_TESTING
  const char *fail_sync = getenv("LARDON3D_TEST_VISUAL_INDEX_FAIL_DIR_FSYNC");
  if (fail_sync && strcmp(fail_sync, "1") == 0) {
    *durable = false;
  }
#endif
  close(directory_fd);
  return *durable ? LARDON3D_VISUAL_INDEX_OK : LARDON3D_VISUAL_INDEX_PUBLISHED_NOT_DURABLE;
}

static Lardon3DVisualIndexResult create_segment_file(
    const char *project_path, const Lardon3DProjectDbVisualIndex *index, Posting *postings,
    size_t posting_count, size_t member_count, unsigned char hash[32], char path[4096],
    uint64_t *size, bool *durable) {
  qsort(postings, posting_count, sizeof(*postings), compare_posting);
  char template_path[PATH_MAX];
  int n = snprintf(template_path, sizeof(template_path), "%s/.lardon3d/visual-index-XXXXXX",
                   project_path);
  if (n <= 0 || (size_t)n >= sizeof(template_path)) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  int descriptor = mkstemp(template_path);
  if (descriptor < 0) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  unsigned char header[SEGMENT_HEADER_SIZE] = {0};
  memcpy(header, "L3DVIDX\0", 8);
  put_u32(header + 8, 1);
  put_u32(header + 12, SEGMENT_HEADER_SIZE);
  put_u32(header + 16, index->table_count);
  put_u32(header + 20, index->key_bits);
  put_u64(header + 24, posting_count);
  put_u64(header + 32, member_count);
  put_u64(header + 40, SEGMENT_HEADER_SIZE);
  *size = SEGMENT_HEADER_SIZE + posting_count * POSTING_SIZE;
  put_u64(header + 48, *size);
  memcpy(header + 56, index->index_parameter_fingerprint, 32);
  memcpy(header + 88, index->feature_parameter_fingerprint, 32);
  bool ok = write_exact(descriptor, header, sizeof(header));
  unsigned char record[POSTING_SIZE];
  for (size_t i = 0; i < posting_count && ok; ++i) {
    memset(record, 0, sizeof(record));
    put_u32(record, postings[i].table);
    put_u32(record + 4, postings[i].key);
    put_u64(record + 8, postings[i].feature_set_id);
    put_u32(record + 16, postings[i].feature_index);
    ok = write_exact(descriptor, record, sizeof(record));
  }
  ok = ok && fsync(descriptor) == 0 && hash_fd(descriptor, hash);
  Lardon3DVisualIndexResult result = ok ? publish_file(project_path, template_path, hash, path, durable)
                                        : LARDON3D_VISUAL_INDEX_IO_ERROR;
  close(descriptor);
  unlink(template_path);
  return result;
}

static Lardon3DVisualIndexResult update_once_internal(
    const char *project_path, Lardon3DProjectDb *database, uint64_t index_id,
    uint64_t producer_task_id, uint64_t after, size_t maximum_feature_sets, uint64_t *last,
    size_t *indexed_count, unsigned int cpu_threads, void (*after_select)(void *),
    void *hook_userdata) {
  if (last) {
    *last = after;
  }
  if (indexed_count) {
    *indexed_count = 0;
  }
  if (!project_path || !database || index_id == 0 || maximum_feature_sets == 0 ||
      maximum_feature_sets > LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX || !last ||
      !indexed_count || cpu_threads == 0 ||
      cpu_threads > LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX) {
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbVisualIndex index;
  Lardon3DProjectDbResult loaded = lardon3d_project_db_load_visual_index(database, index_id, &index);
  if (loaded != LARDON3D_PROJECT_DB_OK) {
    return db_result(loaded);
  }
  Lardon3DProjectDbVisualIndexSegment segments[LARDON3D_VISUAL_INDEX_SEGMENT_MAX];
  size_t segment_count = 0;
  loaded = lardon3d_project_db_list_visual_index_segments(database, index_id, 0, segments,
                                                           LARDON3D_VISUAL_INDEX_SEGMENT_MAX,
                                                           &segment_count);
  if (loaded != LARDON3D_PROJECT_DB_OK) {
    return db_result(loaded);
  }
  if (segment_count >= LARDON3D_VISUAL_INDEX_SEGMENT_MAX) {
    return LARDON3D_VISUAL_INDEX_LIMIT;
  }
  Lardon3DProjectDbFeatureSet sets[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX];
  size_t set_count = 0;
  loaded = lardon3d_project_db_list_visual_index_pending(
      database, index_id, after, sets, maximum_feature_sets, &set_count);
  if (loaded != LARDON3D_PROJECT_DB_OK) {
    return db_result(loaded);
  }
  if (set_count == 0) {
    return LARDON3D_VISUAL_INDEX_NO_CHANGE;
  }
  if (after_select) {
    after_select(hook_userdata);
  }
  Posting *postings = calloc(SEGMENT_POSTING_MAX, sizeof(*postings));
  if (!postings) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  size_t posting_count = 0;
  Lardon3DVisualIndexResult result = LARDON3D_VISUAL_INDEX_OK;
  uint64_t ids[LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX];
  for (size_t i = 0; i < set_count; ++i) {
    ids[i] = sets[i].feature_set_id;
  }
  /* All parallel work ends before the deterministic sort, asset publication,
   * and membership transaction. A failed slice therefore publishes nothing. */
  result = build_postings(project_path, sets, set_count, index.max_features_per_set,
                          cpu_threads, postings, &posting_count);
  unsigned char hash[32];
  char path[4096];
  uint64_t size = 0;
  bool durable = false;
  if (result == LARDON3D_VISUAL_INDEX_OK) {
    result = create_segment_file(project_path, &index, postings, posting_count, set_count, hash,
                                 path, &size, &durable);
  }
  free(postings);
  if (result != LARDON3D_VISUAL_INDEX_OK &&
      result != LARDON3D_VISUAL_INDEX_PUBLISHED_NOT_DURABLE) {
    return result;
  }
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  Lardon3DProjectDbVisualIndexSegment segment = {
      .visual_index_id = index_id,
      .generation = segment_count + 1,
      .size_bytes = size,
      .posting_count = posting_count,
      .feature_set_count = (uint32_t)set_count,
      .durability = durable ? LARDON3D_DB_VISUAL_INDEX_DURABLE
                            : LARDON3D_DB_VISUAL_INDEX_PUBLISHED_NOT_DURABLE,
      .producer_task_id = producer_task_id,
      .created_at = now.tv_sec,
  };
  memcpy(segment.sha256, hash, 32);
  snprintf(segment.path, sizeof(segment.path), "%s", path);
  Lardon3DProjectDbVisualIndexSegment published;
  loaded = lardon3d_project_db_publish_visual_index_segment(database, &segment, ids, set_count,
                                                             &published);
  if (loaded != LARDON3D_PROJECT_DB_OK) {
    return db_result(loaded);
  }
  *last = ids[set_count - 1];
  *indexed_count = set_count;
  return durable ? LARDON3D_VISUAL_INDEX_OK
                 : LARDON3D_VISUAL_INDEX_PUBLISHED_NOT_DURABLE;
}

Lardon3DVisualIndexResult lardon3d_visual_index_update_once(
    const char *project_path, Lardon3DProjectDb *database, uint64_t index_id,
    uint64_t producer_task_id, uint64_t after, size_t maximum_feature_sets, uint64_t *last,
    size_t *indexed_count) {
  return update_once_internal(project_path, database, index_id, producer_task_id, after,
                              maximum_feature_sets, last, indexed_count, 1, NULL, NULL);
}

Lardon3DVisualIndexResult lardon3d_visual_index_update_once_parallel(
    const char *project_path, Lardon3DProjectDb *database, uint64_t index_id,
    uint64_t producer_task_id, uint64_t after, size_t maximum_feature_sets,
    unsigned int cpu_threads, uint64_t *last, size_t *indexed_count) {
  return update_once_internal(project_path, database, index_id, producer_task_id, after,
                              maximum_feature_sets, last, indexed_count, cpu_threads, NULL, NULL);
}

#ifdef LARDON3D_VISUAL_INDEX_TESTING
Lardon3DVisualIndexResult lardon3d_visual_index_test_update_once(
    const char *project_path, Lardon3DProjectDb *database, uint64_t index_id,
    uint64_t producer_task_id, uint64_t after, size_t maximum_feature_sets, uint64_t *last,
    size_t *indexed_count, Lardon3DVisualIndexAfterSelectHook after_select,
    void *hook_userdata) {
  return update_once_internal(project_path, database, index_id, producer_task_id, after,
                              maximum_feature_sets, last, indexed_count, 1, after_select,
                              hook_userdata);
}
#endif

static Lardon3DVisualIndexResult open_segment(
    const char *project_path, const Lardon3DProjectDbVisualIndex *index,
    const Lardon3DProjectDbVisualIndexSegment *segment, int *descriptor, uint64_t *posting_count) {
  char path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/%s", project_path, segment->path);
  if (n <= 0 || (size_t)n >= sizeof(path)) {
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  }
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return errno == ENOENT ? LARDON3D_VISUAL_INDEX_NOT_FOUND : LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  struct stat status;
  unsigned char header[SEGMENT_HEADER_SIZE];
  unsigned char hash[32];
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < SEGMENT_HEADER_SIZE ||
      (uint64_t)status.st_size != segment->size_bytes || !read_exact(fd, header, sizeof(header), 0) ||
      !hash_fd(fd, hash) || memcmp(hash, segment->sha256, 32) != 0) {
    close(fd);
    return LARDON3D_VISUAL_INDEX_CORRUPT;
  }
  uint32_t version = get_u32(header + 8);
  if (memcmp(header, "L3DVIDX\0", 8) != 0) {
    close(fd);
    return LARDON3D_VISUAL_INDEX_CORRUPT;
  }
  if (version != 1) {
    close(fd);
    return LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION;
  }
  uint64_t count = get_u64(header + 24);
  if (get_u32(header + 12) != SEGMENT_HEADER_SIZE || get_u32(header + 16) != index->table_count ||
      get_u32(header + 20) != index->key_bits || get_u64(header + 40) != SEGMENT_HEADER_SIZE ||
      count > SEGMENT_POSTING_MAX || count > (UINT64_MAX - SEGMENT_HEADER_SIZE) / POSTING_SIZE ||
      get_u64(header + 48) != SEGMENT_HEADER_SIZE + count * POSTING_SIZE ||
      get_u64(header + 48) != (uint64_t)status.st_size ||
      memcmp(header + 56, index->index_parameter_fingerprint, 32) != 0 ||
      memcmp(header + 88, index->feature_parameter_fingerprint, 32) != 0) {
    close(fd);
    return LARDON3D_VISUAL_INDEX_CORRUPT;
  }
  for (size_t i = 120; i < 128; ++i) {
    if (header[i] != 0) {
      close(fd);
      return LARDON3D_VISUAL_INDEX_CORRUPT;
    }
  }
  *descriptor = fd;
  *posting_count = count;
  return LARDON3D_VISUAL_INDEX_OK;
}

static bool read_posting(int descriptor, uint64_t index, Posting *posting) {
  unsigned char record[POSTING_SIZE];
  if (!read_exact(descriptor, record, sizeof(record),
                  (off_t)(SEGMENT_HEADER_SIZE + index * POSTING_SIZE))) {
    return false;
  }
  posting->table = get_u32(record);
  posting->key = get_u32(record + 4);
  posting->feature_set_id = get_u64(record + 8);
  posting->feature_index = get_u32(record + 16);
  return get_u32(record + 20) == 0 && posting->table < LARDON3D_VISUAL_INDEX_TABLE_COUNT &&
         posting->feature_set_id > 0 && posting->feature_index < 8192;
}

static int posting_key_compare(const Posting *posting, uint32_t table, uint32_t key) {
  if (posting->table != table) {
    return posting->table < table ? -1 : 1;
  }
  return posting->key < key ? -1 : posting->key > key;
}

static bool bucket_range(int descriptor, uint64_t count, uint32_t table, uint32_t key,
                         uint64_t *begin, uint64_t *end) {
  uint64_t low = 0;
  uint64_t high = count;
  Posting posting;
  while (low < high) {
    uint64_t middle = low + (high - low) / 2;
    if (!read_posting(descriptor, middle, &posting)) {
      return false;
    }
    if (posting_key_compare(&posting, table, key) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  *begin = low;
  high = count;
  while (low < high) {
    uint64_t middle = low + (high - low) / 2;
    if (!read_posting(descriptor, middle, &posting)) {
      return false;
    }
    if (posting_key_compare(&posting, table, key) <= 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  *end = low;
  return true;
}

static void add_evidence(Accumulator *accumulators, size_t *count, uint64_t feature_set_id,
                         uint32_t query_feature) {
  for (size_t i = 0; i < *count; ++i) {
    if (accumulators[i].feature_set_id == feature_set_id) {
      if (accumulators[i].last_query_feature != query_feature) {
        ++accumulators[i].evidence;
        accumulators[i].last_query_feature = query_feature;
      }
      return;
    }
  }
  if (*count < LARDON3D_VISUAL_INDEX_CANDIDATE_MAX) {
    accumulators[*count] = (Accumulator){feature_set_id, 1, query_feature};
    ++*count;
  }
}

static Lardon3DVisualIndexResult count_segment_buckets(
    const char *project_path, const Lardon3DProjectDbVisualIndex *index,
    const Lardon3DProjectDbVisualIndexSegment *segment, const unsigned char *descriptors,
    uint32_t query_count, uint32_t *bucket_sizes) {
  int descriptor = -1;
  uint64_t posting_count = 0;
  Lardon3DVisualIndexResult result =
      open_segment(project_path, index, segment, &descriptor, &posting_count);
  if (result != LARDON3D_VISUAL_INDEX_OK) {
    return result;
  }
  for (uint32_t query = 0; query < query_count; ++query) {
    for (uint32_t table = 0; table < index->table_count; ++table) {
      size_t slot = (size_t)query * index->table_count + table;
      if (bucket_sizes[slot] > index->max_bucket_postings) {
        continue;
      }
      uint64_t begin;
      uint64_t end;
      uint32_t key = lsh_key(descriptors + (size_t)query * DESCRIPTOR_SIZE, table);
      if (!bucket_range(descriptor, posting_count, table, key, &begin, &end)) {
        close(descriptor);
        return LARDON3D_VISUAL_INDEX_CORRUPT;
      }
      uint64_t available = index->max_bucket_postings + 1U - bucket_sizes[slot];
      uint64_t amount = end - begin;
      bucket_sizes[slot] += (uint32_t)(amount < available ? amount : available);
    }
  }
  close(descriptor);
  return LARDON3D_VISUAL_INDEX_OK;
}

static Lardon3DVisualIndexResult query_segment(
    const char *project_path, const Lardon3DProjectDbVisualIndex *index,
    const Lardon3DProjectDbVisualIndexSegment *segment, const unsigned char *descriptors,
    uint32_t query_count, const uint32_t *bucket_sizes, uint64_t self,
    Accumulator *accumulators, size_t *accumulator_count) {
  int descriptor = -1;
  uint64_t posting_count = 0;
  Lardon3DVisualIndexResult result =
      open_segment(project_path, index, segment, &descriptor, &posting_count);
  if (result != LARDON3D_VISUAL_INDEX_OK) {
    return result;
  }
  Posting posting;
  for (uint32_t query = 0; query < query_count; ++query) {
    for (uint32_t table = 0; table < index->table_count; ++table) {
      if (bucket_sizes[(size_t)query * index->table_count + table] >
          index->max_bucket_postings) {
        continue;
      }
      uint64_t begin;
      uint64_t end;
      uint32_t key = lsh_key(descriptors + (size_t)query * DESCRIPTOR_SIZE, table);
      if (!bucket_range(descriptor, posting_count, table, key, &begin, &end)) {
        close(descriptor);
        return LARDON3D_VISUAL_INDEX_CORRUPT;
      }
      for (uint64_t offset = begin; offset < end; ++offset) {
        if (!read_posting(descriptor, offset, &posting)) {
          close(descriptor);
          return LARDON3D_VISUAL_INDEX_CORRUPT;
        }
        if (posting.feature_set_id != self) {
          add_evidence(accumulators, accumulator_count, posting.feature_set_id, query);
        }
      }
    }
  }
  close(descriptor);
  return LARDON3D_VISUAL_INDEX_OK;
}

static int compare_candidate(const void *left, const void *right) {
  const Lardon3DVisualIndexCandidate *a = left;
  const Lardon3DVisualIndexCandidate *b = right;
  if (a->score != b->score) {
    return a->score > b->score ? -1 : 1;
  }
  if (a->evidence_count != b->evidence_count) {
    return a->evidence_count > b->evidence_count ? -1 : 1;
  }
  if (a->image_id != b->image_id) {
    return a->image_id < b->image_id ? -1 : 1;
  }
  return a->feature_set_id < b->feature_set_id ? -1 : a->feature_set_id > b->feature_set_id;
}

Lardon3DVisualIndexResult lardon3d_visual_index_query(
    const char *project_path, Lardon3DProjectDb *database, uint64_t index_id,
    uint64_t query_id, const Lardon3DVisualIndexQueryOptions *options,
    Lardon3DVisualIndexCandidate *results, size_t capacity, size_t *result_count) {
  if (result_count) {
    *result_count = 0;
  }
  if (!project_path || !database || index_id == 0 || query_id == 0 || !options || !results ||
      !result_count || options->top_k == 0 || options->top_k > LARDON3D_VISUAL_INDEX_TOP_K_MAX ||
      capacity < options->top_k || options->minimum_evidence_count > 1024 ||
      options->scanset_filter > LARDON3D_VISUAL_INDEX_OTHER_SCANSETS) {
    return LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbVisualIndex index;
  Lardon3DProjectDbFeatureSet query_set;
  Lardon3DProjectDbResult db = lardon3d_project_db_load_visual_index(database, index_id, &index);
  if (db == LARDON3D_PROJECT_DB_OK) {
    db = lardon3d_project_db_load_feature_set(database, query_id, &query_set);
  }
  if (db != LARDON3D_PROJECT_DB_OK) {
    return db_result(db);
  }
  if (query_set.descriptor_type != index.descriptor_type ||
      query_set.descriptor_dimension != index.descriptor_dimension ||
      query_set.extractor_version != index.extractor_version ||
      strcmp(query_set.extractor_kind, index.extractor_kind) != 0 ||
      memcmp(query_set.parameter_fingerprint, index.feature_parameter_fingerprint, 32) != 0) {
    return LARDON3D_VISUAL_INDEX_INCOMPATIBLE;
  }
  Lardon3DProjectDbImage query_image;
  Lardon3DProjectDbImageAsset query_asset;
  db = lardon3d_project_db_load_image(database, query_set.image_id, &query_image, &query_asset);
  if (db != LARDON3D_PROJECT_DB_OK) {
    return db_result(db);
  }
  uint32_t query_count = query_set.feature_count < index.max_features_per_set
                             ? query_set.feature_count
                             : index.max_features_per_set;
  unsigned char *descriptors = query_count ? malloc((size_t)query_count * DESCRIPTOR_SIZE) : NULL;
  if (query_count && !descriptors) {
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  Lardon3DFeatureStoreResult opened =
      lardon3d_feature_reader_open(project_path, &query_set, &reader, &metadata);
  for (uint32_t start = 0; opened == LARDON3D_FEATURE_STORE_OK && start < query_count;) {
    uint32_t amount = query_count - start;
    if (amount > LARDON3D_FEATURE_READER_RANGE_MAX) {
      amount = LARDON3D_FEATURE_READER_RANGE_MAX;
    }
    opened = lardon3d_feature_reader_descriptors(reader, start,
                                                  descriptors + (size_t)start * DESCRIPTOR_SIZE,
                                                  amount, (size_t)amount * DESCRIPTOR_SIZE);
    start += amount;
  }
  lardon3d_feature_reader_close(reader);
  if (opened != LARDON3D_FEATURE_STORE_OK) {
    free(descriptors);
    return opened == LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION
               ? LARDON3D_VISUAL_INDEX_UNSUPPORTED_VERSION
               : LARDON3D_VISUAL_INDEX_CORRUPT;
  }
  Lardon3DProjectDbVisualIndexSegment segments[LARDON3D_VISUAL_INDEX_SEGMENT_MAX];
  size_t segment_count = 0;
  db = lardon3d_project_db_list_visual_index_segments(database, index_id, 0, segments,
                                                       LARDON3D_VISUAL_INDEX_SEGMENT_MAX,
                                                       &segment_count);
  if (db != LARDON3D_PROJECT_DB_OK) {
    free(descriptors);
    return db_result(db);
  }
  Accumulator *accumulators = calloc(LARDON3D_VISUAL_INDEX_CANDIDATE_MAX, sizeof(*accumulators));
  if (!accumulators) {
    free(descriptors);
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  size_t accumulator_count = 0;
  Lardon3DVisualIndexResult result = LARDON3D_VISUAL_INDEX_OK;
  size_t bucket_count = (size_t)query_count * index.table_count;
  uint32_t *bucket_sizes = bucket_count ? calloc(bucket_count, sizeof(*bucket_sizes)) : NULL;
  if (bucket_count && !bucket_sizes) {
    free(descriptors);
    free(accumulators);
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  for (size_t i = 0; i < segment_count && result == LARDON3D_VISUAL_INDEX_OK; ++i) {
    result = count_segment_buckets(project_path, &index, &segments[i], descriptors, query_count,
                                   bucket_sizes);
  }
  for (size_t i = 0; i < segment_count && result == LARDON3D_VISUAL_INDEX_OK; ++i) {
    result = query_segment(project_path, &index, &segments[i], descriptors, query_count,
                           bucket_sizes, query_id, accumulators, &accumulator_count);
  }
  free(bucket_sizes);
  free(descriptors);
  Lardon3DVisualIndexCandidate *candidates =
      calloc(LARDON3D_VISUAL_INDEX_CANDIDATE_MAX, sizeof(*candidates));
  if (!candidates) {
    free(accumulators);
    return LARDON3D_VISUAL_INDEX_IO_ERROR;
  }
  size_t output = 0;
  for (size_t i = 0; i < accumulator_count && result == LARDON3D_VISUAL_INDEX_OK; ++i) {
    if (accumulators[i].evidence < options->minimum_evidence_count) {
      continue;
    }
    Lardon3DProjectDbFeatureSet set;
    Lardon3DProjectDbImage image;
    Lardon3DProjectDbImageAsset asset;
    db = lardon3d_project_db_load_feature_set(database, accumulators[i].feature_set_id, &set);
    if (db == LARDON3D_PROJECT_DB_OK) {
      db = lardon3d_project_db_load_image(database, set.image_id, &image, &asset);
    }
    if (db != LARDON3D_PROJECT_DB_OK) {
      result = db_result(db);
      break;
    }
    bool same_asset = asset.asset_id == query_asset.asset_id;
    bool scan_allowed = options->scanset_filter == LARDON3D_VISUAL_INDEX_ANY_SCANSET ||
                        (options->scanset_filter == LARDON3D_VISUAL_INDEX_SAME_SCANSET &&
                         image.scanset_id == query_image.scanset_id) ||
                        (options->scanset_filter == LARDON3D_VISUAL_INDEX_OTHER_SCANSETS &&
                         image.scanset_id != query_image.scanset_id);
    if (!scan_allowed || (options->exclude_same_asset && same_asset) ||
        image.image_id == query_image.image_id || output >= LARDON3D_VISUAL_INDEX_CANDIDATE_MAX) {
      continue;
    }
    candidates[output++] = (Lardon3DVisualIndexCandidate){
        .feature_set_id = set.feature_set_id,
        .image_id = image.image_id,
        .scanset_id = image.scanset_id,
        .score = query_count ? (double)accumulators[i].evidence / query_count : 0.0,
        .evidence_count = accumulators[i].evidence,
        .same_image_asset = same_asset,
    };
  }
  free(accumulators);
  if (result != LARDON3D_VISUAL_INDEX_OK) {
    free(candidates);
    return result;
  }
  qsort(candidates, output, sizeof(*candidates), compare_candidate);
  if (output > options->top_k) {
    output = options->top_k;
  }
  memcpy(results, candidates, output * sizeof(*results));
  free(candidates);
  *result_count = output;
  return LARDON3D_VISUAL_INDEX_OK;
}
