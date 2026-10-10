#define _GNU_SOURCE
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CearchStoreMagic 0x4345415243483031ULL

typedef struct {
    uint64_t Magic;
    uint32_t Version;
    uint32_t HeaderSize;
    uint64_t EntryCount;
    uint64_t EntryOffset;
    uint64_t StringOffset;
    uint64_t StringSize;
    uint64_t TrigramOffset;
    uint64_t TrigramCount;
    uint64_t PostingOffset;
    uint64_t PostingCount;
    uint64_t FileSize;
} CearchDiskHeader;

static bool AddOverflowSize(size_t A, size_t B, size_t *Result) {
    if (A > SIZE_MAX - B) return true;
    *Result = A + B;
    return false;
}

static bool MultiplyOverflowSize(size_t A, size_t B, size_t *Result) {
    if (A && B > SIZE_MAX / A) return true;
    *Result = A * B;
    return false;
}

bool CearchStoreWrite(const char *Database,
                      const CearchDiskEntry *Entries, size_t EntryCount,
                      const char *Strings, size_t StringsSize,
                      const CearchTrigramPosting *Trigrams, size_t TrigramCount,
                      const uint32_t *TrigramFileIds, size_t TrigramPostingCount) {
    if (!Database || !*Database || (EntryCount && !Entries) ||
        (StringsSize && !Strings) || (TrigramCount && !Trigrams) ||
        (TrigramPostingCount && !TrigramFileIds)) {
        errno = EINVAL;
        return false;
    }

    size_t EntryBytes = 0U;
    size_t TrigramBytes = 0U;
    size_t PostingBytes = 0U;
    size_t EntryOffset = sizeof(CearchDiskHeader);
    size_t StringOffset = 0U;
    size_t TrigramOffset = 0U;
    size_t PostingOffset = 0U;
    size_t FileSize = 0U;
    if (MultiplyOverflowSize(EntryCount, sizeof(CearchDiskEntry), &EntryBytes) ||
        AddOverflowSize(EntryOffset, EntryBytes, &StringOffset) ||
        AddOverflowSize(StringOffset, StringsSize, &TrigramOffset) ||
        MultiplyOverflowSize(TrigramCount, sizeof(CearchTrigramPosting), &TrigramBytes) ||
        AddOverflowSize(TrigramOffset, TrigramBytes, &PostingOffset) ||
        MultiplyOverflowSize(TrigramPostingCount, sizeof(uint32_t), &PostingBytes) ||
        AddOverflowSize(PostingOffset, PostingBytes, &FileSize)) {
        errno = EOVERFLOW;
        return false;
    }

    size_t NameLength = strlen(Database);
    if (NameLength > SIZE_MAX - 48U) {
        errno = EOVERFLOW;
        return false;
    }
    size_t TemporarySize = NameLength + 48U;
    char *Temporary = malloc(TemporarySize);
    if (!Temporary) return false;
    (void)snprintf(Temporary, TemporarySize, "%s.tmp.%ld", Database, (long)getpid());
    int Descriptor = open(Temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (Descriptor < 0) {
        free(Temporary);
        return false;
    }

    CearchDiskHeader Header = {
        .Magic = CearchStoreMagic,
        .Version = CearchStoreVersion,
        .HeaderSize = sizeof(CearchDiskHeader),
        .EntryCount = EntryCount,
        .EntryOffset = EntryOffset,
        .StringOffset = StringOffset,
        .StringSize = StringsSize,
        .TrigramOffset = TrigramOffset,
        .TrigramCount = TrigramCount,
        .PostingOffset = PostingOffset,
        .PostingCount = TrigramPostingCount,
        .FileSize = FileSize
    };

    const unsigned char *Blocks[] = {
        (const unsigned char *)&Header,
        (const unsigned char *)Entries,
        (const unsigned char *)Strings,
        (const unsigned char *)Trigrams,
        (const unsigned char *)TrigramFileIds
    };
    size_t Sizes[] = {
        sizeof(Header), EntryBytes, StringsSize, TrigramBytes, PostingBytes
    };
    bool Success = true;
    for (size_t Block = 0U; Block < sizeof(Sizes) / sizeof(Sizes[0]) && Success; ++Block) {
        size_t Offset = 0U;
        while (Offset < Sizes[Block]) {
            ssize_t Written = write(Descriptor, Blocks[Block] + Offset, Sizes[Block] - Offset);
            if (Written < 0 && errno == EINTR) continue;
            if (Written <= 0) {
                Success = false;
                break;
            }
            Offset += (size_t)Written;
        }
    }
    if (Success && fsync(Descriptor) != 0) Success = false;
    int SavedErrno = errno;
    if (close(Descriptor) != 0) {
        Success = false;
        SavedErrno = errno;
    }
    if (Success && rename(Temporary, Database) != 0) {
        Success = false;
        SavedErrno = errno;
    }
    if (!Success) {
        (void)unlink(Temporary);
        errno = SavedErrno;
    }
    free(Temporary);
    return Success;
}

bool CearchStoreOpen(const char *Database, CearchStore *Store) {
    if (!Database || !Store) {
        errno = EINVAL;
        return false;
    }
    *Store = (CearchStore){ .Descriptor = -1 };
    int Descriptor = open(Database, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (Descriptor < 0) return false;
    struct stat Status;
    if (fstat(Descriptor, &Status) != 0 || !S_ISREG(Status.st_mode) ||
        Status.st_size < (off_t)sizeof(CearchDiskHeader) ||
        (uintmax_t)Status.st_size > SIZE_MAX) {
        int SavedErrno = errno ? errno : EINVAL;
        (void)close(Descriptor);
        errno = SavedErrno;
        return false;
    }

    size_t MappingSize = (size_t)Status.st_size;
    void *Mapping = mmap(nullptr, MappingSize, PROT_READ, MAP_PRIVATE, Descriptor, 0);
    if (Mapping == MAP_FAILED) {
        int SavedErrno = errno;
        (void)close(Descriptor);
        errno = SavedErrno;
        return false;
    }

    const CearchDiskHeader *Header = Mapping;
    size_t EntryBytes = 0U;
    size_t TrigramBytes = 0U;
    size_t PostingBytes = 0U;
    size_t ExpectedStringOffset = 0U;
    size_t ExpectedTrigramOffset = 0U;
    size_t ExpectedPostingOffset = 0U;
    size_t ExpectedFileSize = 0U;
    bool Valid = Header->Magic == CearchStoreMagic &&
                 Header->Version == CearchStoreVersion &&
                 Header->HeaderSize == sizeof(CearchDiskHeader) &&
                 Header->EntryCount <= SIZE_MAX &&
                 Header->StringSize <= SIZE_MAX &&
                 Header->TrigramCount <= SIZE_MAX &&
                 Header->PostingCount <= SIZE_MAX &&
                 !MultiplyOverflowSize((size_t)Header->EntryCount, sizeof(CearchDiskEntry), &EntryBytes) &&
                 !AddOverflowSize(sizeof(CearchDiskHeader), EntryBytes, &ExpectedStringOffset) &&
                 !AddOverflowSize(ExpectedStringOffset, (size_t)Header->StringSize, &ExpectedTrigramOffset) &&
                 !MultiplyOverflowSize((size_t)Header->TrigramCount, sizeof(CearchTrigramPosting), &TrigramBytes) &&
                 !AddOverflowSize(ExpectedTrigramOffset, TrigramBytes, &ExpectedPostingOffset) &&
                 !MultiplyOverflowSize((size_t)Header->PostingCount, sizeof(uint32_t), &PostingBytes) &&
                 !AddOverflowSize(ExpectedPostingOffset, PostingBytes, &ExpectedFileSize) &&
                 Header->EntryOffset == sizeof(CearchDiskHeader) &&
                 Header->StringOffset == ExpectedStringOffset &&
                 Header->TrigramOffset == ExpectedTrigramOffset &&
                 Header->PostingOffset == ExpectedPostingOffset &&
                 Header->FileSize == ExpectedFileSize &&
                 ExpectedFileSize == MappingSize;

    if (Valid) {
        const unsigned char *Bytes = Mapping;
        const CearchDiskEntry *Entries =
            (const CearchDiskEntry *)(Bytes + Header->EntryOffset);
        const char *Strings = (const char *)(Bytes + Header->StringOffset);
        const CearchTrigramPosting *Trigrams =
            (const CearchTrigramPosting *)(Bytes + Header->TrigramOffset);
        const uint32_t *FileIds =
            (const uint32_t *)(Bytes + Header->PostingOffset);

        for (size_t Index = 0U; Index < (size_t)Header->EntryCount; ++Index) {
            const CearchDiskEntry *Entry = &Entries[Index];
            uint64_t PathEnd = (uint64_t)Entry->PathOffset + Entry->PathLength;
            bool ReservedZero = true;
            for (size_t Byte = 0U; Byte < sizeof(Entry->Reserved); ++Byte)
                if (Entry->Reserved[Byte] != 0U) ReservedZero = false;
            if (Entry->PathLength == 0U || PathEnd >= Header->StringSize ||
                Entry->Type < 1U || Entry->Type > 3U || !ReservedZero ||
                Strings[PathEnd] != '\0' ||
                memchr(Strings + Entry->PathOffset, '\0', Entry->PathLength) != nullptr) {
                Valid = false;
                break;
            }
        }

        uint64_t ExpectedStart = 0U;
        for (size_t Index = 0U; Valid && Index < (size_t)Header->TrigramCount; ++Index) {
            const CearchTrigramPosting *Posting = &Trigrams[Index];
            if (Posting->Reserved != 0U || Posting->Key > 0xFFFFFFU ||
                Posting->Count == 0U || Posting->Start != ExpectedStart ||
                Posting->Start > Header->PostingCount ||
                Posting->Count > Header->PostingCount - Posting->Start ||
                (Index && Trigrams[Index - 1U].Key >= Posting->Key)) {
                Valid = false;
                break;
            }
            uint32_t PreviousId = 0U;
            for (uint64_t Offset = 0U; Offset < Posting->Count; ++Offset) {
                uint32_t FileId = FileIds[(size_t)(Posting->Start + Offset)];
                if (FileId >= Header->EntryCount || (Offset && FileId <= PreviousId)) {
                    Valid = false;
                    break;
                }
                PreviousId = FileId;
            }
            ExpectedStart += Posting->Count;
        }
        if (ExpectedStart != Header->PostingCount) Valid = false;

        if (Valid) {
            Store->Descriptor = Descriptor;
            Store->Mapping = Mapping;
            Store->MappingSize = MappingSize;
            Store->Entries = Entries;
            Store->EntryCount = (size_t)Header->EntryCount;
            Store->Strings = Strings;
            Store->StringsSize = (size_t)Header->StringSize;
            Store->Trigrams = Trigrams;
            Store->TrigramCount = (size_t)Header->TrigramCount;
            Store->TrigramFileIds = FileIds;
            Store->TrigramPostingCount = (size_t)Header->PostingCount;
            return true;
        }
    }

    (void)munmap(Mapping, MappingSize);
    (void)close(Descriptor);
    errno = EINVAL;
    return false;
}

void CearchStoreClose(CearchStore *Store) {
    if (!Store) return;
    if (Store->Mapping && Store->Mapping != MAP_FAILED)
        (void)munmap(Store->Mapping, Store->MappingSize);
    if (Store->Descriptor >= 0) (void)close(Store->Descriptor);
    *Store = (CearchStore){ .Descriptor = -1 };
}

const char *CearchStorePath(const CearchStore *Store, size_t EntryIndex) {
    if (!Store || EntryIndex >= Store->EntryCount) return nullptr;
    const CearchDiskEntry *Entry = &Store->Entries[EntryIndex];
    uint64_t PathEnd = (uint64_t)Entry->PathOffset + Entry->PathLength;
    if (PathEnd >= Store->StringsSize) return nullptr;
    if (Store->Strings[PathEnd] != '\0') return nullptr;
    return Store->Strings + Entry->PathOffset;
}
