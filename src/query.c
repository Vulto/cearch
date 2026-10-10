#include "query.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
                                char *Term) {
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
    return Node;
}

static bool IsKeyword(const char *Text, const char *Keyword) {
    return strcasecmp(Text, Keyword) == 0;
}

static void NextToken(CearchParser *Parser) {
    free(Parser->Text);
    Parser->Text = NULL;
    while (*Parser->Cursor && isspace((unsigned char)*Parser->Cursor)) ++Parser->Cursor;
    if (!*Parser->Cursor) {
        Parser->Type = TokenEnd;
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
    if (IsKeyword(Text, "AND")) Parser->Type = TokenAnd;
    else if (IsKeyword(Text, "OR")) Parser->Type = TokenOr;
    else if (IsKeyword(Text, "NOT")) Parser->Type = TokenNot;
    else Parser->Type = TokenTerm;
}

static CearchQueryNode *ParseOr(CearchParser *Parser);

static CearchQueryNode *ParsePrimary(CearchParser *Parser) {
    if (Parser->Type == TokenTerm) {
        char *Term = Parser->Text;
        Parser->Text = NULL;
        NextToken(Parser);
        return NewNode(CearchQueryTerm, NULL, NULL, Term);
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
    return NewNode(CearchQueryNot, Child, NULL, NULL);
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
        Left = NewNode(CearchQueryAnd, Left, Right, NULL);
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
        Left = NewNode(CearchQueryOr, Left, Right, NULL);
        if (!Left) return NULL;
    }
    return Left;
}

bool CearchQueryParse(const char *Text, CearchQuery *Query) {
    if (!Text || !Query) {
        errno = EINVAL;
        return false;
    }
    *Query = (CearchQuery){0};
    CearchParser Parser = { .Cursor = Text, .Type = TokenEnd, .Text = NULL };
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

static bool MatchNode(const CearchQueryNode *Node, const char *Text) {
    switch (Node->Type) {
        case CearchQueryTerm:
            return ContainsCaseInsensitive(Text, Node->Term);
        case CearchQueryNot:
            return !MatchNode(Node->Left, Text);
        case CearchQueryAnd:
            return MatchNode(Node->Left, Text) && MatchNode(Node->Right, Text);
        case CearchQueryOr:
            return MatchNode(Node->Left, Text) || MatchNode(Node->Right, Text);
    }
    return false;
}

bool CearchQueryMatches(const CearchQuery *Query, const char *Text) {
    return Query && Query->Root && Text && MatchNode(Query->Root, Text);
}
