#define _GNU_SOURCE
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
#include <time.h>
#include <unistd.h>

#define CearchVersion 1U
#define CearchMaxLine (1024U * 1024U)
#define CearchDefaultLimit 1000U

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
    FileEntry *Entry = &Table->Entries[Table->Count++];
    Entry->Path = Copy;
    Entry->Size = Status->st_size < 0 ? 0U : (uint64_t)Status->st_size;
    Entry->Mtime = (int64_t)Status->st_mtime;
    Entry->Device = (uint64_t)Status->st_dev;
    Entry->Inode = (uint64_t)Status->st_ino;
    Entry->Type = S_ISDIR(Status->st_mode) ? 'd' : S_ISREG(Status->st_mode) ? 'f' : 'o';
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
    if (ParentLength > SIZE_MAX - NameLength - 2U) return nullptr;
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

    errno = 0;
    struct dirent *Item;
    while ((Item = readdir(Directory)) != nullptr) {
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
        errno = 0;
    }
    if (errno != 0)
        fprintf(stderr, "cearch: error reading directory %s: %s\n", Path, strerror(errno));
    closedir(Directory);
    return true;
}

static bool WriteEntry(FILE *Stream, const FileEntry *Entry) {
    /* Paths containing newline or tab are escaped; backslash is escaped too. */
    if (fprintf(Stream, "E\t%c\t%" PRIu64 "\t%" PRId64 "\t%" PRIu64 "\t%" PRIu64 "\t",
                Entry->Type, Entry->Size, Entry->Mtime, Entry->Device, Entry->Inode) < 0) return false;
    for (const unsigned char *Cursor = (const unsigned char *)Entry->Path; *Cursor; ++Cursor) {
        if (*Cursor == '\\') {
            if (fputs("\\\\", Stream) == EOF) return false;
        } else if (*Cursor == '\t') {
            if (fputs("\\\t", Stream) == EOF) return false;
        } else if (*Cursor == '\n') {
            if (fputs("\\\n", Stream) == EOF) return false;
        } else if (*Cursor == '\r') {
            if (fputs("\\\r", Stream) == EOF) return false;
        } else if (fputc(*Cursor, Stream) == EOF) return false;
    }
    return fputc('\n', Stream) != EOF;
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

    size_t TempLength = strlen(Database) + 40U;
    char *Temporary = malloc(TempLength);
    if (!Temporary) {
        FreeTable(&Table);
        return false;
    }
    snprintf(Temporary, TempLength, "%s.tmp.%ld", Database, (long)getpid());
    FILE *Stream = fopen(Temporary, "wx");
    if (!Stream) {
        fprintf(stderr, "cearch: cannot create %s: %s\n", Temporary, strerror(errno));
        free(Temporary);
        FreeTable(&Table);
        return false;
    }

    bool Success = fprintf(Stream, "CEARCH\t%u\n", CearchVersion) >= 0;
    for (size_t Index = 0; Success && Index < Table.Count; ++Index)
        Success = WriteEntry(Stream, &Table.Entries[Index]);
    if (Success && fflush(Stream) != 0) Success = false;
    if (Success && fsync(fileno(Stream)) != 0) Success = false;
    if (fclose(Stream) != 0) Success = false;
    if (Success && rename(Temporary, Database) != 0) Success = false;
    if (!Success) {
        fprintf(stderr, "cearch: failed writing database %s: %s\n", Database, strerror(errno));
        unlink(Temporary);
    } else {
        fprintf(stderr, "cearch: indexed %zu entries into %s\n", Table.Count, Database);
    }

    free(Temporary);
    FreeTable(&Table);
    return Success;
}

static char *DecodePath(const char *Text) {
    size_t Length = strlen(Text);
    char *Path = malloc(Length + 1U);
    if (!Path) return nullptr;
    size_t Out = 0;
    for (size_t Index = 0; Index < Length; ++Index) {
        if (Text[Index] == '\\' && Index + 1U < Length) {
            ++Index;
            if (Text[Index] == 't') Path[Out++] = '\t';
            else if (Text[Index] == 'n') Path[Out++] = '\n';
            else if (Text[Index] == 'r') Path[Out++] = '\r';
            else if (Text[Index] == '\\') Path[Out++] = '\\';
            else {
                Path[Out++] = '\\';
                Path[Out++] = Text[Index];
            }
        } else {
            Path[Out++] = Text[Index];
        }
    }
    Path[Out] = '\0';
    return Path;
}

static bool LoadDatabase(const char *Database, FileTable *Table) {
    FILE *Stream = fopen(Database, "r");
    if (!Stream) {
        fprintf(stderr, "cearch: cannot open database %s: %s\n", Database, strerror(errno));
        return false;
    }
    char *Line = nullptr;
    size_t LineCapacity = 0;
    ssize_t LineLength = getline(&Line, &LineCapacity, Stream);
    if (LineLength < 0) {
        fprintf(stderr, "cearch: empty database %s\n", Database);
        free(Line);
        fclose(Stream);
        return false;
    }
    unsigned Version = 0;
    char Extra = 0;
    if (sscanf(Line, "CEARCH\t%u%c", &Version, &Extra) < 1 || Version != CearchVersion) {
        fprintf(stderr, "cearch: unsupported database format\n");
        free(Line);
        fclose(Stream);
        return false;
    }

    bool Success = true;
    while ((LineLength = getline(&Line, &LineCapacity, Stream)) >= 0) {
        if ((size_t)LineLength > CearchMaxLine) {
            fprintf(stderr, "cearch: database record exceeds limit\n");
            Success = false;
            break;
        }
        if (LineLength && Line[LineLength - 1] == '\n') Line[--LineLength] = '\0';
        char *Fields[6] = {0};
        char *Cursor = Line;
        for (size_t Field = 0; Field < 6U; ++Field) {
            Fields[Field] = Cursor;
            char *Tab = strchr(Cursor, '\t');
            if (Field < 5U) {
                if (!Tab) {
                    Success = false;
                    break;
                }
                *Tab = '\0';
                Cursor = Tab + 1;
            }
        }
        if (!Success || strcmp(Fields[0], "E") != 0 || !Fields[1][0]) {
            fprintf(stderr, "cearch: malformed database record\n");
            Success = false;
            break;
        }

        char *End = nullptr;
        errno = 0;
        uint64_t Size = strtoull(Fields[2], &End, 10);
        if (errno || !End || *End) { Success = false; break; }
        errno = 0;
        int64_t Mtime = strtoll(Fields[3], &End, 10);
        if (errno || !End || *End) { Success = false; break; }
        errno = 0;
        uint64_t Device = strtoull(Fields[4], &End, 10);
        if (errno || !End || *End) { Success = false; break; }
        errno = 0;
        uint64_t Inode = strtoull(Fields[5], &End, 10);
        if (errno || !End || *End) { Success = false; break; }

        char *Path = DecodePath(Cursor);
        if (!Path || !ReserveEntries(Table, Table->Count + 1U)) {
            free(Path);
            Success = false;
            break;
        }
        Table->Entries[Table->Count++] = (FileEntry){
            .Path = Path, .Size = Size, .Mtime = Mtime, .Device = Device, .Inode = Inode,
            .Type = Fields[1][0]
        };
    }
    if (ferror(Stream)) Success = false;
    free(Line);
    fclose(Stream);
    if (!Success) {
        fprintf(stderr, "cearch: invalid or unreadable database %s\n", Database);
        FreeTable(Table);
    }
    return Success;
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
    size_t Found = 0;
    for (size_t Index = 0; Index < Table.Count; ++Index) {
        const FileEntry *Entry = &Table.Entries[Index];
        const char *Haystack = MatchPath ? Entry->Path : BaseName(Entry->Path);
        if (!ContainsCaseInsensitive(Haystack, Query)) continue;
        puts(Entry->Path);
        if (++Found >= Limit) break;
    }
    FreeTable(&Table);
    return true;
}

static bool IsBinaryBuffer(const unsigned char *Buffer, size_t Length) {
    return memchr(Buffer, '\0', Length) != nullptr;
}

static bool FileContains(const char *Path, const char *Query) {
    int Descriptor = open(Path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
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
        if (!IsBinaryBuffer(Buffer, Total)) {
            if (memmem(Buffer, Total, Query, QueryLength)) {
                Match = true;
                break;
            }
        } else {
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
        if (Entry->Type != 'f') continue;
        if (!FileContains(Entry->Path, Query)) continue;
        /* Index is a candidate list only. Current file content is always read fresh. */
        puts(Entry->Path);
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
        int FirstRoot = -1;
        for (int Index = 2; Index < Argc; ++Index) {
            if (strcmp(Argv[Index], "--db") == 0) {
                if (++Index >= Argc || Database) {
                    PrintUsage(stderr);
                    return 2;
                }
                Database = Argv[Index];
            } else if (FirstRoot < 0) {
                FirstRoot = Index;
            }
        }
        if (!Database || FirstRoot < 0) {
            PrintUsage(stderr);
            return 2;
        }
        char **Roots = calloc((size_t)(Argc - FirstRoot), sizeof(*Roots));
        if (!Roots) return 1;
        int RootCount = 0;
        for (int Index = FirstRoot; Index < Argc; ++Index)
            if (strcmp(Argv[Index], "--db") != 0 && (Index == 0 || strcmp(Argv[Index - 1], "--db") != 0))
                Roots[RootCount++] = Argv[Index];
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
            } else if (!Query) Query = Argv[Index];
            else {
                fprintf(stderr, "cearch: only one QUERY argument is supported; quote it if it contains spaces\n");
                return 2;
            }
        }
        if (!Database || !Query) {
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
            if (strcmp(Argv[Index], "--db") == 0 && Index + 1 < Argc && !Database)
                Database = Argv[++Index];
            else {
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
