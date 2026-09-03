// Calibration Workflow reaches Project DB through frozen Tooling/Bootstrap
// headers. Establish the C ABI before that transitive include path is guarded;
// project_db.c provides these symbols with C linkage.
extern "C" {
#include <lardon3d/project_db.h>
}

#include <lardon3d/calibration_workflow.h>
#include <lardon3d/optical_profiles.h>

#include <opencv2/imgcodecs.hpp>
#include <openssl/evp.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class OwnedFd {
 public:
  OwnedFd() = default;
  explicit OwnedFd(int fd) : fd_(fd) {}
  ~OwnedFd() {
    if (fd_ >= 0) (void)close(fd_);
  }
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd(OwnedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  OwnedFd& operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) (void)close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
 private:
  int fd_ = -1;
};

Lardon3DCalibrationWorkflowResult db_result(Lardon3DProjectDbResult result) {
  if (result == LARDON3D_PROJECT_DB_OK)
    return LARDON3D_CALIBRATION_WORKFLOW_OK;
  if (result == LARDON3D_PROJECT_DB_NOT_FOUND ||
      result == LARDON3D_PROJECT_DB_CONSTRAINT)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
  return LARDON3D_CALIBRATION_WORKFLOW_PROJECT_DB_ERROR;
}

bool path_component_valid(std::string_view component) {
  return !component.empty() && component != "." && component != "..";
}

Lardon3DCalibrationWorkflowResult open_project_asset(
    const char *project_path, const char *relative_path, uint64_t expected_size,
    const unsigned char expected_sha256[32], std::vector<unsigned char> *bytes) {
  if (!project_path || !*project_path || !relative_path || !*relative_path ||
      !expected_sha256 || !bytes)
    return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;

  std::string_view path(relative_path);
  if (path.front() == '/' || path.back() == '/')
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  int root = open(project_path,
                  O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (root < 0) {
    if (errno == ELOOP || errno == ENOTDIR)
      return LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE;
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  }
  OwnedFd current(root);

  size_t at = 0;
  while (at < path.size()) {
    const size_t slash = path.find('/', at);
    const bool last = slash == std::string_view::npos;
    const size_t end = last ? path.size() : slash;
    const std::string_view component = path.substr(at, end - at);
    if (!path_component_valid(component))
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

    const std::string name(component);
    int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW;
    if (!last) flags |= O_DIRECTORY;
    int next = openat(current.get(), name.c_str(), flags);
    if (next < 0) {
      if (errno == ELOOP || errno == ENOTDIR)
        return LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE;
      return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
    }
    OwnedFd opened(next);

    struct stat st{};
    if (fstat(opened.get(), &st) != 0)
      return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;

    if (!last) {
      if (!S_ISDIR(st.st_mode))
        return LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE;
      current = std::move(opened);
      at = slash + 1;
      continue;
    }

    if (!S_ISREG(st.st_mode))
      return LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE;
    if (st.st_size <= 0 || expected_size == 0 ||
        static_cast<uint64_t>(st.st_size) != expected_size)
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
    if (expected_size > LARDON3D_CALIBRATION_WORKFLOW_MAX_FILE_BYTES)
      return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;

    bytes->assign(static_cast<size_t>(expected_size), 0);
    size_t used = 0;
    while (used < bytes->size()) {
      ssize_t count = pread(opened.get(), bytes->data() + used,
                            bytes->size() - used, static_cast<off_t>(used));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
      used += static_cast<size_t>(count);
    }

    unsigned char actual[32]{};
    unsigned int digest_size = 0;
    if (EVP_Digest(bytes->data(), bytes->size(), actual, &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32)
      return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
    if (std::memcmp(actual, expected_sha256, 32) != 0)
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
    return LARDON3D_CALIBRATION_WORKFLOW_OK;
  }

  return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
}

bool execution_stage_valid(const Lardon3DProjectDbSelectedExecution& execution) {
  if (execution.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION)
    return !execution.has_calibration_scope;
  if (execution.stage == LARDON3D_SELECTED_EXECUTION_READY)
    return execution.has_calibration_scope && execution.calibration_scope_id != 0;
  return false;
}

void fill_entry(const Lardon3DCalibrationWorkflowExternalEvidence& external,
                const Lardon3DProjectDbSelectedExecutionItem& item,
                const Lardon3DProjectDbImageAsset& asset,
                Lardon3DCalibrationToolingEntry *entry) {
  std::memset(entry, 0, sizeof(*entry));
  entry->image_id = item.image_id;
  std::memcpy(entry->representation_sha256, asset.sha256, 32);
  std::memcpy(entry->optical_state_sha256, external.optical_state_sha256, 32);
  entry->width = external.oriented_width;
  entry->height = external.oriented_height;

  const double *p = external.repeated_parameters[0];
  entry->fx = p[0];
  entry->fy = p[1];
  entry->cx = p[2];
  entry->cy = p[3];
  entry->k1 = p[4];
  entry->k2 = p[5];
  entry->p1 = p[6];
  entry->p2 = p[7];

  const double *fit = external.fit_parameters;
  entry->fit_fx = fit[0];
  entry->fit_fy = fit[1];
  entry->fit_cx = fit[2];
  entry->fit_cy = fit[3];
  entry->fit_k1 = fit[4];
  entry->fit_k2 = fit[5];
  entry->fit_p1 = fit[6];
  entry->fit_p2 = fit[7];

  std::memcpy(entry->repeated_parameters, external.repeated_parameters,
              sizeof(entry->repeated_parameters));
  entry->support_images = external.support_images;
  entry->support_observations = external.support_observations;
  entry->reprojection_rmse_px = external.reprojection_rmse_px;
  entry->maximum_parameter_delta = external.maximum_parameter_delta;
  entry->validation_flags = external.validation_flags;
}

void fill_tooling_evidence(
    const Lardon3DCalibrationWorkflowExternalEvidence& external,
    const Lardon3DCalibrationToolingEntry *entries, size_t entry_count,
    Lardon3DCalibrationToolingEvidence *output) {
  std::memset(output, 0, sizeof(*output));
  std::memcpy(output->target_sha256, external.target_sha256, 32);
  std::memcpy(output->optical_state_sha256, external.optical_state_sha256, 32);
  std::memcpy(output->solver_executable_sha256,
              external.solver_executable_sha256, 32);
  std::memcpy(output->solver_configuration_sha256,
              external.solver_configuration_sha256, 32);
  std::memcpy(output->initialization_evidence_sha256,
              external.initialization_evidence_sha256, 32);
  std::memcpy(output->validation_evidence_sha256,
              external.validation_evidence_sha256, 32);

  output->target_family = external.target_family;
  output->target_squares_x = external.target_squares_x;
  output->target_squares_y = external.target_squares_y;
  output->target_square_length_mm = external.target_square_length_mm;
  output->target_marker_length_mm = external.target_marker_length_mm;
  output->target_active_width_mm = external.target_active_width_mm;
  output->target_active_height_mm = external.target_active_height_mm;
  output->target_white_border_mm = external.target_white_border_mm;
  std::memcpy(output->target_measurements_mm, external.target_measurements_mm,
              sizeof(output->target_measurements_mm));
  output->measurement_resolution_mm = external.measurement_resolution_mm;
  output->target_flatness_mm = external.target_flatness_mm;
  output->holdout_rmse_px = external.holdout_rmse_px;
  output->holdout_maximum_residual_px =
      external.holdout_maximum_residual_px;
  output->extra_distortion_coefficient_count =
      external.extra_distortion_coefficient_count;
  output->views = external.views;
  output->view_count = external.view_count;
  output->entries = entries;
  output->entry_count = entry_count;
  output->coordinate_checks = external.coordinate_checks;
  output->coordinate_check_count = external.coordinate_check_count;
}

Lardon3DCalibrationWorkflowResult bind_selected_execution_impl(
    Lardon3DProjectDb *database, const char *project_path,
    const Lardon3DCalibrationWorkflowExternalEvidence *external,
    Lardon3DCalibrationToolingEntry *entries, size_t entry_capacity,
    Lardon3DCalibrationToolingEvidence *output) {
  if (output) std::memset(output, 0, sizeof(*output));
  if (!database || !project_path || !*project_path || !external || !entries ||
      !output)
    return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;

  const uint32_t item_count = external->boundary.capture_count;
  if (item_count == 0 ||
      item_count > LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS ||
      item_count > entry_capacity)
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
  if (!external->views || external->view_count == 0 ||
      external->view_count > LARDON3D_CALIBRATION_TOOLING_MAX_VIEWS ||
      !external->coordinate_checks || external->coordinate_check_count == 0 ||
      external->coordinate_check_count >
          LARDON3D_CALIBRATION_TOOLING_MAX_COORDINATE_CHECKS ||
      external->oriented_width == 0 || external->oriented_height == 0)
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;
  if (std::memcmp(external->optical_state_sha256,
                  external->boundary.optical_state_sha256, 32) != 0)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  std::vector<Lardon3DCalibrationToolingEntry> staged(item_count);

  Lardon3DProjectDbSelectedExecution execution{};
  Lardon3DProjectDbResult dbr = lardon3d_project_db_load_selected_execution(
      database, external->boundary.selected_execution_id, &execution);
  if (dbr != LARDON3D_PROJECT_DB_OK) return db_result(dbr);
  if (!execution_stage_valid(execution) ||
      execution.next_item_index != execution.item_count ||
      execution.item_count != item_count)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  Lardon3DOpticalConfiguration configuration{};
  dbr = lardon3d_optical_configuration_load(
      database, external->boundary.optical_configuration_id, &configuration);
  if (dbr != LARDON3D_PROJECT_DB_OK) return db_result(dbr);
  if (configuration.optical_configuration_id !=
      external->boundary.optical_configuration_id)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  for (uint32_t index = 0; index < item_count; ++index) {
    Lardon3DProjectDbSelectedExecutionItem item{};
    dbr = lardon3d_project_db_load_selected_execution_item(
        database, execution.execution_id, index, &item);
    if (dbr != LARDON3D_PROJECT_DB_OK) return db_result(dbr);
    if (item.item_index != index || !item.has_image || item.image_id == 0 ||
        item.capture_id != external->boundary.capture_ids[index])
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

    Lardon3DOpticalCaptureAssignment assignment{};
    dbr = lardon3d_optical_capture_assignment_load(
        database, item.capture_id, &assignment);
    if (dbr != LARDON3D_PROJECT_DB_OK) return db_result(dbr);
    if (assignment.capture_id != item.capture_id ||
        assignment.optical_configuration_id !=
            external->boundary.optical_configuration_id)
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
    if (assignment.has_campaign_origin &&
        (assignment.campaign_task_id != execution.campaign_task_id ||
         assignment.campaign_group_id != item.campaign_group_id))
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

    Lardon3DProjectDbCapture image_capture{};
    dbr = lardon3d_project_db_find_capture_for_image(
        database, item.image_id, &image_capture);
    if (dbr != LARDON3D_PROJECT_DB_OK) return db_result(dbr);
    if (image_capture.capture_id != item.capture_id)
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

    Lardon3DProjectDbImage image{};
    Lardon3DProjectDbImageAsset asset{};
    dbr = lardon3d_project_db_load_image(database, item.image_id, &image, &asset);
    if (dbr != LARDON3D_PROJECT_DB_OK) return db_result(dbr);
    if (image.image_id != item.image_id || image.asset_id != asset.asset_id ||
        asset.state != LARDON3D_DB_IMAGE_ASSET_READY)
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

    std::vector<unsigned char> bytes;
    Lardon3DCalibrationWorkflowResult result =
        open_project_asset(project_path, asset.path, asset.size_bytes,
                           asset.sha256, &bytes);
    if (result != LARDON3D_CALIBRATION_WORKFLOW_OK) return result;

    try {
      cv::Mat decoded = cv::imdecode(bytes, cv::IMREAD_GRAYSCALE);
      if (decoded.empty() || decoded.cols <= 0 || decoded.rows <= 0 ||
          static_cast<uint32_t>(decoded.cols) != external->oriented_width ||
          static_cast<uint32_t>(decoded.rows) != external->oriented_height)
        return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
    } catch (const cv::Exception&) {
      return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
    } catch (...) {
      return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
    }

    fill_entry(*external, item, asset, &staged[index]);
  }

  std::memcpy(entries, staged.data(),
              static_cast<size_t>(item_count) * sizeof(*entries));
  fill_tooling_evidence(*external, entries, item_count, output);
  return LARDON3D_CALIBRATION_WORKFLOW_OK;
}

}  // namespace

extern "C" Lardon3DCalibrationWorkflowResult
lardon3d_calibration_workflow_bind_selected_execution(
    Lardon3DProjectDb *database, const char *project_path,
    const Lardon3DCalibrationWorkflowExternalEvidence *external,
    Lardon3DCalibrationToolingEntry *entries, size_t entry_capacity,
    Lardon3DCalibrationToolingEvidence *output) {
  try {
    return bind_selected_execution_impl(database, project_path, external,
                                        entries, entry_capacity, output);
  } catch (const std::bad_alloc&) {
    if (output) std::memset(output, 0, sizeof(*output));
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
  } catch (...) {
    if (output) std::memset(output, 0, sizeof(*output));
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  }
}
