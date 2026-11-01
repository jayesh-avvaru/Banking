#ifndef BANK_H
#define BANK_H
#include <pthread.h>
#include "config.h"
#include "storage.h"

typedef struct {
    int32_t acc_no;
    char    name[NAME_LEN];
    char    pin_hash[HASH_LEN];
    int64_t balance;               /* paise */
    int     failed_attempts;
    int     locked;
    pthread_mutex_t lock;          /* one mutex per account */
} Account;

int      bank_init(const char *dir, int nosync);
void     bank_shutdown(void);                       /* write checkpoint (no clients may be active) */
Account *bank_find(int acc_no);

/* All return 0 on success or an ERR_* code. */
int bank_register(const char *name, const char *pin, int *acc_no);
int bank_login(int acc_no, const char *pin, Account **out);
int bank_balance(Account *a, int64_t *bal);
int bank_deposit(Account *a, int64_t amt, int64_t *bal);
int bank_withdraw(Account *a, int64_t amt, int64_t *bal);
int bank_transfer(Account *from, int to_no, int64_t amt, int64_t *bal);
int bank_history(Account *a, int n, LogRec *out);   /* returns count (>=0) or -1 */
#endif
