extern "C" {
#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/project.h>
#include <lardon3d/project_db.h>
#include <lardon3d/task_queue.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {

constexpr unsigned char magic[8] = {'L', '3', 'D', 'A', 'C', 'T', '1', '\0'};
constexpr size_t max_request_size =
    LARDON3D_ACQUISITION_CAMPAIGN_TASK_REQUEST_MAX_BYTES;

/* Codec is transport-safe and deterministic: no native struct serialization is
 * used, integers are fixed-width with explicit endianness, and strings/counts are
 * explicitly bounded before write/read. */
struct Writer {
  unsigned char *p;
  size_t left;
  size_t used;
  bool bytes(const void *v, size_t n) {
    if (n > left)
      return false;
    if (p)
      std::memcpy(p + used, v, n);
    used += n;
    left -= n;
    return true;
  }
  bool u16(uint16_t v) {
    unsigned char b[2] = {static_cast<unsigned char>(v),
                          static_cast<unsigned char>(v >> 8)};
    return bytes(b, 2);
  }
  bool u32(uint32_t v) {
    unsigned char b[4];
    for (int i = 0; i < 4; ++i)
      b[i] = static_cast<unsigned char>(v >> (8 * i));
    return bytes(b, 4);
  }
  bool u64(uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i)
      b[i] = static_cast<unsigned char>(v >> (8 * i));
    return bytes(b, 8);
  }
  bool text(const char *s, size_t cap) {
    if (!s)
      return false;
    const void *end = std::memchr(s, '\0', cap);
    if (!end)
      return false;
    size_t n = static_cast<const char *>(end) - s;
    return n <= UINT16_MAX && u16(static_cast<uint16_t>(n)) && bytes(s, n);
  }
};

struct Reader {
  const unsigned char *p;
  size_t left;
  bool bytes(void *v, size_t n) {
    if (n > left)
      return false;
    std::memcpy(v, p, n);
    p += n;
    left -= n;
    return true;
  }
  bool u16(uint16_t &v) {
    unsigned char b[2];
    if (!bytes(b, 2))
      return false;
    v = uint16_t(b[0]) | uint16_t(uint16_t(b[1]) << 8);
    return true;
  }
  bool u32(uint32_t &v) {
    unsigned char b[4];
    if (!bytes(b, 4))
      return false;
    v = 0;
    for (int i = 3; i >= 0; --i)
      v = (v << 8) | b[i];
    return true;
  }
  bool u64(uint64_t &v) {
    unsigned char b[8];
    if (!bytes(b, 8))
      return false;
    v = 0;
    for (int i = 7; i >= 0; --i)
      v = (v << 8) | b[i];
    return true;
  }
  bool text(char *s, size_t cap) {
    uint16_t n;
    if (!u16(n) || size_t(n) >= cap || size_t(n) > left)
      return false;
    std::memcpy(s, p, n);
    s[n] = '\0';
    p += n;
    left -= n;
    return true;
  }
};

bool encode(const Lardon3DAcquisitionCampaignTaskRequest *r, Writer &w) {
  if (!r || !r->sources || r->source_count == 0 ||
      r->source_count > LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES ||
      (r->confirmation_count && !r->confirmations) ||
      r->confirmation_count > r->source_count ||
      r->ingest_options.representation <
          LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE ||
      r->ingest_options.representation >
          LARDON3D_ACQUISITION_SELECT_DEVELOP_RAW ||
      r->ingest_options.select_representation > 1u ||
      r->ingest_options.imported_at < 0 ||
      r->ingest_options.max_source_bytes == 0u)
    return false;
  std::unique_ptr<Lardon3DAcquisitionCampaignPlan> validated(
      new (std::nothrow) Lardon3DAcquisitionCampaignPlan{});
  if (!validated ||
      lardon3d_acquisition_campaign_plan(
          r->sources, r->source_count, r->confirmations, r->confirmation_count,
          validated.get()) != LARDON3D_ACQUISITION_CAMPAIGN_OK ||
      validated->group_count == 0)
    return false;
  if (!w.bytes(magic, sizeof(magic)) ||
      !w.u32(LARDON3D_ACQUISITION_CAMPAIGN_REQUEST_VERSION) ||
      !w.u32(static_cast<uint32_t>(r->source_count)) ||
      !w.u32(static_cast<uint32_t>(r->confirmation_count)) ||
      !w.u32(static_cast<uint32_t>(r->ingest_options.representation)) ||
      !w.u32(r->ingest_options.select_representation ? 1u : 0u) ||
      !w.u64(static_cast<uint64_t>(r->ingest_options.imported_at)) ||
      !w.u64(r->ingest_options.max_source_bytes))
    return false;
  for (size_t i = 0; i < r->source_count; ++i) {
    const auto &s = r->sources[i];
    const auto &m = s.metadata;
    if (!w.text(s.path, sizeof(s.path)) ||
        !w.u32(static_cast<uint32_t>(s.source_kind)) ||
        !w.u32(static_cast<uint32_t>(s.metadata_result)) ||
        !w.u32(m.policy_version) ||
        !w.u32(static_cast<uint32_t>(m.source_kind)) ||
        !w.u32(m.present_fields) || !w.text(m.make, sizeof(m.make)) ||
        !w.text(m.model, sizeof(m.model)) ||
        !w.text(m.body_serial, sizeof(m.body_serial)) ||
        !w.text(m.datetime_original, sizeof(m.datetime_original)) ||
        !w.text(m.subsec_original, sizeof(m.subsec_original)) ||
        !w.text(m.offset_original, sizeof(m.offset_original)) ||
        !w.text(m.image_unique_id, sizeof(m.image_unique_id)) ||
        !w.u32(m.width) || !w.u32(m.height) || !w.u16(m.orientation))
      return false;
  }
  for (size_t i = 0; i < r->confirmation_count; ++i) {
    const auto &c = r->confirmations[i];
    if (c.source_count == 0 ||
        c.source_count > LARDON3D_ACQUISITION_INGEST_MAX_SOURCES ||
        !w.u32(static_cast<uint32_t>(c.source_count)))
      return false;
    for (size_t j = 0; j < c.source_count; ++j)
      if (c.source_indices[j] >= r->source_count ||
          !w.u32(static_cast<uint32_t>(c.source_indices[j])))
        return false;
  }
  return true;
}

struct Context {
  char project_path[LARDON3D_APP_STATE_PATH_CAPACITY]{};
  Lardon3DProjectDb *db{};
  Lardon3DResourceGovernor *governor{};
  uint64_t scanset{};
  std::vector<Lardon3DAcquisitionCampaignSource> sources;
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations;
  std::vector<unsigned char> encoded;
  Lardon3DAcquisitionCampaignPlan plan{};
  Lardon3DAcquisitionIngestOptions options{};
};

void destroy(void *p) { delete static_cast<Context *>(p); }
void runtime(Context *c, Lardon3DAppState &s) {
  lardon3d_app_state_init(&s);
  s.project_loaded = true;
  s.project_db = c->db;
  s.resource_governor = c->governor;
  std::snprintf(s.project_path, sizeof(s.project_path), "%s", c->project_path);
}

bool checkpoint(Context *c, Lardon3DTask *t, uint32_t cursor) {
  Lardon3DAppState s;
  runtime(c, s);
  Lardon3DProjectDbAcquisitionCampaignTask p{
      lardon3d_task_id(t),
      c->scanset,
      cursor,
      static_cast<uint32_t>(c->plan.group_count),
      c->encoded.data(),
      c->encoded.size()};
  return lardon3d_project_checkpoint_acquisition_campaign_task(&s, t, &p) ==
         LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}
bool run_impl(Lardon3DTask *t, void *p) {
  auto *c = static_cast<Context *>(p);
  Lardon3DProjectDbAcquisitionCampaignTask persisted{};
  // Request is reconstructed from persisted blob and treated as immutable durable
  // input during recovery and replay.
  std::vector<unsigned char> blob(max_request_size);
  if (lardon3d_project_db_load_acquisition_campaign_task(
          c->db, lardon3d_task_id(t), blob.data(), blob.size(), &persisted) !=
      LARDON3D_PROJECT_DB_OK)
    return lardon3d_task_fail(t, "Campagne durable introuvable.");
  for (uint32_t cursor = persisted.next_group_id; cursor < c->plan.group_count;
       ++cursor) {
    const uint32_t group_id = cursor + 1u;
    if (!lardon3d_task_checkpoint(t))
      return false;
    Lardon3DProjectDbAcquisitionCampaignCapture retained{};
    uint64_t resume = 0;
    /* Durable replay uses the stable mapping (task_id, group_id) -> capture_id.
     * The persisted cursor is the zero-based next-work position: after group N
     * retention, its numeric value is N and replay materializes group N + 1.
     */
    auto lr = lardon3d_project_db_load_acquisition_campaign_capture(
        c->db, lardon3d_task_id(t), group_id, &retained);
    if (lr == LARDON3D_PROJECT_DB_OK)
      resume = retained.capture_id;
    else if (lr != LARDON3D_PROJECT_DB_NOT_FOUND)
      return lardon3d_task_fail(t, "Lecture de rétention impossible.");
    auto options = c->options;
    options.grouping = LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT;
    options.resume_capture_id = resume;
    options.producer_task_id = lardon3d_task_id(t);
    Lardon3DAcquisitionIngestOutput out{};
    Lardon3DAcquisitionIngestResult ir{};
    Lardon3DAppState s;
    runtime(c, s);
    auto cr = lardon3d_acquisition_campaign_materialize_group(
        &s, c->scanset, c->sources.data(), c->sources.size(), &c->plan,
        group_id, &options, &out, &ir);
    if (cr != LARDON3D_ACQUISITION_CAMPAIGN_OK ||
        ir != LARDON3D_ACQUISITION_INGEST_OK || out.group_count != 1)
      return lardon3d_task_fail(t, "Échec de matérialisation de campagne.");
#ifdef LARDON3D_ACQUISITION_CAMPAIGN_TASK_TESTING
    const char *before_retention =
        std::getenv("LARDON3D_TEST_CAMPAIGN_FAIL_BEFORE_RETENTION");
    if (before_retention && std::strcmp(before_retention, "1") == 0)
      return lardon3d_task_fail(t, "Échec injecté avant rétention.");
#endif
    if (lardon3d_project_db_retain_acquisition_campaign_capture(
            c->db, lardon3d_task_id(t), group_id, out.groups[0].capture_id,
            group_id) != LARDON3D_PROJECT_DB_OK)
      return lardon3d_task_fail(t, "Rétention de Capture impossible.");
    /* Limite de reprise acceptée: entre le retour de S3-E et cette rétention
     * durable, une identité de capture ne peut pas être déduite à posteriori
     * depuis les chemins/métadonnées/ID d'image.
     */
#ifdef LARDON3D_ACQUISITION_CAMPAIGN_TASK_TESTING
    const char *after_retention =
        std::getenv("LARDON3D_TEST_CAMPAIGN_FAIL_AFTER_RETENTION");
    if (after_retention && std::strcmp(after_retention, "1") == 0)
      return lardon3d_task_fail(t, "Échec injecté après rétention.");
#endif
    unsigned progress = static_cast<unsigned>(
        (uint64_t(group_id) * 100u / c->plan.group_count));
    if (!lardon3d_task_set_progress(t, progress,
                                    "Groupe de campagne matérialisé.") ||
        !checkpoint(c, t, group_id))
      return lardon3d_task_fail(t, "Checkpoint de campagne impossible.");
    if (group_id < c->plan.group_count) {
      // sequence_break: libère la réservation courante et renégocie l'admission
      // via le Governor avant le groupe suivant.
      Lardon3DTaskExecutionContract contract{};
      Lardon3DResourceReservation *reservation = nullptr;
      if (!lardon3d_task_sequence_break(t, c->governor, &reservation,
                                        &contract))
        return false;
    }
  }
  return true;
}
bool run(Lardon3DTask *t, void *p) noexcept {
  try {
    return run_impl(t, p);
  } catch (const std::bad_alloc &) {
    return lardon3d_task_fail(t, "Mémoire insuffisante pour la campagne.");
  } catch (...) {
    return lardon3d_task_fail(t, "Erreur interne de campagne.");
  }
}
void finished(const Lardon3DTask *t, void *p) {
  try {
    auto *c = static_cast<Context *>(p);
    Lardon3DAppState s;
    runtime(c, s);
    (void)lardon3d_project_checkpoint_task(&s, t);
  } catch (...) {
  }
}

Context *make_context(const char *path, Lardon3DProjectDb *db,
                      Lardon3DResourceGovernor *g, uint64_t scanset,
                      const Lardon3DAcquisitionCampaignTaskRequest &r,
                      const unsigned char *encoded, size_t n) {
  if (!path || !path[0])
    return nullptr;
  std::unique_ptr<Context> c(new (std::nothrow) Context);
  if (!c)
    return nullptr;
  const int path_size =
      std::snprintf(c->project_path, sizeof(c->project_path), "%s", path);
  if (path_size < 0 || static_cast<size_t>(path_size) >= sizeof(c->project_path))
    return nullptr;
  c->db = db;
  c->governor = g;
  c->scanset = scanset;
  c->sources.assign(r.sources, r.sources + r.source_count);
  c->confirmations.assign(r.confirmations,
                          r.confirmations + r.confirmation_count);
  c->options = r.ingest_options;
  c->encoded.assign(encoded, encoded + n);
  if (lardon3d_acquisition_campaign_plan(c->sources.data(), c->sources.size(),
                                         c->confirmations.data(),
                                         c->confirmations.size(), &c->plan) !=
          LARDON3D_ACQUISITION_CAMPAIGN_OK ||
      c->plan.group_count == 0)
    return nullptr;
  return c.release();
}
} // namespace

bool request_encode_impl(
    const Lardon3DAcquisitionCampaignTaskRequest *r, unsigned char *out,
    size_t cap, size_t *size) {
  if (size)
    *size = 0;
  if (!size)
    return false;
  Writer measure{nullptr, max_request_size, 0};
  if (!encode(r, measure) || measure.used > max_request_size) {
    return false;
  }
  *size = measure.used;
  if (!out)
    return cap == 0;
  if (cap < measure.used)
    return false;
  Writer writer{out, cap, 0};
  return encode(r, writer) && writer.used == measure.used;
}

bool request_decode_impl(
    const unsigned char *input, size_t size,
    Lardon3DAcquisitionCampaignSource *sources, size_t sc,
    Lardon3DAcquisitionCampaignConfirmation *confirmations, size_t cc,
    Lardon3DAcquisitionCampaignTaskRequest *r) {
  if (!input || !r || size > max_request_size)
    return false;
  std::memset(r, 0, sizeof(*r));
  Reader q{input, size};
  unsigned char m[8];
  uint32_t version, n, c, rep, select;
  uint64_t imported, maxbytes;
  /* Version 1 is the only accepted codec shape; any mismatch rejects replay. */
  if (!q.bytes(m, 8) || std::memcmp(m, magic, 8) || !q.u32(version) ||
      version != 1 || !q.u32(n) || n == 0 || n > sc ||
      n > LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES || !q.u32(c) || c > cc ||
      c > n || !sources || (c > 0 && !confirmations) || !q.u32(rep) ||
      rep < 1 || rep > 2 || !q.u32(select) || select > 1 ||
      !q.u64(imported) || imported > INT64_MAX || !q.u64(maxbytes) ||
      maxbytes == 0u)
    return false;
  std::memset(sources, 0, n * sizeof(*sources));
  std::memset(confirmations, 0, c * sizeof(*confirmations));
  for (uint32_t i = 0; i < n; ++i) {
    auto &s = sources[i];
    auto &md = s.metadata;
    uint32_t sk, mr, msk;
    if (!q.text(s.path, sizeof(s.path)) || !q.u32(sk) || sk > 2 || !q.u32(mr) ||
        mr > LARDON3D_ACQUISITION_INTERNAL_ERROR || !q.u32(md.policy_version) ||
        !q.u32(msk) || msk > 2 || !q.u32(md.present_fields) ||
        !q.text(md.make, sizeof(md.make)) ||
        !q.text(md.model, sizeof(md.model)) ||
        !q.text(md.body_serial, sizeof(md.body_serial)) ||
        !q.text(md.datetime_original, sizeof(md.datetime_original)) ||
        !q.text(md.subsec_original, sizeof(md.subsec_original)) ||
        !q.text(md.offset_original, sizeof(md.offset_original)) ||
        !q.text(md.image_unique_id, sizeof(md.image_unique_id)) ||
        !q.u32(md.width) || !q.u32(md.height) || !q.u16(md.orientation))
      return false;
    s.source_kind = static_cast<Lardon3DAcquisitionSourceKind>(sk);
    s.metadata_result = static_cast<Lardon3DAcquisitionResult>(mr);
    md.source_kind = static_cast<Lardon3DAcquisitionSourceKind>(msk);
  }
  for (uint32_t i = 0; i < c; ++i) {
    uint32_t count;
    if (!q.u32(count) || count == 0 ||
        count > LARDON3D_ACQUISITION_INGEST_MAX_SOURCES)
      return false;
    confirmations[i].source_count = count;
    for (uint32_t j = 0; j < count; ++j) {
      uint32_t index;
      if (!q.u32(index) || index >= n)
        return false;
      confirmations[i].source_indices[j] = index;
    }
  }
  if (q.left)
    return false;
  r->sources = sources;
  r->source_count = n;
  r->confirmations = confirmations;
  r->confirmation_count = c;
  r->ingest_options.representation =
      static_cast<Lardon3DAcquisitionRepresentation>(rep);
  r->ingest_options.select_representation = static_cast<uint8_t>(select);
  r->ingest_options.imported_at = static_cast<int64_t>(imported);
  r->ingest_options.max_source_bytes = maxbytes;
  std::unique_ptr<Lardon3DAcquisitionCampaignPlan> validated(
      new (std::nothrow) Lardon3DAcquisitionCampaignPlan{});
  return validated &&
         lardon3d_acquisition_campaign_plan(sources, n, confirmations, c,
                                            validated.get()) ==
             LARDON3D_ACQUISITION_CAMPAIGN_OK &&
         validated->group_count > 0;
}

bool task_reconstruct_impl(
    const Lardon3DTaskDurableSnapshot *s, void *userdata,
    Lardon3DTaskKindBinding *b) {
  auto *rt = static_cast<Lardon3DTaskReconstructionContext *>(userdata);
  if (!s || !rt || !b)
    return false;
  std::vector<unsigned char> blob(max_request_size);
  Lardon3DProjectDbAcquisitionCampaignTask p{};
  if (lardon3d_project_db_load_acquisition_campaign_task(
          rt->project_db, s->id, blob.data(), blob.size(), &p) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  std::vector<Lardon3DAcquisitionCampaignSource> sources(
      LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES);
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations(
      LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES);
  Lardon3DAcquisitionCampaignTaskRequest r{};
  if (!lardon3d_acquisition_campaign_request_decode(
          blob.data(), p.request_size, sources.data(), sources.size(),
          confirmations.data(), confirmations.size(), &r))
    return false;
  auto *c =
      make_context(rt->project_path, rt->project_db, rt->resource_governor,
                   p.scanset_id, r, blob.data(), p.request_size);
  if (!c)
    return false;
  // This Task kind has no legacy operational-resource reconciliation. Keep
  // every optional Registry hook explicit so a future binding extension
  // cannot accidentally inherit non-null recovery behavior.
  *b = {};
  b->callback = run;
  b->userdata = c;
  b->userdata_destroy = destroy;
  b->finished_callback = finished;
  b->finished_userdata = c;
  return true;
}

Lardon3DTask *create_task_impl(
    Lardon3DAppState *s, uint64_t scanset,
    const Lardon3DAcquisitionCampaignTaskRequest *r, uint64_t *id) {
  if (id)
    *id = 0;
  if (!s || !s->project_loaded || !s->project_db || !s->resource_governor ||
      !r || !id)
    return nullptr;
  Lardon3DProjectDbScanSet ss{};
  if (lardon3d_project_db_load_scanset(s->project_db, scanset, &ss) !=
      LARDON3D_PROJECT_DB_OK)
    return nullptr;
  size_t n = 0;
  if (!lardon3d_acquisition_campaign_request_encode(r, nullptr, 0, &n))
    return nullptr;
  std::vector<unsigned char> blob(n);
  if (!lardon3d_acquisition_campaign_request_encode(r, blob.data(), blob.size(),
                                                    &n))
    return nullptr;
  auto *c = make_context(s->project_path, s->project_db, s->resource_governor,
                         scanset, *r, blob.data(), n);
  if (!c)
    return nullptr;
  uint64_t task_id;
  if (lardon3d_project_db_allocate_task_id(s->project_db, &task_id) !=
      LARDON3D_PROJECT_DB_OK) {
    delete c;
    return nullptr;
  }
  Lardon3DResourceEstimate e{
      256 * 1024, 0, 64 * 1024, 0, 1,
      1,          1, 0,         1, LARDON3D_RESOURCE_TASK_IMPORT};
  auto *t = lardon3d_task_create_typed("Campagne d'acquisition", &e,
                                       LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
                                       1, run, c, destroy);
  if (!t || !lardon3d_task_assign_id(t, task_id) ||
      !lardon3d_task_set_finished_callback(t, finished, c) ||
      !checkpoint(c, t, 0)) {
    lardon3d_task_destroy(t);
    return nullptr;
  }
  *id = task_id;
  return t;
}

/* C ABI boundary: exceptions are trapped so callers never receive C++ throws. */
extern "C" bool lardon3d_acquisition_campaign_request_encode(
    const Lardon3DAcquisitionCampaignTaskRequest *r, unsigned char *out,
    size_t cap, size_t *size) {
  try {
    return request_encode_impl(r, out, cap, size);
  } catch (const std::bad_alloc &) {
    if (size)
      *size = 0;
    return false;
  } catch (...) {
    if (size)
      *size = 0;
    return false;
  }
}

extern "C" bool lardon3d_acquisition_campaign_request_decode(
    const unsigned char *input, size_t size,
    Lardon3DAcquisitionCampaignSource *sources, size_t sc,
    Lardon3DAcquisitionCampaignConfirmation *confirmations, size_t cc,
    Lardon3DAcquisitionCampaignTaskRequest *r) {
  try {
    return request_decode_impl(input, size, sources, sc, confirmations, cc, r);
  } catch (const std::bad_alloc &) {
    return false;
  } catch (...) {
    return false;
  }
}

extern "C" bool lardon3d_acquisition_campaign_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *s, void *userdata,
    Lardon3DTaskKindBinding *b) {
  try {
    return task_reconstruct_impl(s, userdata, b);
  } catch (const std::bad_alloc &) {
    return false;
  } catch (...) {
    return false;
  }
}

extern "C" Lardon3DTask *lardon3d_project_create_acquisition_campaign_task(
    Lardon3DAppState *s, uint64_t scanset,
    const Lardon3DAcquisitionCampaignTaskRequest *r, uint64_t *id) {
  if (id)
    *id = 0;
  try {
    return create_task_impl(s, scanset, r, id);
  } catch (const std::bad_alloc &) {
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

extern "C" bool lardon3d_project_enqueue_acquisition_campaign(
    Lardon3DAppState *s, uint64_t scanset,
    const Lardon3DAcquisitionCampaignTaskRequest *r, uint64_t *id) {
  try {
  if (!s || !s->task_queue)
    return false;
  auto *t =
      lardon3d_project_create_acquisition_campaign_task(s, scanset, r, id);
  if (!t)
    return false;
  if (!lardon3d_task_queue_add(s->task_queue, t, nullptr)) {
    lardon3d_task_destroy(t);
    return false;
  }
  return true;
  } catch (const std::bad_alloc &) {
    return false;
  } catch (...) {
    return false;
  }
}
