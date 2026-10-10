#define _GNU_SOURCE
#include "../src/store.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char Database[] = "/tmp/cearch-store-test-XXXXXX";
    int Descriptor = mkstemp(Database);
    assert(Descriptor >= 0);
    close(Descriptor);
    assert(unlink(Database) == 0);
    const char Path[] = "/tmp/example.txt";
    CearchDiskEntry Entry = { .PathOffset = 0, .PathLength = (uint32_t)(sizeof(Path) - 1U), .Size = 42, .Mtime = 123, .Device = 1, .Inode = 2, .Type = 1 };
    assert(CearchStoreWrite(Database, &Entry, 1, Path, sizeof(Path)));
    CearchStore Store = { .Descriptor = -1 };
    assert(CearchStoreOpen(Database, &Store));
    assert(Store.EntryCount == 1);
    assert(strcmp(CearchStorePath(&Store, 0), Path) == 0);
    CearchStoreClose(&Store);
    Descriptor = open(Database, O_WRONLY | O_CLOEXEC);
    assert(Descriptor >= 0);
    assert(ftruncate(Descriptor, 4) == 0);
    close(Descriptor);
    errno = 0;
    assert(!CearchStoreOpen(Database, &Store));
    assert(errno != 0);
    assert(unlink(Database) == 0);
    puts("cearch store unit tests passed");
    return 0;
}
