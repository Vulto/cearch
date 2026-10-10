#define _GNU_SOURCE
#include "index.h"
#include "store.h"
#include "indexer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CearchVersion 1U
#define CearchMaxLine (1024U * 1024U)
#define CearchDefaultLimit 1000U
#define CearchMaxQuery (1024U * 1024U)

typedef struct {
    CearchFileEntry *Entries;
    size_t Count;
    size_t Capacity;
    bool OwnsPaths;
} FileTable;

static void PrintUsage(FILE *Stream) {
    fprintf(Stream,
        "cearch - CLI file search (C23)\n"
        "Usage:\n"
        "  cearch index   --db DB ROOT [ROOT ...]\n"
        "  cearch search  --db DB [--limit N] [--path] QUERY\n"
        "  cearch content --db DB [--limit N] QUERY\n"
        "  cearch stats   --db DB\n"
        "  cearch help\n");
}

static bool ParseUnsigned(const char *Text, size_t *Value) {
    if (!Text || !*Text) return false;
    size_t Result = 0;
    for (const unsigned char *Cursor = (const unsigned char *)Text; *Cursor; ++Cursor) {
        if (*Cursor < '0' || *Cursor > '9') return false;
        size_t Digit = (size_t)(*Cursor - '0');
        if (Result > (SIZE_MAX - Digit) / 10U) return false;
        Result = Result * 10U + Digit;
    }
    *Value = Result;
    return true;
}

static bool ReserveEntries(FileTable *Table, size_t Needed) {
    if (Needed <= Table->Capacity) return true;
    size_t NewCapacity = Table->Capacity ? Table->Capacity : 1024U;
    while (NewCapacity < Needed) {
        if (NewCapacity > SIZE_MAX / 2U) return false;
        NewCapacity *= 2U;
    }
    if (NewCapacity > SIZE_MAX / sizeof(*Table->Entries)) return false;
    CearchFileEntry *NewEntries = realloc(Table->Entries, NewCapacity * sizeof(*Table->Entries));
    if (!NewEntries) return false;
    Table->Entries = NewEntries;
    Table->Capacity = NewCapacity;
    return true;
}

static void FreeTable(FileTable *Table) {
    if (Table->OwnsPaths)
        for (size_t Index = 0; Index < Table->Count; ++Index) free(Table->Entries[Index].Path);
    free(Table->Entries);
    *Table = (FileTable){0};
}

static bool IndexCommand(const char *Database, int RootCount, char **Roots) {
    if (RootCount <= 0) {
        fprintf(stderr, "cearch: index requires at least one ROOT\n");
        return false;
    }
    return CearchBuildSnapshot(Database, (const char *const *)Roots, (size_t)RootCount, nullptr);
}

static bool LoadDatabase(const char *Database, FileTable *Table, CearchStore *Store) {
    if (!Table || !Store) {
        errno = EINVAL;
        return false;
    }
    *Table = (FileTable){0};
    *Store = (CearchStore){ .Descriptor = -1 };
    if (!CearchStoreOpen(Database, Store)) {
        fprintf(stderr, "cearch: cannot open or validate snapshot %s: %s\n", Database, strerror(errno));
        return false;
    }
    if (Store->EntryCount > SIZE_MAX / sizeof(*Table->Entries) ||
        !ReserveEntries(Table, Store->EntryCount)) {
        fprintf(stderr, "cearch: out of memory loading snapshot\n");
        CearchStoreClose(Store);
        return false;
    }

    for (size_t Index = 0U; Index < Store->EntryCount; ++Index) {
        const CearchDiskEntry *Source = &Store->Entries[Index];
        const char *Path = CearchStorePath(Store, Index);
        if (!Path) {
            fprintf(stderr, "cearch: invalid path offset in snapshot\n");
            CearchStoreClose(Store);
            FreeTable(Table);
            return false;
        }
        Table->Entries[Table->Count++] = (CearchFileEntry){
            .Path = (char *)Path,
            .Size = Source->Size,
            .Mtime = Source->Mtime,
            .Device = Source->Device,
            .Inode = Source->Inode,
            .Type = Source->Type == 1U ? 'f' : Source->Type == 2U ? 'd' : 'o'
        };
    }
    Table->OwnsPaths = false;
    return true;
}

static bool ContainsCaseInsensitive(const char *Haystack, const char *Needle) {
    if (!*Needle) return true;
    size_t NeedleLength = strlen(Needle);
    for (const char *Start = Haystack; *Start; ++Start)
        if (strncasecmp(Start, Needle, NeedleLength) == 0) return true;
    return false;
}

static const char *BaseName(const char *Path) {
    const char *Slash = strrchr(Path, '/');
    return Slash ? Slash + 1 : Path;
}

static bool SearchCommand(const char *Database, const char *Query, bool MatchPath, size_t Limit) {
    FileTable Table = {0};
    if (!LoadDatabase(Database, &Table)) return false;

    CearchIndex Index = {0};
    Index.Entries = Table.Entries;
    Index.Count = Table.Count;
    if (Table.Count > UINT32_MAX || !CearchBuildNameIndex(&Index)) {
        fprintf(stderr, "cearch: could not build trigram index\n");
        FreeTable(&Table);
        CearchFreeNameIndex(&Index);
        return false;
    }

    size_t Found = 0;
    size_t QueryLength = strlen(Query);
    bool UseTrigrams = QueryLength >= 3U && !MatchPath;
    CearchNameQuery Prepared = {0};
    if (UseTrigrams && !CearchNameQueryPrepare(Query, &Prepared)) {
        CearchFreeNameIndex(&Index);
        FreeTable(&Table);
        return false;
    }

    for (size_t EntryIndex = 0; EntryIndex < Table.Count; ++EntryIndex) {
        const CearchFileEntry *Entry = &Table.Entries[EntryIndex];
        const char *Haystack = MatchPath ? Entry->Path : BaseName(Entry->Path);
        if (UseTrigrams &&
            !CearchNameCandidatePrepared(&Index, &Prepared, (uint32_t)EntryIndex)) continue;
        if (!ContainsCaseInsensitive(Haystack, Query)) continue;
        if (puts(Entry->Path) == EOF) {
            CearchNameQueryFree(&Prepared);
            CearchFreeNameIndex(&Index);
            FreeTable(&Table);
            return false;
        }
        if (++Found >= Limit) break;
    }

    CearchNameQueryFree(&Prepared);
    CearchFreeNameIndex(&Index);
    FreeTable(&Table);
    return true;
}

static bool IsBinaryBuffer(const unsigned char *Buffer, size_t Length) {
    return memchr(Buffer, '\0', Length) != nullptr;
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
    size_t BufferCapacity = 64U * 1024U;
    if (QueryLength > SIZE_MAX - BufferCapacity) {
        close(Descriptor);
        return false;
    }
    unsigned char *Buffer = malloc(BufferCapacity + QueryLength);
    if (!Buffer) {
        close(Descriptor);
        return false;
    }

    size_t Carry = 0;
    bool Match = false;
    for (;;) {
        ssize_t Count = read(Descriptor, Buffer + Carry, BufferCapacity);
        if (Count < 0 && errno == EINTR) continue;
        if (Count <= 0) break;
        size_t Total = Carry + (size_t)Count;
        if (IsBinaryBuffer(Buffer, Total)) break;
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

static bool ContentCommand(const char *Database, const char *Query, size_t Limit) {
    FileTable Table = {0};
    if (!LoadDatabase(Database, &Table)) return false;
    size_t Found = 0;
    for (size_t Index = 0; Index < Table.Count; ++Index) {
        const CearchFileEntry *Entry = &Table.Entries[Index];
        if (Entry->Type != 'f' || !FileContains(Entry->Path, Query)) continue;
        if (puts(Entry->Path) == EOF) {
            FreeTable(&Table);
            return false;
        }
        if (++Found >= Limit) break;
    }
    FreeTable(&Table);
    return true;
}

static bool StatsCommand(const char *Database) {
    FileTable Table = {0};
    if (!LoadDatabase(Database, &Table)) return false;
    size_t Files = 0, Directories = 0, Other = 0;
    for (size_t Index = 0; Index < Table.Count; ++Index) {
        if (Table.Entries[Index].Type == 'f') ++Files;
        else if (Table.Entries[Index].Type == 'd') ++Directories;
        else ++Other;
    }
    printf("entries: %zu\nfiles: %zu\ndirectories: %zu\nother: %zu\n",
           Table.Count, Files, Directories, Other);
    FreeTable(&Table);
    return true;
}

int main(int Argc, char **Argv) {
    if (Argc < 2 || strcmp(Argv[1], "help") == 0 || strcmp(Argv[1], "--help") == 0) {
        PrintUsage(stdout);
        return Argc < 2 ? 2 : 0;
    }
    if (strcmp(Argv[1], "index") == 0) {
        const char *Database = nullptr;
        char **Roots = calloc((size_t)Argc, sizeof(*Roots));
        if (!Roots) return 1;
        int RootCount = 0;
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--db") == 0) {
                if (++Index >= Argc || Database) {
                    free(Roots);
                    PrintUsage(stderr);
                    return 2;
                }
                Database = Argv[Index];
            } else {
                Roots[RootCount++] = Argv[Index];
            }
        }
        if (!Database || RootCount == 0) {
            free(Roots);
            PrintUsage(stderr);
            return 2;
        }
        bool Success = IndexCommand(Database, RootCount, Roots);
        free(Roots);
        return Success ? 0 : 1;
    }
    if (strcmp(Argv[1], "search") == 0 || strcmp(Argv[1], "content") == 0) {
        bool Content = strcmp(Argv[1], "content") == 0;
        const char *Database = nullptr;
        const char *Query = nullptr;
        bool MatchPath = false;
        size_t Limit = CearchDefaultLimit;
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--db") == 0) {
                if (++Index >= Argc || Database) {
                    PrintUsage(stderr);
                    return 2;
                }
                Database = Argv[Index];
            } else if (strcmp(Argv[Index], "--path") == 0 && !Content) {
                MatchPath = true;
            } else if (strcmp(Argv[Index], "--limit") == 0) {
                if (++Index >= Argc || !ParseUnsigned(Argv[Index], &Limit) || Limit == 0) {
                    fprintf(stderr, "cearch: --limit requires a positive integer\n");
                    return 2;
                }
            } else if (!Query) {
                Query = Argv[Index];
            } else {
                fprintf(stderr, "cearch: only one QUERY argument is supported; quote it if it contains spaces\n");
                return 2;
            }
        }
        if (!Database || !Query || strlen(Query) > CearchMaxQuery) {
            PrintUsage(stderr);
            return 2;
        }
        bool Success = Content ? ContentCommand(Database, Query, Limit)
                               : SearchCommand(Database, Query, MatchPath, Limit);
        return Success ? 0 : 1;
    }
    if (strcmp(Argv[1], "stats") == 0) {
        const char *Database = nullptr;
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--db") == 0 && Index + 1 < Argc && !Database) {
                Database = Argv[++Index];
            } else {
                PrintUsage(stderr);
                return 2;
            }
        }
        if (!Database) {
            PrintUsage(stderr);
            return 2;
        }
        return StatsCommand(Database) ? 0 : 1;
    }
    PrintUsage(stderr);
    return 2;
}
