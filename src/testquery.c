#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    CearchFileEntry Entries[] = {
        { .Path = "/tmp/needle.txt", .Type = 'f' },
        { .Path = "/tmp/other.txt", .Type = 'f' }
    };
    CearchIndex Index = { .Entries = Entries, .Count = 2U };
    if (!CearchBuildNameIndex(&Index)) return 1;
    CearchNameQuery Query = {0};
    if (!CearchNameQueryPrepare("needle", &Query)) return 2;
    bool First = CearchNameCandidatePrepared(&Index, &Query, 0U);
    bool Second = CearchNameCandidatePrepared(&Index, &Query, 1U);
    CearchNameQueryFree(&Query);
    CearchFreeNameIndex(&Index);
    if (!First || Second) {
        fprintf(stderr, "trigram candidates incorrect: first=%d second=%d\n", First, Second);
        return 3;
    }
    puts("trigram candidate tests passed");
    return 0;
}
