// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions: tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
// "<mode-as-ascii-octal> <n>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
// "100644 hello.txt\0" followed by 32 raw bytes of SHA-256
 
#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── IMPLEMENTED ────────────────────────────────────────────────────────────

// Forward declarations from object.c and index.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int index_load(Index *index);

// Recursive helper: builds a tree for a "virtual directory" described by
// a subset of index entries that all share a common path prefix.
//
// entries[]  — the full index entries array
// count      — how many entries to consider (starting at entries[0])
// prefix     — the directory prefix these entries are relative to
//              (empty string "" for the root)
// id_out     — receives the hash of the written tree object
//
// Returns 0 on success, -1 on error.
static int write_tree_level(const IndexEntry *entries, int count,
                            const char *prefix, ObjectID *id_out)
{
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *full_path = entries[i].path;

        // Strip the current prefix to get the relative name
        const char *rel = full_path;
        if (prefix[0] != '\0') {
            size_t plen = strlen(prefix);
            if (strncmp(full_path, prefix, plen) == 0 && full_path[plen] == '/')
                rel = full_path + plen + 1;
            else
                rel = full_path; // shouldn't happen
        }

        // Does this entry have a '/' in the relative portion?
        char *slash = strchr(rel, '/');

        if (slash == NULL) {
            // It's a plain file — add directly as a blob entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // It's inside a subdirectory — collect all entries that share
            // this subdirectory name, then recurse.
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[512];
            strncpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the new prefix for the recursive call
            char new_prefix[1024];
            if (prefix[0] != '\0')
                snprintf(new_prefix, sizeof(new_prefix), "%s/%s", prefix, dir_name);
            else
                strncpy(new_prefix, dir_name, sizeof(new_prefix) - 1);

            // Find how many consecutive entries belong to this subdirectory
            int j = i;
            while (j < count) {
                const char *fp = entries[j].path;
                const char *rp = fp;
                if (prefix[0] != '\0') {
                    size_t plen = strlen(prefix);
                    if (strncmp(fp, prefix, plen) == 0 && fp[plen] == '/')
                        rp = fp + plen + 1;
                }
                char *s = strchr(rp, '/');
                if (!s) break; // no slash → not in this dir

                size_t dnl = (size_t)(s - rp);
                if (dnl != dir_name_len || strncmp(rp, dir_name, dir_name_len) != 0)
                    break; // different subdirectory

                j++;
            }

            // Recurse for the sub-range [i, j)
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, new_prefix, &sub_id) != 0)
                return -1;

            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = 0040000; // directory
            te->hash = sub_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i = j;
        }
    }

    // Serialize and store this level's tree
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        // Empty tree — write a tree with no entries
        Tree empty;
        empty.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }

    // Entries are already sorted by path (index_save sorts them)
    return write_tree_level(index.entries, index.count, "", id_out);
}
