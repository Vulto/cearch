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

static int CompareTrigramKey(const void *Left, const void *Right) {
    uint32_t A = *(const uint32_t *)Left;
    uint32_t B = *(const uint32_t *)Right;
    return A < B ? -1 : A > B ? 1 : 0;
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
    if (Index->Count > UINT32_MAX) return false;
    TrigramPair *Pairs = nullptr;
    size_t PairCount = 0, PairCapacity = 0;
    for (size_t FileIndex = 0; FileIndex < Index->Count; ++FileIndex) {
        if (!Index->Entries[FileIndex].Path ||
            !AddPathTrigrams(&Pairs, &PairCount, &PairCapacity,
                             Index->Entries[FileIndex].Path, (uint32_t)FileIndex)) {
            free(Pairs);
            return false;
        }
    }
    if (PairCount > 1U) qsort(Pairs, PairCount, sizeof(*Pairs), CompareTrigramPair);

    size_t PostingCount = 0;
    for (size_t PairIndex = 0; PairIndex < PairCount; ++PairIndex) {
        if (PairIndex && Pairs[PairIndex].Key == Pairs[PairIndex - 1U].Key &&
            Pairs[PairIndex].FileId == Pairs[PairIndex - 1U].FileId) continue;
        Pairs[PostingCount++] = Pairs[PairIndex];
    }

    size_t UniqueKeys = 0;
    for (size_t PairIndex = 0; PairIndex < PostingCount; ++PairIndex)
        if (PairIndex == 0U || Pairs[PairIndex].Key != Pairs[PairIndex - 1U].Key)
            ++UniqueKeys;

    if (UniqueKeys > SIZE_MAX / sizeof(CearchTrigramPosting) ||
        PostingCount > SIZE_MAX / sizeof(uint32_t)) {
        free(Pairs);
        return false;
    }
    CearchTrigramPosting *Trigrams = calloc(UniqueKeys ? UniqueKeys : 1U, sizeof(*Trigrams));
    uint32_t *FileIds = malloc(PostingCount ? PostingCount * sizeof(*FileIds) : sizeof(*FileIds));
    if (!Trigrams || !FileIds) {
        free(Trigrams);
        free(FileIds);
        free(Pairs);
        return false;
    }

    size_t PostingIndex = 0U, KeyIndex = 0U;
    while (PostingIndex < PostingCount) {
        size_t Start = PostingIndex;
        uint32_t Key = Pairs[PostingIndex].Key;
        while (PostingIndex < PostingCount && Pairs[PostingIndex].Key == Key) {
            FileIds[PostingIndex] = Pairs[PostingIndex].FileId;
            ++PostingIndex;
        }
        Trigrams[KeyIndex++] = (CearchTrigramPosting){
            .Key = Key,
            .Reserved = 0U,
            .Start = (uint64_t)Start,
            .Count = (uint64_t)(PostingIndex - Start)
        };
    }

    free(Pairs);
    Index->Trigrams = Trigrams;
    Index->TrigramCount = UniqueKeys;
    Index->TrigramFileIds = FileIds;
    Index->TrigramPostingCount = PostingCount;
    Index->OwnsNameIndex = true;
    return true;
}

static int ComparePostingKey(const void *Left, const void *Right) {
    const CearchTrigramPosting *A = Left;
    const CearchTrigramPosting *B = Right;
    return A->Key < B->Key ? -1 : A->Key > B->Key ? 1 : 0;
}

static const CearchTrigramPosting *FindPosting(const CearchIndex *Index, uint32_t Key) {
    if (!Index->TrigramCount || !Index->Trigrams) return nullptr;
    return bsearch(&(CearchTrigramPosting){ .Key = Key },
                   Index->Trigrams, Index->TrigramCount, sizeof(*Index->Trigrams),
                   ComparePostingKey);
}

static bool QueryTrigramKeys(const char *Query, uint32_t **Keys, size_t *Count) {
    if (!Query || !Keys || !Count) return false;
    size_t Length = strlen(Query);
    *Keys = nullptr;
    *Count = 0U;
    if (Length < 3U) return true;
    size_t KeyCount = Length - 2U;
    if (KeyCount > SIZE_MAX / sizeof(**Keys)) return false;
    uint32_t *Result = malloc(KeyCount * sizeof(*Result));
    if (!Result) return false;

    for (size_t Index = 0; Index < KeyCount; ++Index) {
        unsigned char A = (unsigned char)Query[Index];
        unsigned char B = (unsigned char)Query[Index + 1U];
        unsigned char C = (unsigned char)Query[Index + 2U];
        if (A >= 'A' && A <= 'Z') A = (unsigned char)(A + ('a' - 'A'));
        if (B >= 'A' && B <= 'Z') B = (unsigned char)(B + ('a' - 'A'));
        if (C >= 'A' && C <= 'Z') C = (unsigned char)(C + ('a' - 'A'));
        Result[Index] = TrigramKey(A, B, C);
    }
    if (KeyCount > 1U) qsort(Result, KeyCount, sizeof(*Result), CompareTrigramKey);
    size_t UniqueCount = 0U;
    for (size_t Index = 0; Index < KeyCount; ++Index) {
        if (UniqueCount && Result[Index] == Result[UniqueCount - 1U]) continue;
        Result[UniqueCount++] = Result[Index];
    }
    *Keys = Result;
    *Count = UniqueCount;
    return true;
}

static bool HasAllQueryTrigrams(const CearchIndex *Index, uint32_t FileId,
                                const uint32_t *Keys, size_t KeyCount) {
    for (size_t KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex) {
        const CearchTrigramPosting *Posting = FindPosting(Index, Keys[KeyIndex]);
        if (!Posting || Posting->Start > Index->TrigramPostingCount ||
            Posting->Count > Index->TrigramPostingCount - (size_t)Posting->Start) return false;
        size_t Low = (size_t)Posting->Start;
        size_t High = Low + (size_t)Posting->Count;
        while (Low < High) {
            size_t Mid = Low + (High - Low) / 2U;
            uint32_t Candidate = Index->TrigramFileIds[Mid];
            if (Candidate < FileId) Low = Mid + 1U;
            else High = Mid;
        }
        if (Low >= (size_t)Posting->Start + (size_t)Posting->Count ||
            Index->TrigramFileIds[Low] != FileId) return false;
    }
    return true;
}

bool CearchBuildNameIndex(CearchIndex *Index) {
    if (!Index) return false;
    CearchFreeNameIndex(Index);
    if (!BuildTrigramIndex(Index)) {
        CearchFreeNameIndex(Index);
        return false;
    }
    return true;
}

bool CearchAttachNameIndex(CearchIndex *Index, const CearchTrigramPosting *Trigrams,
                           size_t TrigramCount, const uint32_t *FileIds, size_t FileIdCount) {
    if (!Index || (TrigramCount && !Trigrams) || (FileIdCount && !FileIds)) return false;
    uint64_t ExpectedStart = 0U;
    for (size_t IndexPosition = 0; IndexPosition < TrigramCount; ++IndexPosition) {
        const CearchTrigramPosting *Posting = &Trigrams[IndexPosition];
        if (Posting->Reserved != 0U || Posting->Count == 0U ||
            Posting->Key > 0xFFFFFFU || (IndexPosition && Trigrams[IndexPosition - 1U].Key >= Posting->Key) ||
            Posting->Start != ExpectedStart ||
            Posting->Start > FileIdCount ||
            Posting->Count > (uint64_t)FileIdCount - Posting->Start) return false;
        uint32_t PreviousId = 0U;
        for (uint64_t Offset = 0; Offset < Posting->Count; ++Offset) {
            uint32_t FileId = FileIds[(size_t)(Posting->Start + Offset)];
            if (FileId >= Index->Count || (Offset && FileId <= PreviousId)) return false;
            PreviousId = FileId;
        }
        ExpectedStart += Posting->Count;
    }
    if (ExpectedStart != FileIdCount) return false;

    CearchFreeNameIndex(Index);
    Index->Trigrams = Trigrams;
    Index->TrigramCount = TrigramCount;
    Index->TrigramFileIds = FileIds;
    Index->TrigramPostingCount = FileIdCount;
    Index->OwnsNameIndex = false;
    return true;
}

void CearchFreeNameIndex(CearchIndex *Index) {
    if (!Index) return;
    if (Index->OwnsNameIndex) {
        free((void *)Index->Trigrams);
        free((void *)Index->TrigramFileIds);
    }
    Index->Trigrams = nullptr;
    Index->TrigramFileIds = nullptr;
    Index->TrigramCount = 0U;
    Index->TrigramPostingCount = 0U;
    Index->OwnsNameIndex = false;
}

bool CearchNameQueryPrepare(const char *Query, CearchNameQuery *Prepared) {
    if (!Prepared || !Query) return false;
    Prepared->Keys = nullptr;
    Prepared->Count = 0U;
    return QueryTrigramKeys(Query, &Prepared->Keys, &Prepared->Count);
}

void CearchNameQueryFree(CearchNameQuery *Prepared) {
    if (!Prepared) return;
    free(Prepared->Keys);
    Prepared->Keys = nullptr;
    Prepared->Count = 0U;
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
