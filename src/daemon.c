#define _GNU_SOURCE
#include "protocol.h"
#include "store.h"
#include "index.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CearchDefaultLimit 1000U
#define CearchMaximumClients 64U
#define CearchReadBufferSize 4096U

typedef struct {
    CearchStore Store;
    CearchFileEntry *Entries;
    size_t EntryCount;
    CearchIndex NameIndex;
    char *Database;
    int Listener;
    bool Running;
} CearchDaemon;

static volatile sig_atomic_t StopRequested = 0;

static void HandleSignal(int Signal) {
    (void)Signal;
    StopRequested = 1;
}

static bool ReadExact(int Descriptor, void *Buffer, size_t Length) {
    unsigned char *Cursor = Buffer;
    size_t ReadCount = 0;
    while (ReadCount < Length) {
        ssize_t Result = read(Descriptor, Cursor + ReadCount, Length - ReadCount);
        if (Result < 0 && errno == EINTR) {
            if (StopRequested) return false;
            continue;
        }
        if (Result <= 0) return false;
        ReadCount += (size_t)Result;
    }
    return true;
}

static bool WriteExact(int Descriptor, const void *Buffer, size_t Length) {
    const unsigned char *Cursor = Buffer;
    size_t WrittenCount = 0;
    while (WrittenCount < Length) {
        ssize_t Result = send(Descriptor, Cursor + WrittenCount, Length - WrittenCount, MSG_NOSIGNAL);
        if (Result < 0 && errno == EINTR) continue;
        if (Result <= 0) return false;
        WrittenCount += (size_t)Result;
    }
    return true;
}

static bool WriteResponse(int Client, uint16_t Status, uint32_t Count,
                          const void *Payload, size_t PayloadLength) {
    if (PayloadLength > UINT32_MAX) return false;
    CearchResponse Response = {
        .Magic = CearchProtocolMagic,
        .Version = CearchProtocolVersion,
        .Status = Status,
        .Count = Count,
        .MessageLength = (uint32_t)PayloadLength
    };
    return WriteExact(Client, &Response, sizeof(Response)) &&
           (!PayloadLength || WriteExact(Client, Payload, PayloadLength));
}

static bool ContainsCaseInsensitive(const char *Haystack, const char *Needle) {
    if (!Haystack || !Needle) return false;
    if (!*Needle) return true;
    size_t NeedleLength = strlen(Needle);
    for (const char *Start = Haystack; *Start; ++Start) {
        if (strncasecmp(Start, Needle, NeedleLength) == 0) return true;
    }
    return false;
}

static bool FileContains(const char *Path, const char *Query) {
    int Descriptor = open(Path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (Descriptor < 0) return false;
    struct stat Status;
    if (fstat(Descriptor, &Status) != 0 || !S_ISREG(Status.st_mode)) {
        close(Descriptor);
        return false;
    }
    size_t QueryLength = strlen(Query);
    if (!QueryLength) {
        close(Descriptor);
        return true;
    }
    size_t ChunkSize = 65536U;
    if (QueryLength > SIZE_MAX - ChunkSize) {
        close(Descriptor);
        return false;
    }
    unsigned char *Buffer = malloc(ChunkSize + QueryLength);
    if (!Buffer) {
        close(Descriptor);
        return false;
    }
    size_t Carry = 0;
    bool Match = false;
    for (;;) {
        ssize_t Count = read(Descriptor, Buffer + Carry, ChunkSize);
        if (Count < 0 && errno == EINTR) continue;
        if (Count <= 0) break;
        size_t Total = Carry + (size_t)Count;
        if (memchr(Buffer, '\0', Total)) break;
        if (memmem(Buffer, Total, Query, QueryLength)) {
            Match = true;
            break;
        }
        Carry = Total < QueryLength ? Total : QueryLength - 1U;
        if (Carry) memmove(Buffer, Buffer + Total - Carry, Carry);
    }
    free(Buffer);
    close(Descriptor);
    return Match;
}

static bool BuildDaemonIndex(CearchDaemon *Daemon, const char *Database) {
    CearchStore Store = { .Descriptor = -1 };
    if (!CearchStoreOpen(Database, &Store)) {
        fprintf(stderr, "cearchd: cannot open snapshot %s: %s\n", Database, strerror(errno));
        return false;
    }
    if (Store.EntryCount > UINT32_MAX || Store.EntryCount > SIZE_MAX / sizeof(*Daemon->Entries)) {
        CearchStoreClose(&Store);
        errno = EOVERFLOW;
        return false;
    }

    CearchFileEntry *Entries = calloc(Store.EntryCount ? Store.EntryCount : 1U, sizeof(*Entries));
    if (!Entries) {
        CearchStoreClose(&Store);
        return false;
    }
    for (size_t Index = 0; Index < Store.EntryCount; ++Index) {
        const CearchDiskEntry *DiskEntry = &Store.Entries[Index];
        const char *Path = CearchStorePath(&Store, Index);
        if (!Path) {
            free(Entries);
            CearchStoreClose(&Store);
            errno = EINVAL;
            return false;
        }
        Entries[Index] = (CearchFileEntry){
            .Path = (char *)Path,
            .Size = DiskEntry->Size,
            .Mtime = DiskEntry->Mtime,
            .Device = DiskEntry->Device,
            .Inode = DiskEntry->Inode,
            .Type = DiskEntry->Type == 1U ? 'f' : DiskEntry->Type == 2U ? 'd' : 'o'
        };
    }

    Daemon->Store = Store;
    Daemon->Entries = Entries;
    Daemon->EntryCount = Store.EntryCount;
    Daemon->NameIndex.Entries = Entries;
    Daemon->NameIndex.Count = Store.EntryCount;
    if (!CearchBuildNameIndex(&Daemon->NameIndex)) {
        free(Daemon->Entries);
        Daemon->Entries = nullptr;
        Daemon->EntryCount = 0;
        CearchStoreClose(&Daemon->Store);
        return false;
    }
    Daemon->Database = strdup(Database);
    if (!Daemon->Database) {
        CearchFreeNameIndex(&Daemon->NameIndex);
        free(Daemon->Entries);
        Daemon->Entries = nullptr;
        Daemon->EntryCount = 0;
        CearchStoreClose(&Daemon->Store);
        return false;
    }
    return true;
}

static void FreeDaemonIndex(CearchDaemon *Daemon) {
    CearchFreeNameIndex(&Daemon->NameIndex);
    free(Daemon->Entries);
    Daemon->Entries = nullptr;
    Daemon->EntryCount = 0;
    CearchStoreClose(&Daemon->Store);
    free(Daemon->Database);
    Daemon->Database = nullptr;
}


