#ifndef CEARCH_STORE_H
#define CEARCH_STORE_H

#include "index.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CearchStoreVersion 4U

typedef struct {
    uint32_t PathOffset;
    uint32_t PathLength;
    uint64_t Size;
    int64_t Mtime;
    uint32_t MtimeNsec;
    int64_t Ctime;
    uint32_t CtimeNsec;
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
    const unsigned char *TrigramData;
    size_t TrigramDataSize;
    const CearchPostingSkip *TrigramSkips;
    size_t TrigramSkipCount;
    size_t TrigramPostingCount;
    const CearchTrigramPosting *ContentTrigrams;
    size_t ContentTrigramCount;
    const unsigned char *ContentTrigramData;
    size_t ContentTrigramDataSize;
    const CearchPostingSkip *ContentTrigramSkips;
    size_t ContentTrigramSkipCount;
    size_t ContentTrigramPostingCount;
} CearchStore;

bool CearchStoreWrite(const char *Database,
                      const CearchDiskEntry *Entries, size_t EntryCount,
                      const char *Strings, size_t StringsSize,
                      const CearchTrigramPosting *Trigrams, size_t TrigramCount,
                      const unsigned char *PostingData, size_t PostingDataSize,
                      const CearchPostingSkip *Skips, size_t SkipCount,
                      size_t TrigramPostingCount,
                      const CearchTrigramPosting *ContentTrigrams, size_t ContentTrigramCount,
                      const unsigned char *ContentPostingData, size_t ContentPostingDataSize,
                      const CearchPostingSkip *ContentSkips, size_t ContentSkipCount,
                      size_t ContentPostingCount);
bool CearchStoreOpen(const char *Database, CearchStore *Store);
void CearchStoreClose(CearchStore *Store);
const char *CearchStorePath(const CearchStore *Store, size_t EntryIndex);

#endif
