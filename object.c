// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions: object_write, object_read
 
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}
 
// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Write an object to the store.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header: "blob <size>\0", "tree <size>\0", or "commit <size>\0"
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // Header is at most "commit 9999999999\0" = 18 chars; 64 is plenty
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    // header_len does NOT include the null terminator, but the null IS the separator
    size_t full_len = (size_t)header_len + 1 + len; // header + '\0' + data

    uint8_t *full = malloc(full_len);
    if (!full) return -1;

    memcpy(full, header, (size_t)header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);
 
    // 2. Compute SHA-256 of the full object
    ObjectID id;
    compute_hash(full, full_len, &id);
    if (id_out) *id_out = id;

    // 3. Deduplication — if already stored, nothing to do
    if (object_exists(&id)) {
        free(full);
        return 0;
    }

    // 4. Create shard directory .pes/objects/XX/
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // Ignore error if already exists

    // 5. Build final path and a temp path in the same directory
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);

    char tmp_path[560];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    // 6. Write to temp file
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    ssize_t written = write(fd, full, full_len);
    free(full);

    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // 7. fsync the temp file
    fsync(fd);
    close(fd);

    // 8. Atomically rename temp -> final
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // 9. fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// Read an object from the store.
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build the path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 3. Integrity check: recompute hash and compare to filename
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1; // Corrupt object
    }

    // 4. Parse header: find the '\0' separating header from data
    uint8_t *null_pos = memchr(buf, '\0', (size_t)file_size);
    if (!null_pos) {
        free(buf);
        return -1;
    }

    // 5. Parse type from header
    if (strncmp((char *)buf, "blob ", 5) == 0) {
        if (type_out) *type_out = OBJ_BLOB;
    } else if (strncmp((char *)buf, "tree ", 5) == 0) {
        if (type_out) *type_out = OBJ_TREE;
    } else if (strncmp((char *)buf, "commit ", 7) == 0) {
        if (type_out) *type_out = OBJ_COMMIT;
    } else {
        free(buf);
        return -1;
    }
 
    // 6. Extract data portion (everything after the '\0')
    size_t header_len = (size_t)(null_pos - buf);
    size_t data_len   = (size_t)file_size - header_len - 1;

    uint8_t *data_copy = malloc(data_len + 1); // +1 for safety null terminator
    if (!data_copy) {
        free(buf);
        return -1;
    }
    memcpy(data_copy, null_pos + 1, data_len);
    data_copy[data_len] = '\0';

    free(buf);

    if (data_out)  *data_out  = data_copy;
    if (len_out)   *len_out   = data_len;

    return 0;
}
