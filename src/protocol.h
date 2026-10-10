#ifndef CEARCH_PROTOCOL_H
#define CEARCH_PROTOCOL_H

#include <stdint.h>

#define CearchProtocolMagic 0x43454152U
#define CearchProtocolVersion 1U
#define CearchSocketName "cearch.sock"
#define CearchMaximumQueryLength 1048576U
#define CearchMaximumResults 100000U

enum CearchCommand {
    CearchCommandSearch = 1,
    CearchCommandContent = 2,
    CearchCommandStats = 3,
    CearchCommandPing = 4,
    CearchCommandShutdown = 5
};

typedef struct {
    uint32_t Magic;
    uint16_t Version;
    uint16_t Command;
    uint32_t Flags;
    uint32_t QueryLength;
    uint32_t Limit;
} CearchRequest;

typedef struct {
    uint32_t Magic;
    uint16_t Version;
    uint16_t Status;
    uint32_t Count;
    uint32_t MessageLength;
} CearchResponse;

#endif
