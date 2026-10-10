#define _GNU_SOURCE
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CearchStoreMagic 0x4345415243483031ULL
#define CearchPostingSkipStride 32U

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
    uint64_t PostingDataOffset;
    uint64_t PostingDataSize;
    uint64_t SkipOffset;
    uint64_t SkipCount;
    uint64_t FileIdCount;
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

static bool AlignSize(size_t Value, size_t Alignment, size_t *Result) {
    if (!Alignment) return true;
    size_t Remainder = Value % Alignment;
    if (!Remainder) {
        *Result = Value;
        return false;
    }
    return AddOverflowSize(Value, Alignment - Remainder, Result);
}

static bool AppendZeros(int Descriptor, size_t Count) {
    static const unsigned char Zeros[16] = {0};
    while (Count) {
        size_t BlockSize = Count < sizeof(Zeros) ? Count : sizeof(Zeros);
        size_t Offset = 0U;
        while (Offset < BlockSize) {
            ssize_t Written = write(Descriptor, Zeros + Offset, BlockSize - Offset);
            if (Written < 0 && errno == EINTR) continue;
            if (Written <= 0) return false;
            Offset += (size_t)Written;
        }
        Count -= BlockSize;
    }
    return true;
}

static bool SyncParentDirectory(const char *Database) {
    const char *Slash = strrchr(Database, '/');
    char *Directory = nullptr;
    if (!Slash) {
        Directory = strdup(".");
    } else if (Slash == Database) {
        Directory = strdup("/");
    } else {
        size_t Length = (size_t)(Slash - Database);
        Directory = malloc(Length + 1U);
        if (Directory) {
            memcpy(Directory, Database, Length);
            Directory[Length] = '\0';
        }
    }
    if (!Directory) return false;

    int Descriptor = open(Directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    int SavedErrno = errno;
    free(Directory);
    if (Descriptor < 0) {
        errno = SavedErrno;
        return false;
    }
    bool Success = fsync(Descriptor) == 0;
    SavedErrno = errno;
    if (close(Descriptor) != 0 && Success) {
        Success = false;
        SavedErrno = errno;
    }
    if (!Success) errno = SavedErrno ? SavedErrno : EIO;
    return Success;
}

bool CearchStoreWrite(const char *Database,
                      const CearchDiskEntry *Entries, size_t EntryCount,
                      const char *Strings, size_t StringsSize,
                      const CearchTrigramPosting *Trigrams, size_t TrigramCount,
                      const unsigned char *PostingData, size_t PostingDataSize,
                      const CearchPostingSkip *Skips, size_t SkipCount,
                      size_t TrigramPostingCount) {
    if (!Database || !*Database || (EntryCount && !Entries) ||
        (StringsSize && !Strings) || (TrigramCount && !Trigrams) ||
        (PostingDataSize && !PostingData) || (SkipCount && !Skips)) {
        errno = EINVAL;
        return false;
    }

    size_t EntryBytes = 0U;
    size_t TrigramBytes = 0U;
    size_t SkipBytes = 0U;
    size_t EntryOffset = sizeof(CearchDiskHeader);
    size_t StringOffset = 0U;
    size_t StringEnd = 0U;
    size_t TrigramOffset = 0U;
    size_t PostingDataOffset = 0U;
    size_t PostingDataEnd = 0U;
    size_t SkipOffset = 0U;
    size_t FileSize = 0U;
    if (MultiplyOverflowSize(EntryCount, sizeof(CearchDiskEntry), &EntryBytes) ||
        AddOverflowSize(EntryOffset, EntryBytes, &StringOffset) ||
        AddOverflowSize(StringOffset, StringsSize, &StringEnd) ||
        AlignSize(StringEnd, alignof(CearchTrigramPosting), &TrigramOffset) ||
        MultiplyOverflowSize(TrigramCount, sizeof(CearchTrigramPosting), &TrigramBytes) ||
        AddOverflowSize(TrigramOffset, TrigramBytes, &PostingDataOffset) ||
        AddOverflowSize(PostingDataOffset, PostingDataSize, &PostingDataEnd) ||
        AlignSize(PostingDataEnd, alignof(CearchPostingSkip), &SkipOffset) ||
        MultiplyOverflowSize(SkipCount, sizeof(CearchPostingSkip), &SkipBytes) ||
        AddOverflowSize(SkipOffset, SkipBytes, &FileSize)) {
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
        .PostingDataOffset = PostingDataOffset,
        .PostingDataSize = PostingDataSize,
        .SkipOffset = SkipOffset,
        .SkipCount = SkipCount,
        .FileIdCount = TrigramPostingCount,
        .FileSize = FileSize
    };

    bool Success = true;
    const unsigned char *HeaderBytes = (const unsigned char *)&Header;
    size_t Offset = 0U;
    while (Success && Offset < sizeof(Header)) {
        ssize_t Written = write(Descriptor, HeaderBytes + Offset, sizeof(Header) - Offset);
        if (Written < 0 && errno == EINTR) continue;
        if (Written <= 0) {
            Success = false;
            break;
        }
        Offset += (size_t)Written;
    }

    const unsigned char *Blocks[] = {
        (const unsigned char *)Entries,
        (const unsigned char *)Strings,
        (const unsigned char *)Trigrams,
        PostingData,
        (const unsigned char *)Skips
    };
    size_t Sizes[] = { EntryBytes, StringsSize, TrigramBytes, PostingDataSize, SkipBytes };
    size_t Cursor = sizeof(Header);
    size_t Starts[] = { EntryOffset, StringOffset, TrigramOffset, PostingDataOffset, SkipOffset };
    for (size_t Block = 0U; Block < sizeof(Sizes) / sizeof(Sizes[0]) && Success; ++Block) {
        if (Cursor > Starts[Block]) {
            Success = false;
            errno = EINVAL;
            break;
        }
        Success = AppendZeros(Descriptor, Starts[Block] - Cursor);
        size_t WrittenOffset = 0U;
        while (Success && WrittenOffset < Sizes[Block]) {
            ssize_t Written = write(Descriptor, Blocks[Block] + WrittenOffset,
                                    Sizes[Block] - WrittenOffset);
            if (Written < 0 && errno == EINTR) continue;
            if (Written <= 0) {
                Success = false;
                break;
            }
            WrittenOffset += (size_t)Written;
        }
        Cursor = Starts[Block] + Sizes[Block];
    }
    if (Success && Cursor < FileSize) Success = AppendZeros(Descriptor, FileSize - Cursor);
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
    if (Success && !SyncParentDirectory(Database)) {
        Success = false;
        SavedErrno = errno;
    }
    if (!Success) {
        (void)unlink(Temporary);
        errno = SavedErrno ? SavedErrno : EIO;
    }
    free(Temporary);
    return Success;
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

static bool PaddingIsZero(const unsigned char *Bytes, size_t Start, size_t End) {
    if (End < Start) return false;
    for (size_t Offset = Start; Offset < End; ++Offset)
        if (Bytes[Offset] != 0U) return false;
    return true;
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
    size_t SkipBytes = 0U;
    size_t ExpectedStringOffset = 0U;
    size_t StringEnd = 0U;
    size_t ExpectedTrigramOffset = 0U;
    size_t ExpectedPostingDataOffset = 0U;
    size_t PostingDataEnd = 0U;
    size_t ExpectedSkipOffset = 0U;
    size_t ExpectedFileSize = 0U;
    bool Valid = Header->Magic == CearchStoreMagic &&
                 Header->Version == CearchStoreVersion &&
                 Header->HeaderSize == sizeof(CearchDiskHeader) &&
                 Header->EntryCount <= SIZE_MAX &&
                 Header->StringSize <= SIZE_MAX &&
                 Header->TrigramCount <= SIZE_MAX &&
                 Header->PostingDataSize <= SIZE_MAX &&
                 Header->SkipCount <= SIZE_MAX &&
                 Header->FileIdCount <= SIZE_MAX &&
                 !MultiplyOverflowSize((size_t)Header->EntryCount, sizeof(CearchDiskEntry), &EntryBytes) &&
                 !AddOverflowSize(sizeof(CearchDiskHeader), EntryBytes, &ExpectedStringOffset) &&
                 !AddOverflowSize(ExpectedStringOffset, (size_t)Header->StringSize, &StringEnd) &&
                 !AlignSize(StringEnd, alignof(CearchTrigramPosting), &ExpectedTrigramOffset) &&
                 !MultiplyOverflowSize((size_t)Header->TrigramCount, sizeof(CearchTrigramPosting), &TrigramBytes) &&
                 !AddOverflowSize(ExpectedTrigramOffset, TrigramBytes, &ExpectedPostingDataOffset) &&
                 !AddOverflowSize(ExpectedPostingDataOffset, (size_t)Header->PostingDataSize, &PostingDataEnd) &&
                 !AlignSize(PostingDataEnd, alignof(CearchPostingSkip), &ExpectedSkipOffset) &&
                 !MultiplyOverflowSize((size_t)Header->SkipCount, sizeof(CearchPostingSkip), &SkipBytes) &&
                 !AddOverflowSize(ExpectedSkipOffset, SkipBytes, &ExpectedFileSize) &&
                 Header->EntryOffset == sizeof(CearchDiskHeader) &&
                 Header->StringOffset == ExpectedStringOffset &&
                 Header->TrigramOffset == ExpectedTrigramOffset &&
                 Header->PostingDataOffset == ExpectedPostingDataOffset &&
                 Header->SkipOffset == ExpectedSkipOffset &&
                 Header->FileSize == ExpectedFileSize &&
                 ExpectedFileSize == MappingSize;

    if (Valid) {
        const unsigned char *Bytes = Mapping;
        const CearchDiskEntry *Entries =
            (const CearchDiskEntry *)(Bytes + Header->EntryOffset);
        const char *Strings = (const char *)(Bytes + Header->StringOffset);
        const CearchTrigramPosting *Trigrams =
            (const CearchTrigramPosting *)(Bytes + Header->TrigramOffset);
        const unsigned char *PostingData = Bytes + Header->PostingDataOffset;
        const CearchPostingSkip *Skips =
            (const CearchPostingSkip *)(Bytes + Header->SkipOffset);

        if (!PaddingIsZero(Bytes, StringEnd, ExpectedTrigramOffset) ||
            !PaddingIsZero(Bytes, PostingDataEnd, ExpectedSkipOffset)) Valid = false;

        for (size_t Index = 0U; Valid && Index < (size_t)Header->EntryCount; ++Index) {
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

        uint64_t ExpectedDataStart = 0U;
        uint64_t ExpectedSkipStart = 0U;
        uint64_t TotalFileIds = 0U;
        for (size_t PostingIndex = 0U; Valid && PostingIndex < (size_t)Header->TrigramCount; ++PostingIndex) {
            const CearchTrigramPosting *Posting = &Trigrams[PostingIndex];
            uint64_t ExpectedLocalSkips = Posting->Count / CearchPostingSkipStride +
                                          (Posting->Count % CearchPostingSkipStride != 0U);
            if (Posting->Reserved != 0U || Posting->Key > 0xFFFFFFU ||
                Posting->Count == 0U || Posting->Start != ExpectedDataStart ||
                Posting->Start > Header->PostingDataSize ||
                Posting->SkipStart != ExpectedSkipStart ||
                Posting->SkipCount != ExpectedLocalSkips ||
                Posting->SkipStart > Header->SkipCount ||
                Posting->SkipCount > Header->SkipCount - Posting->SkipStart ||
                (PostingIndex && Trigrams[PostingIndex - 1U].Key >= Posting->Key) ||
                Posting->Count > Header->FileIdCount - TotalFileIds) {
                Valid = false;
                break;
            }

            size_t Offset = (size_t)Posting->Start;
            uint32_t CurrentId = 0U;
            for (uint64_t Ordinal = 0U; Ordinal < Posting->Count; ++Ordinal) {
                uint32_t Delta = 0U;
                if (!ReadUleb128(PostingData, (size_t)Header->PostingDataSize, &Offset, &Delta)) {
                    Valid = false;
                    break;
                }
                if (Ordinal == 0U) {
                    CurrentId = Delta;
                } else {
                    if (Delta == 0U || CurrentId > UINT32_MAX - Delta) {
                        Valid = false;
                        break;
                    }
                    CurrentId += Delta;
                }
                if (CurrentId >= Header->EntryCount) {
                    Valid = false;
                    break;
                }

                if (Ordinal % CearchPostingSkipStride == 0U) {
                    size_t SkipIndex = (size_t)(Posting->SkipStart + Ordinal / CearchPostingSkipStride);
                    if (SkipIndex >= (size_t)Header->SkipCount) {
                        Valid = false;
                        break;
                    }
                    const CearchPostingSkip *Skip = &Skips[SkipIndex];
                    if (PostingIndex > UINT32_MAX || Ordinal > UINT32_MAX ||
                        Skip->PostingIndex != (uint32_t)PostingIndex ||
                        Skip->Ordinal != (uint32_t)Ordinal ||
                        Skip->ByteOffset != Offset ||
                        Skip->FileId != CurrentId ||
                        Skip->Reserved != 0U) {
                        Valid = false;
                        break;
                    }
                }
            }
            ExpectedDataStart = Offset;
            ExpectedSkipStart += Posting->SkipCount;
            TotalFileIds += Posting->Count;
        }
        if (ExpectedDataStart != Header->PostingDataSize ||
            ExpectedSkipStart != Header->SkipCount ||
            TotalFileIds != Header->FileIdCount) Valid = false;

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
            Store->TrigramData = PostingData;
            Store->TrigramDataSize = (size_t)Header->PostingDataSize;
            Store->TrigramSkips = Skips;
            Store->TrigramSkipCount = (size_t)Header->SkipCount;
            Store->TrigramPostingCount = (size_t)Header->FileIdCount;
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
