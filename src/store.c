#define _GNU_SOURCE
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
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

bool CearchStoreWrite(const char *Database, const CearchDiskEntry *Entries,
                      size_t EntryCount, const char *Strings, size_t StringsSize) {
    size_t EntryBytes = 0, EntryOffset = sizeof(CearchDiskHeader);
    size_t StringOffset = 0, FileSize = 0;
    if (MultiplyOverflowSize(EntryCount, sizeof(CearchDiskEntry), &EntryBytes) ||
        AddOverflowSize(EntryOffset, EntryBytes, &StringOffset) ||
        AddOverflowSize(StringOffset, StringsSize, &FileSize) ||
        EntryCount > UINT64_MAX || StringsSize > UINT64_MAX) {
        errno = EOVERFLOW;
        return false;
    }

    size_t NameLength = strlen(Database);
    if (NameLength > SIZE_MAX - 48U) {
        errno = EOVERFLOW;
        return false;
    }
    char *Temporary = malloc(NameLength + 48U);
    if (!Temporary) return false;
    (void)snprintf(Temporary, NameLength + 48U, "%s.tmp.%ld", Database, (long)getpid());
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
        .FileSize = FileSize
    };

    bool Success = true;
    const unsigned char *Blocks[] = {
        (const unsigned char *)&Header,
        (const unsigned char *)Entries,
        (const unsigned char *)Strings
    };
    size_t Sizes[] = { sizeof(Header), EntryBytes, StringsSize };
    for (size_t Block = 0; Block < 3U && Success; ++Block) {
        size_t Offset = 0;
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
    if (!Store) {
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
        close(Descriptor);
        errno = SavedErrno;
        return false;
    }
    size_t MappingSize = (size_t)Status.st_size;
    void *Mapping = mmap(nullptr, MappingSize, PROT_READ, MAP_PRIVATE, Descriptor, 0);
    if (Mapping == MAP_FAILED) {
        int SavedErrno = errno;
        close(Descriptor);
        errno = SavedErrno;
        return false;
    }

    const CearchDiskHeader *Header = Mapping;
    size_t EntryBytes = 0, ExpectedStringOffset = 0, ExpectedFileSize = 0;
    bool Valid = Header->Magic == CearchStoreMagic &&
                 Header->Version == CearchStoreVersion &&
                 Header->HeaderSize == sizeof(CearchDiskHeader) &&
                 Header->EntryCount <= SIZE_MAX &&
                 Header->StringSize <= SIZE_MAX &&
                 !MultiplyOverflowSize((size_t)Header->EntryCount, sizeof(CearchDiskEntry), &EntryBytes) &&
                 !AddOverflowSize(sizeof(CearchDiskHeader), EntryBytes, &ExpectedStringOffset) &&
                 !AddOverflowSize(ExpectedStringOffset, (size_t)Header->StringSize, &ExpectedFileSize) &&
                 Header->EntryOffset == sizeof(CearchDiskHeader) &&
                 Header->StringOffset == ExpectedStringOffset &&
                 Header->FileSize == ExpectedFileSize &&
                 ExpectedFileSize == MappingSize;
    if (Valid) {
        const CearchDiskEntry *Entries = (const CearchDiskEntry *)((const unsigned char *)Mapping + Header->EntryOffset);
        const char *Strings = (const char *)Mapping + Header->StringOffset;
        for (size_t Index = 0; Index < (size_t)Header->EntryCount; ++Index) {
            const CearchDiskEntry *Entry = &Entries[Index];
            uint64_t PathEnd = (uint64_t)Entry->PathOffset + Entry->PathLength;
            if (Entry->PathLength == 0U || PathEnd >= Header->StringSize ||
                Entry->Type < 1U || Entry->Type > 3U ||
                Strings[PathEnd] != '\\0') {
                Valid = false;
                break;
            }
        }
        if (Valid) {
            Store->Descriptor = Descriptor;
            Store->Mapping = Mapping;
            Store->MappingSize = MappingSize;
            Store->Entries = Entries;
            Store->EntryCount = (size_t)Header->EntryCount;
            Store->Strings = Strings;
            Store->StringsSize = (size_t)Header->StringSize;
            return true;
        }
    }

    munmap(Mapping, MappingSize);
    close(Descriptor);
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
    if ((uint64_t)Entry->PathOffset + Entry->PathLength >= Store->StringsSize) return nullptr;
    if (Store->Strings[Entry->PathOffset + Entry->PathLength] != '\0') return nullptr;
    return Store->Strings + Entry->PathOffset;
}
