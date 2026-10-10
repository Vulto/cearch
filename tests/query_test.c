#include "../src/query.h"

#include <assert.h>
#include <stdio.h>

static bool Matches(const char *QueryText, const char *Text) {
    CearchQuery Query = {0};
    assert(CearchQueryParse(QueryText, &Query));
    bool Result = CearchQueryMatches(&Query, Text);
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
    CearchQueryFree(&Query);
    puts("cearch query unit tests passed");
    return 0;
}
