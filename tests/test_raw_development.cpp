#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <lardon3d/image_catalog.h>
}
#include <lardon3d/raw_development.h>

extern "C" size_t lardon3d_raw_development_test_policy_payload_size(void);
extern "C" Lardon3DRawDevelopmentResult lardon3d_raw_development_test_ensure_derivation(
    Lardon3DProjectDb *database, const Lardon3DProjectDbAssetDerivation *expected);
extern "C" Lardon3DRawDevelopmentResult
lardon3d_raw_development_test_ensure_capture_asset_role(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t asset_id,
    Lardon3DProjectDbCaptureAssetRole expected_role);

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "Failure line %d: %s\\n", __LINE__, #condition); return false; \
} } while (0)

static bool write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  size_t size = std::strlen(text);
  bool ok = write(fd, text, size) == (ssize_t)size;
  return close(fd) == 0 && ok;
}

static bool remove_tree(const char *path) {
  struct stat info;
  if (lstat(path, &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
  DIR *directory = opendir(path);
  if (!directory) return false;
  bool ok = true;
  for (dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    char child[4096];
    int count = std::snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (count <= 0 || count >= (int)sizeof(child) || !remove_tree(child)) ok = false;
  }
  return closedir(directory) == 0 && rmdir(path) == 0 && ok;
}

static void print_hex(const char *key, const unsigned char value[32]) {
  static const char digits[] = "0123456789abcdef";
  char text[65];
  for (size_t index = 0; index < 32; ++index) {
    text[index * 2] = digits[value[index] >> 4];
    text[index * 2 + 1] = digits[value[index] & 15U];
  }
  text[64] = '\0';
  std::printf("%s=%s\n", key, text);
}

static bool capture_has_asset(Lardon3DProjectDb *db, uint64_t capture_id,
    uint64_t asset_id, Lardon3DProjectDbCaptureAssetRole role) {
  Lardon3DProjectDbCaptureAsset assets[4];
  size_t count = 0;
  if (lardon3d_project_db_list_capture_assets(db, capture_id, 0, assets, 4, &count)
      != LARDON3D_PROJECT_DB_OK) return false;
  for (size_t index = 0; index < count; ++index) {
    if (assets[index].asset_id == asset_id && assets[index].role == role) return true;
  }
  return false;
}

static bool run_golden(Lardon3DAppState *state, Lardon3DProjectDb *db,
    const char *golden_path, bool keep, const char *root) {
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_project_db_create_scanset(db, "A6000 golden", &scanset)
        == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbCapture capture;
  CHECK(lardon3d_project_db_create_capture(db, scanset.scanset_id, 2, &capture)
        == LARDON3D_PROJECT_DB_OK);

  Lardon3DRawDevelopmentOutput first;
  Lardon3DRawDevelopmentResult first_result = lardon3d_raw_develop_to_capture(
      state, capture.capture_id, golden_path, 0, 2, &first);
  std::fprintf(stderr, "GOLDEN_FIRST_RESULT=%d\n", (int)first_result);
  CHECK(first_result == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(first.source_asset.asset_id != 0 && first.derived_asset.asset_id != 0
        && first.image.image_id != 0);
  CHECK(first.source_asset.asset_id != first.derived_asset.asset_id);
  Lardon3DProjectDbCapture image_capture;
  CHECK(lardon3d_project_db_find_capture_for_image(db, first.image.image_id, &image_capture)
        == LARDON3D_PROJECT_DB_OK && image_capture.capture_id == capture.capture_id);
  CHECK(capture_has_asset(db, capture.capture_id, first.source_asset.asset_id,
      LARDON3D_DB_CAPTURE_ASSET_SOURCE));
  CHECK(capture_has_asset(db, capture.capture_id, first.derived_asset.asset_id,
      LARDON3D_DB_CAPTURE_ASSET_DERIVED));
  Lardon3DProjectDbAssetDerivation derivation;
  CHECK(lardon3d_project_db_load_asset_derivation(db, first.derived_asset.asset_id, &derivation)
        == LARDON3D_PROJECT_DB_OK);
  CHECK(derivation.parent_asset_id == first.source_asset.asset_id
        && derivation.child_asset_id == first.derived_asset.asset_id);
  CHECK(std::memcmp(derivation.parameter_fingerprint, first.parameter_fingerprint,
      sizeof(first.parameter_fingerprint)) == 0);
  uint64_t selected = 0;
  CHECK(lardon3d_project_db_get_selected_capture_image(db, capture.capture_id, &selected)
        == LARDON3D_PROJECT_DB_NOT_FOUND);
  CHECK(first.width > 0 && first.height > 0 && first.width <= 16384 && first.height <= 16384
        && (uint64_t)first.width * first.height <= 40000000U);
  for (size_t index = 0; index < 4; ++index)
    CHECK(std::isfinite(first.resolved_camera_wb[index]) && first.resolved_camera_wb[index] > 0.0F);

  char png_path[4096];
  CHECK(std::snprintf(png_path, sizeof(png_path), "%s/%s", state->project_path,
      first.derived_asset.path) > 0);
  struct stat png_info;
  CHECK(stat(png_path, &png_info) == 0 && S_ISREG(png_info.st_mode) && png_info.st_size > 0);

  std::printf("GOLDEN_WIDTH=%u\n", first.width);
  std::printf("GOLDEN_HEIGHT=%u\n", first.height);
  for (size_t index = 0; index < 4; ++index)
    std::printf("GOLDEN_WB_%zu=%.9g\n", index, first.resolved_camera_wb[index]);
  print_hex("GOLDEN_SOURCE_SHA256", first.source_asset.sha256);
  print_hex("GOLDEN_DERIVED_SHA256", first.derived_asset.sha256);
  print_hex("GOLDEN_PARAMETER_FINGERPRINT", first.parameter_fingerprint);
  std::printf("GOLDEN_SOURCE_ASSET_ID=%llu\n",
      (unsigned long long)first.source_asset.asset_id);
  std::printf("GOLDEN_DERIVED_ASSET_ID=%llu\n",
      (unsigned long long)first.derived_asset.asset_id);
  std::printf("GOLDEN_IMAGE_ID=%llu\n", (unsigned long long)first.image.image_id);
  std::printf("GOLDEN_LIBRAW=%s\n", first.libraw_version);
  std::printf("GOLDEN_PNG_ENCODER=%s\n", first.png_encoder_version);
  std::printf("GOLDEN_PNG_PATH=%s\n", png_path);
  std::printf("GOLDEN_PNG_BYTES=%llu\n", (unsigned long long)png_info.st_size);

  Lardon3DRawDevelopmentOutput second;
  Lardon3DRawDevelopmentResult second_result = lardon3d_raw_develop_to_capture(
      state, capture.capture_id, golden_path, 0, 2, &second);
  std::fprintf(stderr, "GOLDEN_SECOND_RESULT=%d\n", (int)second_result);
  CHECK(second_result == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(second.source_asset.asset_id == first.source_asset.asset_id
        && second.derived_asset.asset_id == first.derived_asset.asset_id
        && second.image.image_id == first.image.image_id
        && std::memcmp(second.source_asset.sha256, first.source_asset.sha256, 32) == 0
        && std::memcmp(second.derived_asset.sha256, first.derived_asset.sha256, 32) == 0
        && std::memcmp(second.parameter_fingerprint, first.parameter_fingerprint, 32) == 0
        && second.width == first.width && second.height == first.height
        && std::memcmp(second.resolved_camera_wb, first.resolved_camera_wb,
            sizeof(first.resolved_camera_wb)) == 0);
  uint64_t image_count = 0;
  CHECK(lardon3d_project_db_count_images(db, scanset.scanset_id, &image_count)
        == LARDON3D_PROJECT_DB_OK && image_count == 1);
  CHECK(lardon3d_project_db_get_selected_capture_image(db, capture.capture_id, &selected)
        == LARDON3D_PROJECT_DB_NOT_FOUND);
  if (keep) std::printf("GOLDEN_PROJECT_PATH=%s\n", root);
  std::printf("GOLDEN_A6000_RESULT=PASS\n");
  return true;
}

static bool run_test() {
  float wb_a[4] = {2.0F, 1.0F, 1.0F, 1.5F};
  float wb_b[4] = {2.1F, 1.0F, 1.0F, 1.5F};
  unsigned char first[32], second[32], changed[32];
  CHECK(lardon3d_raw_development_policy_fingerprint(wb_a, first) == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(lardon3d_raw_development_policy_fingerprint(wb_a, second) == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(lardon3d_raw_development_policy_fingerprint(wb_b, changed) == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(std::memcmp(first, second, sizeof(first)) == 0);
  CHECK(std::memcmp(first, changed, sizeof(first)) != 0);
  CHECK(lardon3d_raw_development_test_policy_payload_size() == 200);
  CHECK(lardon3d_raw_development_policy_fingerprint(nullptr, first) == LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT);

  char root[] = "/tmp/lardon3d-raw-XXXXXX";
  CHECK(mkdtemp(root) != nullptr);
  char database_path[4096], corrupt_path[4096], link_path[4096], parent_path[4096], child_path[4096],
      other_parent_path[4096];
  CHECK(std::snprintf(database_path, sizeof(database_path), "%s/project.db", root) > 0);
  CHECK(std::snprintf(corrupt_path, sizeof(corrupt_path), "%s/not-arw.bin", root) > 0);
  CHECK(std::snprintf(link_path, sizeof(link_path), "%s/link.arw", root) > 0);
  CHECK(std::snprintf(parent_path, sizeof(parent_path), "%s/parent.bin", root) > 0);
  CHECK(std::snprintf(child_path, sizeof(child_path), "%s/child.bin", root) > 0);
  CHECK(std::snprintf(other_parent_path, sizeof(other_parent_path), "%s/other.bin", root) > 0);
  CHECK(write_file(corrupt_path, "not a raw image"));
  CHECK(write_file(parent_path, "parent"));
  CHECK(write_file(child_path, "child"));
  CHECK(write_file(other_parent_path, "other"));
  Lardon3DProjectDb *db = nullptr;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(database_path, &db, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DAppState state; lardon3d_app_state_init(&state);
  state.project_loaded = true; state.project_db = db;
  CHECK(std::snprintf(state.project_path, sizeof(state.project_path), "%s", root) > 0);
  Lardon3DProjectDbImageAsset parent_asset, child_asset, other_parent_asset;
  CHECK(lardon3d_image_catalog_publish_asset_file(&state, parent_path, 1, UINT64_MAX,
      &parent_asset) == LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
  CHECK(lardon3d_image_catalog_publish_asset_file(&state, child_path, 1, UINT64_MAX,
      &child_asset) == LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
  CHECK(lardon3d_image_catalog_publish_asset_file(&state, other_parent_path, 1, UINT64_MAX,
      &other_parent_asset) == LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
  Lardon3DProjectDbAssetDerivation expected = {};
  expected.parent_asset_id = parent_asset.asset_id;
  expected.child_asset_id = child_asset.asset_id;
  expected.kind = LARDON3D_DB_ASSET_DERIVATION_GENERIC_VERSIONED;
  expected.version = 1;
  std::memset(expected.parameter_fingerprint, 0x5a, sizeof(expected.parameter_fingerprint));
  expected.created_at = 5;
  Lardon3DProjectDbAssetDerivation stored;
  CHECK(lardon3d_project_db_load_asset_derivation(db, child_asset.asset_id, &stored)
      == LARDON3D_PROJECT_DB_NOT_FOUND);
  CHECK(lardon3d_raw_development_test_ensure_derivation(db, &expected)
      == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(lardon3d_project_db_load_asset_derivation(db, child_asset.asset_id, &stored)
      == LARDON3D_PROJECT_DB_OK && stored.parent_asset_id == parent_asset.asset_id
      && stored.child_asset_id == child_asset.asset_id && stored.kind == expected.kind
      && stored.version == expected.version && !stored.has_producer_task
      && stored.created_at == expected.created_at
      && std::memcmp(stored.parameter_fingerprint, expected.parameter_fingerprint, 32) == 0);
  Lardon3DProjectDbAssetDerivation retry = expected;
  retry.has_producer_task = true;
  retry.producer_task_id = 1;
  retry.created_at = 9;
  CHECK(lardon3d_raw_development_test_ensure_derivation(db, &retry)
      == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(lardon3d_project_db_load_asset_derivation(db, child_asset.asset_id, &stored)
      == LARDON3D_PROJECT_DB_OK && !stored.has_producer_task && stored.created_at == 5);
  retry.parent_asset_id = other_parent_asset.asset_id;
  CHECK(lardon3d_raw_development_test_ensure_derivation(db, &retry)
      == LARDON3D_RAW_DEVELOPMENT_CONSTRAINT);
  retry.parent_asset_id = parent_asset.asset_id;
  retry.parameter_fingerprint[0] ^= 1U;
  CHECK(lardon3d_raw_development_test_ensure_derivation(db, &retry)
      == LARDON3D_RAW_DEVELOPMENT_CONSTRAINT);
  CHECK(lardon3d_project_db_load_asset_derivation(db, child_asset.asset_id, &stored)
      == LARDON3D_PROJECT_DB_OK && stored.parent_asset_id == parent_asset.asset_id
      && std::memcmp(stored.parameter_fingerprint, expected.parameter_fingerprint, 32) == 0);
  Lardon3DProjectDbScanSet attachment_scanset;
  CHECK(lardon3d_project_db_create_scanset(db, "Attachment", &attachment_scanset)
      == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbCapture attachment_capture;
  CHECK(lardon3d_project_db_create_capture(db, attachment_scanset.scanset_id, 1,
      &attachment_capture) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_raw_development_test_ensure_capture_asset_role(db,
      attachment_capture.capture_id, parent_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_SOURCE)
      == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(capture_has_asset(db, attachment_capture.capture_id, parent_asset.asset_id,
      LARDON3D_DB_CAPTURE_ASSET_SOURCE));
  CHECK(lardon3d_raw_development_test_ensure_capture_asset_role(db,
      attachment_capture.capture_id, parent_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_SOURCE)
      == LARDON3D_RAW_DEVELOPMENT_OK);
  CHECK(lardon3d_project_db_attach_capture_asset(db, attachment_capture.capture_id,
      other_parent_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_SOURCE) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbCaptureAsset attachment_page[4];
  size_t attachment_count = 0;
  CHECK(lardon3d_project_db_list_capture_assets(db, attachment_capture.capture_id, 0,
      attachment_page, 4, &attachment_count) == LARDON3D_PROJECT_DB_OK
      && attachment_count == 2);
  Lardon3DProjectDbCapture conflict_capture;
  CHECK(lardon3d_project_db_create_capture(db, attachment_scanset.scanset_id, 2,
      &conflict_capture) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_attach_capture_asset(db, conflict_capture.capture_id,
      child_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_DERIVED) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_raw_development_test_ensure_capture_asset_role(db,
      conflict_capture.capture_id, child_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_SOURCE)
      == LARDON3D_RAW_DEVELOPMENT_CONSTRAINT);
  CHECK(capture_has_asset(db, conflict_capture.capture_id, child_asset.asset_id,
      LARDON3D_DB_CAPTURE_ASSET_DERIVED));
  Lardon3DProjectDbImageAsset rejected_asset;
  CHECK(lardon3d_image_catalog_publish_asset_file(&state, corrupt_path, 1, 1,
      &rejected_asset) == LARDON3D_IMAGE_CATALOG_ASSET_TOO_LARGE);
  CHECK(rejected_asset.asset_id == 0);
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_project_db_create_scanset(db, "RAW", &scanset) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbCapture capture;
  CHECK(lardon3d_project_db_create_capture(db, scanset.scanset_id, 1, &capture) == LARDON3D_PROJECT_DB_OK);
  Lardon3DRawDevelopmentOutput output;
  CHECK(lardon3d_raw_develop_to_capture(&state, capture.capture_id, "/missing/file.arw", 0, 1, &output)
        == LARDON3D_RAW_DEVELOPMENT_SOURCE_NOT_FOUND);
  CHECK(symlink(corrupt_path, link_path) == 0);
  CHECK(lardon3d_raw_develop_to_capture(&state, capture.capture_id, link_path, 0, 1, &output)
        == LARDON3D_RAW_DEVELOPMENT_SOURCE_NOT_FOUND);
  Lardon3DRawDevelopmentResult corrupt = lardon3d_raw_develop_to_capture(
      &state, capture.capture_id, corrupt_path, 0, 1, &output);
  CHECK(corrupt == LARDON3D_RAW_DEVELOPMENT_UNSUPPORTED_RAW
        || corrupt == LARDON3D_RAW_DEVELOPMENT_CORRUPT_RAW);
  CHECK(output.source_asset.asset_id != 0);
  const uint64_t corrupt_asset_id = output.source_asset.asset_id;
  CHECK(lardon3d_raw_develop_asset_to_capture(
            &state, capture.capture_id, corrupt_asset_id, 0, 1, &output) ==
        LARDON3D_RAW_DEVELOPMENT_CONSTRAINT);
  CHECK(lardon3d_project_db_record_capture_source_asset(
            db, capture.capture_id, corrupt_asset_id,
            LARDON3D_DB_CAPTURE_SOURCE_RAW) == LARDON3D_PROJECT_DB_OK);
  Lardon3DRawDevelopmentResult explicit_corrupt = lardon3d_raw_develop_asset_to_capture(
      &state, capture.capture_id, corrupt_asset_id, 0, 1, &output);
  CHECK(explicit_corrupt == LARDON3D_RAW_DEVELOPMENT_UNSUPPORTED_RAW ||
        explicit_corrupt == LARDON3D_RAW_DEVELOPMENT_CORRUPT_RAW);
  uint64_t selected = 0;
  CHECK(lardon3d_project_db_get_selected_capture_image(db, capture.capture_id, &selected)
        == LARDON3D_PROJECT_DB_NOT_FOUND);
  uint64_t count = 1;
  CHECK(lardon3d_project_db_count_images(db, scanset.scanset_id, &count) == LARDON3D_PROJECT_DB_OK && count == 0);
  const char *golden_path = std::getenv("LARDON3D_RAW_GOLDEN");
  bool golden = golden_path && golden_path[0];
  const char *keep_value = std::getenv("LARDON3D_RAW_GOLDEN_KEEP");
  bool keep = golden && keep_value && std::strcmp(keep_value, "1") == 0;
  if (golden) CHECK(run_golden(&state, db, golden_path, keep, root));
  lardon3d_project_db_close(db);
  return keep || remove_tree(root);
}

int main() { return run_test() ? 0 : 1; }
