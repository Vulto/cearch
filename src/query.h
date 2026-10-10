#ifndef CEARCH_QUERY_H
#define CEARCH_QUERY_H

#include "index.h"

#include <stdbool.h>
#include <stddef.h>
#include <regex.h>
#include <stdint.h>

typedef enum {
    CearchQuerySubstring,
    CearchQueryExact,
    CearchQueryPrefix,
    CearchQuerySuffix
} CearchQueryMatchMode;

typedef enum {
    CearchQueryTerm,
    CearchQueryFilterExtension,
    CearchQueryFilterType,
    CearchQueryFilterSize,
    CearchQueryFilterMtime,
    CearchQueryFilterPath,
    CearchQueryFilterIn,
    CearchQueryRegex,
    CearchQueryFuzzy,
    CearchQueryNot,
    CearchQueryAnd,
    CearchQueryOr
} CearchQueryNodeType;

typedef enum {
    CearchQueryCompareEqual,
    CearchQueryCompareGreater,
    CearchQueryCompareLess,
    CearchQueryCompareGreaterEqual,
    CearchQueryCompareLessEqual
} CearchQueryComparison;

typedef struct CearchQueryNode {
    CearchQueryNodeType Type;
    char *Term;
    CearchQueryMatchMode Mode;
    CearchQueryComparison Comparison;
    uint64_t Value;
    regex_t Regex;
    bool RegexCompiled;
    struct CearchQueryNode *Left;
    struct CearchQueryNode *Right;
} CearchQueryNode;

typedef struct {
    CearchQueryNode *Root;
} CearchQuery;

typedef struct {
    const char *Path;
    uint32_t Score;
} CearchQueryHit;

/* Grammar: terms, quoted phrases, exact:/prefix:/suffix:, NOT, AND, OR, and parentheses. Query must be zero-initialized or previously parsed. */
bool CearchQueryParse(const char *Text, CearchQuery *Query);
const char *CearchQuerySimpleTerm(const CearchQuery *Query);
bool CearchQueryMatches(const CearchQuery *Query, const char *Text);
bool CearchQueryMatchesEntry(const CearchQuery *Query,
                             const CearchFileEntry *Entry, const char *Text);
bool CearchQueryScoreEntry(const CearchQuery *Query, const CearchFileEntry *Entry,
                           const char *Text, uint32_t *Score);
bool CearchQueryHasFuzzy(const CearchQuery *Query);
int CearchQueryHitCompare(const void *Left, const void *Right);
void CearchQueryHitConsider(CearchQueryHit *Hits, size_t *Count, size_t Capacity,
                            const char *Path, uint32_t Score);
void CearchQueryFree(CearchQuery *Query);

#endif
