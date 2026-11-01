#include "bank.h"
#include "auth.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Account         accounts[MAX_ACCOUNTS];
static int             n_accounts = 0;
static pthread_mutex_t reg_mutex = PTHREAD_MUTEX_INITIALIZER;   /* guards n_accounts */

static void init_account(Account *a, int32_t no, const char *name, const char *hash, int64_t bal)
{
    a->acc_no = no;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->pin_hash, sizeof a->pin_hash, "%s", hash);
    a->balance = bal;
    a->failed_attempts = 0;
    a->locked = 0;
}

static void set_balance(int acc_no, int64_t bal)
{
    int idx = acc_no - BASE_ACC_NO;
    if (idx >= 0 && idx < n_accounts) accounts[idx].balance = bal;
}

/* Crash recovery: apply one ledger record (absolute balances => idempotent). */
static void apply_rec(const LogRec *r)
{
    switch (r->type) {
    case TX_OPEN: {
        int idx = r->from_acc - BASE_ACC_NO;
        if (idx == n_accounts && idx < MAX_ACCOUNTS) {
            init_account(&accounts[idx], r->from_acc, r->name, r->pin_hash, 0);
            n_accounts++;
        }
        break;
    }
    case TX_DEPOSIT:  set_balance(r->to_acc, r->bal_to);     break;
    case TX_WITHDRAW: set_balance(r->from_acc, r->bal_from); break;
    case TX_TRANSFER:
        set_balance(r->from_acc, r->bal_from);
        set_balance(r->to_acc, r->bal_to);
        break;
    }
}

static int write_checkpoint(void)
{
    DiskAccount *d = calloc((size_t)(n_accounts ? n_accounts : 1), sizeof *d);
    if (!d) return -1;
    for (int i = 0; i < n_accounts; i++) {
        d[i].acc_no = accounts[i].acc_no;
        snprintf(d[i].name, sizeof d[i].name, "%s", accounts[i].name);
        snprintf(d[i].pin_hash, sizeof d[i].pin_hash, "%s", accounts[i].pin_hash);
        d[i].balance = accounts[i].balance;
    }
    int rc = storage_snapshot_write(d, n_accounts);
    free(d);
    return rc;
}

int bank_init(const char *dir, int nosync)
{
    for (int i = 0; i < MAX_ACCOUNTS; i++) pthread_mutex_init(&accounts[i].lock, NULL);
    if (storage_set_dir(dir) != 0) return -1;
    storage_set_sync(!nosync);

    DiskAccount *d = calloc(MAX_ACCOUNTS, sizeof *d);
    if (!d) return -1;
    SnapHeader h;
    int n = storage_snapshot_read(&h, d, MAX_ACCOUNTS);
    if (n < 0) { log_msg("accounts.dat is corrupt"); free(d); return -1; }
    for (int i = 0; i < n; i++)
        init_account(&accounts[i], d[i].acc_no, d[i].name, d[i].pin_hash, d[i].balance);
    n_accounts = n;
    free(d);

    long replayed = 0;
    if (storage_open_log(h.last_txn_id, apply_rec, &replayed) != 0) return -1;
    log_msg("Loaded %d accounts from checkpoint, replayed %ld log records", n, replayed);

    if (replayed > 0 && write_checkpoint() != 0) log_msg("warning: could not write checkpoint");
    return 0;
}

void bank_shutdown(void)
{
    if (write_checkpoint() == 0) log_msg("Checkpoint written (%d accounts)", n_accounts);
    else log_msg("warning: checkpoint failed (ledger is still intact)");
    storage_close();
}

Account *bank_find(int acc_no)
{
    Account *a = NULL;
    pthread_mutex_lock(&reg_mutex);
    int idx = acc_no - BASE_ACC_NO;
    if (idx >= 0 && idx < n_accounts) a = &accounts[idx];
    pthread_mutex_unlock(&reg_mutex);
    return a;
}

int bank_register(const char *name, const char *pin, int *acc_no)
{
    char hash[HASH_LEN];
    auth_hash_pin(pin, hash);                    /* slow work done before taking any lock */

    pthread_mutex_lock(&reg_mutex);
    if (n_accounts >= MAX_ACCOUNTS) {
        pthread_mutex_unlock(&reg_mutex);
        return ERR_SERVER_FULL;
    }
    int no = BASE_ACC_NO + n_accounts;

    LogRec r;
    memset(&r, 0, sizeof r);
    r.type = TX_OPEN;
    r.from_acc = no;
    snprintf(r.name, sizeof r.name, "%s", name);
    snprintf(r.pin_hash, sizeof r.pin_hash, "%s", hash);
    if (storage_append(&r) != 0) {               /* log first, then change memory */
        pthread_mutex_unlock(&reg_mutex);
        return ERR_INTERNAL;
    }
    init_account(&accounts[n_accounts], no, name, hash, 0);
    n_accounts++;
    pthread_mutex_unlock(&reg_mutex);

    *acc_no = no;
    return 0;
}

int bank_login(int acc_no, const char *pin, Account **out)
{
    Account *a = bank_find(acc_no);
    if (!a) return ERR_AUTH;                     /* do not reveal whether the account exists */

    pthread_mutex_lock(&a->lock);
    if (a->locked) {
        pthread_mutex_unlock(&a->lock);
        return ERR_LOCKED;
    }
    int ok = auth_verify_pin(pin, a->pin_hash);
    int rc = 0;
    if (ok) {
        a->failed_attempts = 0;
        *out = a;
    } else {
        if (++a->failed_attempts >= MAX_PIN_ATTEMPTS) {
            a->locked = 1;
            log_msg("Account %d locked after %d failed PIN attempts", acc_no, a->failed_attempts);
            rc = ERR_LOCKED;
        } else {
            rc = ERR_AUTH;
        }
    }
    pthread_mutex_unlock(&a->lock);
    return rc;
}

int bank_balance(Account *a, int64_t *bal)
{
    pthread_mutex_lock(&a->lock);
    *bal = a->balance;
    pthread_mutex_unlock(&a->lock);
    return 0;
}

int bank_deposit(Account *a, int64_t amt, int64_t *bal)
{
    if (amt <= 0 || amt > MAX_TXN_PAISE) return ERR_AMOUNT;
    pthread_mutex_lock(&a->lock);
    if (a->balance > MAX_BALANCE_PAISE - amt) {
        pthread_mutex_unlock(&a->lock);
        return ERR_AMOUNT;
    }
    LogRec r;
    memset(&r, 0, sizeof r);
    r.type = TX_DEPOSIT;
    r.to_acc = a->acc_no;
    r.amount = amt;
    r.bal_to = a->balance + amt;
    if (storage_append(&r) != 0) {
        pthread_mutex_unlock(&a->lock);
        return ERR_INTERNAL;
    }
    a->balance = r.bal_to;
    *bal = a->balance;
    pthread_mutex_unlock(&a->lock);
    return 0;
}

int bank_withdraw(Account *a, int64_t amt, int64_t *bal)
{
    if (amt <= 0 || amt > MAX_TXN_PAISE) return ERR_AMOUNT;
    pthread_mutex_lock(&a->lock);
    if (a->balance - amt < MIN_BALANCE_PAISE) {
        pthread_mutex_unlock(&a->lock);
        return ERR_FUNDS;
    }
    LogRec r;
    memset(&r, 0, sizeof r);
    r.type = TX_WITHDRAW;
    r.from_acc = a->acc_no;
    r.amount = amt;
    r.bal_from = a->balance - amt;
    if (storage_append(&r) != 0) {
        pthread_mutex_unlock(&a->lock);
        return ERR_INTERNAL;
    }
    a->balance = r.bal_from;
    *bal = a->balance;
    pthread_mutex_unlock(&a->lock);
    return 0;
}

int bank_transfer(Account *from, int to_no, int64_t amt, int64_t *bal)
{
    if (amt <= 0 || amt > MAX_TXN_PAISE) return ERR_AMOUNT;
    Account *to = bank_find(to_no);
    if (!to) return ERR_NO_ACCOUNT;
    if (to == from) return ERR_SAME_ACCOUNT;

    /* Lock in account-number order: A->B and B->A can never deadlock. */
    Account *first  = from->acc_no < to->acc_no ? from : to;
    Account *second = first == from ? to : from;
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);

    int rc = 0;
    if (from->balance - amt < MIN_BALANCE_PAISE) rc = ERR_FUNDS;
    else if (to->balance > MAX_BALANCE_PAISE - amt) rc = ERR_AMOUNT;
    else {
        LogRec r;
        memset(&r, 0, sizeof r);
        r.type = TX_TRANSFER;
        r.from_acc = from->acc_no;
        r.to_acc = to->acc_no;
        r.amount = amt;
        r.bal_from = from->balance - amt;
        r.bal_to = to->balance + amt;
        if (storage_append(&r) != 0) rc = ERR_INTERNAL;
        else {
            from->balance = r.bal_from;
            to->balance = r.bal_to;
            *bal = from->balance;
        }
    }
    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    return rc;
}

int bank_history(Account *a, int n, LogRec *out)
{
    if (n > MAX_HISTORY) n = MAX_HISTORY;
    return storage_history(a->acc_no, n, out);
}
