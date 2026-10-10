#ifndef CEARCH_INDEX_H
#define CEARCH_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *Path;
    uint64_t Size;
    int64_t Mtime;
    uint64_t Device;
    uint64_t Inode;
    char Type;
} CearchFileEntry;

typedef struct {
    uint32_t Key;
    size_t Start;
    size_t Count;
} CearchTrigramPosting;

typedef struct {
    CearchFileEntry *Entries;
    size_t Count;
    size_t Capacity;
    CearchTrigramPosting *Trigrams;
    size_t TrigramCount;
    uint32_t *TrigramFileIds;
    size_t TrigramPostingCount;
} CearchIndex;

bool CearchBuildNameIndex(CearchIndex *Index);
void CearchFreeNameIndex(CearchIndex *Index);
bool CearchNameCandidate(CearchIndex *Index, const char *Query, uint32_t FileId);

#endif
