/*ipc_io.h: send and receive the messages of include/ipc.h over af_unix sock_seqpacket*/
/*no logging here: the caller knows the context(which peer,which compontent) and and logs. */
/*every function returns -1 with errno set on a syscall failur unless noted*/
#ifndef GT_IPC_IO_H
#define GT_IPC_IO_H

#include <stddef.h>
#include <stdint.h>

#include "ipc.h"

#define IPC_EOF (-2) /* the peer closed its end*/
#define IPC_BAD (-3) /* a malformed message was read and dropped; keep reading*/
/* Listening socket at path; a stake socket file is replaced. ENAMETOOLONG if the path
does not fit sockaddr_un*/

int ipc_listen(const char* path);
/*Next pending connection (close on exexc)/ eagain if the listen fd is non blocking and idle.*/
int ipc_accept(int listen_fd);

/*connect to a listening socket*/
int ipc_connect(const char* path);

/* One message: header plus payload in a single send. len must be the payload size of 
type and sensor must suit it (soil/light messages carry their own sensor id), else ENVIAL.
Returns 0. Never raises sigpipe; a dead peer gives epipe*/
int ipc_send(int fd, gt_msg_type type, gt_sensor_id sensor, uint32_t seq, const void* payload,
	size_t len);
/* One message into hdr and payload (payload may be NULL when cap is 0).  Returns the
* payload length (>= 0), IPC_EOF, IPC_BAD, or -1: EAGAIN if non-blocking and nothing is
* waiting, EMSGSIZE if cap is too small (the message is consumed).  hdr is filled
* whenever at least a header arrived, so the caller can log a bad message's origin.
* A zero-length message looks like EOF; the peers here never send one. */
int ipc_recv(int fd, gt_msg_hdr* hdr, void* payload, size_t cap);

#endif /*GT_IPC_IO_H*/