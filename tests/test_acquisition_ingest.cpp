#include <lardon3d/acquisition_ingest.h>
#include <lardon3d/acquisition_pairing.h>

extern "C" {
#include <lardon3d/image_catalog.h>
}

#include <libexif/exif-data.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void lardon3d_acquisition_ingest_test_set_metadata(
    const Lardon3DAcquisitionMetadata *metadata, size_t count);

#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "Failure line %d: %s\\n", __LINE__, #condition); return false; } \
} while (0)

namespace {

bool remove_tree(const char *path) {
  struct stat info {};
  if (lstat(path, &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
  DIR *directory = opendir(path);
  if (directory == nullptr) return false;
  bool ok = true;
  for (dirent *entry = readdir(directory); entry != nullptr; entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    char child[4096];
    const int length = std::snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(child) || !remove_tree(child)) ok = false;
  }
  return closedir(directory) == 0 && rmdir(path) == 0 && ok;
}

void add_ascii(ExifData *data, ExifIfd ifd, ExifTag tag, const char *text) {
  ExifEntry *entry = exif_entry_new();
  entry->tag = tag;
  entry->format = EXIF_FORMAT_ASCII;
  entry->components = std::strlen(text) + 1u;
  entry->size = static_cast<unsigned int>(entry->components);
  entry->data = static_cast<unsigned char *>(std::malloc(entry->size));
  std::memcpy(entry->data, text, entry->size);
  exif_content_add_entry(data->ifd[ifd], entry);
  exif_entry_unref(entry);
}

bool write_jpeg(const char *path, const char *unique_id) {
  ExifData *data = exif_data_new();
  if (data == nullptr) return false;
  exif_data_set_byte_order(data, EXIF_BYTE_ORDER_INTEL);
  add_ascii(data, EXIF_IFD_0, EXIF_TAG_MODEL, "Lardon test camera");
  if (unique_id != nullptr) add_ascii(data, EXIF_IFD_EXIF, EXIF_TAG_IMAGE_UNIQUE_ID, unique_id);
  unsigned char *encoded = nullptr;
  unsigned int encoded_size = 0u;
  exif_data_save_data(data, &encoded, &encoded_size);
  FILE *file = std::fopen(path, "wb");
  const unsigned char soi[] = {0xffu, 0xd8u, 0xffu, 0xe1u};
  const unsigned int segment_size = encoded_size + 2u;
  const unsigned char length[] = {static_cast<unsigned char>(segment_size >> 8u),
                                  static_cast<unsigned char>(segment_size)};
  const unsigned char eoi[] = {0xffu, 0xd9u};
  const bool ok = file != nullptr && encoded != nullptr && encoded_size != 0u &&
      segment_size <= UINT16_MAX && std::fwrite(soi, 1, sizeof(soi), file) == sizeof(soi) &&
      std::fwrite(length, 1, sizeof(length), file) == sizeof(length) &&
      std::fwrite(encoded, 1, encoded_size, file) == encoded_size &&
      std::fwrite(eoi, 1, sizeof(eoi), file) == sizeof(eoi) && std::fclose(file) == 0;
  if (file != nullptr && !ok) std::fclose(file);
  std::free(encoded);
  exif_data_unref(data);
  return ok;
}

struct Fixture {
  char root[4096] = "/tmp/lardon3d-ingest-XXXXXX";
  char database_path[4096]{};
  Lardon3DProjectDb *database = nullptr;
  Lardon3DAppState state{};
  Lardon3DProjectDbScanSet scanset{};

  bool open() {
    if (mkdtemp(root) == nullptr || std::snprintf(database_path, sizeof(database_path), "%s/project.db", root) <= 0)
      return false;
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
    if (lardon3d_project_db_open(database_path, &database, error) != LARDON3D_PROJECT_DB_OK) return false;
    state.project_loaded = true;
    state.project_db = database;
    if (std::snprintf(state.project_path, sizeof(state.project_path), "%s", root) <= 0) return false;
    return lardon3d_project_db_create_scanset(database, "S3-E", &scanset) == LARDON3D_PROJECT_DB_OK;
  }
  void close() {
    lardon3d_acquisition_ingest_test_set_metadata(nullptr, 0u);
    if (database != nullptr) lardon3d_project_db_close(database);
    database = nullptr;
    if (root[0] != '\0') (void)remove_tree(root);
  }
  bool path(char output[4096], const char *name) const {
    const int length = std::snprintf(output, 4096, "%s/%s", root, name);
    return length > 0 && length < 4096;
  }
};

Lardon3DAcquisitionIngestOptions options() {
  Lardon3DAcquisitionIngestOptions value{};
  value.grouping = LARDON3D_ACQUISITION_GROUP_AUTOMATIC;
  value.representation = LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE;
  value.imported_at = 7;
  value.max_source_bytes = 1024 * 1024;
  return value;
}

Lardon3DAcquisitionMetadata metadata(const char *uid, Lardon3DAcquisitionSourceKind kind = LARDON3D_ACQUISITION_SOURCE_JPEG) {
  Lardon3DAcquisitionMetadata value{};
  value.policy_version = LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
  value.source_kind = kind;
  if (uid != nullptr) {
    std::strcpy(value.image_unique_id, uid);
    value.present_fields = LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID;
  }
  return value;
}

size_t capture_count(Lardon3DProjectDb *database, uint64_t scanset_id) {
  Lardon3DProjectDbCapture captures[64]{};
  size_t count = 0u;
  return lardon3d_project_db_list_captures(database, scanset_id, 0, captures, 64, &count) ==
      LARDON3D_PROJECT_DB_OK ? count : SIZE_MAX;
}

bool capture_assets(Lardon3DProjectDb *database, uint64_t capture_id, size_t expected) {
  Lardon3DProjectDbCaptureAsset assets[64]{};
  size_t count = 0u;
  if (lardon3d_project_db_list_capture_assets(database, capture_id, 0, assets, 64, &count) !=
      LARDON3D_PROJECT_DB_OK || count != expected) return false;
  for (size_t i = 0; i < count; ++i)
    if (assets[i].role != LARDON3D_DB_CAPTURE_ASSET_SOURCE) return false;
  return true;
}

bool test_ingest_grouping_and_resume() {
  Fixture fixture;
  CHECK(fixture.open());
  char a[4096], b[4096], c[4096], d[4096];
  CHECK(fixture.path(a, "DSC03350.JPG") && fixture.path(b, "DSC03350.ARW") &&
        fixture.path(c, "other.JPG") && fixture.path(d, "fourth.JPG"));
  CHECK(write_jpeg(a, "A") && write_jpeg(b, "B") && write_jpeg(c, "C") && write_jpeg(d, "D"));
  Lardon3DAcquisitionIngestSource sources[] = {{a, 0}, {b, 0}};
  Lardon3DAcquisitionIngestOutput output{};
  Lardon3DAcquisitionIngestOptions automatic_options = options();

  /* Same basename and second-level timestamp are not passed as pairing hints. */
  Lardon3DAcquisitionMetadata weak[] = {metadata(nullptr), metadata(nullptr)};
  lardon3d_acquisition_ingest_test_set_metadata(weak, 2);
  CHECK(lardon3d_acquisition_ingest(&fixture.state, fixture.scanset.scanset_id, sources, 2,
                                    &automatic_options, &output) == LARDON3D_ACQUISITION_INGEST_OK);
  CHECK(output.group_count == 2 && output.groups[0].basis == LARDON3D_ACQUISITION_GROUP_SINGLETON &&
        output.groups[1].basis == LARDON3D_ACQUISITION_GROUP_SINGLETON && capture_count(fixture.database, fixture.scanset.scanset_id) == 2);

  /* Controlled S3-D evidence: distinct source kinds, one unique mutual strong pair. */
  Fixture strong;
  CHECK(strong.open());
  char raw_like[4096], jpeg[4096];
  CHECK(strong.path(raw_like, "raw-like.bin") && strong.path(jpeg, "camera.JPG") &&
        write_jpeg(raw_like, "physical-one") && write_jpeg(jpeg, "physical-two"));
  Lardon3DAcquisitionIngestSource pair[] = {{raw_like, 0}, {jpeg, 0}};
  Lardon3DAcquisitionMetadata same[] = {metadata("SHUTTER", LARDON3D_ACQUISITION_SOURCE_RAW),
                                        metadata("SHUTTER", LARDON3D_ACQUISITION_SOURCE_JPEG)};
  lardon3d_acquisition_ingest_test_set_metadata(same, 2);
  Lardon3DAcquisitionIngestOutput paired{};
  Lardon3DAcquisitionIngestOptions jpeg_options = options();
  CHECK(lardon3d_acquisition_ingest(&strong.state, strong.scanset.scanset_id, pair, 2,
                                    &jpeg_options, &paired) == LARDON3D_ACQUISITION_INGEST_OK);
  CHECK(paired.group_count == 1 && paired.groups[0].basis == LARDON3D_ACQUISITION_GROUP_STRONG &&
        paired.groups[0].source_count == 2 && capture_count(strong.database, strong.scanset.scanset_id) == 1 &&
        capture_assets(strong.database, paired.groups[0].capture_id, 2));

  /* A competing equal strong candidate remains entirely ungrouped. */
  Fixture ambiguous;
  CHECK(ambiguous.open());
  char paths[3][4096];
  const char *physical_ids[] = {"physical-a", "physical-b", "physical-c"};
  for (size_t i = 0; i < 3; ++i) {
    CHECK(ambiguous.path(paths[i], i == 0 ? "a.JPG" : i == 1 ? "b.JPG" : "c.JPG"));
    CHECK(write_jpeg(paths[i], physical_ids[i]));
  }
  Lardon3DAcquisitionIngestSource triple[] = {{paths[0], 0}, {paths[1], 0}, {paths[2], 0}};
  Lardon3DAcquisitionMetadata tied[] = {metadata("TIE"), metadata("TIE"), metadata("TIE")};
  lardon3d_acquisition_ingest_test_set_metadata(tied, 3);
  Lardon3DAcquisitionIngestOutput ambiguous_output{};
  CHECK(lardon3d_acquisition_ingest(&ambiguous.state, ambiguous.scanset.scanset_id, triple, 3,
                                    &jpeg_options, &ambiguous_output) == LARDON3D_ACQUISITION_INGEST_OK);
  CHECK(ambiguous_output.group_count == 3 && capture_count(ambiguous.database, ambiguous.scanset.scanset_id) == 3);
  for (size_t i = 0; i < 3; ++i) CHECK(ambiguous_output.groups[i].basis == LARDON3D_ACQUISITION_GROUP_SINGLETON);

  /* Resume is only the known Capture ID and converges image/source state. */
  Fixture resumed;
  CHECK(resumed.open());
  char resume_file[4096]; CHECK(resumed.path(resume_file, "resume.JPG") && write_jpeg(resume_file, "resume"));
  Lardon3DProjectDbCapture existing{};
  CHECK(lardon3d_project_db_create_capture(resumed.database, resumed.scanset.scanset_id, 7, &existing) == LARDON3D_PROJECT_DB_OK);
  Lardon3DAcquisitionIngestSource one[] = {{resume_file, 0}};
  Lardon3DAcquisitionMetadata one_metadata[] = {metadata("RESUME")};
  Lardon3DAcquisitionIngestOptions resume_options = options(); resume_options.resume_capture_id = existing.capture_id;
  lardon3d_acquisition_ingest_test_set_metadata(one_metadata, 1);
  Lardon3DAcquisitionIngestOutput first{};
  CHECK(lardon3d_acquisition_ingest(&resumed.state, resumed.scanset.scanset_id, one, 1, &resume_options, &first) == LARDON3D_ACQUISITION_INGEST_OK && first.groups[0].capture_id == existing.capture_id);
  Lardon3DAcquisitionIngestOutput retry{};
  CHECK(lardon3d_acquisition_ingest(&resumed.state, resumed.scanset.scanset_id, one, 1, &resume_options, &retry) == LARDON3D_ACQUISITION_INGEST_OK && capture_count(resumed.database, resumed.scanset.scanset_id) == 1 && capture_assets(resumed.database, existing.capture_id, 1));
  uint64_t images = 0; CHECK(lardon3d_project_db_count_images(resumed.database, resumed.scanset.scanset_id, &images) == LARDON3D_PROJECT_DB_OK && images == 1);

  fixture.close(); strong.close(); ambiguous.close(); resumed.close();
  return true;
}

bool test_explicit_and_db_helper() {
  Fixture fixture;
  CHECK(fixture.open());
  char a[4096], b[4096], c[4096], helper_path[4096];
  CHECK(fixture.path(a, "a.JPG") && fixture.path(b, "b.JPG") && fixture.path(c, "c.JPG") &&
        fixture.path(helper_path, "helper.JPG") && write_jpeg(a, "a") && write_jpeg(b, "b") &&
        write_jpeg(c, "c") && write_jpeg(helper_path, "helper"));
  Lardon3DAcquisitionIngestSource inputs[] = {{a, 9}, {b, 9}, {c, 10}};
  Lardon3DAcquisitionIngestOptions explicit_options = options();
  explicit_options.grouping = LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT;
  Lardon3DAcquisitionIngestOutput output{};
  CHECK(lardon3d_acquisition_ingest(&fixture.state, fixture.scanset.scanset_id, inputs, 3,
                                    &explicit_options, &output) == LARDON3D_ACQUISITION_INGEST_OK);
  CHECK(output.group_count == 2 && output.groups[0].basis == LARDON3D_ACQUISITION_GROUP_EXPLICIT &&
        output.groups[0].source_count == 2 && output.groups[1].basis == LARDON3D_ACQUISITION_GROUP_EXPLICIT);

  Lardon3DProjectDbCapture capture{}, foreign{};
  CHECK(lardon3d_project_db_create_capture(fixture.database, fixture.scanset.scanset_id, 8, &capture) == LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_create_capture(fixture.database, fixture.scanset.scanset_id, 9, &foreign) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbImageAsset asset{};
  CHECK(lardon3d_image_catalog_publish_asset_file(&fixture.state, helper_path, 8, UINT64_MAX, &asset) == LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
  Lardon3DProjectDbImageRegisterStatus status{}; Lardon3DProjectDbImage image{};
  CHECK(lardon3d_project_db_publish_source_capture_image(fixture.database, capture.capture_id, asset.asset_id,
      "helper.JPG", helper_path, 0, 8, false, &status, &image) == LARDON3D_PROJECT_DB_OK && status == LARDON3D_PROJECT_DB_IMAGE_REGISTERED);
  uint64_t selected = 0;
  CHECK(lardon3d_project_db_get_selected_capture_image(fixture.database, capture.capture_id, &selected) == LARDON3D_PROJECT_DB_NOT_FOUND);
  CHECK(lardon3d_project_db_publish_source_capture_image(fixture.database, capture.capture_id, asset.asset_id,
      "renamed-on-resume.JPG", "/relocated/source.JPG", 0, 99, true, &status, &image) == LARDON3D_PROJECT_DB_OK && status == LARDON3D_PROJECT_DB_IMAGE_ALREADY_PRESENT &&
      lardon3d_project_db_get_selected_capture_image(fixture.database, capture.capture_id, &selected) == LARDON3D_PROJECT_DB_OK && selected == image.image_id);
  CHECK(lardon3d_project_db_publish_source_capture_image(fixture.database, foreign.capture_id, asset.asset_id,
      "helper.JPG", helper_path, 0, 8, false, &status, &image) == LARDON3D_PROJECT_DB_CONSTRAINT);
  fixture.close();
  return true;
}

}  // namespace

int main() {
  return test_ingest_grouping_and_resume() && test_explicit_and_db_helper() ? 0 : 1;
}
