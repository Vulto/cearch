#include "index.h"

#include <stdlib.h>
#include <string.h>

#define TrigramTableInitialCapacity 4096U

typedef struct {
    uint32_t Key;
    uint32_t FileId;
} TrigramPair;

static uint32_t TrigramKey(unsigned char A, unsigned char B, unsigned char C) {
    return ((uint32_t)A << 16U) | ((uint32_t)B << 8U) | (uint32_t)C;
}

static int CompareTrigramPair(const void *Left, const void *Right) {
    const TrigramPair *A = Left;
    const TrigramPair *B = Right;
    if (A->Key < B->Key) return -1;
    if (A->Key > B->Key) return 1;
    if (A->FileId < B->FileId) return -1;
    if (A->FileId > B->FileId) return 1;
    return 0;
}

static bool ReservePairs(TrigramPair **Pairs, size_t *Capacity, size_t Needed) {
    if (Needed <= *Capacity) return true;
    size_t Next = *Capacity ? *Capacity : TrigramTableInitialCapacity;
    while (Next < Needed) {
        if (Next > SIZE_MAX / 2U) return false;
        Next *= 2U;
    }
    if (Next > SIZE_MAX / sizeof(**Pairs)) return false;
    TrigramPair *NewPairs = realloc(*Pairs, Next * sizeof(**Pairs));
    if (!NewPairs) return false;
    *Pairs = NewPairs;
    *Capacity = Next;
    return true;
}

static bool AddPathTrigrams(TrigramPair **Pairs, size_t *Count, size_t *Capacity,
                            const char *Text, uint32_t FileId) {
    size_t Length = strlen(Text);
    if (Length < 3U) return true;
    for (size_t Index = 0; Index + 2U < Length; ++Index) {
        unsigned char A = (unsigned char)Text[Index];
        unsigned char B = (unsigned char)Text[Index + 1U];
        unsigned char C = (unsigned char)Text[Index + 2U];
        if (A >= 'A' && A <= 'Z') A = (unsigned char)(A + ('a' - 'A'));
        if (B >= 'A' && B <= 'Z') B = (unsigned char)(B + ('a' - 'A'));
        if (C >= 'A' && C <= 'Z') C = (unsigned char)(C + ('a' - 'A'));
        uint32_t Key = TrigramKey(A, B, C);
        if (*Count == SIZE_MAX || !ReservePairs(Pairs, Capacity, *Count + 1U)) return false;
        (*Pairs)[(*Count)++] = (TrigramPair){ .Key = Key, .FileId = FileId };
    }
    return true;
}

static bool BuildTrigramIndex(CearchIndex *Index) {
    TrigramPair *Pairs = nullptr;
    size_t PairCount = 0, PairCapacity = 0;
    for (size_t FileIndex = 0; FileIndex < Index->Count; ++FileIndex) {
        if (!AddPathTrigrams(&Pairs, &PairCount, &PairCapacity,
                             Index->Entries[FileIndex].Path, (uint32_t)FileIndex)) {
            free(Pairs);
            return false;
        }
    }
    qsort(Pairs, PairCount, sizeof(*Pairs), CompareTrigramPair);

    size_t PostingCount = 0;
    for (size_t PairIndex = 0; PairIndex < PairCount; ++PairIndex) {
        if (PairIndex && Pairs[PairIndex].Key == Pairs[PairIndex - 1U].Key &&
            Pairs[PairIndex].FileId == Pairs[PairIndex - 1U].FileId) continue;
        Pairs[PostingCount++] = Pairs[PairIndex];
    }

    size_t UniqueKeys = 0;
    for (size_t PairIndex = 0; PairIndex < PostingCount; ++PairIndex)
        if (PairIndex == 0 || Pairs[PairIndex].Key != Pairs[PairIndex - 1U].Key)
            ++UniqueKeys;
    if (UniqueKeys > SIZE_MAX / sizeof(*Index->Trigrams) || PostingCount > SIZE_MAX / sizeof(*Index->TrigramFileIds)) {
        free(Pairs);
        return false;
    }

    Index->Trigrams = calloc(UniqueKeys, sizeof(*Index->Trigrams));
    Index->TrigramFileIds = malloc(PostingCount * sizeof(*Index->TrigramFileIds));
    if ((UniqueKeys && !Index->Trigrams) || (PostingCount && !Index->TrigramFileIds)) {
        free(Pairs);
        return false;
    }
    Index->TrigramCount = UniqueKeys;
    Index->TrigramPostingCount = PostingCount;

    size_t PostingIndex = 0, KeyIndex = 0;
    while (PostingIndex < PostingCount) {
        size_t Start = PostingIndex;
        uint32_t Key = Pairs[PostingIndex].Key;
        while (PostingIndex < PostingCount && Pairs[PostingIndex].Key == Key) {
            Index->TrigramFileIds[PostingIndex] = Pairs[PostingIndex].FileId;
            ++PostingIndex;
        }
        Index->Trigrams[KeyIndex++] = (CearchTrigramPosting){
            .Key = Key, .Start = Start, .Count = PostingIndex - Start
        };
    }
    free(Pairs);
    return true;
}

static int ComparePostingKey(const void *Left, const void *Right) {
    const CearchTrigramPosting *A = Left;
    const CearchTrigramPosting *B = Right;
    return A->Key < B->Key ? -1 : A->Key > B->Key ? 1 : 0;
}

static const CearchTrigramPosting *FindPosting(const CearchIndex *Index, uint32_t Key) {
    return bsearch(&(CearchTrigramPosting){ .Key = Key },
                   Index->Trigrams, Index->TrigramCount, sizeof(*Index->Trigrams),
                   ComparePostingKey);
}

static bool QueryTrigramKeys(const char *Query, uint32_t **Keys, size_t *Count) {
    size_t Length = strlen(Query);
    *Keys = nullptr;
    *Count = 0;
    if (Length < 3U) return true;
    if (Length - 2U > SIZE_MAX / sizeof(**Keys)) return false;
    uint32_t *Result = malloc((Length - 2U) * sizeof(*Result));
    if (!Result) return false;
    for (size_t Index = 0; Index + 2U < Length; ++Index) {
        unsigned char A = (unsigned char)Query[Index];
        unsigned char B = (unsigned char)Query[Index + 1U];
        unsigned char C = (unsigned char)Query[Index + 2U];
        if (A >= 'A' && A <= 'Z') A = (unsigned char)(A + ('a' - 'A'));
        if (B >= 'A' && B <= 'Z') B = (unsigned char)(B + ('a' - 'A'));
        if (C >= 'A' && C <= 'Z') C = (unsigned char)(C + ('a' - 'A'));
        uint32_t Key = TrigramKey(A, B, C);
        bool Seen = false;
        for (size_t Previous = 0; Previous < *Count; ++Previous)
            if (Result[Previous] == Key) { Seen = true; break; }
        if (!Seen) Result[(*Count)++] = Key;
    }
    *Keys = Result;
    return true;
}

static bool HasAllQueryTrigrams(const CearchIndex *Index, uint32_t FileId,
                                const uint32_t *Keys, size_t KeyCount) {
    (void)fprintf(stderr, "index-debug: file=%u keys=%zu postings=%zu trigrams=%zu\\n", FileId, KeyCount, Index->TrigramPostingCount, Index->TrigramCount);
    for (size_t KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex) {
        const CearchTrigramPosting *Posting = FindPosting(Index, Keys[KeyIndex]);
        if (!Posting) { (void)fprintf(stderr, "index-debug: missing key=%u\\n", Keys[KeyIndex]); return false; }
        bool Found = false;
        size_t Low = Posting->Start, High = Posting->Start + Posting->Count;
        while (Low < High) {
            size_t Mid = Low + (High - Low) / 2U;
            uint32_t Candidate = Index->TrigramFileIds[Mid];
            if (Candidate < FileId) Low = Mid + 1U;
            else High = Mid;
        }
        Found = Low < Posting->Start + Posting->Count && Index->TrigramFileIds[Low] == FileId;
        if (!Found) { (void)fprintf(stderr, "index-debug: absent file=%u key=%u\\n", FileId, Keys[KeyIndex]); return false; }
    }
    return true;
}

bool CearchBuildNameIndex(CearchIndex *Index) {
    return BuildTrigramIndex(Index);
}

void CearchFreeNameIndex(CearchIndex *Index) {
    free(Index->Trigrams);
    free(Index->TrigramFileIds);
    Index->Trigrams = nullptr;
    Index->TrigramFileIds = nullptr;
    Index->TrigramCount = 0;
    Index->TrigramPostingCount = 0;
}

bool CearchNameQueryPrepare(const char *Query, CearchNameQuery *Prepared) {
    if (!Prepared || !Query) return false;
    Prepared->Keys = nullptr;
    Prepared->Count = 0;
    return QueryTrigramKeys(Query, &Prepared->Keys, &Prepared->Count);
}

void CearchNameQueryFree(CearchNameQuery *Prepared) {
    if (!Prepared) return;
    free(Prepared->Keys);
    Prepared->Keys = nullptr;
    Prepared->Count = 0;
}

bool CearchNameCandidatePrepared(const CearchIndex *Index, const CearchNameQuery *Query, uint32_t FileId) {
    if (!Index || !Query) return false;
    if (!Query->Count) return true;
    return HasAllQueryTrigrams(Index, FileId, Query->Keys, Query->Count);
}

bool CearchNameCandidate(CearchIndex *Index, const char *Query, uint32_t FileId) {
    CearchNameQuery Prepared = {0};
    if (!CearchNameQueryPrepare(Query, &Prepared)) return false;
    bool Match = CearchNameCandidatePrepared(Index, &Prepared, FileId);
    CearchNameQueryFree(&Prepared);
    return Match;
}
