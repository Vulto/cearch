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
    uint64_t FileSize;
} TestDiskHeader;

static void ExpectRejected(const char *Database) {
    CearchStore Store = { .Descriptor = -1 };
    errno = 0;
    assert(!CearchStoreOpen(Database, &Store));
    assert(errno != 0);
}

int main(void) {
    char Database[] = "/tmp/cearch-store-test-XXXXXX";
    int Descriptor = mkstemp(Database);
    assert(Descriptor >= 0);
    close(Descriptor);
    assert(unlink(Database) == 0);

    const char Path[] = "/tmp/example.txt";
    CearchDiskEntry Entry = {
        .PathOffset = 0,
        .PathLength = (uint32_t)(sizeof(Path) - 1U),
        .Size = 42, .Mtime = 123, .Device = 1, .Inode = 2, .Type = 1
    };
    assert(CearchStoreWrite(Database, &Entry, 1, Path, sizeof(Path)));

    CearchStore Store = { .Descriptor = -1 };
    assert(CearchStoreOpen(Database, &Store));
    assert(Store.EntryCount == 1);
    assert(strcmp(CearchStorePath(&Store, 0), Path) == 0);
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

    assert(CearchStoreWrite(Database, &Entry, 1, Path, sizeof(Path)));
    Descriptor = open(Database, O_RDWR | O_CLOEXEC);
    assert(Descriptor >= 0);
    assert(pread(Descriptor, &Header, sizeof(Header), 0) == (ssize_t)sizeof(Header));
    const char EmbeddedNul = '\0';
    assert(pwrite(Descriptor, &EmbeddedNul, 1, (off_t)(Header.StringOffset + 1U)) == 1);
    close(Descriptor);
    ExpectRejected(Database);

    assert(CearchStoreWrite(Database, &Entry, 1, Path, sizeof(Path)));
    Descriptor = open(Database, O_WRONLY | O_CLOEXEC);
    assert(Descriptor >= 0);
    assert(ftruncate(Descriptor, 4) == 0);
    close(Descriptor);
    ExpectRejected(Database);

    assert(unlink(Database) == 0);
    puts("cearch store unit tests passed");
    return 0;
}
