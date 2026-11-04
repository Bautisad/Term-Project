#include "analyze.h"

#include <errno.h>

int analysis_run(const char* dir, uint64_t from_ms, uint64_t to_ms, const dry_params* p,
                 dry_result* out)
{
    dry_acc a;
    int     rc;
    int     err;

    if (dir == NULL || out == NULL || from_ms >= to_ms) {
        errno = EINVAL;
        return -1;
    }
    if (dry_acc_init(&a, p) != 0) {
        return -1; /* errno is EINVAL */
    }
    rc = store_read_range(dir, from_ms, to_ms, dry_acc_visit, &a);
    err = errno;
    if (rc != 0) {
        dry_acc_free(&a);
        errno = err;
        return -1;
    }
    err = dry_acc_error(&a);
    if (err != 0) {
        dry_acc_free(&a);
        errno = err;
        return -1;
    }
    dry_analyze(&a, out);
    dry_acc_free(&a);
    return 0;
}
