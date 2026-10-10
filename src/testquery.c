#include "index.h"
#include <stdio.h>
int main(void) {
    CearchFileEntry Entries[] = {{.Path="/tmp/needle.txt"},{.Path="/tmp/other.txt"}};
    CearchIndex Index = {.Entries=Entries,.Count=2};
    if (!CearchBuildNameIndex(&Index)) return 1;
    CearchNameQuery Query = {0};
    if (!CearchNameQueryPrepare("needle", &Query)) return 2;
    printf("count=%zu keys=%zu p=%zu t=%zu first=%d second=%d\n", Index.Count, Query.Count, Index.TrigramPostingCount, Index.TrigramCount, CearchNameCandidatePrepared(&Index,&Query,0), CearchNameCandidatePrepared(&Index,&Query,1));
    return 0;
}
