#include "spermfs.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

uint64_t spermfs_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void spermfs_uuid_generate(uint8_t uuid[16])
{
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)(spermfs_time_ns() & 0xFFFFFFFF));
        seeded = 1;
    }

    for (int i = 0; i < 16; i++)
        uuid[i] = (uint8_t)(rand() & 0xFF);

    /* Set version 4 (random) */
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    /* Set variant (RFC 4122) */
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}
