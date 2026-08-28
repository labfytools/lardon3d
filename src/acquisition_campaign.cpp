#include <lardon3d/acquisition_campaign.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool bounded(const char *text, size_t capacity) {
  return text != nullptr && text[0] != '\0' && std::memchr(text, '\0', capacity) != nullptr;
}

bool normalized_absolute(const char *path) {
  if (!bounded(path, LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY) || path[0] != '/') return false;
  const size_t length = std::strlen(path);
  if (length > 1u && path[length - 1u] == '/') return false;
  for (const char *part = path + 1; *part != '\0';) {
    const char *slash = std::strchr(part, '/');
    const size_t size = slash == nullptr ? std::strlen(part) : static_cast<size_t>(slash - part);
    if (size == 0u || (size == 1u && part[0] == '.') ||
        (size == 2u && part[0] == '.' && part[1] == '.')) return false;
    if (slash == nullptr) break;
    part = slash + 1;
  }
  return true;
}

Lardon3DAcquisitionSourceKind extension_kind(const char *name) {
  const char *dot = std::strrchr(name, '.');
  if (dot == nullptr) return LARDON3D_ACQUISITION_SOURCE_UNSUPPORTED;
  char extension[6]{};
  const size_t length = std::strlen(dot);
  if (length >= sizeof(extension)) return LARDON3D_ACQUISITION_SOURCE_UNSUPPORTED;
  for (size_t i = 0; i <= length; ++i)
    extension[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(dot[i])));
  if (std::strcmp(extension, ".arw") == 0) return LARDON3D_ACQUISITION_SOURCE_RAW;
  if (std::strcmp(extension, ".jpg") == 0 || std::strcmp(extension, ".jpeg") == 0)
    return LARDON3D_ACQUISITION_SOURCE_JPEG;
  return LARDON3D_ACQUISITION_SOURCE_UNSUPPORTED;
}

bool same_stem(const char *left, const char *right) {
  const char *left_name = std::strrchr(left, '/'); left_name = left_name ? left_name + 1 : left;
  const char *right_name = std::strrchr(right, '/'); right_name = right_name ? right_name + 1 : right;
  const char *left_dot = std::strrchr(left_name, '.');
  const char *right_dot = std::strrchr(right_name, '.');
  const size_t left_size = left_dot ? static_cast<size_t>(left_dot - left_name) : std::strlen(left_name);
  const size_t right_size = right_dot ? static_cast<size_t>(right_dot - right_name) : std::strlen(right_name);
  return left_size == right_size && std::memcmp(left_name, right_name, left_size) == 0;
}

void add_proposal(Lardon3DAcquisitionCampaignPlan &plan, size_t left, size_t right,
                  Lardon3DAcquisitionCampaignProposalKind kind,
                  const Lardon3DAcquisitionPairResult &pair) {
  // Proposals are a bounded review aid, not acquisition identity or a scientific
  // dataset-size limit. Retain the deterministic pair-order prefix while the
  // complete summary and strong-partner analysis continue across every pair.
  if (plan.proposal_count >= LARDON3D_ACQUISITION_CAMPAIGN_MAX_PROPOSALS) return;
  auto &proposal = plan.proposals[plan.proposal_count++];
  proposal = {left, right, kind, pair};
}

}  // namespace

extern "C" Lardon3DAcquisitionCampaignResult lardon3d_acquisition_campaign_discover(
    const Lardon3DAcquisitionCampaignRoot *roots, size_t root_count,
    Lardon3DAcquisitionCampaignDiscovery *discovery) {
  if (discovery != nullptr) std::memset(discovery, 0, sizeof(*discovery));
  if (roots == nullptr || root_count == 0u ||
      root_count > LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS || discovery == nullptr)
    return LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT;
  try {
    std::unique_ptr<Lardon3DAcquisitionCampaignDiscovery> storage(
        new Lardon3DAcquisitionCampaignDiscovery{});
    auto &result = *storage;
    for (size_t i = 0; i < root_count; ++i) {
      if (!normalized_absolute(roots[i].path)) return LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT;
      for (size_t j = 0; j < i; ++j)
        if (std::strcmp(roots[i].path, roots[j].path) == 0)
          return LARDON3D_ACQUISITION_CAMPAIGN_DUPLICATE;
      int root_fd = open(roots[i].path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (root_fd < 0) return LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR;
      DIR *directory = fdopendir(root_fd);
      if (directory == nullptr) { close(root_fd); return LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR; }
      for (;;) {
        errno = 0;
        dirent *entry = readdir(directory);
        if (entry == nullptr) {
          const int read_errno = errno;
          closedir(directory);
          if (read_errno != 0) return LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR;
          break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        ++result.summary.discovered_entry_count;
        struct stat status{};
        if (fstatat(root_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
          closedir(directory); return LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR;
        }
        if (!S_ISREG(status.st_mode)) { ++result.summary.unsupported_entry_count; continue; }
        int leaf_fd = openat(root_fd, entry->d_name,
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (leaf_fd < 0) {
          if (errno == ELOOP) { ++result.summary.unsupported_entry_count; continue; }
          closedir(directory); return LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR;
        }
        if (fstat(leaf_fd, &status) != 0) { close(leaf_fd); closedir(directory); return LARDON3D_ACQUISITION_CAMPAIGN_IO_ERROR; }
        const auto kind = extension_kind(entry->d_name);
        if (!S_ISREG(status.st_mode) || kind == LARDON3D_ACQUISITION_SOURCE_UNSUPPORTED) {
          ++result.summary.unsupported_entry_count; close(leaf_fd); continue;
        }
        if (result.source_count >= LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES) {
          close(leaf_fd); closedir(directory); return LARDON3D_ACQUISITION_CAMPAIGN_LIMIT_EXCEEDED;
        }
        auto &source = result.sources[result.source_count++];
        int written = std::snprintf(source.path, sizeof(source.path), "%s/%s", roots[i].path,
                                    entry->d_name);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(source.path)) {
          close(leaf_fd); closedir(directory); return LARDON3D_ACQUISITION_CAMPAIGN_PATH_TOO_LONG;
        }
        source.source_kind = kind;
        char descriptor_path[64]{};
        written = std::snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", leaf_fd);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(descriptor_path)) {
          close(leaf_fd); closedir(directory); return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR;
        }
        source.metadata_result = lardon3d_acquisition_extract_metadata(descriptor_path, &source.metadata);
        if (source.metadata_result == LARDON3D_ACQUISITION_OK) ++result.summary.metadata_ok_count;
        else ++result.summary.metadata_error_count;
        ++result.summary.supported_source_count;
        close(leaf_fd);
      }
    }
    std::sort(result.sources, result.sources + result.source_count,
              [](const auto &left, const auto &right) { return std::strcmp(left.path, right.path) < 0; });
    for (size_t i = 1; i < result.source_count; ++i)
      if (std::strcmp(result.sources[i - 1].path, result.sources[i].path) == 0)
        return LARDON3D_ACQUISITION_CAMPAIGN_DUPLICATE;
    *discovery = result;
    return LARDON3D_ACQUISITION_CAMPAIGN_OK;
  } catch (const std::bad_alloc &) { return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR; }
  catch (...) { return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR; }
}

extern "C" Lardon3DAcquisitionCampaignResult lardon3d_acquisition_campaign_plan(
    const Lardon3DAcquisitionCampaignSource *sources, size_t source_count,
    const Lardon3DAcquisitionCampaignConfirmation *confirmations, size_t confirmation_count,
    Lardon3DAcquisitionCampaignPlan *plan) {
  if (plan != nullptr) std::memset(plan, 0, sizeof(*plan));
  if (sources == nullptr || source_count == 0u ||
      source_count > LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES || plan == nullptr ||
      (confirmation_count != 0u && confirmations == nullptr) || confirmation_count > source_count)
    return LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT;
  try {
    std::unique_ptr<Lardon3DAcquisitionCampaignPlan> storage(new Lardon3DAcquisitionCampaignPlan{});
    auto &result = *storage;
    result.summary.source_count = source_count;
    bool assigned[LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES]{};
    for (size_t i = 0; i < source_count; ++i) {
      if (!normalized_absolute(sources[i].path)) return LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT;
      if (i != 0u && std::strcmp(sources[i - 1].path, sources[i].path) >= 0)
        return LARDON3D_ACQUISITION_CAMPAIGN_DUPLICATE;
      if (sources[i].metadata_result == LARDON3D_ACQUISITION_OK) ++result.summary.metadata_ok_count;
      else ++result.summary.metadata_error_count;
    }
    for (size_t c = 0; c < confirmation_count; ++c) {
      const auto &confirmation = confirmations[c];
      if (confirmation.source_count == 0u ||
          confirmation.source_count > LARDON3D_ACQUISITION_INGEST_MAX_SOURCES)
        return LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT;
      auto &group = result.groups[result.group_count++];
      group.basis = LARDON3D_ACQUISITION_GROUP_EXPLICIT;
      group.source_count = confirmation.source_count;
      for (size_t m = 0; m < confirmation.source_count; ++m) {
        const size_t index = confirmation.source_indices[m];
        if (index >= source_count || assigned[index]) return LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT;
        for (size_t previous = 0; previous < m; ++previous)
          if (confirmation.source_indices[previous] == index)
            return LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT;
        group.source_indices[m] = index; assigned[index] = true;
      }
      std::sort(group.source_indices, group.source_indices + group.source_count);
      ++result.summary.explicit_group_count;
    }
    size_t strong_count[LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES]{};
    size_t strong_partner[LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES]{};
    for (size_t i = 0; i < source_count; ++i) for (size_t j = i + 1; j < source_count; ++j) {
      if (sources[i].metadata_result != LARDON3D_ACQUISITION_OK ||
          sources[j].metadata_result != LARDON3D_ACQUISITION_OK) continue;
      Lardon3DAcquisitionPairResult pair{};
      const bool stem = same_stem(sources[i].path, sources[j].path);
      if (lardon3d_acquisition_compare(&sources[i].metadata, &sources[j].metadata, 0, stem,
                                       &pair) != LARDON3D_ACQUISITION_OK)
        return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR;
      if (pair.contradictions != 0u) ++result.summary.contradictory_pair_count;
      if (pair.decision == LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG) {
        ++strong_count[i]; ++strong_count[j]; strong_partner[i] = j; strong_partner[j] = i;
      } else if (pair.decision == LARDON3D_ACQUISITION_SAME_ACQUISITION_CANDIDATE) {
        if (stem) {
          ++result.summary.candidate_pair_count;
          add_proposal(result, i, j,
                       LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_CANDIDATE, pair);
        } else {
          ++result.summary.insufficient_pair_count;
        }
      } else if (pair.decision == LARDON3D_ACQUISITION_AMBIGUOUS) {
        ++result.summary.ambiguous_pair_count;
        add_proposal(result, i, j, LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_AMBIGUOUS, pair);
      } else if (pair.decision == LARDON3D_ACQUISITION_INSUFFICIENT && stem) {
        ++result.summary.candidate_pair_count;
        add_proposal(result, i, j, LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_CANDIDATE, pair);
      } else if (pair.decision == LARDON3D_ACQUISITION_INSUFFICIENT) {
        ++result.summary.insufficient_pair_count;
      }
    }
    for (size_t i = 0; i < source_count; ++i) for (size_t j = i + 1; j < source_count; ++j) {
      if (sources[i].metadata_result != LARDON3D_ACQUISITION_OK ||
          sources[j].metadata_result != LARDON3D_ACQUISITION_OK) continue;
      Lardon3DAcquisitionPairResult pair{};
      if (lardon3d_acquisition_compare(&sources[i].metadata, &sources[j].metadata, 0,
                                       same_stem(sources[i].path, sources[j].path), &pair) !=
          LARDON3D_ACQUISITION_OK)
        return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR;
      if (pair.decision != LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG) continue;
      if (strong_count[i] > 1u || strong_count[j] > 1u) {
        ++result.summary.ambiguous_pair_count;
        add_proposal(result, i, j, LARDON3D_ACQUISITION_CAMPAIGN_PROPOSAL_AMBIGUOUS, pair);
      }
      if (strong_count[i] == 1u && strong_count[j] == 1u &&
          strong_partner[i] == j && strong_partner[j] == i && (assigned[i] || assigned[j]))
        return LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT;
    }
    for (size_t i = 0; i < source_count; ++i) {
      if (assigned[i] || strong_count[i] != 1u) continue;
      const size_t partner = strong_partner[i];
      if (partner >= source_count || assigned[partner] || strong_count[partner] != 1u ||
          strong_partner[partner] != i) continue;
      auto &group = result.groups[result.group_count++];
      group.basis = LARDON3D_ACQUISITION_GROUP_STRONG; group.source_count = 2u;
      group.source_indices[0] = i; group.source_indices[1] = partner;
      assigned[i] = assigned[partner] = true; ++result.summary.strong_group_count;
    }
    for (size_t i = 0; i < source_count; ++i) if (!assigned[i]) {
      auto &group = result.groups[result.group_count++];
      group.basis = LARDON3D_ACQUISITION_GROUP_SINGLETON; group.source_count = 1u;
      group.source_indices[0] = i; assigned[i] = true; ++result.summary.unresolved_source_count;
    }
    std::sort(result.groups, result.groups + result.group_count, [](const auto &left, const auto &right) {
      return left.source_indices[0] < right.source_indices[0];
    });
    for (size_t i = 0; i < result.group_count; ++i) result.groups[i].group_id = static_cast<uint32_t>(i + 1u);
    *plan = result;
    return LARDON3D_ACQUISITION_CAMPAIGN_OK;
  } catch (const std::bad_alloc &) { return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR; }
  catch (...) { return LARDON3D_ACQUISITION_CAMPAIGN_INTERNAL_ERROR; }
}

extern "C" Lardon3DAcquisitionCampaignResult lardon3d_acquisition_campaign_materialize_group(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionCampaignSource *sources, size_t source_count,
    const Lardon3DAcquisitionCampaignPlan *plan, uint32_t group_id,
    const Lardon3DAcquisitionIngestOptions *options, Lardon3DAcquisitionIngestOutput *output,
    Lardon3DAcquisitionIngestResult *ingest_result) {
  if (output != nullptr) std::memset(output, 0, sizeof(*output));
  if (ingest_result != nullptr) *ingest_result = LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT;
  if (state == nullptr || sources == nullptr || source_count == 0u ||
      source_count > LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES || plan == nullptr ||
      group_id == 0u || group_id > plan->group_count || options == nullptr || output == nullptr ||
      ingest_result == nullptr) return LARDON3D_ACQUISITION_CAMPAIGN_INVALID_ARGUMENT;
  const auto &group = plan->groups[group_id - 1u];
  if (group.group_id != group_id || group.source_count == 0u ||
      group.source_count > LARDON3D_ACQUISITION_INGEST_MAX_SOURCES)
    return LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT;
  Lardon3DAcquisitionIngestSource inputs[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES]{};
  for (size_t i = 0; i < group.source_count; ++i) {
    if (group.source_indices[i] >= source_count) return LARDON3D_ACQUISITION_CAMPAIGN_CONSTRAINT;
    inputs[i].source_path = sources[group.source_indices[i]].path;
    inputs[i].explicit_group = group.basis == LARDON3D_ACQUISITION_GROUP_EXPLICIT ? 1u : 0u;
  }
  Lardon3DAcquisitionIngestOptions effective = *options;
  effective.grouping = group.basis == LARDON3D_ACQUISITION_GROUP_EXPLICIT
                           ? LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT
                           : LARDON3D_ACQUISITION_GROUP_AUTOMATIC;
  *ingest_result = lardon3d_acquisition_ingest(state, scanset_id, inputs, group.source_count,
                                                &effective, output);
  return *ingest_result == LARDON3D_ACQUISITION_INGEST_OK
             ? LARDON3D_ACQUISITION_CAMPAIGN_OK
             : LARDON3D_ACQUISITION_CAMPAIGN_INGEST_ERROR;
}
