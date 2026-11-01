#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>
#include "protocol.h"

#define HASH_LEN            96
#define MAX_ACCOUNTS        10000
#define BASE_ACC_NO         1001
#define MAX_CLIENTS         200
#define MAX_PIN_ATTEMPTS    3
#define IDLE_TIMEOUT_SEC    300
#define MAX_HISTORY         50

/* All money is stored as integer paise (1/100) - never floating point. */
#define MIN_BALANCE_PAISE   0LL
#define MAX_TXN_PAISE       1000000000LL        /* 10,000,000.00 per transaction */
#define MAX_BALANCE_PAISE   100000000000LL      /* 1,000,000,000.00 per account  */

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
