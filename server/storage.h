#ifndef STORAGE_H
#define STORAGE_H
#include "config.h"

enum { TX_OPEN = 1, TX_DEPOSIT, TX_WITHDRAW, TX_TRANSFER };

/* One ledger entry. txn.log is an append-only array of these (the write-ahead log).
 * Balances are stored as absolute "after" values so replay is idempotent. */
typedef struct {
    int64_t txn_id;
    int64_t ts;
    int32_t type;
    int32_t from_acc, to_acc;      /* 0 = not applicable */
    int64_t amount;
    int64_t bal_from, bal_to;
    char    name[NAME_LEN];        /* TX_OPEN only */
    char    pin_hash[HASH_LEN];    /* TX_OPEN only */
} LogRec;

/* accounts.dat = SnapHeader + count * DiskAccount (periodic checkpoint) */
typedef struct {
    char    magic[8];
    int32_t count, reserved;
    int64_t last_txn_id;
} SnapHeader;

typedef struct {
    int32_t acc_no;
    char    name[NAME_LEN];
    char    pin_hash[HASH_LEN];
    int64_t balance;
} DiskAccount;

int     storage_set_dir(const char *dir);
void    storage_set_sync(int on);
int     storage_snapshot_read(SnapHeader *h, DiskAccount *out, int max);
int     storage_snapshot_write(const DiskAccount *accs, int n);
/* Opens txn.log, replays records newer than snap_last_id via apply(), drops a torn tail. */
int     storage_open_log(int64_t snap_last_id, void (*apply)(const LogRec *), long *replayed);
/* Assigns txn_id/ts, appends and (optionally) fdatasyncs. 0 on success. Thread-safe. */
int     storage_append(LogRec *r);
int     storage_history(int acc_no, int max, LogRec *out);
int64_t storage_last_id(void);
void    storage_close(void);
#endif
