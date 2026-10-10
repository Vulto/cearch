#include "index.h"

#include <stdlib.h>
#include <string.h>

#define TrigramTableInitialCapacity 4096U
#define CearchPostingSkipStride 32U

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
    TrigramPair *NewPairs = realloc(*Pairs, Next * sizeof(*NewPairs));
    if (!NewPairs) return false;
    *Pairs = NewPairs;
    *Capacity = Next;
    return true;
}

static bool AddPathTrigrams(TrigramPair **Pairs, size_t *Count, size_t *Capacity,
                            const char *Text, uint32_t FileId) {
    size_t Length = strlen(Text);
    if (Length < 3U) return true;
    for (size_t Index = 0U; Index + 2U < Length; ++Index) {
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
    size_t PairCount = 0U, PairCapacity = 0U;
    for (size_t FileIndex = 0U; FileIndex < Index->Count; ++FileIndex) {
        if (!Index->Entries[FileIndex].Path ||
            !AddPathTrigrams(&Pairs, &PairCount, &PairCapacity,
                             Index->Entries[FileIndex].Path, (uint32_t)FileIndex)) {
            free(Pairs);
            return false;
        }
    }
    if (PairCount > 1U) qsort(Pairs, PairCount, sizeof(*Pairs), CompareTrigramPair);

    size_t PostingCount = 0U;
    for (size_t PairIndex = 0U; PairIndex < PairCount; ++PairIndex) {
        if (PairIndex && Pairs[PairIndex].Key == Pairs[PairIndex - 1U].Key &&
            Pairs[PairIndex].FileId == Pairs[PairIndex - 1U].FileId) continue;
        Pairs[PostingCount++] = Pairs[PairIndex];
    }

    size_t UniqueKeys = 0U;
    for (size_t PairIndex = 0U; PairIndex < PostingCount; ++PairIndex)
        if (PairIndex == 0U || Pairs[PairIndex].Key != Pairs[PairIndex - 1U].Key)
            ++UniqueKeys;

    if (UniqueKeys > SIZE_MAX / sizeof(CearchTrigramPosting) ||
        PostingCount > SIZE_MAX / sizeof(uint32_t)) {
        free(Pairs);
        return false;
    }
    CearchTrigramPosting *Trigrams =
        calloc(UniqueKeys ? UniqueKeys : 1U, sizeof(*Trigrams));
    uint32_t *FileIds = malloc(PostingCount ? PostingCount * sizeof(*FileIds) : sizeof(*FileIds));
    if (!Trigrams || !FileIds) {
        free(Trigrams);
        free(FileIds);
        free(Pairs);
        return false;
    }

    size_t PostingIndex = 0U;
    size_t KeyIndex = 0U;
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
            .Count = (uint64_t)(PostingIndex - Start),
            .SkipStart = 0U,
            .SkipCount = 0U
        };
    }

    free(Pairs);
    Index->Trigrams = Trigrams;
    Index->TrigramCount = UniqueKeys;
    Index->TrigramFileIds = FileIds;
    Index->TrigramPostingCount = PostingCount;
    Index->TrigramData = nullptr;
    Index->TrigramDataSize = 0U;
    Index->TrigramSkips = nullptr;
    Index->TrigramSkipCount = 0U;
    Index->OwnsNameIndex = true;
    return true;
}

static int ComparePostingKey(const void *Left, const void *Right) {
    const CearchTrigramPosting *A = Left;
    const CearchTrigramPosting *B = Right;
    return A->Key < B->Key ? -1 : A->Key > B->Key ? 1 : 0;
}

static const CearchTrigramPosting *FindPosting(const CearchIndex *Index, uint32_t Key) {
    if (!Index || !Index->TrigramCount || !Index->Trigrams) return nullptr;
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

    for (size_t Index = 0U; Index < KeyCount; ++Index) {
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
    for (size_t Index = 0U; Index < KeyCount; ++Index) {
        if (UniqueCount && Result[Index] == Result[UniqueCount - 1U]) continue;
        Result[UniqueCount++] = Result[Index];
    }
    *Keys = Result;
    *Count = UniqueCount;
    return true;
}

static bool ReserveBytes(unsigned char **Buffer, size_t *Capacity, size_t Needed) {
    if (Needed <= *Capacity) return true;
    size_t Next = *Capacity ? *Capacity : 4096U;
    while (Next < Needed) {
        if (Next > SIZE_MAX / 2U) {
            Next = Needed;
            break;
        }
        Next *= 2U;
    }
    unsigned char *NewBuffer = realloc(*Buffer, Next);
    if (!NewBuffer) return false;
    *Buffer = NewBuffer;
    *Capacity = Next;
    return true;
}

static bool AppendUleb128(unsigned char **Buffer, size_t *Length, size_t *Capacity, uint32_t Value) {
    do {
        unsigned char Byte = (unsigned char)(Value & 0x7FU);
        Value >>= 7U;
        if (Value) Byte |= 0x80U;
        if (*Length == SIZE_MAX || !ReserveBytes(Buffer, Capacity, *Length + 1U)) return false;
        (*Buffer)[(*Length)++] = Byte;
    } while (Value);
    return true;
}

static bool ReadUleb128(const unsigned char *Data, size_t DataSize,
                        size_t *Offset, uint32_t *Value) {
    if (!Data || !Offset || !Value) return false;
    uint32_t Result = 0U;
    unsigned Shift = 0U;
    for (unsigned ByteIndex = 0U; ByteIndex < 5U; ++ByteIndex) {
        if (*Offset >= DataSize) return false;
        unsigned char Byte = Data[(*Offset)++];
        if (ByteIndex == 4U && (Byte & 0xF0U) != 0U) return false;
        Result |= (uint32_t)(Byte & 0x7FU) << Shift;
        if ((Byte & 0x80U) == 0U) {
            *Value = Result;
            return true;
        }
        Shift += 7U;
    }
    return false;
}

bool CearchEncodeNameIndex(const CearchIndex *Index,
                           CearchTrigramPosting **EncodedTrigrams,
                           unsigned char **PostingData, size_t *PostingDataSize,
                           CearchPostingSkip **Skips, size_t *SkipCount) {
    if (!Index || !EncodedTrigrams || !PostingData || !PostingDataSize || !Skips || !SkipCount ||
        (Index->TrigramCount && !Index->Trigrams) ||
        (Index->TrigramPostingCount && !Index->TrigramFileIds)) return false;

    *EncodedTrigrams = nullptr;
    *PostingData = nullptr;
    *PostingDataSize = 0U;
    *Skips = nullptr;
    *SkipCount = 0U;
    if (Index->TrigramCount > SIZE_MAX / sizeof(**EncodedTrigrams)) return false;

    size_t TotalSkips = 0U;
    for (size_t PostingIndex = 0U; PostingIndex < Index->TrigramCount; ++PostingIndex) {
        uint64_t Count = Index->Trigrams[PostingIndex].Count;
        size_t NeededSkips = (size_t)(Count / CearchPostingSkipStride +
                                      (Count % CearchPostingSkipStride != 0U));
        if (NeededSkips > SIZE_MAX - TotalSkips) return false;
        TotalSkips += NeededSkips;
    }
    if (TotalSkips > SIZE_MAX / sizeof(**Skips)) return false;

    CearchTrigramPosting *DiskTrigrams =
        calloc(Index->TrigramCount ? Index->TrigramCount : 1U, sizeof(*DiskTrigrams));
    CearchPostingSkip *DiskSkips =
        calloc(TotalSkips ? TotalSkips : 1U, sizeof(*DiskSkips));
    unsigned char *Data = nullptr;
    size_t DataSize = 0U, DataCapacity = 0U;
    if (!DiskTrigrams || !DiskSkips) {
        free(DiskTrigrams);
        free(DiskSkips);
        return false;
    }

    size_t SkipCursor = 0U;
    bool Success = true;
    for (size_t PostingIndex = 0U; PostingIndex < Index->TrigramCount && Success; ++PostingIndex) {
        const CearchTrigramPosting *Source = &Index->Trigrams[PostingIndex];
        if (Source->Start > Index->TrigramPostingCount ||
            Source->Count > Index->TrigramPostingCount - (size_t)Source->Start ||
            Source->Count == 0U) {
            Success = false;
            break;
        }
        uint64_t LocalSkipCount = Source->Count / CearchPostingSkipStride +
                                  (Source->Count % CearchPostingSkipStride != 0U);
        DiskTrigrams[PostingIndex] = (CearchTrigramPosting){
            .Key = Source->Key,
            .Reserved = 0U,
            .Start = (uint64_t)DataSize,
            .Count = Source->Count,
            .SkipStart = (uint64_t)SkipCursor,
            .SkipCount = LocalSkipCount
        };
        uint32_t PreviousId = 0U;
        for (uint64_t Ordinal = 0U; Ordinal < Source->Count; ++Ordinal) {
            uint32_t FileId = Index->TrigramFileIds[(size_t)(Source->Start + Ordinal)];
            if (FileId >= Index->Count || (Ordinal && FileId <= PreviousId)) {
                Success = false;
                break;
            }
            uint32_t Delta = Ordinal == 0U ? FileId : FileId - PreviousId;
            if (!AppendUleb128(&Data, &DataSize, &DataCapacity, Delta)) {
                Success = false;
                break;
            }
            if (Ordinal % CearchPostingSkipStride == 0U) {
                if (PostingIndex > UINT32_MAX || Ordinal > UINT32_MAX || SkipCursor >= TotalSkips) {
                    Success = false;
                    break;
                }
                DiskSkips[SkipCursor++] = (CearchPostingSkip){
                    .PostingIndex = (uint32_t)PostingIndex,
                    .Ordinal = (uint32_t)Ordinal,
                    .ByteOffset = (uint64_t)DataSize,
                    .FileId = FileId,
                    .Reserved = 0U
                };
            }
            PreviousId = FileId;
        }
    }

    if (!Success || SkipCursor != TotalSkips) {
        free(DiskTrigrams);
        free(DiskSkips);
        free(Data);
        return false;
    }
    *EncodedTrigrams = DiskTrigrams;
    *PostingData = Data;
    *PostingDataSize = DataSize;
    *Skips = DiskSkips;
    *SkipCount = TotalSkips;
    return true;
}

static bool PostingContains(const CearchIndex *Index,
                            const CearchTrigramPosting *Posting, uint32_t FileId) {
    if (!Index || !Posting) return false;

    if (Index->TrigramData) {
        if (!Index->TrigramSkips || Posting->SkipCount == 0U ||
            Posting->SkipStart > Index->TrigramSkipCount ||
            Posting->SkipCount > Index->TrigramSkipCount - (size_t)Posting->SkipStart)
            return false;
        size_t Low = (size_t)Posting->SkipStart;
        size_t High = Low + (size_t)Posting->SkipCount;
        while (Low < High) {
            size_t Mid = Low + (High - Low) / 2U;
            const CearchPostingSkip *Skip = &Index->TrigramSkips[Mid];
            if (Skip->FileId <= FileId) Low = Mid + 1U;
            else High = Mid;
        }
        if (Low == (size_t)Posting->SkipStart) return false;
        const CearchPostingSkip *Skip = &Index->TrigramSkips[Low - 1U];
        if (Skip->FileId == FileId) return true;
        if (Skip->PostingIndex >= Index->TrigramCount ||
            Posting->Count <= Skip->Ordinal) return false;
        uint64_t Ordinal = (uint64_t)Skip->Ordinal + 1U;
        size_t Offset = (size_t)Skip->ByteOffset;
        uint32_t CurrentId = Skip->FileId;
        while (Ordinal < Posting->Count) {
            uint32_t Delta = 0U;
            if (!ReadUleb128(Index->TrigramData, Index->TrigramDataSize, &Offset, &Delta) ||
                Delta == 0U || CurrentId > UINT32_MAX - Delta) return false;
            CurrentId += Delta;
            if (CurrentId == FileId) return true;
            if (CurrentId > FileId) return false;
            ++Ordinal;
        }
        return false;
    }

    if (!Index->TrigramFileIds ||
        Posting->Start > Index->TrigramPostingCount ||
        Posting->Count > Index->TrigramPostingCount - (size_t)Posting->Start) return false;
    size_t Low = (size_t)Posting->Start;
    size_t High = Low + (size_t)Posting->Count;
    while (Low < High) {
        size_t Mid = Low + (High - Low) / 2U;
        uint32_t Candidate = Index->TrigramFileIds[Mid];
        if (Candidate < FileId) Low = Mid + 1U;
        else High = Mid;
    }
    return Low < (size_t)Posting->Start + (size_t)Posting->Count &&
           Index->TrigramFileIds[Low] == FileId;
}

static bool HasAllQueryTrigrams(const CearchIndex *Index, uint32_t FileId,
                                const uint32_t *Keys, size_t KeyCount) {
    for (size_t KeyIndex = 0U; KeyIndex < KeyCount; ++KeyIndex) {
        const CearchTrigramPosting *Posting = FindPosting(Index, Keys[KeyIndex]);
        if (!Posting || !PostingContains(Index, Posting, FileId)) return false;
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

bool CearchAttachNameIndex(CearchIndex *Index,
                           const CearchTrigramPosting *Trigrams, size_t TrigramCount,
                           const uint32_t *FileIds, size_t FileIdCount,
                           const unsigned char *PostingData, size_t PostingDataSize,
                           const CearchPostingSkip *Skips, size_t SkipCount) {
    if (!Index || (TrigramCount && !Trigrams) ||
        (FileIdCount && !FileIds && !PostingData) ||
        (PostingDataSize && !PostingData) || (SkipCount && !Skips)) return false;
    CearchFreeNameIndex(Index);
    Index->Trigrams = Trigrams;
    Index->TrigramCount = TrigramCount;
    Index->TrigramFileIds = FileIds;
    Index->TrigramPostingCount = FileIdCount;
    Index->TrigramData = PostingData;
    Index->TrigramDataSize = PostingDataSize;
    Index->TrigramSkips = Skips;
    Index->TrigramSkipCount = SkipCount;
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
    Index->TrigramCount = 0U;
    Index->TrigramFileIds = nullptr;
    Index->TrigramPostingCount = 0U;
    Index->TrigramData = nullptr;
    Index->TrigramDataSize = 0U;
    Index->TrigramSkips = nullptr;
    Index->TrigramSkipCount = 0U;
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

bool CearchNameCandidatesBegin(const CearchIndex *Index, const CearchNameQuery *Query,
                               CearchNameCandidateIterator *Iterator) {
    if (!Index || !Query || !Iterator) return false;
    *Iterator = (CearchNameCandidateIterator){
        .Index = Index,
        .Query = Query,
        .Position = 0U,
        .End = 0U,
        .AllCursor = 0U,
        .Seed = nullptr,
        .BytePosition = 0U,
        .CurrentFileId = 0U,
        .EncodedSeed = Index->TrigramData != nullptr,
        .AllFiles = Query->Count == 0U,
        .Empty = false
    };
    if (Iterator->AllFiles) return true;

    const CearchTrigramPosting *Seed = nullptr;
    for (size_t KeyIndex = 0U; KeyIndex < Query->Count; ++KeyIndex) {
        const CearchTrigramPosting *Posting = FindPosting(Index, Query->Keys[KeyIndex]);
        if (!Posting) {
            Iterator->Empty = true;
            return true;
        }
        if (!Seed || Posting->Count < Seed->Count) Seed = Posting;
    }
    if (!Seed || Seed->Count == 0U) {
        Iterator->Empty = true;
        return true;
    }
    if (Iterator->EncodedSeed) {
        if (Seed->Start > Index->TrigramDataSize) {
            Iterator->Empty = true;
            return true;
        }
        Iterator->BytePosition = Seed->Start;
    } else if (Seed->Start > Index->TrigramPostingCount ||
               Seed->Count > Index->TrigramPostingCount - (size_t)Seed->Start) {
        Iterator->Empty = true;
        return true;
    }
    Iterator->Seed = Seed;
    Iterator->End = Seed->Count;
    return true;
}

bool CearchNameCandidatesNext(CearchNameCandidateIterator *Iterator, uint32_t *FileId) {
    if (!Iterator || !Iterator->Index || !Iterator->Query || !FileId || Iterator->Empty)
        return false;
    const CearchIndex *Index = Iterator->Index;
    if (Iterator->AllFiles) {
        if ((size_t)Iterator->AllCursor >= Index->Count) return false;
        *FileId = Iterator->AllCursor++;
        return true;
    }
    if (!Iterator->Seed) return false;

    while (Iterator->Position < Iterator->End) {
        uint32_t Candidate = 0U;
        if (Iterator->EncodedSeed) {
            uint32_t Delta = 0U;
            if (!ReadUleb128(Index->TrigramData, Index->TrigramDataSize,
                             (size_t *)&Iterator->BytePosition, &Delta)) {
                Iterator->Empty = true;
                return false;
            }
            if (Iterator->Position == 0U) {
                Candidate = Delta;
            } else {
                if (Delta == 0U || Iterator->CurrentFileId > UINT32_MAX - Delta) {
                    Iterator->Empty = true;
                    return false;
                }
                Candidate = Iterator->CurrentFileId + Delta;
            }
            Iterator->CurrentFileId = Candidate;
        } else {
            uint64_t ArrayIndex = Iterator->Seed->Start + Iterator->Position;
            if (!Index->TrigramFileIds || ArrayIndex >= Index->TrigramPostingCount) {
                Iterator->Empty = true;
                return false;
            }
            Candidate = Index->TrigramFileIds[(size_t)ArrayIndex];
        }
        ++Iterator->Position;
        if (Candidate >= Index->Count) continue;
        if (HasAllQueryTrigrams(Index, Candidate, Iterator->Query->Keys, Iterator->Query->Count)) {
            *FileId = Candidate;
            return true;
        }
    }
    return false;
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
