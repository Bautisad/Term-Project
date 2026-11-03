/*hub_proc.h the hub process loop. hub_main.c and the tests both call hub_run, so the real 
binary and the tests run the same code*/
#ifndef GT_HUB_PROC_H
#define GT_HUB_PROC_H

#define HUB_STORE_DEFAULT "/var/lib/greenthumb"

typedef struct hub_opts {
	const char* sock_path; /*listening socket for the sensors*/
	const char* store_dir; /*the store directory; the hub is its only writer*/
	int         ctl_fd;   /*supervisor control socket for heartbeats; -1 for none*/
	unsigned    grace_ms; /*wait this long for the second sensor once a cycle is open*/
	unsigned    idle_ms; /*no cycle for this long after a record: write an empty one*/
	unsigned    heartbeat_ms; /*time between heartbeats*/
} hub_opts;

/*the production settings: ipc.h's socket and heartbeat interval, hub_logic.h timings.*/
hub_opts hub_opts_default(void);

/*Open the store, listen then collect sensor messages and append one record per cycle until
* told to stop.  Returns a gt_exit_code:
 *   GT_EXIT_OK      SIGTERM or SIGINT
 *   GT_EXIT_FATAL   the store could not be opened (including another writer holding it), the
 *                   socket could not be created, or a signal or poll call failed
 *   GT_EXIT_IPC     the supervisor's control socket closed
 *   GT_EXIT_CONFIG  opts out of range, or the store is damaged beyond repair (EUCLEAN):
 *                   restarting would not help
 * A failing store (disk full, read-only, I/O error) is not an exit: the record is dropped,
 * the heartbeats say DEGRADED, and the next cycle tries again. */
int hub_run(const hub_opts* opts);

#endif /*GT_HUB_PROC_H*/