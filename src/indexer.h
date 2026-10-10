#ifndef CEARCH_INDEXER_H
#define CEARCH_INDEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t EntryCount;
    size_t FileCount;
    size_t DirectoryCount;
} CearchIndexStats;

bool CearchBuildSnapshot(const char *Database, const char *const *Roots,
                         size_t RootCount, CearchIndexStats *Stats);

#endif
