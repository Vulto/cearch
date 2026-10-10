#include "query.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define CearchQueryMaximumLength 4096U
#define CearchQueryMaximumTokens 128U

typedef enum {
    TokenEnd,
    TokenTerm,
    TokenAnd,
    TokenOr,
    TokenNot,
    TokenLeftParen,
    TokenRightParen,
    TokenInvalid
} CearchTokenType;

typedef struct {
    const char *Cursor;
    CearchTokenType Type;
    char *Text;
    size_t TokenCount;
    CearchQueryMatchMode Mode;
} CearchParser;

static void FreeNode(CearchQueryNode *Node) {
    if (!Node) return;
    FreeNode(Node->Left);
    FreeNode(Node->Right);
    free(Node->Term);
    free(Node);
}

void CearchQueryFree(CearchQuery *Query) {
    if (!Query) return;
    FreeNode(Query->Root);
    Query->Root = NULL;
}

static CearchQueryNode *NewNode(CearchQueryNodeType Type,
                                CearchQueryNode *Left,
                                CearchQueryNode *Right,
                                char *Term,
                                CearchQueryMatchMode Mode) {
    CearchQueryNode *Node = calloc(1U, sizeof(*Node));
    if (!Node) {
        FreeNode(Left);
        FreeNode(Right);
        free(Term);
        return NULL;
    }
    Node->Type = Type;
    Node->Left = Left;
    Node->Right = Right;
    Node->Term = Term;
    Node->Mode = Mode;
    return Node;
}

static bool IsKeyword(const char *Text, const char *Keyword) {
    return strcasecmp(Text, Keyword) == 0;
}

static void NextToken(CearchParser *Parser) {
    free(Parser->Text);
    Parser->Text = NULL;
    Parser->Mode = CearchQuerySubstring;
    while (*Parser->Cursor && isspace((unsigned char)*Parser->Cursor)) ++Parser->Cursor;
    if (!*Parser->Cursor) {
        Parser->Type = TokenEnd;
        return;
    }
    if (++Parser->TokenCount > CearchQueryMaximumTokens) {
        Parser->Type = TokenInvalid;
        return;
    }
    if (*Parser->Cursor == '(') {
        ++Parser->Cursor;
        Parser->Type = TokenLeftParen;
        return;
    }
    if (*Parser->Cursor == ')') {
        ++Parser->Cursor;
        Parser->Type = TokenRightParen;
        return;
    }

    if (strncasecmp(Parser->Cursor, "exact:", 6U) == 0) {
        Parser->Mode = CearchQueryExact;
        Parser->Cursor += 6U;
    } else if (strncasecmp(Parser->Cursor, "prefix:", 7U) == 0) {
        Parser->Mode = CearchQueryPrefix;
        Parser->Cursor += 7U;
    } else if (strncasecmp(Parser->Cursor, "suffix:", 7U) == 0) {
        Parser->Mode = CearchQuerySuffix;
        Parser->Cursor += 7U;
    }
    if (!*Parser->Cursor || isspace((unsigned char)*Parser->Cursor) ||
        *Parser->Cursor == '(' || *Parser->Cursor == ')') {
        Parser->Type = TokenInvalid;
        return;
    }
    bool Quoted = *Parser->Cursor == '"';
    const char *Start = Parser->Cursor + (Quoted ? 1 : 0);
    size_t Capacity = strlen(Start) + 1U;
    char *Text = malloc(Capacity);
    if (!Text) {
        Parser->Type = TokenInvalid;
        return;
    }
    size_t Length = 0U;
    if (Quoted) {
        ++Parser->Cursor;
        while (*Parser->Cursor && *Parser->Cursor != '"') {
            if (*Parser->Cursor == '\\' &&
                (Parser->Cursor[1] == '"' || Parser->Cursor[1] == '\\')) ++Parser->Cursor;
            Text[Length++] = *Parser->Cursor++;
        }
        if (*Parser->Cursor != '"' || Length == 0U) {
            free(Text);
            Parser->Type = TokenInvalid;
            return;
        }
        ++Parser->Cursor;
        Text[Length] = '\0';
        Parser->Text = Text;
        Parser->Type = TokenTerm;
        return;
    }

    while (*Parser->Cursor && !isspace((unsigned char)*Parser->Cursor) &&
           *Parser->Cursor != '(' && *Parser->Cursor != ')') {
        Text[Length++] = *Parser->Cursor++;
    }
    Text[Length] = '\0';
    Parser->Text = Text;
    if (Parser->Mode == CearchQuerySubstring && IsKeyword(Text, "AND")) Parser->Type = TokenAnd;
    else if (Parser->Mode == CearchQuerySubstring && IsKeyword(Text, "OR")) Parser->Type = TokenOr;
    else if (Parser->Mode == CearchQuerySubstring && IsKeyword(Text, "NOT")) Parser->Type = TokenNot;
    else Parser->Type = TokenTerm;
}

static bool CompareValue(uint64_t Left, CearchQueryComparison Comparison, uint64_t Right) {
    switch (Comparison) {
        case CearchQueryCompareEqual: return Left == Right;
        case CearchQueryCompareGreater: return Left > Right;
        case CearchQueryCompareLess: return Left < Right;
        case CearchQueryCompareGreaterEqual: return Left >= Right;
        case CearchQueryCompareLessEqual: return Left <= Right;
    }
    return false;
}

static bool ParseValueAndUnit(const char *Text, bool Duration,
                              CearchQueryComparison *Comparison, uint64_t *Value) {
    const char *Cursor = Text;
    if (Cursor[0] == '>' && Cursor[1] == '=') {
        *Comparison = CearchQueryCompareGreaterEqual;
        Cursor += 2;
    } else if (Cursor[0] == '<' && Cursor[1] == '=') {
        *Comparison = CearchQueryCompareLessEqual;
        Cursor += 2;
    } else if (*Cursor == '>') {
        *Comparison = CearchQueryCompareGreater;
        ++Cursor;
    } else if (*Cursor == '<') {
        *Comparison = CearchQueryCompareLess;
        ++Cursor;
    } else if (*Cursor == '=') {
        *Comparison = CearchQueryCompareEqual;
        ++Cursor;
    } else {
        return false;
    }
    if (*Cursor < '0' || *Cursor > '9') return false;
    uint64_t Number = 0U;
    while (*Cursor >= '0' && *Cursor <= '9') {
        uint64_t Digit = (uint64_t)(*Cursor - '0');
        if (Number > (UINT64_MAX - Digit) / 10U) return false;
        Number = Number * 10U + Digit;
        ++Cursor;
    }
    uint64_t Multiplier = 1U;
    if (!*Cursor) {
        Multiplier = 1U;
    } else if (!Duration && strcasecmp(Cursor, "b") == 0) Multiplier = 1U;
    else if (!Duration && strcasecmp(Cursor, "kb") == 0) Multiplier = 1024ULL;
    else if (!Duration && strcasecmp(Cursor, "mb") == 0) Multiplier = 1024ULL * 1024ULL;
    else if (!Duration && strcasecmp(Cursor, "gb") == 0) Multiplier = 1024ULL * 1024ULL * 1024ULL;
    else if (!Duration && strcasecmp(Cursor, "tb") == 0) Multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    else if (Duration && strcasecmp(Cursor, "s") == 0) Multiplier = 1U;
    else if (Duration && strcasecmp(Cursor, "m") == 0) Multiplier = 60U;
    else if (Duration && strcasecmp(Cursor, "h") == 0) Multiplier = 3600U;
    else if (Duration && strcasecmp(Cursor, "d") == 0) Multiplier = 86400U;
    else if (Duration && strcasecmp(Cursor, "w") == 0) Multiplier = 604800U;
    else return false;
    if (Number > UINT64_MAX / Multiplier) return false;
    *Value = Number * Multiplier;
    return true;
}

static CearchQueryNode *BuildTermNode(char *Term, CearchQueryMatchMode Mode) {
    if (Mode != CearchQuerySubstring) return NewNode(CearchQueryTerm, NULL, NULL, Term, Mode);
    CearchQueryNodeType Type = CearchQueryTerm;
    if (strncasecmp(Term, "ext:", 4U) == 0) {
        Type = CearchQueryFilterExtension;
        memmove(Term, Term + 4U, strlen(Term + 4U) + 1U);
        if (*Term == '.') memmove(Term, Term + 1U, strlen(Term));
        if (!*Term) { free(Term); errno = EINVAL; return NULL; }
    } else if (strncasecmp(Term, "type:", 5U) == 0) {
        Type = CearchQueryFilterType;
        memmove(Term, Term + 5U, strlen(Term + 5U) + 1U);
        if (strcasecmp(Term, "file") != 0 && strcasecmp(Term, "dir") != 0 &&
            strcasecmp(Term, "other") != 0) { free(Term); errno = EINVAL; return NULL; }
    } else if (strncasecmp(Term, "path:", 5U) == 0) {
        Type = CearchQueryFilterPath;
        memmove(Term, Term + 5U, strlen(Term + 5U) + 1U);
        if (!*Term) { free(Term); errno = EINVAL; return NULL; }
    } else if (strncasecmp(Term, "in:", 3U) == 0) {
        Type = CearchQueryFilterIn;
        memmove(Term, Term + 3U, strlen(Term + 3U) + 1U);
        if (!*Term) { free(Term); errno = EINVAL; return NULL; }
    } else if (strncasecmp(Term, "size:", 5U) == 0 ||
               strncasecmp(Term, "mtime:", 6U) == 0) {
        bool IsMtime = strncasecmp(Term, "mtime:", 6U) == 0;
        const char *ValueText = Term + (IsMtime ? 6U : 5U);
        CearchQueryComparison Comparison;
        uint64_t Value;
        if (!ParseValueAndUnit(ValueText, IsMtime, &Comparison, &Value)) {
            free(Term);
            errno = EINVAL;
            return NULL;
        }
        free(Term);
        CearchQueryNode *Node = NewNode(IsMtime ? CearchQueryFilterMtime : CearchQueryFilterSize,
                                        NULL, NULL, NULL, CearchQuerySubstring);
        if (Node) {
            Node->Comparison = Comparison;
            Node->Value = Value;
        }
        return Node;
    }
    CearchQueryNode *Node = NewNode(Type, NULL, NULL, Term, CearchQuerySubstring);
    return Node;
}

static CearchQueryNode *ParseOr(CearchParser *Parser);

static CearchQueryNode *ParsePrimary(CearchParser *Parser) {
    if (Parser->Type == TokenTerm) {
        char *Term = Parser->Text;
        CearchQueryMatchMode Mode = Parser->Mode;
        Parser->Text = NULL;
        NextToken(Parser);
        return NewNode(CearchQueryTerm, NULL, NULL, Term, Mode);
    }
    if (Parser->Type == TokenLeftParen) {
        NextToken(Parser);
        CearchQueryNode *Inside = ParseOr(Parser);
        if (!Inside || Parser->Type != TokenRightParen) {
            FreeNode(Inside);
            Parser->Type = TokenInvalid;
            return NULL;
        }
        NextToken(Parser);
        return Inside;
    }
    Parser->Type = TokenInvalid;
    return NULL;
}

static CearchQueryNode *ParseUnary(CearchParser *Parser) {
    if (Parser->Type != TokenNot) return ParsePrimary(Parser);
    NextToken(Parser);
    CearchQueryNode *Child = ParseUnary(Parser);
    if (!Child) return NULL;
    return NewNode(CearchQueryNot, Child, NULL, NULL, CearchQuerySubstring);
}

static CearchQueryNode *ParseAnd(CearchParser *Parser) {
    CearchQueryNode *Left = ParseUnary(Parser);
    if (!Left) return NULL;
    while (Parser->Type == TokenAnd) {
        NextToken(Parser);
        CearchQueryNode *Right = ParseUnary(Parser);
        if (!Right) {
            FreeNode(Left);
            return NULL;
        }
        Left = NewNode(CearchQueryAnd, Left, Right, NULL, CearchQuerySubstring);
        if (!Left) return NULL;
    }
    return Left;
}

static CearchQueryNode *ParseOr(CearchParser *Parser) {
    CearchQueryNode *Left = ParseAnd(Parser);
    if (!Left) return NULL;
    while (Parser->Type == TokenOr) {
        NextToken(Parser);
        CearchQueryNode *Right = ParseAnd(Parser);
        if (!Right) {
            FreeNode(Left);
            return NULL;
        }
        Left = NewNode(CearchQueryOr, Left, Right, NULL, CearchQuerySubstring);
        if (!Left) return NULL;
    }
    return Left;
}

bool CearchQueryParse(const char *Text, CearchQuery *Query) {
    if (!Text || !Query) {
        errno = EINVAL;
        return false;
    }
    if (strlen(Text) > CearchQueryMaximumLength) {
        errno = E2BIG;
        return false;
    }
    CearchQueryFree(Query);
    CearchParser Parser = { .Cursor = Text, .Type = TokenEnd, .Text = NULL, .TokenCount = 0U };
    NextToken(&Parser);
    CearchQueryNode *Root = ParseOr(&Parser);
    bool Success = Root != NULL && Parser.Type == TokenEnd;
    free(Parser.Text);
    if (!Success) {
        FreeNode(Root);
        errno = EINVAL;
        return false;
    }
    Query->Root = Root;
    return true;
}

const char *CearchQuerySimpleTerm(const CearchQuery *Query) {
    if (!Query || !Query->Root || Query->Root->Type != CearchQueryTerm) return NULL;
    return Query->Root->Term;
}

static bool ContainsCaseInsensitive(const char *Haystack, const char *Needle) {
    if (!Haystack || !Needle) return false;
    size_t Length = strlen(Needle);
    if (Length == 0U) return true;
    for (const char *Start = Haystack; *Start; ++Start)
        if (strncasecmp(Start, Needle, Length) == 0) return true;
    return false;
}

static bool MatchTerm(const CearchQueryNode *Node, const char *Text) {
    size_t TextLength = strlen(Text);
    size_t TermLength = strlen(Node->Term);
    switch (Node->Mode) {
        case CearchQuerySubstring:
            return ContainsCaseInsensitive(Text, Node->Term);
        case CearchQueryExact:
            return strcasecmp(Text, Node->Term) == 0;
        case CearchQueryPrefix:
            return TextLength >= TermLength &&
                   strncasecmp(Text, Node->Term, TermLength) == 0;
        case CearchQuerySuffix:
            return TextLength >= TermLength &&
                   strncasecmp(Text + TextLength - TermLength, Node->Term, TermLength) == 0;
    }
    return false;
}

static const char *BaseName(const char *Path) {
    const char *Slash = strrchr(Path, '/');
    return Slash ? Slash + 1 : Path;
}

static bool MatchFilter(const CearchQueryNode *Node, const CearchFileEntry *Entry) {
    if (!Entry) return false;
    switch (Node->Type) {
        case CearchQueryFilterExtension: {
            const char *Base = BaseName(Entry->Path);
            const char *Dot = strrchr(Base, '.');
            return Dot && Dot != Base && strcasecmp(Dot + 1, Node->Term) == 0;
        }
        case CearchQueryFilterType:
            if (strcasecmp(Node->Term, "file") == 0) return Entry->Type == 'f';
            if (strcasecmp(Node->Term, "dir") == 0) return Entry->Type == 'd';
            return Entry->Type == 'o';
        case CearchQueryFilterSize:
            return CompareValue(Entry->Size, Node->Comparison, Node->Value);
        case CearchQueryFilterMtime: {
            time_t Current = time(NULL);
            if (Current < 0 || Entry->Mtime < 0) return false;
            uint64_t Age = (int64_t)Current > Entry->Mtime
                ? (uint64_t)((int64_t)Current - Entry->Mtime) : 0U;
            return CompareValue(Age, Node->Comparison, Node->Value);
        }
        case CearchQueryFilterPath:
            return ContainsCaseInsensitive(Entry->Path, Node->Term);
        case CearchQueryFilterIn: {
            size_t RootLength = strlen(Node->Term);
            while (RootLength > 1U && Node->Term[RootLength - 1U] == '/') --RootLength;
            if (RootLength == 1U && Node->Term[0] == '/') return Entry->Path[0] == '/';
            if (strncmp(Entry->Path, Node->Term, RootLength) != 0) return false;
            return Entry->Path[RootLength] == '\0' || Entry->Path[RootLength] == '/';
        }
        default:
            return false;
    }
}

static bool MatchNode(const CearchQueryNode *Node, const CearchFileEntry *Entry, const char *Text) {
    switch (Node->Type) {
        case CearchQueryTerm:
            return MatchTerm(Node, Text);
        case CearchQueryFilterExtension:
        case CearchQueryFilterType:
        case CearchQueryFilterSize:
        case CearchQueryFilterMtime:
        case CearchQueryFilterPath:
        case CearchQueryFilterIn:
            return MatchFilter(Node, Entry);
        case CearchQueryNot:
            return !MatchNode(Node->Left, Entry, Text);
        case CearchQueryAnd:
            return MatchNode(Node->Left, Entry, Text) && MatchNode(Node->Right, Entry, Text);
        case CearchQueryOr:
            return MatchNode(Node->Left, Entry, Text) || MatchNode(Node->Right, Entry, Text);
    }
    return false;
}

bool CearchQueryMatches(const CearchQuery *Query, const char *Text) {
    return Query && Query->Root && Text && MatchNode(Query->Root, NULL, Text);
}

bool CearchQueryMatchesEntry(const CearchQuery *Query,
                             const CearchFileEntry *Entry, const char *Text) {
    return Query && Query->Root && Entry && Text && MatchNode(Query->Root, Entry, Text);
}
