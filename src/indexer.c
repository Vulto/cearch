#define _GNU_SOURCE
#include "indexer.h"
#include "store.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char *Path;
    uint64_t Size;
    int64_t Mtime;
    uint64_t Device;
    uint64_t Inode;
    char Type;
} CearchScanEntry;

typedef struct {
    CearchScanEntry *Entries;
    size_t Count;
    size_t Capacity;
} CearchScanTable;

static bool ReserveEntries(CearchScanTable *Table, size_t Needed) {
    if (Needed <= Table->Capacity) return true;
    size_t Next = Table->Capacity ? Table->Capacity : 1024U;
    while (Next < Needed) {
        if (Next > SIZE_MAX / 2U) return false;
        Next *= 2U;
    }
    if (Next > SIZE_MAX / sizeof(*Table->Entries)) return false;
    CearchScanEntry *NewEntries = realloc(Table->Entries, Next * sizeof(*NewEntries));
    if (!NewEntries) return false;
    Table->Entries = NewEntries;
    Table->Capacity = Next;
    return true;
}

static void FreeEntries(CearchScanTable *Table) {
    for (size_t Index = 0; Index < Table->Count; ++Index) free(Table->Entries[Index].Path);
    free(Table->Entries);
    *Table = (CearchScanTable){0};
}

static bool AddEntry(CearchScanTable *Table, const char *Path, const struct stat *Status) {
    if (!ReserveEntries(Table, Table->Count + 1U)) return false;
    char *Copy = strdup(Path);
    if (!Copy) return false;
    Table->Entries[Table->Count++] = (CearchScanEntry){
        .Path = Copy,
        .Size = Status->st_size < 0 ? 0U : (uint64_t)Status->st_size,
        .Mtime = (int64_t)Status->st_mtime,
        .Device = (uint64_t)Status->st_dev,
        .Inode = (uint64_t)Status->st_ino,
        .Type = S_ISDIR(Status->st_mode) ? 'd' : S_ISREG(Status->st_mode) ? 'f' : 'o'
    };
    return true;
}

static char *JoinPath(const char *Parent, const char *Name) {
    size_t ParentLength = strlen(Parent);
    size_t NameLength = strlen(Name);
    bool HasSlash = ParentLength && Parent[ParentLength - 1U] == '/';
    if (ParentLength > SIZE_MAX - NameLength - 2U) return nullptr;
    size_t Length = ParentLength + NameLength + (HasSlash ? 1U : 2U);
    char *Path = malloc(Length);
    if (!Path) return nullptr;
    memcpy(Path, Parent, ParentLength);
    size_t Offset = ParentLength;
    if (!HasSlash) Path[Offset++] = '/';
    memcpy(Path + Offset, Name, NameLength + 1U);
    return Path;
}

static bool IsDotEntry(const char *Name) {
    return strcmp(Name, ".") == 0 || strcmp(Name, "..") == 0;
}

static bool WalkPath(CearchScanTable *Table, const char *Path) {
    struct stat Status;
    if (lstat(Path, &Status) != 0) {
        fprintf(stderr, "cearch: cannot stat %s: %s\n", Path, strerror(errno));
        return false;
    }
    if (!AddEntry(Table, Path, &Status)) {
        fprintf(stderr, "cearch: out of memory indexing %s\n", Path);
        return false;
    }
    if (!S_ISDIR(Status.st_mode)) return true;

    DIR *Directory = opendir(Path);
    if (!Directory) {
        fprintf(stderr, "cearch: cannot read directory %s: %s\n", Path, strerror(errno));
        return false;
    }
    for (;;) {
        errno = 0;
        struct dirent *Item = readdir(Directory);
        if (!Item) {
            bool ReadSuccess = errno == 0;
            if (!ReadSuccess)
                fprintf(stderr, "cearch: error reading directory %s: %s\n", Path, strerror(errno));
            closedir(Directory);
            return ReadSuccess;
        }
        if (IsDotEntry(Item->d_name)) continue;
        char *Child = JoinPath(Path, Item->d_name);
        if (!Child) {
            closedir(Directory);
            fprintf(stderr, "cearch: out of memory building path\n");
            return false;
        }
        bool Success = WalkPath(Table, Child);
        free(Child);
        if (!Success) {
            closedir(Directory);
            return false;
        }
    }
    closedir(Directory);
    return true;
}

static bool WriteSnapshot(const char *Database, const CearchScanTable *Table) {
    if (Table->Count > UINT32_MAX) {
        fprintf(stderr, "cearch: snapshot supports at most UINT32_MAX entries\n");
        return false;
    }

    size_t StringsSize = 0U;
    for (size_t Index = 0U; Index < Table->Count; ++Index) {
        size_t Length = strlen(Table->Entries[Index].Path);
        if (Length > UINT32_MAX || Length + 1U > UINT32_MAX - StringsSize) {
            fprintf(stderr, "cearch: path string table exceeds 4 GiB\n");
            return false;
        }
        StringsSize += Length + 1U;
    }

    char *Strings = malloc(StringsSize ? StringsSize : 1U);
    CearchDiskEntry *Entries = calloc(Table->Count ? Table->Count : 1U, sizeof(*Entries));
    CearchFileEntry *IndexEntries = calloc(Table->Count ? Table->Count : 1U, sizeof(*IndexEntries));
    if (!Strings || !Entries || !IndexEntries) {
        fprintf(stderr, "cearch: out of memory creating snapshot\n");
        free(Strings);
        free(Entries);
        free(IndexEntries);
        return false;
    }

    size_t StringOffset = 0U;
    for (size_t Index = 0U; Index < Table->Count; ++Index) {
        const CearchScanEntry *Source = &Table->Entries[Index];
        size_t Length = strlen(Source->Path);
        Entries[Index] = (CearchDiskEntry){
            .PathOffset = (uint32_t)StringOffset,
            .PathLength = (uint32_t)Length,
            .Size = Source->Size,
            .Mtime = Source->Mtime,
            .Device = Source->Device,
            .Inode = Source->Inode,
            .Type = Source->Type == 'f' ? 1U : Source->Type == 'd' ? 2U : 3U
        };
        IndexEntries[Index] = (CearchFileEntry){
            .Path = Source->Path,
            .Size = Source->Size,
            .Mtime = Source->Mtime,
            .Device = Source->Device,
            .Inode = Source->Inode,
            .Type = Source->Type
        };
        memcpy(Strings + StringOffset, Source->Path, Length + 1U);
        StringOffset += Length + 1U;
    }

    CearchIndex NameIndex = {
        .Entries = IndexEntries,
        .Count = Table->Count
    };
    bool Success = CearchBuildNameIndex(&NameIndex);
    CearchTrigramPosting *EncodedTrigrams = nullptr;
    unsigned char *PostingData = nullptr;
    size_t PostingDataSize = 0U;
    CearchPostingSkip *Skips = nullptr;
    size_t SkipCount = 0U;
    if (Success) {
        Success = CearchEncodeNameIndex(&NameIndex, &EncodedTrigrams, &PostingData,
                                        &PostingDataSize, &Skips, &SkipCount);
    }
    if (Success) {
        Success = CearchStoreWrite(Database, Entries, Table->Count, Strings, StringsSize,
                                   EncodedTrigrams, NameIndex.TrigramCount,
                                   PostingData, PostingDataSize, Skips, SkipCount,
                                   NameIndex.TrigramPostingCount);
    }
    free(EncodedTrigrams);
    free(PostingData);
    free(Skips);
    if (!Success)
        fprintf(stderr, "cearch: failed building or writing snapshot %s: %s\n", Database, strerror(errno));

    CearchFreeNameIndex(&NameIndex);
    free(IndexEntries);
    free(Strings);
    free(Entries);
    return Success;
}

bool CearchBuildSnapshot(const char *Database, const char *const *Roots,
                         size_t RootCount, CearchIndexStats *Stats) {
    if (!Database || !*Database || !Roots || RootCount == 0U) {
        errno = EINVAL;
        return false;
    }
    CearchScanTable Table = {0};
    for (size_t Index = 0; Index < RootCount; ++Index) {
        if (!Roots[Index] || !*Roots[Index]) {
            FreeEntries(&Table);
            errno = EINVAL;
            return false;
        }
        if (!WalkPath(&Table, Roots[Index])) {
            FreeEntries(&Table);
            return false;
        }
    }

    bool Success = WriteSnapshot(Database, &Table);
    if (Success && Stats) {
        CearchIndexStats Result = { .EntryCount = Table.Count };
        for (size_t Index = 0; Index < Table.Count; ++Index) {
            if (Table.Entries[Index].Type == 'f') ++Result.FileCount;
            if (Table.Entries[Index].Type == 'd') ++Result.DirectoryCount;
        }
        *Stats = Result;
    }
    if (Success)
        fprintf(stderr, "cearch: indexed %zu entries into %s\n", Table.Count, Database);
    FreeEntries(&Table);
    return Success;
}
