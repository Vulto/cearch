#define _GNU_SOURCE
#include "../src/store.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
} TestDiskHeader;

static void ExpectRejected(const char *Database) {
    CearchStore Store = { .Descriptor = -1 };
    errno = 0;
    assert(!CearchStoreOpen(Database, &Store));
    assert(errno != 0);
}

static void WriteValidSnapshot(const char *Database,
                               const CearchDiskEntry *Entry,
                               const char *Path,
                               const CearchTrigramPosting *Trigrams,
                               size_t TrigramCount,
                               const unsigned char *PostingData,
                               size_t PostingDataSize,
                               const CearchPostingSkip *Skips,
                               size_t SkipCount,
                               size_t PostingCount) {
    assert(CearchStoreWrite(Database, Entry, 1U, Path, strlen(Path) + 1U,
                            Trigrams, TrigramCount, PostingData, PostingDataSize,
                            Skips, SkipCount, PostingCount));
}

int main(void) {
    char Database[] = "/tmp/cearch-store-test-XXXXXX";
    int Descriptor = mkstemp(Database);
    assert(Descriptor >= 0);
    close(Descriptor);
    assert(unlink(Database) == 0);

    const char Path[] = "/tmp/example.txt";
    CearchDiskEntry Entry = {
        .PathOffset = 0U,
        .PathLength = (uint32_t)(sizeof(Path) - 1U),
        .Size = 42U, .Mtime = 123, .Device = 1U, .Inode = 2U, .Type = 1U
    };
    const CearchTrigramPosting Trigrams[] = {
        { .Key = 0x616263U, .Reserved = 0U, .Start = 0U, .Count = 1U }
    };
    const unsigned char PostingData[] = { 0U };
    const CearchPostingSkip Skips[] = {
        { .PostingIndex = 0U, .Ordinal = 0U, .ByteOffset = 0U, .FileId = 0U, .Reserved = 0U }
    };
    WriteValidSnapshot(Database, &Entry, Path, Trigrams, 1U, PostingData,
                       sizeof(PostingData), Skips, 1U, 1U);

    CearchStore Store = { .Descriptor = -1 };
    assert(CearchStoreOpen(Database, &Store));
    assert(Store.EntryCount == 1U);
    assert(strcmp(CearchStorePath(&Store, 0U), Path) == 0);
    assert(Store.TrigramCount == 1U);
    assert(Store.TrigramPostingCount == 1U);
    assert(Store.Trigrams[0].Key == 0x616263U);
    assert(Store.Trigrams[0].Start == 0U);
    assert(Store.Trigrams[0].Count == 1U);
    assert(Store.TrigramDataSize == sizeof(PostingData));
    assert(Store.TrigramData[0] == 0U);
    assert(Store.TrigramSkipCount == 1U);
    assert(Store.TrigramSkips[0].FileId == 0U);
    CearchStoreClose(&Store);

    Descriptor = open(Database, O_RDWR | O_CLOEXEC);
    assert(Descriptor >= 0);
    TestDiskHeader Header;
    assert(pread(Descriptor, &Header, sizeof(Header), 0) == (ssize_t)sizeof(Header));
    CearchDiskEntry BadEntry = Entry;
    BadEntry.PathOffset = (uint32_t)(Header.StringSize - 1U);
    BadEntry.PathLength = 1U;
    assert(pwrite(Descriptor, &BadEntry, sizeof(BadEntry), (off_t)sizeof(Header)) == (ssize_t)sizeof(BadEntry));
    close(Descriptor);
    ExpectRejected(Database);

    WriteValidSnapshot(Database, &Entry, Path, Trigrams, 1U, FileIds, 1U);
    Descriptor = open(Database, O_RDWR | O_CLOEXEC);
    assert(Descriptor >= 0);
    assert(pread(Descriptor, &Header, sizeof(Header), 0) == (ssize_t)sizeof(Header));
    const char EmbeddedNul = '\0';
    assert(pwrite(Descriptor, &EmbeddedNul, 1, (off_t)(Header.StringOffset + 1U)) == 1);
    close(Descriptor);
    ExpectRejected(Database);

    WriteValidSnapshot(Database, &Entry, Path, Trigrams, 1U, FileIds, 1U);
    Descriptor = open(Database, O_RDWR | O_CLOEXEC);
    assert(Descriptor >= 0);
    assert(pread(Descriptor, &Header, sizeof(Header), 0) == (ssize_t)sizeof(Header));
    CearchTrigramPosting BadPosting = Trigrams[0];
    BadPosting.Start = 1U;
    assert(pwrite(Descriptor, &BadPosting, sizeof(BadPosting), (off_t)Header.TrigramOffset) == (ssize_t)sizeof(BadPosting));
    close(Descriptor);
    ExpectRejected(Database);

    WriteValidSnapshot(Database, &Entry, Path, Trigrams, 1U, FileIds, 1U);
    Descriptor = open(Database, O_WRONLY | O_CLOEXEC);
    assert(Descriptor >= 0);
    assert(ftruncate(Descriptor, 4) == 0);
    close(Descriptor);
    ExpectRejected(Database);

    assert(unlink(Database) == 0);
    puts("cearch store unit tests passed");
    return 0;
}
