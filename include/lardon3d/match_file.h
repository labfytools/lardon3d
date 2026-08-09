#ifndef LARDON3D_MATCH_FILE_H
#define LARDON3D_MATCH_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    LARDON3D_MATCH_FILE_VERSION = 1,
    LARDON3D_MATCH_FILE_HEADER_SIZE = 32,
    LARDON3D_MATCH_FILE_ENTRY_SIZE = 12,
    LARDON3D_MATCH_FILE_MAX_MATCHES = 8192,
    LARDON3D_MATCH_FILE_MAX_SIZE = 98336,
};

#define LARDON3D_MATCH_FILE_MAGIC "L3DM"

typedef struct {
    uint32_t feature_index_a;
    uint32_t feature_index_b;
    float distance;
} Lardon3DMatchFileEntry;

typedef struct {
    unsigned char magic[4];
    uint8_t format_version;
    uint8_t descriptor_type;
    uint16_t reserved;
    uint32_t match_count;
    uint32_t descriptor_dimension;
    uint64_t feature_set_id_a;
    uint64_t feature_set_id_b;
} Lardon3DMatchFileHeader;

typedef enum {
    LARDON3D_MATCH_FILE_OK = 0,
    LARDON3D_MATCH_FILE_INVALID_ARGUMENT,
    LARDON3D_MATCH_FILE_IO_ERROR,
    LARDON3D_MATCH_FILE_BAD_MAGIC,
    LARDON3D_MATCH_FILE_BAD_VERSION,
    LARDON3D_MATCH_FILE_BAD_TYPE,
    LARDON3D_MATCH_FILE_BAD_COUNT,
    LARDON3D_MATCH_FILE_BAD_SIZE,
    LARDON3D_MATCH_FILE_BAD_ENTRY,
    LARDON3D_MATCH_FILE_CORRUPT,
} Lardon3DMatchFileResult;

Lardon3DMatchFileResult lardon3d_match_file_write(
    int fd,
    uint8_t descriptor_type,
    uint32_t descriptor_dimension,
    uint64_t feature_set_id_a,
    uint64_t feature_set_id_b,
    const Lardon3DMatchFileEntry *entries,
    uint32_t match_count);

Lardon3DMatchFileResult lardon3d_match_file_read(
    int fd,
    Lardon3DMatchFileHeader *header,
    Lardon3DMatchFileEntry *entries,
    size_t entry_capacity,
    uint32_t *match_count,
    uint64_t expected_feature_set_id_a,
    uint64_t expected_feature_set_id_b,
    uint32_t expected_feature_count_a,
    uint32_t expected_feature_count_b);

Lardon3DMatchFileResult lardon3d_match_file_validate(
    const char *path,
    Lardon3DMatchFileHeader *header,
    uint64_t expected_feature_set_id_a,
    uint64_t expected_feature_set_id_b,
    uint32_t expected_feature_count_a,
    uint32_t expected_feature_count_b);

Lardon3DMatchFileResult lardon3d_match_file_validate_asset(
    const char *path,
    const unsigned char expected_sha256[32],
    uint64_t expected_size,
    Lardon3DMatchFileHeader *header,
    uint64_t expected_feature_set_id_a,
    uint64_t expected_feature_set_id_b,
    uint32_t expected_feature_count_a,
    uint32_t expected_feature_count_b);

#endif
