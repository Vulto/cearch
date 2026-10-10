#include "../src/index.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void TestEncodedPostingCheckpoints(void) {
    enum { EntryCount = 70 };
    char Paths[EntryCount][32];
    CearchFileEntry Entries[EntryCount] = {0};
    for (size_t Index = 0U; Index < EntryCount; ++Index) {
        int Length = snprintf(Paths[Index], sizeof(Paths[Index]), "/tmp/alpha-%02zu.txt", Index);
        assert(Length > 0 && (size_t)Length < sizeof(Paths[Index]));
        Entries[Index].Path = Paths[Index];
    }

    CearchIndex Index = { .Entries = Entries, .Count = EntryCount };
    assert(CearchBuildNameIndex(&Index));
    CearchTrigramPosting *Trigrams = nullptr;
    unsigned char *PostingData = nullptr;
    size_t PostingDataSize = 0U;
    CearchPostingSkip *Skips = nullptr;
    size_t SkipCount = 0U;
    assert(CearchEncodeNameIndex(&Index, &Trigrams, &PostingData, &PostingDataSize,
                                 &Skips, &SkipCount));
    assert(PostingDataSize > 0U && SkipCount > 0U);

    CearchIndex MappedIndex = { .Entries = Entries, .Count = EntryCount };
    assert(CearchAttachNameIndex(&MappedIndex, Trigrams, Index.TrigramCount, nullptr,
                                 Index.TrigramPostingCount, PostingData, PostingDataSize,
                                 Skips, SkipCount));
    CearchNameQuery Query = {0};
    CearchNameCandidateIterator Iterator = {0};
    assert(CearchNameQueryPrepare("alpha", &Query));
    assert(CearchNameCandidatesBegin(&MappedIndex, &Query, &Iterator));
    uint32_t FileId = UINT32_MAX;
    for (uint32_t Expected = 0U; Expected < EntryCount; ++Expected) {
        assert(CearchNameCandidatesNext(&Iterator, &FileId));
        assert(FileId == Expected);
    }
    assert(!CearchNameCandidatesNext(&Iterator, &FileId));
    CearchNameQueryFree(&Query);
    CearchFreeNameIndex(&MappedIndex);
    CearchFreeNameIndex(&Index);
    free(Trigrams);
    free(PostingData);
    free(Skips);
}

int main(void) {
    CearchFileEntry Entries[] = {
        { .Path = "/tmp/Alpha.txt" },
        { .Path = "/tmp/beta-notes.md" },
        { .Path = "/tmp/aaaaaa" },
        { .Path = "/tmp/other" }
    };
    CearchIndex Index = {
        .Entries = Entries,
        .Count = sizeof(Entries) / sizeof(Entries[0])
    };

    assert(CearchBuildNameIndex(&Index));
    assert(Index.TrigramCount > 0U);
    assert(Index.TrigramPostingCount > 0U);
    assert(Index.OwnsNameIndex);
    assert(CearchNameCandidate(&Index, "ALPHA", 0U));
    assert(CearchNameCandidate(&Index, "pha", 0U));
    assert(!CearchNameCandidate(&Index, "missing", 0U));
    assert(CearchNameCandidate(&Index, "aaaaa", 2U));
    assert(!CearchNameCandidate(&Index, "aaaaa", 3U));
    assert(CearchNameCandidate(&Index, "be", 1U));
    assert(CearchNameCandidate(&Index, "", 3U));

    CearchNameQuery Prepared = {0};
    assert(CearchNameQueryPrepare("ALPHA", &Prepared));
    CearchNameCandidateIterator Iterator = {0};
    assert(CearchNameCandidatesBegin(&Index, &Prepared, &Iterator));
    uint32_t FileId = UINT32_MAX;
    assert(CearchNameCandidatesNext(&Iterator, &FileId) && FileId == 0U);
    assert(!CearchNameCandidatesNext(&Iterator, &FileId));
    CearchNameQueryFree(&Prepared);

    assert(CearchNameQueryPrepare("missing", &Prepared));
    assert(CearchNameCandidatesBegin(&Index, &Prepared, &Iterator));
    assert(!CearchNameCandidatesNext(&Iterator, &FileId));
    CearchNameQueryFree(&Prepared);

    assert(CearchNameQueryPrepare("be", &Prepared));
    assert(CearchNameCandidatesBegin(&Index, &Prepared, &Iterator));
    for (uint32_t Expected = 0U; Expected < 4U; ++Expected) {
        assert(CearchNameCandidatesNext(&Iterator, &FileId));
        assert(FileId == Expected);
    }
    assert(!CearchNameCandidatesNext(&Iterator, &FileId));
    CearchNameQueryFree(&Prepared);

    const CearchTrigramPosting *SavedTrigrams = Index.Trigrams;
    const uint32_t *SavedFileIds = Index.TrigramFileIds;
    size_t SavedTrigramCount = Index.TrigramCount;
    size_t SavedPostingCount = Index.TrigramPostingCount;
    const unsigned char *SavedPostingData = Index.TrigramData;
    size_t SavedPostingDataSize = Index.TrigramDataSize;
    const CearchPostingSkip *SavedSkips = Index.TrigramSkips;
    size_t SavedSkipCount = Index.TrigramSkipCount;

    CearchIndex MappedIndex = { .Entries = Entries, .Count = Index.Count };
    assert(CearchAttachNameIndex(&MappedIndex, SavedTrigrams, SavedTrigramCount,
                                 SavedFileIds, SavedPostingCount, SavedPostingData,
                                 SavedPostingDataSize, SavedSkips, SavedSkipCount));
    assert(!MappedIndex.OwnsNameIndex);
    assert(CearchNameQueryPrepare("ALPHA", &Prepared));
    assert(CearchNameCandidatesBegin(&MappedIndex, &Prepared, &Iterator));
    assert(CearchNameCandidatesNext(&Iterator, &FileId) && FileId == 0U);
    assert(!CearchNameCandidatesNext(&Iterator, &FileId));
    CearchNameQueryFree(&Prepared);

    CearchFreeNameIndex(&MappedIndex);
    assert(MappedIndex.Trigrams == NULL);
    assert(MappedIndex.TrigramFileIds == NULL);
    assert(Index.Trigrams == SavedTrigrams);
    assert(Index.TrigramFileIds == SavedFileIds);

    CearchFreeNameIndex(&Index);
    assert(Index.Trigrams == NULL);
    assert(Index.TrigramFileIds == NULL);
    assert(Index.TrigramCount == 0U);
    assert(Index.TrigramPostingCount == 0U);

    TestEncodedPostingCheckpoints();
    puts("cearch index unit tests passed");
    return 0;
}
