#define _GNU_SOURCE
#include "protocol.h"
#include "store.h"
#include "index.h"
#include "query.h"
#include "indexer.h"
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <sys/inotify.h>
#include <time.h>

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
#define CearchWatchBufferSize (64U * 1024U)
#define CearchReconcileInterval 30

typedef struct {
    CearchStore Store;
    CearchFileEntry *Entries;
    size_t EntryCount;
    CearchIndex NameIndex;
    char *Database;
    int Listener;
    bool Running;
    char **Roots;
    size_t RootCount;
    int WatchFd;
    bool Dirty;
    time_t LastReconcile;
    pthread_rwlock_t StateLock;
    pthread_mutex_t ClientMutex;
    pthread_cond_t ClientCondition;
    size_t ActiveClients;
} CearchDaemon;

static bool BuildDaemonIndex(CearchDaemon *Daemon, const char *Database);
static void FreeDaemonIndex(CearchDaemon *Daemon);

static volatile sig_atomic_t SignalStopRequested = 0;
static atomic_bool ClientStopRequested = false;

static void HandleSignal(int Signal) {
    (void)Signal;
    SignalStopRequested = 1;
}

static bool ReadExact(int Descriptor, void *Buffer, size_t Length) {
    unsigned char *Cursor = Buffer;
    size_t ReadCount = 0;
    while (ReadCount < Length) {
        ssize_t Result = read(Descriptor, Cursor + ReadCount, Length - ReadCount);
        if (Result < 0 && errno == EINTR) {
            if (SignalStopRequested || atomic_load_explicit(&ClientStopRequested, memory_order_relaxed)) return false;
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

static bool AddWatchRecursive(int WatchFd, const char *Path, unsigned Depth) {
    if (Depth > 128U) return true;
    struct stat Status;
    if (lstat(Path, &Status) != 0 || !S_ISDIR(Status.st_mode)) return true;
    if (inotify_add_watch(WatchFd, Path,
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |
            IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
            IN_ONLYDIR) < 0) {
        if (errno != EACCES && errno != ENOENT && errno != ENOTDIR)
            fprintf(stderr, "cearchd: cannot watch %s: %s\n", Path, strerror(errno));
        return true;
    }
    DIR *Directory = opendir(Path);
    if (!Directory) return true;
    for (;;) {
        errno = 0;
        struct dirent *Entry = readdir(Directory);
        if (!Entry) break;
        if (strcmp(Entry->d_name, ".") == 0 || strcmp(Entry->d_name, "..") == 0) continue;
        char *Child = nullptr;
        size_t ParentLength = strlen(Path), NameLength = strlen(Entry->d_name);
        bool HasSlash = ParentLength > 0U && Path[ParentLength - 1U] == '/';
        if (ParentLength > SIZE_MAX - NameLength - (HasSlash ? 1U : 2U)) continue;
        Child = malloc(ParentLength + NameLength + (HasSlash ? 1U : 2U));
        if (!Child) {
            closedir(Directory);
            return false;
        }
        memcpy(Child, Path, ParentLength);
        size_t Offset = ParentLength;
        if (!HasSlash) Child[Offset++] = '/';
        memcpy(Child + Offset, Entry->d_name, NameLength + 1U);
        struct stat ChildStatus;
        bool IsDirectory = lstat(Child, &ChildStatus) == 0 && S_ISDIR(ChildStatus.st_mode);
        if (IsDirectory && !AddWatchRecursive(WatchFd, Child, Depth + 1U)) {
            free(Child);
            closedir(Directory);
            return false;
        }
        free(Child);
    }
    closedir(Directory);
    return true;
}

static bool ResetWatches(CearchDaemon *Daemon) {
    if (Daemon->WatchFd < 0) return false;
    (void)close(Daemon->WatchFd);
    Daemon->WatchFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (Daemon->WatchFd < 0) return false;
    for (size_t Index = 0; Index < Daemon->RootCount; ++Index) {
        if (!AddWatchRecursive(Daemon->WatchFd, Daemon->Roots[Index], 0U)) return false;
    }
    return true;
}

static void FreeRoots(CearchDaemon *Daemon) {
    for (size_t Index = 0; Index < Daemon->RootCount; ++Index) free(Daemon->Roots[Index]);
    free(Daemon->Roots);
    Daemon->Roots = nullptr;
    Daemon->RootCount = 0;
}

static bool CopyRoots(CearchDaemon *Daemon, const char *const *Roots, size_t RootCount) {
    if (!RootCount) return true;
    if (RootCount > SIZE_MAX / sizeof(*Daemon->Roots)) return false;
    Daemon->Roots = calloc(RootCount, sizeof(*Daemon->Roots));
    if (!Daemon->Roots) return false;
    for (size_t Index = 0; Index < RootCount; ++Index) {
        Daemon->Roots[Index] = strdup(Roots[Index]);
        if (!Daemon->Roots[Index]) {
            Daemon->RootCount = Index;
            FreeRoots(Daemon);
            return false;
        }
        ++Daemon->RootCount;
    }
    return true;
}

static bool ReconcileDaemon(CearchDaemon *Daemon) {
    if (!Daemon->RootCount) return true;
    CearchIndexStats Stats = {0};
    if (!CearchBuildSnapshot(Daemon->Database,
            (const char *const *)Daemon->Roots, Daemon->RootCount, &Stats)) {
        fprintf(stderr, "cearchd: reconciliation failed; retaining previous in-memory snapshot\n");
        return false;
    }

    CearchDaemon Replacement = { .Store = { .Descriptor = -1 } };
    if (!BuildDaemonIndex(&Replacement, Daemon->Database)) {
        fprintf(stderr, "cearchd: rebuilt snapshot cannot be loaded; current process snapshot retained\n");
        FreeDaemonIndex(&Replacement);
        return false;
    }

    if (pthread_rwlock_wrlock(&Daemon->StateLock) != 0) {
        FreeDaemonIndex(&Replacement);
        return false;
    }
    CearchStore OldStore = Daemon->Store;
    CearchFileEntry *OldEntries = Daemon->Entries;
    CearchIndex OldIndex = Daemon->NameIndex;
    char *OldDatabase = Daemon->Database;
    Daemon->Store = Replacement.Store;
    Daemon->Entries = Replacement.Entries;
    Daemon->EntryCount = Replacement.EntryCount;
    Daemon->NameIndex = Replacement.NameIndex;
    Daemon->Database = Replacement.Database;
    Replacement.Store = (CearchStore){ .Descriptor = -1 };
    Replacement.Entries = nullptr;
    Replacement.EntryCount = 0;
    Replacement.NameIndex = (CearchIndex){0};
    Replacement.Database = nullptr;

    CearchFreeNameIndex(&OldIndex);
    free(OldEntries);
    CearchStoreClose(&OldStore);
    free(OldDatabase);
    (void)pthread_rwlock_unlock(&Daemon->StateLock);
    Daemon->Dirty = false;
    Daemon->LastReconcile = time(nullptr);
    fprintf(stderr, "cearchd: reconciled %zu entries\n", Stats.EntryCount);
    if (Daemon->WatchFd >= 0) (void)ResetWatches(Daemon);
    return true;
}

static void DrainWatchEvents(CearchDaemon *Daemon) {
    if (Daemon->WatchFd < 0) return;
    unsigned char Buffer[CearchWatchBufferSize];
    for (;;) {
        ssize_t Count = read(Daemon->WatchFd, Buffer, sizeof(Buffer));
        if (Count < 0 && errno == EINTR) continue;
        if (Count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (Count <= 0) break;
        size_t Offset = 0;
        while (Offset + sizeof(struct inotify_event) <= (size_t)Count) {
            const struct inotify_event *Event = (const struct inotify_event *)(Buffer + Offset);
            if ((Event->mask & IN_Q_OVERFLOW) != 0U) {
                Daemon->Dirty = true;
                fprintf(stderr, "cearchd: inotify queue overflow; forcing reconciliation\n");
            } else if ((Event->mask & (IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                       IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                       IN_MOVE_SELF | IN_IGNORED)) != 0U) {
                Daemon->Dirty = true;
            }
            Offset += sizeof(struct inotify_event) + Event->len;
        }
    }
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
    for (size_t Index = 0U; Index < Store.EntryCount; ++Index) {
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
            .MtimeNsec = DiskEntry->MtimeNsec,
            .Ctime = DiskEntry->Ctime,
            .CtimeNsec = DiskEntry->CtimeNsec,
            .Device = DiskEntry->Device,
            .Inode = DiskEntry->Inode,
            .Type = DiskEntry->Type == 1U ? 'f' : DiskEntry->Type == 2U ? 'd' : 'o',
            .ContentIndexed = DiskEntry->Reserved[0] != 0U
        };
    }

    Daemon->Store = Store;
    Daemon->Entries = Entries;
    Daemon->EntryCount = Store.EntryCount;
    Daemon->NameIndex.Entries = Entries;
    Daemon->NameIndex.Count = Store.EntryCount;
    if (!CearchAttachNameIndex(&Daemon->NameIndex, Store.Trigrams, Store.TrigramCount,
                               nullptr, Store.TrigramPostingCount, Store.TrigramData,
                               Store.TrigramDataSize, Store.TrigramSkips, Store.TrigramSkipCount)) {
        free(Daemon->Entries);
        Daemon->Entries = nullptr;
        Daemon->EntryCount = 0U;
        CearchStoreClose(&Daemon->Store);
        errno = EINVAL;
        return false;
    }
    Daemon->Database = strdup(Database);
    if (!Daemon->Database) {
        CearchFreeNameIndex(&Daemon->NameIndex);
        free(Daemon->Entries);
        Daemon->Entries = nullptr;
        Daemon->EntryCount = 0U;
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


static bool AppendResultPath(char **Result, size_t *Length, size_t *Capacity,
                             const char *Path, bool NullDelimited) {
    size_t PathLength = strlen(Path);
    if (PathLength > SIZE_MAX - *Length - 1U) return false;
    size_t Needed = *Length + PathLength + 1U;
    if (Needed > *Capacity) {
        size_t Next = *Capacity;
        while (Next < Needed) {
            if (Next > SIZE_MAX / 2U) return false;
            Next *= 2U;
        }
        char *NewResult = realloc(*Result, Next);
        if (!NewResult) return false;
        *Result = NewResult;
        *Capacity = Next;
    }
    memcpy(*Result + *Length, Path, PathLength);
    *Length += PathLength;
    (*Result)[(*Length)++] = NullDelimited ? '\0' : '\n';
    return true;
}

static bool SearchName(CearchDaemon *Daemon, const char *Query, uint32_t Limit,
                       bool MatchPath, bool NullDelimited, char **Output, size_t *OutputLength, uint32_t *Found) {
    size_t Capacity = 4096U;
    char *Result = malloc(Capacity);
    if (!Result) return false;

    size_t Length = 0U;
    uint32_t Matches = 0U;
    CearchQuery ParsedQuery = {0};
    if (!CearchQueryParse(Query, &ParsedQuery)) {
        free(Result);
        return false;
    }
    const char *SimpleTerm = CearchQuerySimpleTerm(&ParsedQuery);
    CearchNameQuery Prepared = {0};
    CearchNameCandidateIterator Iterator = {0};
    if (SimpleTerm &&
        (!CearchNameQueryPrepare(SimpleTerm, &Prepared) ||
         !CearchNameCandidatesBegin(&Daemon->NameIndex, &Prepared, &Iterator))) {
        CearchQueryFree(&ParsedQuery);
        CearchNameQueryFree(&Prepared);
        free(Result);
        return false;
    }

    if (CearchQueryHasFuzzy(&ParsedQuery)) {
        size_t CapacityHits = Daemon->EntryCount < (size_t)Limit
            ? Daemon->EntryCount : (size_t)Limit;
        CearchQueryHit *Hits = CapacityHits ? calloc(CapacityHits, sizeof(*Hits)) : NULL;
        if (CapacityHits && !Hits) {
            CearchQueryFree(&ParsedQuery);
            CearchNameQueryFree(&Prepared);
            free(Result);
            return false;
        }
        size_t HitCount = 0U;
        for (size_t EntryIndex = 0U; EntryIndex < Daemon->EntryCount; ++EntryIndex) {
            const CearchFileEntry *Entry = &Daemon->Entries[EntryIndex];
            const char *Base = strrchr(Entry->Path, '/');
            Base = Base ? Base + 1 : Entry->Path;
            const char *Haystack = MatchPath ? Entry->Path : Base;
            uint32_t Score = 0U;
            if (CearchQueryScoreEntry(&ParsedQuery, Entry, Haystack, &Score))
                CearchQueryHitConsider(Hits, &HitCount, CapacityHits, Entry->Path, Score);
        }
        if (HitCount > 1U) qsort(Hits, HitCount, sizeof(*Hits), CearchQueryHitCompare);
        bool Success = true;
        for (size_t HitIndex = 0U; HitIndex < HitCount; ++HitIndex) {
            if (!AppendResultPath(&Result, &Length, &Capacity, Hits[HitIndex].Path, NullDelimited)) {
                Success = false;
                break;
            }
        }
        free(Hits);
        CearchQueryFree(&ParsedQuery);
        CearchNameQueryFree(&Prepared);
        if (!Success) { free(Result); return false; }
        *Output = Result;
        *OutputLength = Length;
        *Found = (uint32_t)HitCount;
        return true;
    }

    size_t ScanIndex = 0U;
    uint32_t FileId = 0U;
    while (Matches < Limit) {
        size_t EntryIndex;
        if (SimpleTerm) {
            if (!CearchNameCandidatesNext(&Iterator, &FileId)) break;
            EntryIndex = (size_t)FileId;
        } else {
            if (ScanIndex >= Daemon->EntryCount) break;
            EntryIndex = ScanIndex++;
        }
        if (EntryIndex >= Daemon->EntryCount) continue;
        const CearchFileEntry *Entry = &Daemon->Entries[EntryIndex];
        const char *Base = strrchr(Entry->Path, '/');
        Base = Base ? Base + 1 : Entry->Path;
        const char *Haystack = MatchPath ? Entry->Path : Base;
        if (!CearchQueryMatchesEntry(&ParsedQuery, Entry, Haystack)) continue;

        if (!AppendResultPath(&Result, &Length, &Capacity, Entry->Path, NullDelimited)) {
            CearchQueryFree(&ParsedQuery);
            CearchNameQueryFree(&Prepared);
            free(Result);
            return false;
        }
        ++Matches;
    }

    CearchQueryFree(&ParsedQuery);
    CearchNameQueryFree(&Prepared);
    *Output = Result;
    *OutputLength = Length;
    *Found = Matches;
    return true;
}

static bool ContentIndexFresh(const CearchFileEntry *Entry) {
    if (!Entry || !Entry->ContentIndexed || Entry->Type != 'f') return false;
    struct stat Status;
    if (stat(Entry->Path, &Status) != 0 || !S_ISREG(Status.st_mode)) return false;
    return (uint64_t)Status.st_size == Entry->Size &&
           (int64_t)Status.st_mtim.tv_sec == Entry->Mtime &&
           (uint32_t)Status.st_mtim.tv_nsec == Entry->MtimeNsec &&
           (int64_t)Status.st_ctim.tv_sec == Entry->Ctime &&
           (uint32_t)Status.st_ctim.tv_nsec == Entry->CtimeNsec &&
           (uint64_t)Status.st_dev == Entry->Device &&
           (uint64_t)Status.st_ino == Entry->Inode;
}

static bool SearchContent(CearchDaemon *Daemon, const char *Query, uint32_t Limit, bool NullDelimited,
                          char **Output, size_t *OutputLength, uint32_t *Found) {
    size_t Capacity = 4096U;
    char *Result = malloc(Capacity);
    if (!Result) return false;
    CearchNameQuery Prepared = {0};
    if (!CearchNameQueryPrepare(Query, &Prepared)) {
        free(Result);
        return false;
    }

    size_t Length = 0U;
    uint32_t Matches = 0U;
    bool Success = true;
    if (Prepared.Count == 0U) {
        for (size_t Index = 0U; Index < Daemon->EntryCount && Matches < Limit; ++Index) {
            const CearchFileEntry *Entry = &Daemon->Entries[Index];
            if (Entry->Type != 'f' || !FileContains(Entry->Path, Query)) continue;
            if (!AppendResultPath(&Result, &Length, &Capacity, Entry->Path, NullDelimited)) {
                Success = false;
                break;
            }
            ++Matches;
        }
    } else {
        CearchIndex ContentIndex = { .Entries = Daemon->Entries, .Count = Daemon->EntryCount };
        if (!CearchAttachNameIndex(&ContentIndex, Daemon->Store.ContentTrigrams,
                                   Daemon->Store.ContentTrigramCount, nullptr,
                                   Daemon->Store.ContentTrigramPostingCount,
                                   Daemon->Store.ContentTrigramData,
                                   Daemon->Store.ContentTrigramDataSize,
                                   Daemon->Store.ContentTrigramSkips,
                                   Daemon->Store.ContentTrigramSkipCount)) {
            Success = false;
        } else {
            bool *Fresh = calloc(Daemon->EntryCount ? Daemon->EntryCount : 1U, sizeof(*Fresh));
            if (!Fresh) Success = false;
            if (Success) {
                for (size_t Index = 0U; Index < Daemon->EntryCount; ++Index)
                    Fresh[Index] = ContentIndexFresh(&Daemon->Entries[Index]);
                CearchNameCandidateIterator Iterator = {0};
                CearchNameQuery PreparedCopy = Prepared;
                if (!CearchNameCandidatesBegin(&ContentIndex, &PreparedCopy, &Iterator)) {
                    Success = false;
                } else {
                    uint32_t FileId = 0U;
                    while (Success && Matches < Limit &&
                           CearchNameCandidatesNext(&Iterator, &FileId)) {
                        if ((size_t)FileId >= Daemon->EntryCount || !Fresh[FileId]) continue;
                        const CearchFileEntry *Entry = &Daemon->Entries[FileId];
                        if (!FileContains(Entry->Path, Query)) continue;
                        if (!AppendResultPath(&Result, &Length, &Capacity, Entry->Path, NullDelimited)) {
                            Success = false;
                            break;
                        }
                        ++Matches;
                    }
                    for (size_t Index = 0U; Success && Matches < Limit && Index < Daemon->EntryCount; ++Index) {
                        const CearchFileEntry *Entry = &Daemon->Entries[Index];
                        if (Entry->Type != 'f' || Fresh[Index] || !FileContains(Entry->Path, Query)) continue;
                        if (!AppendResultPath(&Result, &Length, &Capacity, Entry->Path, NullDelimited)) {
                            Success = false;
                            break;
                        }
                        ++Matches;
                    }
                }
            }
            free(Fresh);
        }
        CearchFreeNameIndex(&ContentIndex);
    }

    CearchNameQueryFree(&Prepared);
    if (!Success) { free(Result); return false; }
    *Output = Result;
    *OutputLength = Length;
    *Found = Matches;
    return true;
}

static bool HandleClient(CearchDaemon *Daemon, int Client) {
    CearchRequest Request;
    if (!ReadExact(Client, &Request, sizeof(Request))) return false;
    if (Request.Magic != CearchProtocolMagic || Request.Version != CearchProtocolVersion ||
        Request.QueryLength > CearchMaximumQueryLength || Request.Limit > CearchMaximumResults)
        return WriteResponse(Client, 1U, 0U, "invalid request", 15U);

    char *Query = calloc((size_t)Request.QueryLength + 1U, 1U);
    if (!Query) return WriteResponse(Client, 2U, 0U, "out of memory", 13U);
    if (Request.QueryLength && !ReadExact(Client, Query, Request.QueryLength)) {
        free(Query);
        return false;
    }

    bool Success = true;
    char *Output = nullptr;
    size_t OutputLength = 0;
    uint32_t Found = 0;
    uint32_t Limit = Request.Limit ? Request.Limit : CearchDefaultLimit;
    switch (Request.Command) {
        case CearchCommandSearch:
            Success = SearchName(Daemon, Query, Limit, (Request.Flags & 1U) != 0U,
                                 (Request.Flags & 2U) != 0U, &Output, &OutputLength, &Found);
            break;
        case CearchCommandContent:
            Success = SearchContent(Daemon, Query, Limit, (Request.Flags & 2U) != 0U,
                                    &Output, &OutputLength, &Found);
            break;
        case CearchCommandStats: {
            char Buffer[128];
            int Length = snprintf(Buffer, sizeof(Buffer), "entries: %zu\n", Daemon->EntryCount);
            Success = Length > 0;
            if (Success) {
                Output = strndup(Buffer, (size_t)Length);
                Success = Output != nullptr;
                OutputLength = (size_t)Length;
            }
            break;
        }
        case CearchCommandPing:
            Output = strdup("ok\n");
            Success = Output != nullptr;
            OutputLength = 3U;
            break;
        case CearchCommandShutdown:
            atomic_store_explicit(&ClientStopRequested, true, memory_order_relaxed);
            Output = strdup("stopping\n");
            Success = Output != nullptr;
            OutputLength = 9U;
            break;
        default:
            Success = false;
            break;
    }

    free(Query);
    if (!Success) {
        free(Output);
        return WriteResponse(Client, 2U, 0U, "query failed", 12U);
    }
    bool Written = WriteResponse(Client, 0U, Found, Output, OutputLength);
    free(Output);
    return Written;
}

typedef struct {
    CearchDaemon *Daemon;
    int Client;
} CearchClientTask;

static void DestroyDaemonSync(CearchDaemon *Daemon) {
    (void)pthread_cond_destroy(&Daemon->ClientCondition);
    (void)pthread_mutex_destroy(&Daemon->ClientMutex);
    (void)pthread_rwlock_destroy(&Daemon->StateLock);
}

static void *ClientWorker(void *Context) {
    CearchClientTask *Task = Context;
    CearchDaemon *Daemon = Task->Daemon;
    int Client = Task->Client;
    struct timeval Timeout = { .tv_sec = 10, .tv_usec = 0 };
    (void)setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout));
    (void)setsockopt(Client, SOL_SOCKET, SO_SNDTIMEO, &Timeout, sizeof(Timeout));
    if (pthread_rwlock_rdlock(&Daemon->StateLock) == 0) {
        (void)HandleClient(Daemon, Client);
        (void)pthread_rwlock_unlock(&Daemon->StateLock);
    }
    close(Client);
    free(Task);
    (void)pthread_mutex_lock(&Daemon->ClientMutex);
    if (Daemon->ActiveClients > 0U) --Daemon->ActiveClients;
    (void)pthread_cond_broadcast(&Daemon->ClientCondition);
    (void)pthread_mutex_unlock(&Daemon->ClientMutex);
    return nullptr;
}

static void StartClientWorker(CearchDaemon *Daemon, int Client) {
    CearchClientTask *Task = malloc(sizeof(*Task));
    if (!Task) { close(Client); return; }
    *Task = (CearchClientTask){ .Daemon = Daemon, .Client = Client };
    (void)pthread_mutex_lock(&Daemon->ClientMutex);
    if (Daemon->ActiveClients >= CearchMaximumClients) {
        (void)pthread_mutex_unlock(&Daemon->ClientMutex);
        free(Task);
        close(Client);
        return;
    }
    ++Daemon->ActiveClients;
    (void)pthread_mutex_unlock(&Daemon->ClientMutex);

    pthread_t Worker;
    int Error = pthread_create(&Worker, nullptr, ClientWorker, Task);
    if (Error != 0) {
        (void)pthread_mutex_lock(&Daemon->ClientMutex);
        --Daemon->ActiveClients;
        (void)pthread_cond_broadcast(&Daemon->ClientCondition);
        (void)pthread_mutex_unlock(&Daemon->ClientMutex);
        free(Task);
        close(Client);
        return;
    }
    if (pthread_detach(Worker) != 0) (void)pthread_join(Worker, nullptr);
}

static char *DefaultSocketPath(void) {
    const char *Runtime = getenv("XDG_RUNTIME_DIR");
    if (!Runtime || !*Runtime) {
        errno = ENOENT;
        return nullptr;
    }
    size_t Length = strlen(Runtime);
    static const char Suffix[] = "/cearch.sock";
    if (Length > SIZE_MAX - sizeof(Suffix)) return nullptr;
    char *Path = malloc(Length + sizeof(Suffix));
    if (!Path) return nullptr;
    memcpy(Path, Runtime, Length);
    memcpy(Path + Length, Suffix, sizeof(Suffix));
    return Path;
}

static int RunDaemon(const char *Database, const char *SocketPath,
                     const char *const *Roots, size_t RootCount) {
    CearchDaemon Daemon = {
        .Listener = -1,
        .Store = { .Descriptor = -1 },
        .WatchFd = -1,
        .StateLock = PTHREAD_RWLOCK_INITIALIZER,
        .ClientMutex = PTHREAD_MUTEX_INITIALIZER,
        .ClientCondition = PTHREAD_COND_INITIALIZER
    };
    SignalStopRequested = 0;
    atomic_store_explicit(&ClientStopRequested, false, memory_order_relaxed);
    if (!BuildDaemonIndex(&Daemon, Database)) {
        fprintf(stderr, "cearchd: cannot load index: %s\n", strerror(errno));
        FreeDaemonIndex(&Daemon);
        DestroyDaemonSync(&Daemon);
        return 1;
    }
    if (!CopyRoots(&Daemon, Roots, RootCount)) {
        fprintf(stderr, "cearchd: cannot allocate root list\n");
        FreeDaemonIndex(&Daemon);
        DestroyDaemonSync(&Daemon);
        return 1;
    }
    if (Daemon.RootCount) {
        Daemon.WatchFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (Daemon.WatchFd < 0 || !ResetWatches(&Daemon)) {
            fprintf(stderr, "cearchd: unable to initialize filesystem watches: %s\n", strerror(errno));
            if (Daemon.WatchFd >= 0) close(Daemon.WatchFd);
            Daemon.WatchFd = -1;
            FreeRoots(&Daemon);
            FreeDaemonIndex(&Daemon);
            DestroyDaemonSync(&Daemon);
            return 1;
        }
    }

    int Listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Listener < 0) {
        fprintf(stderr, "cearchd: socket: %s\n", strerror(errno));
        if (Daemon.WatchFd >= 0) close(Daemon.WatchFd);
        FreeRoots(&Daemon);
        FreeDaemonIndex(&Daemon);
        DestroyDaemonSync(&Daemon);
        return 1;
    }
    struct sockaddr_un Address = { .sun_family = AF_UNIX };
    if (strlen(SocketPath) >= sizeof(Address.sun_path)) {
        fprintf(stderr, "cearchd: socket path too long\n");
        close(Listener);
        if (Daemon.WatchFd >= 0) close(Daemon.WatchFd);
        FreeRoots(&Daemon);
        FreeDaemonIndex(&Daemon);
        DestroyDaemonSync(&Daemon);
        return 1;
    }
    memcpy(Address.sun_path, SocketPath, strlen(SocketPath) + 1U);
    (void)unlink(SocketPath);
    mode_t OldMask = umask(0077);
    int BindResult = bind(Listener, (struct sockaddr *)&Address, sizeof(Address));
    (void)umask(OldMask);
    if (BindResult != 0 || chmod(SocketPath, 0600) != 0 || listen(Listener, 32) != 0) {
        fprintf(stderr, "cearchd: cannot bind/listen %s: %s\n", SocketPath, strerror(errno));
        close(Listener);
        (void)unlink(SocketPath);
        if (Daemon.WatchFd >= 0) close(Daemon.WatchFd);
        FreeRoots(&Daemon);
        FreeDaemonIndex(&Daemon);
        DestroyDaemonSync(&Daemon);
        return 1;
    }

    struct sigaction Action = { .sa_handler = HandleSignal };
    sigemptyset(&Action.sa_mask);
    sigaction(SIGINT, &Action, nullptr);
    sigaction(SIGTERM, &Action, nullptr);
    Daemon.Listener = Listener;
    Daemon.Running = true;
    Daemon.LastReconcile = time(nullptr);
    fprintf(stderr, "cearchd: serving %zu entries on %s%s\n",
            Daemon.EntryCount, SocketPath, Daemon.RootCount ? " with live reconciliation" : "");

    while (!SignalStopRequested && !atomic_load_explicit(&ClientStopRequested, memory_order_relaxed)) {
        struct pollfd PollDescriptors[2];
        nfds_t PollCount = 1U;
        PollDescriptors[0] = (struct pollfd){ .fd = Listener, .events = POLLIN };
        if (Daemon.WatchFd >= 0) {
            PollDescriptors[1] = (struct pollfd){ .fd = Daemon.WatchFd, .events = POLLIN };
            PollCount = 2U;
        }
        int PollResult = poll(PollDescriptors, PollCount, 1000);
        if (PollResult < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "cearchd: poll: %s\n", strerror(errno));
            break;
        }
        if (PollCount == 2U && (PollDescriptors[1].revents & POLLIN) != 0) {
            DrainWatchEvents(&Daemon);
        }
        time_t Now = time(nullptr);
        if (Daemon.RootCount && ((Daemon.Dirty && Now - Daemon.LastReconcile >= 1) ||
                                 Now - Daemon.LastReconcile >= CearchReconcileInterval)) {
            (void)ReconcileDaemon(&Daemon);
        }
        if ((PollDescriptors[0].revents & POLLIN) == 0) continue;
        int Client = accept4(Listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (Client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "cearchd: accept: %s\n", strerror(errno));
            break;
        }
        StartClientWorker(&Daemon, Client);
    }

    close(Listener);
    if (Daemon.WatchFd >= 0) close(Daemon.WatchFd);
    (void)unlink(SocketPath);
    (void)pthread_mutex_lock(&Daemon.ClientMutex);
    while (Daemon.ActiveClients != 0U)
        (void)pthread_cond_wait(&Daemon.ClientCondition, &Daemon.ClientMutex);
    (void)pthread_mutex_unlock(&Daemon.ClientMutex);
    FreeRoots(&Daemon);
    FreeDaemonIndex(&Daemon);
    DestroyDaemonSync(&Daemon);
    return 0;
}

static bool ConnectSocket(const char *SocketPath, int *Descriptor) {
    int Socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Socket < 0) return false;
    struct sockaddr_un Address = { .sun_family = AF_UNIX };
    if (strlen(SocketPath) >= sizeof(Address.sun_path)) {
        close(Socket);
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(Address.sun_path, SocketPath, strlen(SocketPath) + 1U);
    if (connect(Socket, (struct sockaddr *)&Address, sizeof(Address)) != 0) {
        int SavedErrno = errno;
        close(Socket);
        errno = SavedErrno;
        return false;
    }
    *Descriptor = Socket;
    return true;
}

static int RunClient(const char *SocketPath, uint16_t Command, uint32_t Flags,
                     uint32_t Limit, const char *Query) {
    int Socket = -1;
    if (!ConnectSocket(SocketPath, &Socket)) {
        fprintf(stderr, "cearch: cannot connect to %s: %s\n", SocketPath, strerror(errno));
        return 1;
    }
    size_t QueryLength = Query ? strlen(Query) : 0U;
    if (QueryLength > CearchMaximumQueryLength) {
        close(Socket);
        fprintf(stderr, "cearch: query too long\n");
        return 2;
    }
    CearchRequest Request = {
        .Magic = CearchProtocolMagic,
        .Version = CearchProtocolVersion,
        .Command = Command,
        .Flags = Flags,
        .QueryLength = (uint32_t)QueryLength,
        .Limit = Limit
    };
    bool Success = WriteExact(Socket, &Request, sizeof(Request)) &&
                   (!QueryLength || WriteExact(Socket, Query, QueryLength));
    CearchResponse Response;
    if (!Success || !ReadExact(Socket, &Response, sizeof(Response)) ||
        Response.Magic != CearchProtocolMagic || Response.Version != CearchProtocolVersion ||
        Response.MessageLength > 64U * 1024U * 1024U) {
        fprintf(stderr, "cearch: invalid daemon response\n");
        close(Socket);
        return 1;
    }
    char *Output = malloc((size_t)Response.MessageLength + 1U);
    if (!Output) {
        close(Socket);
        return 1;
    }
    if (Response.MessageLength && !ReadExact(Socket, Output, Response.MessageLength)) {
        free(Output);
        close(Socket);
        return 1;
    }
    Output[Response.MessageLength] = '\0';
    if (Response.Status == 0U) {
        if (Response.MessageLength) (void)fwrite(Output, 1U, Response.MessageLength, stdout);
    } else {
        fprintf(stderr, "cearch: %s\n", Output);
    }
    free(Output);
    close(Socket);
    return Response.Status == 0U ? 0 : 1;
}

static void Usage(FILE *Stream) {
    fprintf(Stream,
        "Usage:\n"
        "  cearchd serve --db DB [--socket PATH]\n"
        "  cearchd client [--socket PATH] [--limit N] search [--path] [--null] QUERY\n"
        "  cearchd client [--socket PATH] content [--null] [--limit N] QUERY\n"
        "  cearchd client [--socket PATH] stats|ping|shutdown\n");
}

static bool ParseNumber(const char *Text, uint32_t *Value) {
    if (!Text || !*Text) return false;
    uint64_t Number = 0;
    for (const unsigned char *Cursor = (const unsigned char *)Text; *Cursor; ++Cursor) {
        if (*Cursor < '0' || *Cursor > '9') return false;
        if (Number > (UINT32_MAX - (uint64_t)(*Cursor - '0')) / 10U) return false;
        Number = Number * 10U + (uint64_t)(*Cursor - '0');
    }
    *Value = (uint32_t)Number;
    return true;
}

int main(int Argc, char **Argv) {
    if (Argc < 2 || strcmp(Argv[1], "help") == 0 || strcmp(Argv[1], "--help") == 0) {
        Usage(stdout);
        return Argc < 2 ? 2 : 0;
    }

    char *DefaultPath = nullptr;
    const char *SocketPath = nullptr;
    int Start = 2;
    if (strcmp(Argv[1], "client") == 0) {
        uint32_t ClientLimit = CearchDefaultLimit;
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--socket") == 0 && Index + 1 < Argc) {
                SocketPath = Argv[++Index];
            } else if (strcmp(Argv[Index], "--limit") == 0 && Index + 1 < Argc) {
                if (!ParseNumber(Argv[++Index], &ClientLimit) || !ClientLimit ||
                    ClientLimit > CearchMaximumResults) {
                    fprintf(stderr, "cearch: invalid limit\n");
                    return 2;
                }
            } else {
                Start = Index;
                break;
            }
        }
        if (!SocketPath) {
            DefaultPath = DefaultSocketPath();
            SocketPath = DefaultPath;
        }
        if (!SocketPath || Start >= Argc) {
            Usage(stderr);
            free(DefaultPath);
            return 2;
        }
        uint16_t Command = 0;
        uint32_t Flags = 0, Limit = ClientLimit;
        const char *Query = nullptr;
        const char *Action = Argv[Start++];
        if (strcmp(Action, "search") == 0) Command = CearchCommandSearch;
        else if (strcmp(Action, "content") == 0) Command = CearchCommandContent;
        else if (strcmp(Action, "stats") == 0) Command = CearchCommandStats;
        else if (strcmp(Action, "ping") == 0) Command = CearchCommandPing;
        else if (strcmp(Action, "shutdown") == 0) Command = CearchCommandShutdown;
        else {
            Usage(stderr);
            free(DefaultPath);
            return 2;
        }

        for (int Index = Start; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--path") == 0 && Command == CearchCommandSearch) Flags |= 1U;
            else if (strcmp(Argv[Index], "--null") == 0 &&
                     (Command == CearchCommandSearch || Command == CearchCommandContent)) Flags |= 2U;
            else if (strcmp(Argv[Index], "--limit") == 0 && Index + 1 < Argc) {
                if (!ParseNumber(Argv[++Index], &Limit) || !Limit || Limit > CearchMaximumResults) {
                    fprintf(stderr, "cearch: invalid limit\n");
                    free(DefaultPath);
                    return 2;
                }
            } else if (!Query) Query = Argv[Index];
            else {
                Usage(stderr);
                free(DefaultPath);
                return 2;
            }
        }
        if ((Command == CearchCommandSearch || Command == CearchCommandContent) && !Query) {
            Usage(stderr);
            free(DefaultPath);
            return 2;
        }
        int Result = RunClient(SocketPath, Command, Flags, Limit, Query);
        free(DefaultPath);
        return Result;
    }

    if (strcmp(Argv[1], "serve") == 0) {
        const char *Database = nullptr;
        const char **Roots = calloc((size_t)Argc, sizeof(*Roots));
        if (!Roots) return 1;
        size_t RootCount = 0;
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--db") == 0 && Index + 1 < Argc && !Database) {
                Database = Argv[++Index];
            } else if (strcmp(Argv[Index], "--socket") == 0 && Index + 1 < Argc) {
                SocketPath = Argv[++Index];
            } else if (strcmp(Argv[Index], "--root") == 0 && Index + 1 < Argc) {
                Roots[RootCount++] = Argv[++Index];
            } else {
                free(Roots);
                Usage(stderr);
                return 2;
            }
        }
        if (!SocketPath) {
            DefaultPath = DefaultSocketPath();
            SocketPath = DefaultPath;
        }
        if (!Database || !SocketPath) {
            Usage(stderr);
            free(Roots);
            free(DefaultPath);
            return 2;
        }
        int Result = RunDaemon(Database, SocketPath, Roots, RootCount);
        free(Roots);
        free(DefaultPath);
        return Result;
    }

    Usage(stderr);
    return 2;
}
