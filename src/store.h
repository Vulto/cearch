#ifndef CEARCH_STORE_H
#define CEARCH_STORE_H

#include "index.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CearchStoreVersion 2U

typedef struct {
    uint32_t PathOffset;
    uint32_t PathLength;
    uint64_t Size;
    int64_t Mtime;
    uint64_t Device;
    uint64_t Inode;
    uint8_t Type;
    uint8_t Reserved[7];
} CearchDiskEntry;

typedef struct {
    int Descriptor;
    void *Mapping;
    size_t MappingSize;
    const CearchDiskEntry *Entries;
    size_t EntryCount;
    const char *Strings;
    size_t StringsSize;
    const CearchTrigramPosting *Trigrams;
    size_t TrigramCount;
    const uint32_t *TrigramFileIds;
    size_t TrigramPostingCount;
} CearchStore;

bool CearchStoreWrite(const char *Database,
                      const CearchDiskEntry *Entries, size_t EntryCount,
                      const char *Strings, size_t StringsSize,
                      const CearchTrigramPosting *Trigrams, size_t TrigramCount,
                      const uint32_t *TrigramFileIds, size_t TrigramPostingCount);
bool CearchStoreOpen(const char *Database, CearchStore *Store);
void CearchStoreClose(CearchStore *Store);
const char *CearchStorePath(const CearchStore *Store, size_t EntryIndex);

#endif
