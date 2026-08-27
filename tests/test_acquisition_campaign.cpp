#include <lardon3d/acquisition_campaign.h>
#include <libexif/exif-data.h>
extern "C" {
#include <lardon3d/project_db.h>
}
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

void source(Lardon3DAcquisitionCampaignSource &s, const char *path, const char *id = nullptr,
            const char *serial = nullptr) {
  s = {}; std::snprintf(s.path, sizeof(s.path), "%s", path);
  s.source_kind = std::strstr(path, ".arw") ? LARDON3D_ACQUISITION_SOURCE_RAW : LARDON3D_ACQUISITION_SOURCE_JPEG;
  s.metadata_result = LARDON3D_ACQUISITION_OK;
  s.metadata.policy_version = LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
  s.metadata.source_kind = s.source_kind;
  if (id) { s.metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID;
    std::snprintf(s.metadata.image_unique_id, sizeof(s.metadata.image_unique_id), "%s", id); }
  if (serial) { s.metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_BODY_SERIAL;
    std::snprintf(s.metadata.body_serial, sizeof(s.metadata.body_serial), "%s", serial); }
}

bool remove_tree(const char *path) {
  struct stat st{}; if (lstat(path, &st) != 0) return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
  DIR *dir = opendir(path); if (!dir) return false; bool ok = true;
  for (dirent *e = readdir(dir); e; e = readdir(dir)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    char child[4096]{}; int n = std::snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(child) || !remove_tree(child)) ok = false;
  }
  return closedir(dir) == 0 && rmdir(path) == 0 && ok;
}

void ascii(ExifData *d, ExifIfd ifd, ExifTag tag, const char *text) {
  ExifEntry *e = exif_entry_new(); e->tag = tag; e->format = EXIF_FORMAT_ASCII;
  e->components = std::strlen(text) + 1; e->size = static_cast<unsigned>(e->components);
  e->data = static_cast<unsigned char *>(std::malloc(e->size)); std::memcpy(e->data, text, e->size);
  exif_content_add_entry(d->ifd[ifd], e); exif_entry_unref(e);
}

bool jpeg(const char *path, const char *id) {
  ExifData *d = exif_data_new(); if (!d) return false; exif_data_set_byte_order(d, EXIF_BYTE_ORDER_INTEL);
  ascii(d, EXIF_IFD_0, EXIF_TAG_MODEL, "Campaign test camera");
  ascii(d, EXIF_IFD_EXIF, EXIF_TAG_IMAGE_UNIQUE_ID, id);
  unsigned char *encoded = nullptr; unsigned encoded_size = 0; exif_data_save_data(d, &encoded, &encoded_size);
  FILE *f = std::fopen(path, "wb"); unsigned segment = encoded_size + 2;
  const unsigned char head[] = {0xff, 0xd8, 0xff, 0xe1, static_cast<unsigned char>(segment >> 8),
                                static_cast<unsigned char>(segment)};
  const unsigned char tail[] = {0xff, 0xd9};
  bool ok = f && encoded && segment <= UINT16_MAX && std::fwrite(head, 1, sizeof(head), f) == sizeof(head) &&
      std::fwrite(encoded, 1, encoded_size, f) == encoded_size && std::fwrite(tail, 1, 2, f) == 2 && std::fclose(f) == 0;
  if (f && !ok) std::fclose(f);
  std::free(encoded);
  exif_data_unref(d);
  return ok;
}

bool empty_file(const char *path) {
  FILE *file = std::fopen(path, "wb");
  return file != nullptr && std::fclose(file) == 0;
}

struct Fixture {
  char root[4096] = "/tmp/lardon3d-campaign-XXXXXX", db_path[4096]{};
  Lardon3DProjectDb *db{}; Lardon3DAppState state{}; Lardon3DProjectDbScanSet scanset{};
  bool open() {
    if (!mkdtemp(root) || std::snprintf(db_path, sizeof(db_path), "%s/project.db", root) <= 0) return false;
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
    if (lardon3d_project_db_open(db_path, &db, error) != LARDON3D_PROJECT_DB_OK) return false;
    state.project_loaded = true; state.project_db = db;
    std::snprintf(state.project_path, sizeof(state.project_path), "%s", root);
    return lardon3d_project_db_create_scanset(db, "campaign", &scanset) == LARDON3D_PROJECT_DB_OK;
  }
  bool path(char out[4096], const char *name) { int n = std::snprintf(out, 4096, "%s/%s", root, name); return n > 0 && n < 4096; }
  ~Fixture() { if (db) lardon3d_project_db_close(db); (void)remove_tree(root); }
};

size_t captures(Fixture &f) { Lardon3DProjectDbCapture c[16]{}; size_t n = 0;
  return lardon3d_project_db_list_captures(f.db, f.scanset.scanset_id, 0, c, 16, &n) == LARDON3D_PROJECT_DB_OK ? n : SIZE_MAX; }
Lardon3DAcquisitionIngestOptions options() { Lardon3DAcquisitionIngestOptions o{};
  o.grouping = LARDON3D_ACQUISITION_GROUP_AUTOMATIC; o.representation = LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE;
  o.select_representation = 1; o.imported_at = 17; o.max_source_bytes = 1024 * 1024; return o; }

void planner_tests() {
  Lardon3DAcquisitionCampaignSource v[4]{};
  source(v[0], "/a/0.arw", "same", "camera-a"); source(v[1], "/a/1.jpg", "same", "camera-a");
  source(v[2], "/a/2.arw", "same", "camera-b"); source(v[3], "/a/3.jpg", "other", "camera-a");
  Lardon3DAcquisitionCampaignPlan p{};
  CHECK(lardon3d_acquisition_campaign_plan(v, 4, nullptr, 0, &p) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(p.summary.strong_group_count == 1 && p.groups[0].source_indices[0] == 0 && p.groups[0].source_indices[1] == 1);

  Lardon3DAcquisitionCampaignSource explicit_sources[4]{};
  source(explicit_sources[0], "/e/0.jpg"); source(explicit_sources[1], "/e/1.jpg");
  source(explicit_sources[2], "/e/2.jpg"); source(explicit_sources[3], "/e/3.jpg");
  Lardon3DAcquisitionCampaignConfirmation x[2]{}; x[0].source_count = 3;
  x[0].source_indices[0] = 2; x[0].source_indices[1] = 0; x[0].source_indices[2] = 1;
  CHECK(lardon3d_acquisition_campaign_plan(explicit_sources, 4, x, 1, &p) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(p.groups[0].basis == LARDON3D_ACQUISITION_GROUP_EXPLICIT && p.groups[0].source_count == 3 &&
        p.groups[0].source_indices[0] == 0 && p.groups[0].source_indices[2] == 2);
  x[0].source_count = 2; x[0].source_indices[0] = 0; x[0].source_indices[1] = 0;
  CHECK(lardon3d_acquisition_campaign_plan(explicit_sources, 4, x, 1, &p) == LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT);
  x[0].source_indices[1] = 4;
  CHECK(lardon3d_acquisition_campaign_plan(explicit_sources, 4, x, 1, &p) == LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT);
  x[0].source_indices[1] = 1; x[1].source_count = 2; x[1].source_indices[0] = 1; x[1].source_indices[1] = 2;
  CHECK(lardon3d_acquisition_campaign_plan(explicit_sources, 4, x, 2, &p) == LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT);

  Lardon3DAcquisitionCampaignSource a[4]{v[0], v[1], v[2], v[3]}, b[4]{v[3], v[1], v[0], v[2]};
  auto less = [](const auto &l, const auto &r) { return std::strcmp(l.path, r.path) < 0; };
  std::sort(a, a + 4, less); std::sort(b, b + 4, less);
  Lardon3DAcquisitionCampaignPlan pa{}, pb{};
  CHECK(lardon3d_acquisition_campaign_plan(a, 4, nullptr, 0, &pa) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(lardon3d_acquisition_campaign_plan(b, 4, nullptr, 0, &pb) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(std::memcmp(&pa, &pb, sizeof(pa)) == 0);

  constexpr size_t weak_source_count = 100;
  auto weak = std::make_unique<Lardon3DAcquisitionCampaignSource[]>(weak_source_count);
  for (size_t i = 0; i < weak_source_count - 2u; ++i) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/weak/%03zu.jpg", i);
    source(weak[i], path, nullptr, "shared-camera");
  }
  source(weak[weak_source_count - 2u], "/weak/match.arw", nullptr, "shared-camera");
  source(weak[weak_source_count - 1u], "/weak/match.jpg", nullptr, "shared-camera");
  std::sort(weak.get(), weak.get() + weak_source_count, less);
  auto weak_plan = std::make_unique<Lardon3DAcquisitionCampaignPlan>();
  CHECK(lardon3d_acquisition_campaign_plan(weak.get(), weak_source_count, nullptr, 0,
                                           weak_plan.get()) ==
        LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(weak_plan->proposal_count == 1u);
  CHECK(weak_plan->summary.candidate_pair_count == 1u);
  CHECK(weak_plan->summary.insufficient_pair_count == 4949u);
  CHECK(weak_plan->proposals[0].kind == LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_CANDIDATE);
  CHECK(std::strstr(weak[weak_plan->proposals[0].left_source_index].path, "/match.") != nullptr);
  CHECK(std::strstr(weak[weak_plan->proposals[0].right_source_index].path, "/match.") != nullptr);
}

void materialization_tests() {
  Fixture f; CHECK(f.open()); if (!f.db) return; char paths[3][4096]{};
  CHECK(f.path(paths[0], "a.JPG") && f.path(paths[1], "b.JPG") && f.path(paths[2], "c.JPG"));
  CHECK(jpeg(paths[0], "a") && jpeg(paths[1], "b") && jpeg(paths[2], "c"));
  Lardon3DAcquisitionCampaignSource s[3]{}; source(s[0], paths[0]); source(s[1], paths[1]); source(s[2], paths[2]);
  Lardon3DAcquisitionCampaignConfirmation c[2]{}; c[0].source_count = 2; c[0].source_indices[0] = 0; c[0].source_indices[1] = 1;
  c[1].source_count = 1; c[1].source_indices[0] = 2; Lardon3DAcquisitionCampaignPlan p{};
  CHECK(lardon3d_acquisition_campaign_plan(s, 3, c, 2, &p) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  auto o = options(); Lardon3DAcquisitionIngestOutput out{}; Lardon3DAcquisitionIngestResult ir{};
  CHECK(lardon3d_acquisition_campaign_materialize_group(&f.state, f.scanset.scanset_id, s, 3, &p, 1, &o, &out, &ir) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(ir == LARDON3D_ACQUISITION_INGEST_OK && out.group_count == 1 && out.groups[0].source_count == 2 && captures(f) == 1);
  uint64_t capture_id = out.groups[0].capture_id, image_count = 0, selected = 0; size_t asset_count = 0;
  Lardon3DProjectDbCaptureAsset assets[4]{};
  CHECK(lardon3d_project_db_list_capture_assets(f.db, capture_id, 0, assets, 4, &asset_count) == LARDON3D_PROJECT_DB_OK && asset_count == 2);
  CHECK(assets[0].role == LARDON3D_DB_CAPTURE_ASSET_SOURCE && assets[1].role == LARDON3D_DB_CAPTURE_ASSET_SOURCE);
  CHECK(lardon3d_project_db_count_images(f.db, f.scanset.scanset_id, &image_count) == LARDON3D_PROJECT_DB_OK && image_count == 1);
  CHECK(lardon3d_project_db_get_selected_capture_image(f.db, capture_id, &selected) == LARDON3D_PROJECT_DB_OK && selected != 0);
  o.resume_capture_id = capture_id;
  CHECK(lardon3d_acquisition_campaign_materialize_group(&f.state, f.scanset.scanset_id, s, 3, &p, 1, &o, &out, &ir) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(out.groups[0].capture_id == capture_id && captures(f) == 1);
  asset_count = 0; CHECK(lardon3d_project_db_list_capture_assets(f.db, capture_id, 0, assets, 4, &asset_count) == LARDON3D_PROJECT_DB_OK && asset_count == 2);
  CHECK(lardon3d_project_db_count_images(f.db, f.scanset.scanset_id, &image_count) == LARDON3D_PROJECT_DB_OK && image_count == 1);
  o.resume_capture_id = 0;
  CHECK(lardon3d_acquisition_campaign_materialize_group(&f.state, f.scanset.scanset_id, s, 3, &p, 2, &o, &out, &ir) == LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(out.groups[0].capture_id != capture_id && captures(f) == 2);
}

bool same_discovery(const Lardon3DAcquisitionCampaignDiscovery &a, const Lardon3DAcquisitionCampaignDiscovery &b) {
  return a.source_count == b.source_count && !std::memcmp(&a.summary, &b.summary, sizeof(a.summary)) &&
      !std::memcmp(a.sources, b.sources, a.source_count * sizeof(a.sources[0]));
}

void discovery_tests() {
  char base[] = "/tmp/lardon3d-campaign-discovery-XXXXXX";
  CHECK(mkdtemp(base) != nullptr);
  char left[4096]{}, right[4096]{}, nested[4096]{}, path[4096]{}, target[4096]{};
  CHECK(std::snprintf(left, sizeof(left), "%s/left", base) > 0);
  CHECK(std::snprintf(right, sizeof(right), "%s/right", base) > 0);
  CHECK(std::snprintf(nested, sizeof(nested), "%s/nested", left) > 0);
  CHECK(mkdir(left, 0700) == 0 && mkdir(right, 0700) == 0 && mkdir(nested, 0700) == 0);
  CHECK(std::snprintf(path, sizeof(path), "%s/good.jpeg", left) > 0 && jpeg(path, "valid-id"));
  CHECK(std::snprintf(path, sizeof(path), "%s/bad.JPG", left) > 0 && empty_file(path));
  CHECK(std::snprintf(path, sizeof(path), "%s/raw.ARW", right) > 0 && empty_file(path));
  CHECK(std::snprintf(path, sizeof(path), "%s/other.jpg", right) > 0 && empty_file(path));
  CHECK(std::snprintf(path, sizeof(path), "%s/ignored.txt", left) > 0 && empty_file(path));
  CHECK(std::snprintf(path, sizeof(path), "%s/hidden.jpg", nested) > 0 && jpeg(path, "nested"));
  CHECK(std::snprintf(target, sizeof(target), "%s/good.jpeg", left) > 0);
  CHECK(std::snprintf(path, sizeof(path), "%s/link.jpg", left) > 0 && symlink(target, path) == 0);
  CHECK(std::snprintf(path, sizeof(path), "%s/pipe.jpeg", right) > 0 && mkfifo(path, 0600) == 0);

  Lardon3DAcquisitionCampaignRoot roots[2]{};
  std::snprintf(roots[0].path, sizeof(roots[0].path), "%s", right);
  std::snprintf(roots[1].path, sizeof(roots[1].path), "%s", left);
  auto first = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  auto second = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  CHECK(lardon3d_acquisition_campaign_discover(roots, 2, first.get()) ==
        LARDON3D_ACQUISITION_CAMPAIGN_OK);
  std::swap(roots[0], roots[1]);
  CHECK(lardon3d_acquisition_campaign_discover(roots, 2, second.get()) ==
        LARDON3D_ACQUISITION_CAMPAIGN_OK);
  CHECK(same_discovery(*first, *second));
  CHECK(first->source_count == 4 && first->summary.discovered_entry_count == 8 &&
        first->summary.supported_source_count == 4 && first->summary.unsupported_entry_count == 4);
  for (size_t i = 1; i < first->source_count; ++i)
    CHECK(std::strcmp(first->sources[i - 1].path, first->sources[i].path) < 0);
  bool valid_jpeg = false, corrupt_jpeg = false;
  for (size_t i = 0; i < first->source_count; ++i) {
    if (std::strstr(first->sources[i].path, "/good.jpeg"))
      valid_jpeg = first->sources[i].metadata_result == LARDON3D_ACQUISITION_OK;
    if (std::strstr(first->sources[i].path, "/bad.JPG"))
      corrupt_jpeg = first->sources[i].metadata_result != LARDON3D_ACQUISITION_OK;
    CHECK(std::strstr(first->sources[i].path, "/nested/") == nullptr);
    CHECK(std::strstr(first->sources[i].path, "/link.jpg") == nullptr);
    CHECK(std::strstr(first->sources[i].path, "/pipe.jpeg") == nullptr);
  }
  CHECK(valid_jpeg && corrupt_jpeg);

  auto rejected = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  auto reject = [&](const Lardon3DAcquisitionCampaignRoot *r, size_t count,
                    Lardon3DAcquisitionCampaignResult expected) {
    std::memset(rejected.get(), 0xa5, sizeof(*rejected));
    CHECK(lardon3d_acquisition_campaign_discover(r, count, rejected.get()) == expected);
    auto zero = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
    CHECK(std::memcmp(rejected.get(), zero.get(), sizeof(*zero)) == 0);
  };
  Lardon3DAcquisitionCampaignRoot invalid{};
  std::snprintf(invalid.path, sizeof(invalid.path), "relative");
  reject(&invalid, 1, LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT);
  std::snprintf(invalid.path, sizeof(invalid.path), "%s/./left", base);
  reject(&invalid, 1, LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT);
  std::snprintf(invalid.path, sizeof(invalid.path), "%s/../discovery", base);
  reject(&invalid, 1, LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT);
  Lardon3DAcquisitionCampaignRoot duplicate[2]{};
  std::snprintf(duplicate[0].path, sizeof(duplicate[0].path), "%s", left);
  duplicate[1] = duplicate[0];
  reject(duplicate, 2, LARDON3D_ACQUISITION_CAMPAIGN_DUPLICATE);
  Lardon3DAcquisitionCampaignRoot too_many[LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS + 1]{};
  reject(too_many, LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS + 1,
         LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT);

  char limit_root[4096]{};
  CHECK(std::snprintf(limit_root, sizeof(limit_root), "%s/limit", base) > 0 &&
        mkdir(limit_root, 0700) == 0);
  bool files_ok = true;
  for (size_t i = 0; i <= LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES; ++i) {
    int written = std::snprintf(path, sizeof(path), "%s/%04zu.jpg", limit_root, i);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(path) || !empty_file(path)) {
      files_ok = false;
      break;
    }
  }
  CHECK(files_ok);
  Lardon3DAcquisitionCampaignRoot limit{};
  std::snprintf(limit.path, sizeof(limit.path), "%s", limit_root);
  reject(&limit, 1, LARDON3D_ACQUISITION_CAMPAIGN_LIMIT_EXCEEDED);
  CHECK(remove_tree(base));
}

int dry_run(const char *a, const char *b) {
  Lardon3DAcquisitionCampaignRoot roots[2]{}; std::snprintf(roots[0].path, sizeof(roots[0].path), "%s", a);
  std::snprintf(roots[1].path, sizeof(roots[1].path), "%s", b);
  auto d1 = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>(); auto d2 = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  auto p1 = std::make_unique<Lardon3DAcquisitionCampaignPlan>(); auto p2 = std::make_unique<Lardon3DAcquisitionCampaignPlan>();
  auto result = lardon3d_acquisition_campaign_discover(roots, 2, d1.get());
  if (result != LARDON3D_ACQUISITION_CAMPAIGN_OK) {
    std::fprintf(stderr, "CAMPAIGN_DRY_RUN_STAGE=DISCOVER_1\nCAMPAIGN_RESULT=%d\n", result);
    return 2;
  }
  std::swap(roots[0], roots[1]);
  result = lardon3d_acquisition_campaign_discover(roots, 2, d2.get());
  if (result != LARDON3D_ACQUISITION_CAMPAIGN_OK) {
    std::fprintf(stderr, "CAMPAIGN_DRY_RUN_STAGE=DISCOVER_2\nCAMPAIGN_RESULT=%d\n", result);
    return 3;
  }
  const auto plan_result_1 = lardon3d_acquisition_campaign_plan(
      d1->sources, d1->source_count, nullptr, 0, p1.get());
  const auto plan_result_2 = lardon3d_acquisition_campaign_plan(
      d2->sources, d2->source_count, nullptr, 0, p2.get());
  if (plan_result_1 != LARDON3D_ACQUISITION_CAMPAIGN_OK ||
      plan_result_2 != LARDON3D_ACQUISITION_CAMPAIGN_OK) {
    std::fprintf(stderr,
                 "CAMPAIGN_DRY_RUN_STAGE=PLAN\nCAMPAIGN_PLAN_RESULT_1=%d\n"
                 "CAMPAIGN_PLAN_RESULT_2=%d\n",
                 plan_result_1, plan_result_2);
    return 4;
  }
  size_t raw = 0, jpeg_count = 0, unavailable = 0;
  size_t raw_ok = 0, raw_unavailable = 0, raw_error = 0;
  size_t jpeg_ok = 0, jpeg_unavailable = 0, jpeg_error = 0;
  for (size_t i = 0; i < d1->source_count; ++i) {
    const bool is_raw = d1->sources[i].source_kind == LARDON3D_ACQUISITION_SOURCE_RAW;
    raw += is_raw;
    jpeg_count += !is_raw;
    const bool is_ok = d1->sources[i].metadata_result == LARDON3D_ACQUISITION_OK;
    const bool is_unavailable =
        d1->sources[i].metadata_result == LARDON3D_ACQUISITION_METADATA_UNAVAILABLE;
    unavailable += is_unavailable;
    (is_raw ? raw_ok : jpeg_ok) += is_ok;
    (is_raw ? raw_unavailable : jpeg_unavailable) += is_unavailable;
    (is_raw ? raw_error : jpeg_error) += !is_ok && !is_unavailable;
  }
  bool deterministic = same_discovery(*d1, *d2) && !std::memcmp(p1.get(), p2.get(), sizeof(*p1));
  std::printf("CAMPAIGN_RAW_COUNT=%zu\nCAMPAIGN_JPEG_COUNT=%zu\nCAMPAIGN_TOTAL_SOURCE_COUNT=%zu\n", raw, jpeg_count, d1->source_count);
  std::printf("METADATA_OK=%zu\nMETADATA_UNAVAILABLE=%zu\nMETADATA_ERROR=%zu\n", d1->summary.metadata_ok_count, unavailable, d1->summary.metadata_error_count - unavailable);
  std::printf("RAW_METADATA_OK=%zu\nRAW_METADATA_UNAVAILABLE=%zu\nRAW_METADATA_ERROR=%zu\n",
              raw_ok, raw_unavailable, raw_error);
  std::printf("JPEG_METADATA_OK=%zu\nJPEG_METADATA_UNAVAILABLE=%zu\nJPEG_METADATA_ERROR=%zu\n",
              jpeg_ok, jpeg_unavailable, jpeg_error);
  std::printf("STRONG_GROUPS=%zu\nCANDIDATE_PAIRS=%zu\nAMBIGUOUS_PAIRS=%zu\nCONTRADICTORY_PAIRS=%zu\nINSUFFICIENT_PAIRS=%zu\nUNPAIRED_SOURCES=%zu\n",
      p1->summary.strong_group_count, p1->summary.candidate_pair_count, p1->summary.ambiguous_pair_count,
      p1->summary.contradictory_pair_count, p1->summary.insufficient_pair_count, p1->summary.unresolved_source_count);
  /* With no confirmations this is the automatic plan's strong groups plus singletons. */
  std::printf("PROPOSED_PHYSICAL_CAPTURE_COUNT=%zu\nCAMPAIGN_PLAN_DETERMINISTIC=%s\n", p1->group_count, deterministic ? "PASS" : "FAIL");
  if (!deterministic) std::fprintf(stderr, "CAMPAIGN_DRY_RUN_STAGE=DETERMINISM\nCAMPAIGN_RESULT=FAIL\n");
  return deterministic ? 0 : 5;
}
}  // namespace

int main(int argc, char **argv) {
  if (argc == 4 && !std::strcmp(argv[1], "--dry-run")) return dry_run(argv[2], argv[3]);
  if (argc != 1) { std::fprintf(stderr, "usage: %s [--dry-run ROOT1 ROOT2]\n", argv[0]); return 64; }
  discovery_tests(); planner_tests(); materialization_tests(); return failures ? 1 : 0;
}
