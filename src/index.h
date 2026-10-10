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
    uint32_t Reserved;
    uint64_t Start;
    uint64_t Count;
    uint64_t SkipStart;
    uint64_t SkipCount;
} CearchTrigramPosting;

typedef struct {
    uint32_t PostingIndex;
    uint32_t Ordinal;
    uint64_t ByteOffset;
    uint32_t FileId;
    uint32_t Reserved;
} CearchPostingSkip;

typedef struct {
    uint32_t *Keys;
    size_t Count;
} CearchNameQuery;

typedef struct {
    CearchFileEntry *Entries;
    size_t Count;
    size_t Capacity;
    const CearchTrigramPosting *Trigrams;
    size_t TrigramCount;
    const uint32_t *TrigramFileIds;
    size_t TrigramPostingCount;
    const unsigned char *TrigramData;
    size_t TrigramDataSize;
    const CearchPostingSkip *TrigramSkips;
    size_t TrigramSkipCount;
    bool OwnsNameIndex;
} CearchIndex;

typedef struct {
    const CearchIndex *Index;
    const CearchNameQuery *Query;
    uint64_t Position;
    uint64_t End;
    uint32_t AllCursor;
    const CearchTrigramPosting *Seed;
    size_t BytePosition;
    uint32_t CurrentFileId;
    bool EncodedSeed;
    bool AllFiles;
    bool Empty;
} CearchNameCandidateIterator;

bool CearchBuildNameIndex(CearchIndex *Index);
/* Attaches arrays already validated by CearchStoreOpen or produced by CearchBuildNameIndex. */
bool CearchAttachNameIndex(CearchIndex *Index,
                           const CearchTrigramPosting *Trigrams, size_t TrigramCount,
                           const uint32_t *FileIds, size_t FileIdCount,
                           const unsigned char *PostingData, size_t PostingDataSize,
                           const CearchPostingSkip *Skips, size_t SkipCount);
bool CearchEncodeNameIndex(const CearchIndex *Index,
                           CearchTrigramPosting **EncodedTrigrams,
                           unsigned char **PostingData, size_t *PostingDataSize,
                           CearchPostingSkip **Skips, size_t *SkipCount);
void CearchFreeNameIndex(CearchIndex *Index);
bool CearchNameQueryPrepare(const char *Query, CearchNameQuery *Prepared);
void CearchNameQueryFree(CearchNameQuery *Prepared);
bool CearchNameCandidatesBegin(const CearchIndex *Index, const CearchNameQuery *Query,
                               CearchNameCandidateIterator *Iterator);
bool CearchNameCandidatesNext(CearchNameCandidateIterator *Iterator, uint32_t *FileId);
bool CearchNameCandidatePrepared(const CearchIndex *Index, const CearchNameQuery *Query, uint32_t FileId);
bool CearchNameCandidate(CearchIndex *Index, const char *Query, uint32_t FileId);

#endif
