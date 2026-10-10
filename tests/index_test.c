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
    assert(CearchNameCandidate(&Index, "ALPHA", 0));
    assert(CearchNameCandidate(&Index, "pha", 0));
    assert(!CearchNameCandidate(&Index, "missing", 0));
    assert(CearchNameCandidate(&Index, "aaaaa", 2));
    assert(!CearchNameCandidate(&Index, "aaaaa", 3));
    assert(CearchNameCandidate(&Index, "be", 1));
    assert(CearchNameCandidate(&Index, "", 3));

    CearchFreeNameIndex(&Index);
    assert(Index.Trigrams == NULL);
    assert(Index.TrigramFileIds == NULL);
    assert(Index.TrigramCount == 0);
    assert(Index.TrigramPostingCount == 0);

    puts("cearch index unit tests passed");
    return 0;
}
