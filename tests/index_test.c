#include "../src/index.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

    const CearchTrigramPosting *SavedTrigrams = Index.Trigrams;
    const uint32_t *SavedFileIds = Index.TrigramFileIds;
    size_t SavedTrigramCount = Index.TrigramCount;
    size_t SavedPostingCount = Index.TrigramPostingCount;

    CearchIndex MappedIndex = { .Entries = Entries, .Count = Index.Count };
    assert(CearchAttachNameIndex(&MappedIndex, SavedTrigrams, SavedTrigramCount,
                                 SavedFileIds, SavedPostingCount));
    assert(!MappedIndex.OwnsNameIndex);
    CearchNameQuery Prepared = {0};
    assert(CearchNameQueryPrepare("ALPHA", &Prepared));
    assert(CearchNameCandidatePrepared(&MappedIndex, &Prepared, 0U));
    assert(!CearchNameCandidatePrepared(&MappedIndex, &Prepared, 1U));
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

    puts("cearch index unit tests passed");
    return 0;
}
