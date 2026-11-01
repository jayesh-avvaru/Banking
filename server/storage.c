#include "storage.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* macOS has no fdatasync; F_FULLFSYNC is the real "flush to disk" there */
static int sync_fd(int fd)
{
#ifdef __APPLE__
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return fsync(fd);
#else
    return fdatasync(fd);
#endif
}

static const char MAGIC[8] = {'B','N','K','S','N','A','P','1'};

static char    log_path[600], snap_path[600], tmp_path[620];
static int     log_fd = -1;
static off_t   log_size = 0;
static int64_t last_txn_id = 0;
static int     do_sync = 1;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

int storage_set_dir(const char *dir)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(log_path,  sizeof log_path,  "%s/txn.log", dir);
    snprintf(snap_path, sizeof snap_path, "%s/accounts.dat", dir);
    snprintf(tmp_path,  sizeof tmp_path,  "%s.tmp", snap_path);
    return 0;
}

void storage_set_sync(int on) { do_sync = on; }
int64_t storage_last_id(void) { return last_txn_id; }

int storage_snapshot_read(SnapHeader *h, DiskAccount *out, int max)
{
    memset(h, 0, sizeof *h);
    FILE *f = fopen(snap_path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;
    if (fread(h, sizeof *h, 1, f) != 1 || memcmp(h->magic, MAGIC, 8) != 0 ||
        h->count < 0 || h->count > max ||
        fread(out, sizeof *out, (size_t)h->count, f) != (size_t)h->count) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return h->count;
}

/* write temp file, fsync, atomic rename -> a crash never leaves a half-written snapshot */
int storage_snapshot_write(const DiskAccount *accs, int n)
{
    SnapHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, MAGIC, 8);
    h.count = n;
    h.last_txn_id = last_txn_id;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;
    int ok = fwrite(&h, sizeof h, 1, f) == 1 &&
             (n == 0 || fwrite(accs, sizeof *accs, (size_t)n, f) == (size_t)n) &&
             fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp_path, snap_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int storage_open_log(int64_t snap_last_id, void (*apply)(const LogRec *), long *replayed)
{
    int64_t max_id = snap_last_id;
    long good = 0, rep = 0;
    LogRec r;

    FILE *f = fopen(log_path, "rb");
    if (f) {
        while (fread(&r, sizeof r, 1, f) == 1) {     /* short/torn last record is ignored */
            good++;
            if (r.txn_id > max_id) max_id = r.txn_id;
            if (r.txn_id > snap_last_id) { apply(&r); rep++; }
        }
        fclose(f);
    }
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) return -1;
    log_size = (off_t)good * (off_t)sizeof(LogRec);
    if (ftruncate(log_fd, log_size) != 0) return -1;  /* cut a torn tail so appends stay aligned */
    last_txn_id = max_id;
    if (replayed) *replayed = rep;
    return 0;
}

int storage_append(LogRec *r)
{
    int ok = 1;
    pthread_mutex_lock(&log_mutex);

    r->txn_id = last_txn_id + 1;
    r->ts = (int64_t)time(NULL);

    const char *p = (const char *)r;
    size_t left = sizeof *r;
    while (left > 0) {
        ssize_t w = write(log_fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (ok && do_sync && sync_fd(log_fd) != 0) ok = 0;

    if (ok) {
        last_txn_id++;
        log_size += (off_t)sizeof *r;
    } else if (ftruncate(log_fd, log_size) != 0) {
        /* best effort: remove the partial record */
    }
    pthread_mutex_unlock(&log_mutex);
    return ok ? 0 : -1;
}

int storage_history(int acc_no, int max, LogRec *out)
{
    if (max <= 0) return 0;
    LogRec *ring = malloc((size_t)max * sizeof *ring);
    if (!ring) return -1;
    FILE *f = fopen(log_path, "rb");
    if (!f) { free(ring); return 0; }

    LogRec r;
    long total = 0;
    while (fread(&r, sizeof r, 1, f) == 1) {
        if (r.type != TX_OPEN && (r.from_acc == acc_no || r.to_acc == acc_no))
            ring[total++ % max] = r;
    }
    fclose(f);

    int count = total < max ? (int)total : max;
    long start = total < max ? 0 : total % max;
    for (int i = 0; i < count; i++) out[i] = ring[(start + i) % max];
    free(ring);
    return count;
}

void storage_close(void)
{
    if (log_fd >= 0) { close(log_fd); log_fd = -1; }
}
