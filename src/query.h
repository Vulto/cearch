#ifndef CEARCH_QUERY_H
#define CEARCH_QUERY_H

#include <stdbool.h>

typedef enum {
    CearchQueryTerm,
    CearchQueryNot,
    CearchQueryAnd,
    CearchQueryOr
} CearchQueryNodeType;

typedef struct CearchQueryNode {
    CearchQueryNodeType Type;
    char *Term;
    struct CearchQueryNode *Left;
    struct CearchQueryNode *Right;
} CearchQueryNode;

typedef struct {
    CearchQueryNode *Root;
} CearchQuery;

/* Grammar: terms, quoted phrases, NOT, AND, OR, and parentheses. Query must be zero-initialized or previously parsed. */
bool CearchQueryParse(const char *Text, CearchQuery *Query);
const char *CearchQuerySimpleTerm(const CearchQuery *Query);
bool CearchQueryMatches(const CearchQuery *Query, const char *Text);
void CearchQueryFree(CearchQuery *Query);

#endif
