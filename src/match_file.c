#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/match_file.h>

enum { FT_U8 = 1, FT_F32 = 2 };

static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put_u64(unsigned char *p, uint64_t v) {
  put_u32(p, (uint32_t)v);
  put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get_u64(const unsigned char *p) {
  return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static void put_float(unsigned char *p, float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  put_u32(p, bits);
}

static float get_float(const unsigned char *p) {
  uint32_t bits = get_u32(p);
  float value = 0;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static bool write_exact(int fd, const void *data, size_t size) {
  const unsigned char *bytes = data;
  size_t used = 0;
  while (used < size) {
    ssize_t n = write(fd, bytes + used, size - used);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    used += (size_t)n;
  }
  return true;
}

static bool read_exact(int fd, void *data, size_t size) {
  unsigned char *bytes = data;
  size_t used = 0;
  while (used < size) {
    ssize_t n = read(fd, bytes + used, size - used);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    used += (size_t)n;
  }
  return true;
}

static bool valid_dimension(uint8_t type, uint32_t dimension) {
  return (type == FT_U8 && dimension == 32) || (type == FT_F32 && dimension == 128);
}

static void decode_header(const unsigned char raw[32], Lardon3DMatchFileHeader *header) {
  memcpy(header->magic, raw, 4);
  header->format_version = raw[4];
  header->descriptor_type = raw[5];
  header->reserved = (uint16_t)(raw[6] | ((uint16_t)raw[7] << 8));
  header->match_count = get_u32(raw + 8);
  header->descriptor_dimension = get_u32(raw + 12);
  header->feature_set_id_a = get_u64(raw + 16);
  header->feature_set_id_b = get_u64(raw + 24);
}

static Lardon3DMatchFileResult validate_header(const Lardon3DMatchFileHeader *header,
                                               uint64_t file_size,
                                               uint64_t expected_a, uint64_t expected_b) {
  if (memcmp(header->magic, LARDON3D_MATCH_FILE_MAGIC, 4) != 0)
    return LARDON3D_MATCH_FILE_BAD_MAGIC;
  if (header->format_version != LARDON3D_MATCH_FILE_VERSION)
    return LARDON3D_MATCH_FILE_BAD_VERSION;
  if (header->reserved != 0) return LARDON3D_MATCH_FILE_CORRUPT;
  if (!valid_dimension(header->descriptor_type, header->descriptor_dimension))
    return LARDON3D_MATCH_FILE_BAD_TYPE;
  if (header->match_count > LARDON3D_MATCH_FILE_MAX_MATCHES)
    return LARDON3D_MATCH_FILE_BAD_COUNT;
  if (header->feature_set_id_a == 0 || header->feature_set_id_b == 0 ||
      header->feature_set_id_a == header->feature_set_id_b)
    return LARDON3D_MATCH_FILE_CORRUPT;
  if ((expected_a != 0 && header->feature_set_id_a != expected_a) ||
      (expected_b != 0 && header->feature_set_id_b != expected_b))
    return LARDON3D_MATCH_FILE_CORRUPT;
  uint64_t expected_size = LARDON3D_MATCH_FILE_HEADER_SIZE +
                           (uint64_t)header->match_count * LARDON3D_MATCH_FILE_ENTRY_SIZE;
  return file_size == expected_size ? LARDON3D_MATCH_FILE_OK : LARDON3D_MATCH_FILE_BAD_SIZE;
}

static bool valid_entry(const Lardon3DMatchFileEntry *entry, uint32_t count_a,
                        uint32_t count_b) {
  uint32_t limit_a = count_a == 0 ? 8192 : count_a;
  uint32_t limit_b = count_b == 0 ? 8192 : count_b;
  return entry->feature_index_a < limit_a && entry->feature_index_b < limit_b &&
         isfinite(entry->distance) && entry->distance >= 0.0F;
}

Lardon3DMatchFileResult lardon3d_match_file_write(
    int fd, uint8_t descriptor_type, uint32_t descriptor_dimension,
    uint64_t feature_set_id_a, uint64_t feature_set_id_b,
    const Lardon3DMatchFileEntry *entries, uint32_t match_count) {
  if (fd < 0 || !valid_dimension(descriptor_type, descriptor_dimension) ||
      feature_set_id_a == 0 || feature_set_id_b == 0 || feature_set_id_a == feature_set_id_b ||
      match_count > LARDON3D_MATCH_FILE_MAX_MATCHES || (match_count > 0 && !entries))
    return LARDON3D_MATCH_FILE_INVALID_ARGUMENT;

  size_t file_size = LARDON3D_MATCH_FILE_HEADER_SIZE +
                     (size_t)match_count * LARDON3D_MATCH_FILE_ENTRY_SIZE;
  unsigned char *raw = calloc(1, file_size);
  if (!raw) return LARDON3D_MATCH_FILE_IO_ERROR;
  memcpy(raw, LARDON3D_MATCH_FILE_MAGIC, 4);
  raw[4] = LARDON3D_MATCH_FILE_VERSION;
  raw[5] = descriptor_type;
  put_u32(raw + 8, match_count);
  put_u32(raw + 12, descriptor_dimension);
  put_u64(raw + 16, feature_set_id_a);
  put_u64(raw + 24, feature_set_id_b);
  for (uint32_t i = 0; i < match_count; ++i) {
    if (!valid_entry(&entries[i], 0, 0) ||
        (i > 0 && entries[i].feature_index_a <= entries[i - 1].feature_index_a)) {
      free(raw);
      return LARDON3D_MATCH_FILE_BAD_ENTRY;
    }
    unsigned char *item = raw + LARDON3D_MATCH_FILE_HEADER_SIZE +
                          (size_t)i * LARDON3D_MATCH_FILE_ENTRY_SIZE;
    put_u32(item, entries[i].feature_index_a);
    put_u32(item + 4, entries[i].feature_index_b);
    put_float(item + 8, entries[i].distance);
  }
  bool written = write_exact(fd, raw, file_size);
  free(raw);
  return written ? LARDON3D_MATCH_FILE_OK : LARDON3D_MATCH_FILE_IO_ERROR;
}

Lardon3DMatchFileResult lardon3d_match_file_read(
    int fd, Lardon3DMatchFileHeader *header, Lardon3DMatchFileEntry *entries,
    size_t entry_capacity, uint32_t *match_count, uint64_t expected_a,
    uint64_t expected_b, uint32_t count_a, uint32_t count_b) {
  if (fd < 0 || !header || !match_count || (!entries && entry_capacity != 0) ||
      count_a > 8192 || count_b > 8192)
    return LARDON3D_MATCH_FILE_INVALID_ARGUMENT;
  *match_count = 0;
  memset(header, 0, sizeof(*header));
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
    return LARDON3D_MATCH_FILE_IO_ERROR;
  unsigned char raw[32];
  if (!read_exact(fd, raw, sizeof(raw))) return LARDON3D_MATCH_FILE_BAD_SIZE;
  decode_header(raw, header);
  Lardon3DMatchFileResult result = validate_header(header, (uint64_t)info.st_size,
                                                   expected_a, expected_b);
  if (result != LARDON3D_MATCH_FILE_OK) return result;
  if (header->match_count > entry_capacity) return LARDON3D_MATCH_FILE_BAD_COUNT;
  for (uint32_t i = 0; i < header->match_count; ++i) {
    unsigned char item[12];
    if (!read_exact(fd, item, sizeof(item))) return LARDON3D_MATCH_FILE_BAD_SIZE;
    entries[i].feature_index_a = get_u32(item);
    entries[i].feature_index_b = get_u32(item + 4);
    entries[i].distance = get_float(item + 8);
    if (!valid_entry(&entries[i], count_a, count_b) ||
        (i > 0 && entries[i].feature_index_a <= entries[i - 1].feature_index_a))
      return LARDON3D_MATCH_FILE_BAD_ENTRY;
  }
  *match_count = header->match_count;
  return LARDON3D_MATCH_FILE_OK;
}

Lardon3DMatchFileResult lardon3d_match_file_validate(
    const char *path, Lardon3DMatchFileHeader *header, uint64_t expected_a,
    uint64_t expected_b, uint32_t count_a, uint32_t count_b) {
  if (!path || !header || count_a > 8192 || count_b > 8192)
    return LARDON3D_MATCH_FILE_INVALID_ARGUMENT;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return LARDON3D_MATCH_FILE_IO_ERROR;
  struct stat info;
  unsigned char raw[32];
  Lardon3DMatchFileResult result = LARDON3D_MATCH_FILE_IO_ERROR;
  if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0) {
    if (!read_exact(fd, raw, sizeof(raw))) {
      result = LARDON3D_MATCH_FILE_BAD_SIZE;
    } else {
      decode_header(raw, header);
      result = validate_header(header, (uint64_t)info.st_size, expected_a, expected_b);
      if (result == LARDON3D_MATCH_FILE_OK) {
        Lardon3DMatchFileEntry entry;
        uint32_t previous_a = 0;
        for (uint32_t i = 0; i < header->match_count; ++i) {
          unsigned char item[12];
          if (!read_exact(fd, item, sizeof(item))) {
            result = LARDON3D_MATCH_FILE_BAD_SIZE;
            break;
          }
          entry.feature_index_a = get_u32(item);
          entry.feature_index_b = get_u32(item + 4);
          entry.distance = get_float(item + 8);
          if (!valid_entry(&entry, count_a, count_b) ||
              (i > 0 && entry.feature_index_a <= previous_a)) {
            result = LARDON3D_MATCH_FILE_BAD_ENTRY;
            break;
          }
          previous_a = entry.feature_index_a;
        }
      }
    }
  }
  (void)close(fd);
  return result;
}

Lardon3DMatchFileResult lardon3d_match_file_validate_asset(
    const char *path, const unsigned char expected_sha256[32], uint64_t expected_size,
    Lardon3DMatchFileHeader *header, uint64_t expected_a, uint64_t expected_b,
    uint32_t count_a, uint32_t count_b) {
  if (!path || !expected_sha256 || !header || expected_size < LARDON3D_MATCH_FILE_HEADER_SIZE ||
      expected_size > LARDON3D_MATCH_FILE_MAX_SIZE || count_a > 8192 || count_b > 8192)
    return LARDON3D_MATCH_FILE_INVALID_ARGUMENT;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return LARDON3D_MATCH_FILE_IO_ERROR;
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      (uint64_t)info.st_size != expected_size) {
    (void)close(fd);
    return LARDON3D_MATCH_FILE_BAD_SIZE;
  }
  unsigned char *bytes = malloc((size_t)expected_size);
  if (!bytes) {
    (void)close(fd);
    return LARDON3D_MATCH_FILE_IO_ERROR;
  }
  bool read_ok = read_exact(fd, bytes, (size_t)expected_size);
  (void)close(fd);
  if (!read_ok) {
    free(bytes);
    return LARDON3D_MATCH_FILE_BAD_SIZE;
  }
  unsigned char actual_sha256[32];
  unsigned int digest_size = 0;
  bool hash_ok = EVP_Digest(bytes, (size_t)expected_size, actual_sha256, &digest_size,
                            EVP_sha256(), NULL) == 1 && digest_size == 32;
  if (!hash_ok || memcmp(actual_sha256, expected_sha256, 32) != 0) {
    free(bytes);
    return LARDON3D_MATCH_FILE_CORRUPT;
  }
  decode_header(bytes, header);
  Lardon3DMatchFileResult result = validate_header(header, expected_size, expected_a, expected_b);
  uint32_t previous_a = 0;
  for (uint32_t i = 0; result == LARDON3D_MATCH_FILE_OK && i < header->match_count; ++i) {
    const unsigned char *item = bytes + LARDON3D_MATCH_FILE_HEADER_SIZE +
                                (size_t)i * LARDON3D_MATCH_FILE_ENTRY_SIZE;
    Lardon3DMatchFileEntry entry = {get_u32(item), get_u32(item + 4), get_float(item + 8)};
    if (!valid_entry(&entry, count_a, count_b) ||
        (i > 0 && entry.feature_index_a <= previous_a))
      result = LARDON3D_MATCH_FILE_BAD_ENTRY;
    previous_a = entry.feature_index_a;
  }
  free(bytes);
  return result;
}
