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
    if (!*Needle) return true;
    size_t NeedleLength = strlen(Needle);
    for (const char *Start = Haystack; *Start; ++Start)
        if (strncasecmp(Start, Needle, NeedleLength) == 0) return true;
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
            for (size_t FreeIndex = 0; FreeIndex < Index; ++FreeIndex) free(Entries[FreeIndex].Path);
            free(Entries);
            CearchStoreClose(&Store);
            errno = EINVAL;
            return false;
        }
        Entries[Index] = (CearchFileEntry){
            .Path = strdup(Path),
            .Size = DiskEntry->Size,
            .Mtime = DiskEntry->Mtime,
            .Device = DiskEntry->Device,
            .Inode = DiskEntry->Inode,
            .Type = DiskEntry->Type == 1U ? 'f' : DiskEntry->Type == 2U ? 'd' : 'o'
        };
        if (!Entries[Index].Path) {
            for (size_t FreeIndex = 0; FreeIndex < Index; ++FreeIndex) free(Entries[FreeIndex].Path);
            free(Entries);
            CearchStoreClose(&Store);
            return false;
        }
    }

    Daemon->Store = Store;
    Daemon->Entries = Entries;
    Daemon->EntryCount = Store.EntryCount;
    Daemon->NameIndex.Entries = Entries;
    Daemon->NameIndex.Count = Store.EntryCount;
    if (!CearchBuildNameIndex(&Daemon->NameIndex)) {
        for (size_t Index = 0; Index < Daemon->EntryCount; ++Index) free(Daemon->Entries[Index].Path);
        free(Daemon->Entries);
        Daemon->Entries = nullptr;
        Daemon->EntryCount = 0;
        CearchStoreClose(&Daemon->Store);
        return false;
    }
    Daemon->Database = strdup(Database);
    return Daemon->Database != nullptr;
}

static void FreeDaemonIndex(CearchDaemon *Daemon) {
    CearchFreeNameIndex(&Daemon->NameIndex);
    for (size_t Index = 0; Index < Daemon->EntryCount; ++Index) free(Daemon->Entries[Index].Path);
    free(Daemon->Entries);
    Daemon->Entries = nullptr;
    Daemon->EntryCount = 0;
    CearchStoreClose(&Daemon->Store);
    free(Daemon->Database);
    Daemon->Database = nullptr;
}

static bool SearchName(CearchDaemon *Daemon, const char *Query, uint32_t Limit,
                       bool MatchPath, char **Output, size_t *OutputLength, uint32_t *Found) {
    size_t Capacity = 4096U;
    char *Result = malloc(Capacity);
    if (!Result) return false;
    size_t Length = 0;
    uint32_t Matches = 0;
    size_t QueryLength = strlen(Query);
    bool UseTrigrams = QueryLength >= 3U && !MatchPath;
    CearchNameQuery Prepared = {0};
    if (UseTrigrams && !CearchNameQueryPrepare(Query, &Prepared)) {
        free(Result);
        return false;
    }
    for (size_t Index = 0; Index < Daemon->EntryCount && Matches < Limit; ++Index) {
        const CearchFileEntry *Entry = &Daemon->Entries[Index];
        const char *Base = strrchr(Entry->Path, '/');
        Base = Base ? Base + 1 : Entry->Path;
        const char *Haystack = MatchPath ? Entry->Path : Base;
        if (!ContainsCaseInsensitive(Haystack, Query)) continue;
        if (UseTrigrams && !CearchNameCandidatePrepared(&Daemon->NameIndex, &Prepared, (uint32_t)Index)) continue;
        size_t PathLength = strlen(Entry->Path);
        if (PathLength > SIZE_MAX - Length - 1U) {
            CearchNameQueryFree(&Prepared);
            free(Result);
            return false;
        }
        size_t Needed = Length + PathLength + 1U;
        if (Needed > Capacity) {
            size_t Next = Capacity;
            while (Next < Needed) {
                if (Next > SIZE_MAX / 2U) {
                    CearchNameQueryFree(&Prepared);
                    free(Result);
                    return false;
                }
                Next *= 2U;
            }
            char *NewResult = realloc(Result, Next);
            if (!NewResult) {
                CearchNameQueryFree(&Prepared);
                free(Result);
                return false;
            }
            Result = NewResult;
            Capacity = Next;
        }
        memcpy(Result + Length, Entry->Path, PathLength);
        Length += PathLength;
        Result[Length++] = '\n';
        ++Matches;
    }
    CearchNameQueryFree(&Prepared);
    *Output = Result;
    *OutputLength = Length;
    *Found = Matches;
    return true;
}

static bool SearchContent(CearchDaemon *Daemon, const char *Query, uint32_t Limit,
                          char **Output, size_t *OutputLength, uint32_t *Found) {
    size_t Capacity = 4096U;
    char *Result = malloc(Capacity);
    if (!Result) return false;
    size_t Length = 0;
    uint32_t Matches = 0;
    for (size_t Index = 0; Index < Daemon->EntryCount && Matches < Limit; ++Index) {
        const CearchFileEntry *Entry = &Daemon->Entries[Index];
        if (Entry->Type != 'f' || !FileContains(Entry->Path, Query)) continue;
        size_t PathLength = strlen(Entry->Path);
        if (PathLength > SIZE_MAX - Length - 1U) {
            free(Result);
            return false;
        }
        size_t Needed = Length + PathLength + 1U;
        if (Needed > Capacity) {
            size_t Next = Capacity;
            while (Next < Needed) {
                if (Next > SIZE_MAX / 2U) {
                    free(Result);
                    return false;
                }
                Next *= 2U;
            }
            char *NewResult = realloc(Result, Next);
            if (!NewResult) {
                free(Result);
                return false;
            }
            Result = NewResult;
            Capacity = Next;
        }
        memcpy(Result + Length, Entry->Path, PathLength);
        Length += PathLength;
        Result[Length++] = '\n';
        ++Matches;
    }
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
                                 &Output, &OutputLength, &Found);
            break;
        case CearchCommandContent:
            Success = SearchContent(Daemon, Query, Limit, &Output, &OutputLength, &Found);
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
            StopRequested = 1;
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

static int RunDaemon(const char *Database, const char *SocketPath) {
    CearchDaemon Daemon = { .Listener = -1, .Store = { .Descriptor = -1 } };
    if (!BuildDaemonIndex(&Daemon, Database)) {
        fprintf(stderr, "cearchd: cannot load index: %s\n", strerror(errno));
        FreeDaemonIndex(&Daemon);
        return 1;
    }
    int Listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Listener < 0) {
        fprintf(stderr, "cearchd: socket: %s\n", strerror(errno));
        FreeDaemonIndex(&Daemon);
        return 1;
    }
    struct sockaddr_un Address = { .sun_family = AF_UNIX };
    if (strlen(SocketPath) >= sizeof(Address.sun_path)) {
        fprintf(stderr, "cearchd: socket path too long\n");
        close(Listener);
        FreeDaemonIndex(&Daemon);
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
        FreeDaemonIndex(&Daemon);
        return 1;
    }

    struct sigaction Action = { .sa_handler = HandleSignal };
    sigemptyset(&Action.sa_mask);
    sigaction(SIGINT, &Action, nullptr);
    sigaction(SIGTERM, &Action, nullptr);
    Daemon.Listener = Listener;
    Daemon.Running = true;
    fprintf(stderr, "cearchd: serving %zu entries on %s\n", Daemon.EntryCount, SocketPath);

    while (!StopRequested) {
        int Client = accept4(Listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (Client < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "cearchd: accept: %s\n", strerror(errno));
            break;
        }
        (void)HandleClient(&Daemon, Client);
        close(Client);
    }

    close(Listener);
    (void)unlink(SocketPath);
    FreeDaemonIndex(&Daemon);
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
        if (Response.MessageLength) fwrite(Output, 1U, Response.MessageLength, stdout);
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
        "  cearchd client [--socket PATH] search [--path] [--limit N] QUERY\n"
        "  cearchd client [--socket PATH] content [--limit N] QUERY\n"
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
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--socket") == 0 && Index + 1 < Argc) {
                SocketPath = Argv[++Index];
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
        uint32_t Flags = 0, Limit = CearchDefaultLimit;
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
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--db") == 0 && Index + 1 < Argc && !Database)
                Database = Argv[++Index];
            else if (strcmp(Argv[Index], "--socket") == 0 && Index + 1 < Argc)
                SocketPath = Argv[++Index];
            else {
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
            free(DefaultPath);
            return 2;
        }
        int Result = RunDaemon(Database, SocketPath);
        free(DefaultPath);
        return Result;
    }

    Usage(stderr);
    return 2;
}
