// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── Forward Declarations ───────────────────────────────────────────────────

// Declare object_write from object.c to fix linking errors
extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
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

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
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
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

// Helper to sort index entries by path (crucial for grouping subdirectories)
static int cmp_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Recursive helper to build tree from a slice of index entries at a given path depth.
// `depth` represents how many characters into the path string we have already processed.
static int build_tree_recursive(IndexEntry *entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    
    int i = 0;
    while (i < count) {
        if (tree.count >= MAX_TREE_ENTRIES) return -1;

        // The remaining relative path we are looking at for this level
        const char *path = entries[i].path + depth;
        
        // Find the first '/' to see if this belongs in a subdirectory
        const char *slash = strchr(path, '/');
        
        if (!slash) {
            // No slash found -> Direct file in the current directory level
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = entries[i].mode;
            entry->hash = entries[i].hash;
            strncpy(entry->name, path, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            i++;
        } else {
            // Slash found -> This is a subdirectory
            size_t dir_len = slash - path;
            char dir_name[256];
            if (dir_len >= sizeof(dir_name)) return -1;
            
            memcpy(dir_name, path, dir_len);
            dir_name[dir_len] = '\0';
            
            // Because the index is sorted by path, all files inside this subdirectory
            // will be contiguous in the array. We scan forward to group them.
            int j = i;
            while (j < count) {
                const char *j_path = entries[j].path + depth;
                // If it starts with the same directory name followed by a slash, it belongs inside
                if (strncmp(j_path, dir_name, dir_len) == 0 && j_path[dir_len] == '/') {
                    j++;
                } else {
                    break;
                }
            }
            
            // Recursively build the subtree for this group of files
            ObjectID subtree_id;
            // The new depth jumps past the directory name and the slash (+1)
            if (build_tree_recursive(&entries[i], j - i, depth + dir_len + 1, &subtree_id) != 0) {
                return -1;
            }
            
            // Add the new directory tree object to our current tree
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = MODE_DIR;
            entry->hash = subtree_id;
            strncpy(entry->name, dir_name, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            
            // Advance the main index (i) past the whole group we just processed
            i = j;
        }
    }
    
    // Serialize and write the tree object to disk
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) {
        return -1;
    }
    
    int result = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    
    return result;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index idx;
    
    // Load the index containing all staged files
    if (index_load(&idx) != 0) {
        return -1;
    }
    
    if (idx.count == 0) {
        fprintf(stderr, "error: no files staged\n");
        return -1;
    }
    
    // Sort index entries lexicographically by path.
    // This is a strict requirement for Git trees and makes recursive grouping trivial.
    qsort(idx.entries, idx.count, sizeof(IndexEntry), cmp_index_entries);
    
    // Start building the tree from depth 0 (the root path)
    return build_tree_recursive(idx.entries, idx.count, 0, id_out);
}
