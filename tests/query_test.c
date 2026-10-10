#include "../src/query.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool Matches(const char *QueryText, const char *Text) {
    CearchQuery Query = {0};
    assert(CearchQueryParse(QueryText, &Query));
    bool Result = CearchQueryMatches(&Query, Text);
    CearchQueryFree(&Query);
    return Result;
}

static bool MatchesEntry(const char *QueryText, const CearchFileEntry *Entry, const char *Text) {
    CearchQuery Query = {0};
    assert(CearchQueryParse(QueryText, &Query));
    bool Result = CearchQueryMatchesEntry(&Query, Entry, Text);
    CearchQueryFree(&Query);
    return Result;
}

static void Rejects(const char *Text) {
    CearchQuery Query = {0};
    assert(!CearchQueryParse(Text, &Query));
    CearchQueryFree(&Query);
}

int main(void) {
    CearchQuery Query = {0};
    assert(CearchQueryParse("alpha", &Query));
    assert(CearchQuerySimpleTerm(&Query));
    assert(Matches("alpha AND beta", "alpha-beta.txt"));
    assert(!Matches("alpha AND beta", "alpha.txt"));
    assert(Matches("alpha OR beta", "BETA.txt"));
    assert(Matches("NOT alpha", "beta.txt"));
    assert(!Matches("NOT alpha", "alpha.txt"));
    assert(Matches("alpha OR beta AND gamma", "alpha.txt"));
    assert(!Matches("alpha OR beta AND gamma", "beta.txt"));
    assert(Matches("alpha OR beta AND gamma", "beta-gamma.txt"));
    assert(Matches("(alpha OR beta) AND gamma", "beta-gamma.txt"));
    assert(!Matches("(alpha OR beta) AND gamma", "alpha.txt"));
    assert(Matches("\"two words\" AND file", "TWO WORDS-file.txt"));
    assert(Matches("NOT (alpha OR beta)", "gamma.txt"));
    assert(Matches("exact:alpha.txt", "ALPHA.txt"));
    assert(!Matches("exact:alpha.txt", "alpha-beta.txt"));
    assert(Matches("prefix:alpha", "alpha-beta.txt"));
    assert(!Matches("prefix:alpha", "beta-alpha.txt"));
    assert(Matches("suffix:.txt", "alpha.txt"));
    assert(Matches("regex:^alpha.*[.]txt$", "alpha-report.txt"));
    assert(!Matches("regex:^alpha.*[.]txt$", "xalpha-report.txt"));
    assert(Matches("regex:\"two words\"", "TWO WORDS-file.txt"));
    assert(Matches("fuzzy:report", "report.txt"));
    assert(Matches("fuzzy:report", "reprot.txt"));
    assert(!Matches("fuzzy:report", "banana.txt"));
    assert(!Matches("fuzzy:ab", "ac"));
    assert(Matches("fuzzy:report AND ext:txt", "report.txt") == false);
    assert(!Matches("suffix:.txt", "alpha.md"));
    assert(Matches("prefix:\"two words\"", "two words-file.txt"));
    assert(!Matches("prefix:\"two words\"", "prefix-two words-file.txt"));
    assert(!CearchQueryParse("alpha AND", &Query));
    Rejects("(alpha OR beta");
    Rejects("alpha XOR beta");
    Rejects("NOT");
    Rejects("\"\"");
    Rejects("prefix:");
    Rejects("exact:(alpha)");
    Rejects("ext:");
    Rejects("type:pipe");
    Rejects("size:>999999999999999999999999gb");
    Rejects("mtime:<7x");
    Rejects("path:");
    Rejects("in:");
    Rejects("regex:");
    Rejects("regex:[");
    Rejects("fuzzy:");
    Rejects("fuzzy:two words");
    Rejects("fuzzy:report!");

    CearchFileEntry Entry = {
        .Path = "/home/me/Documents/example.txt",
        .Size = 11U * 1024U * 1024U,
        .Mtime = (int64_t)time(NULL) - 3 * 86400,
        .Type = 'f'
    };
    assert(MatchesEntry("ext:txt", &Entry, "example.txt"));
    assert(!MatchesEntry("ext:md", &Entry, "example.txt"));
    CearchFileEntry DirectoryEntry = Entry;
    DirectoryEntry.Type = 'd';
    assert(!MatchesEntry("ext:txt", &DirectoryEntry, "example.txt"));
    assert(MatchesEntry("type:file", &Entry, "example.txt"));
    assert(!MatchesEntry("type:dir", &Entry, "example.txt"));
    assert(MatchesEntry("size:>10mb", &Entry, "example.txt"));
    assert(MatchesEntry("size:>=11mb", &Entry, "example.txt"));
    assert(!MatchesEntry("size:<10mb", &Entry, "example.txt"));
    assert(MatchesEntry("mtime:<7d", &Entry, "example.txt"));
    assert(!MatchesEntry("mtime:>7d", &Entry, "example.txt"));
    assert(MatchesEntry("path:/Documents/example", &Entry, "example.txt"));
    assert(!MatchesEntry("path:/missing", &Entry, "example.txt"));
    assert(MatchesEntry("in:/home/me", &Entry, "example.txt"));
    assert(!MatchesEntry("in:/home/m", &Entry, "example.txt"));
    assert(MatchesEntry("ext:txt AND size:>10mb", &Entry, "example.txt"));
    Entry.Type = 'd';
    assert(MatchesEntry("type:dir", &Entry, "example.txt"));
    assert(MatchesEntry("in:/", &Entry, "example.txt"));
    char TooLong[4098];
    memset(TooLong, 'a', sizeof(TooLong) - 1U);
    TooLong[sizeof(TooLong) - 1U] = '\0';
    Rejects(TooLong);
    char ManyTokens[512];
    size_t Offset = 0U;
    for (size_t Index = 0U; Index < 70U; ++Index) {
        if (Index != 0U) {
            memcpy(ManyTokens + Offset, " OR ", 4U);
            Offset += 4U;
        }
        ManyTokens[Offset++] = 'a';
    }
    ManyTokens[Offset] = '\0';
    Rejects(ManyTokens);
    CearchQueryFree(&Query);
    puts("cearch query unit tests passed");
    return 0;
}
