#define _GNU_SOURCE
#include "index.h"
#include "store.h"
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
    char *Path;
    uint64_t Size;
    int64_t Mtime;
    uint64_t Device;
    uint64_t Inode;
    char Type;
} FileEntry;

typedef struct {
    FileEntry *Entries;
    size_t Count;
    size_t Capacity;
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
    FileEntry *NewEntries = realloc(Table->Entries, NewCapacity * sizeof(*Table->Entries));
    if (!NewEntries) return false;
    Table->Entries = NewEntries;
    Table->Capacity = NewCapacity;
    return true;
}

static bool AddEntry(FileTable *Table, const char *Path, const struct stat *Status) {
    if (!ReserveEntries(Table, Table->Count + 1U)) return false;
    char *Copy = strdup(Path);
    if (!Copy) return false;
    Table->Entries[Table->Count++] = (FileEntry){
        .Path = Copy,
        .Size = Status->st_size < 0 ? 0U : (uint64_t)Status->st_size,
        .Mtime = (int64_t)Status->st_mtime,
        .Device = (uint64_t)Status->st_dev,
        .Inode = (uint64_t)Status->st_ino,
        .Type = S_ISDIR(Status->st_mode) ? 'd' : S_ISREG(Status->st_mode) ? 'f' : 'o'
    };
    return true;
}

static void FreeTable(FileTable *Table) {
    for (size_t Index = 0; Index < Table->Count; ++Index) free(Table->Entries[Index].Path);
    free(Table->Entries);
    *Table = (FileTable){0};
}

static char *JoinPath(const char *Parent, const char *Name) {
    size_t ParentLength = strlen(Parent);
    size_t NameLength = strlen(Name);
    bool HasSlash = ParentLength && Parent[ParentLength - 1U] == '/';
    if (NameLength > SIZE_MAX - ParentLength - 2U) return nullptr;
    size_t Length = ParentLength + NameLength + (HasSlash ? 1U : 2U);
    char *Path = malloc(Length);
    if (!Path) return nullptr;
    memcpy(Path, Parent, ParentLength);
    size_t Offset = ParentLength;
    if (!HasSlash) Path[Offset++] = '/';
    memcpy(Path + Offset, Name, NameLength + 1U);
    return Path;
}

static bool IsDotEntry(const char *Name) {
    return strcmp(Name, ".") == 0 || strcmp(Name, "..") == 0;
}

static bool WalkPath(FileTable *Table, const char *Path) {
    struct stat Status;
    if (lstat(Path, &Status) != 0) {
        fprintf(stderr, "cearch: cannot stat %s: %s\n", Path, strerror(errno));
        return true;
    }
    if (!AddEntry(Table, Path, &Status)) {
        fprintf(stderr, "cearch: out of memory indexing %s\n", Path);
        return false;
    }
    if (!S_ISDIR(Status.st_mode)) return true;

    DIR *Directory = opendir(Path);
    if (!Directory) {
        fprintf(stderr, "cearch: cannot read directory %s: %s\n", Path, strerror(errno));
        return true;
    }
    int DirectoryFd = dirfd(Directory);
    struct dirent *Item;
    for (;;) {
        errno = 0;
        Item = readdir(Directory);
        if (!Item) {
            if (errno != 0)
                fprintf(stderr, "cearch: error reading directory %s: %s\n", Path, strerror(errno));
            break;
        }
        if (IsDotEntry(Item->d_name)) continue;
        char *Child = JoinPath(Path, Item->d_name);
        if (!Child) {
            closedir(Directory);
            fprintf(stderr, "cearch: out of memory building path\n");
            return false;
        }
        bool Success = WalkPath(Table, Child);
        free(Child);
        if (!Success) {
            closedir(Directory);
            return false;
        }
    }
    (void)DirectoryFd;
    closedir(Directory);
    return true;
}

static bool IndexCommand(const char *Database, int RootCount, char **Roots) {
    if (RootCount <= 0) {
        fprintf(stderr, "cearch: index requires at least one ROOT\n");
        return false;
    }
    FileTable Table = {0};
    for (int Index = 0; Index < RootCount; ++Index) {
        if (!WalkPath(&Table, Roots[Index])) {
            FreeTable(&Table);
            return false;
        }
    }

    if (Table.Count > UINT32_MAX) {
        fprintf(stderr, "cearch: this snapshot format supports at most UINT32_MAX entries\n");
        FreeTable(&Table);
        return false;
    }

    size_t StringsSize = 0;
    for (size_t Index = 0; Index < Table.Count; ++Index) {
        size_t Length = strlen(Table.Entries[Index].Path);
        if (Length > UINT32_MAX || StringsSize > UINT32_MAX - Length - 1U) {
            fprintf(stderr, "cearch: path string table exceeds 4 GiB\n");
            FreeTable(&Table);
            return false;
        }
        StringsSize += Length + 1U;
    }
    char *Strings = malloc(StringsSize ? StringsSize : 1U);
    CearchDiskEntry *Entries = calloc(Table.Count ? Table.Count : 1U, sizeof(*Entries));
    if (!Strings || !Entries) {
        fprintf(stderr, "cearch: out of memory creating snapshot\n");
        free(Strings);
        free(Entries);
        FreeTable(&Table);
        return false;
    }

    size_t StringOffset = 0;
    for (size_t Index = 0; Index < Table.Count; ++Index) {
        FileEntry *Source = &Table.Entries[Index];
        size_t Length = strlen(Source->Path);
        Entries[Index] = (CearchDiskEntry){
            .PathOffset = (uint32_t)StringOffset,
            .PathLength = (uint32_t)Length,
            .Size = Source->Size,
            .Mtime = Source->Mtime,
            .Device = Source->Device,
            .Inode = Source->Inode,
            .Type = Source->Type == 'f' ? 1U : Source->Type == 'd' ? 2U : 3U
        };
        memcpy(Strings + StringOffset, Source->Path, Length + 1U);
        StringOffset += Length + 1U;
    }

    bool Success = CearchStoreWrite(Database, Entries, Table.Count, Strings, StringsSize);
    if (!Success)
        fprintf(stderr, "cearch: failed writing snapshot %s: %s\n", Database, strerror(errno));
    else
        fprintf(stderr, "cearch: indexed %zu entries into %s\n", Table.Count, Database);

    free(Strings);
    free(Entries);
    FreeTable(&Table);
    return Success;
}

static bool LoadDatabase(const char *Database, FileTable *Table) {
    CearchStore Store = { .Descriptor = -1 };
    if (!CearchStoreOpen(Database, &Store)) {
        fprintf(stderr, "cearch: cannot open or validate snapshot %s: %s\n", Database, strerror(errno));
        return false;
    }
    if (Store.EntryCount > SIZE_MAX / sizeof(*Table->Entries) ||
        !ReserveEntries(Table, Store.EntryCount)) {
        fprintf(stderr, "cearch: out of memory loading snapshot\n");
        CearchStoreClose(&Store);
        return false;
    }
    for (size_t Index = 0; Index < Store.EntryCount; ++Index) {
        const CearchDiskEntry *Source = &Store.Entries[Index];
        const char *Path = CearchStorePath(&Store, Index);
        if (!Path) {
            fprintf(stderr, "cearch: invalid path offset in snapshot\n");
            CearchStoreClose(&Store);
            FreeTable(Table);
            return false;
        }
        char *Copy = strdup(Path);
        if (!Copy) {
            CearchStoreClose(&Store);
            FreeTable(Table);
            return false;
        }
        Table->Entries[Table->Count++] = (FileEntry){
            .Path = Copy,
            .Size = Source->Size,
            .Mtime = Source->Mtime,
            .Device = Source->Device,
            .Inode = Source->Inode,
            .Type = Source->Type == 1U ? 'f' : Source->Type == 2U ? 'd' : 'o'
        };
    }
    CearchStoreClose(&Store);
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
    Index.Entries = (CearchFileEntry *)Table.Entries;
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
        const FileEntry *Entry = &Table.Entries[EntryIndex];
        const char *Haystack = MatchPath ? Entry->Path : BaseName(Entry->Path);
        if (UseTrigrams && !CearchNameCandidatePrepared(&Index, &Prepared, (uint32_t)EntryIndex)) continue;
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
        const FileEntry *Entry = &Table.Entries[Index];
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
