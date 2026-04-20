// index.c — Staging area implementation
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                (uint32_t)st.st_size != index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

int index_load(Index *index) {
    memset(index, 0, sizeof(Index));
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  /* no index file = empty index, not an error */

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        unsigned long long mtime_tmp;
        unsigned int size_tmp;
        int n = fscanf(f, "%o %64s %llu %u %255s\n",
                       &e->mode, hex, &mtime_tmp, &size_tmp, e->path);
        if (n != 5) break;
        if (hex_to_hash(hex, &e->hash) != 0) break;
        e->mtime_sec = (uint64_t)mtime_tmp;
        e->size      = (uint32_t)size_tmp;
        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    /* sort a copy by path */
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;

    for (int i = 0; i < sorted->count - 1; i++)
        for (int j = i + 1; j < sorted->count; j++)
            if (strcmp(sorted->entries[i].path, sorted->entries[j].path) > 0) {
                IndexEntry tmp = sorted->entries[i];
                sorted->entries[i] = sorted->entries[j];
                sorted->entries[j] = tmp;
            }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        IndexEntry *e = &sorted->entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                (unsigned int)e->size,
                e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);
    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    /* read file contents */
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t file_size = (sz > 0) ? (size_t)sz : 0;

    /* malloc(0) is undefined — use 1 byte minimum */
    void *contents = malloc(file_size + 1);
    if (!contents) { fclose(f); return -1; }
    if (file_size > 0)
        fread(contents, 1, file_size, f);
    fclose(f);

    /* write blob to object store */
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, file_size, &blob_id) != 0) {
        free(contents);
        return -1;
    }
    free(contents);

    /* get file metadata */
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    /* find existing entry or create new one */
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    entry->hash      = blob_id;
    entry->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size      = (uint32_t)st.st_size;

    return index_save(index);
}
