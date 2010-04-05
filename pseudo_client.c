/*
 * pseudo_client.c, pseudo client library code
 *
 * Copyright (c) 2008-2010 Wind River Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the Lesser GNU General Public License version 2.1 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the Lesser GNU General Public License for more details.
 *
 * You should have received a copy of the Lesser GNU General Public License
 * version 2.1 along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA 
 *
 */
#include <stdio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "pseudo.h"
#include "pseudo_ipc.h"
#include "pseudo_client.h"

static char *base_path(int dirfd, const char *path, int leave_last);

static int connect_fd = -1;
static int server_pid = 0;
int pseudo_dir_fd = -1;
int pseudo_pwd_fd = -1;
int pseudo_pwd_lck_fd = -1;
char *pseudo_pwd_lck_name = NULL;
FILE *pseudo_pwd = NULL;
int pseudo_grp_fd = -1;
FILE *pseudo_grp = NULL;
char *pseudo_cwd = NULL;
size_t pseudo_cwd_len;
char *pseudo_chroot = NULL;
char *pseudo_passwd = NULL;
size_t pseudo_chroot_len = 0;
char *pseudo_cwd_rel = NULL;

static char **fd_paths = NULL;
static int nfds = 0;
static int messages = 0;
static struct timeval message_time = { .tv_sec = 0 };
static int pseudo_inited = 0;

/* note: these are int, not uid_t/gid_t, so I can use 'em with scanf */
uid_t pseudo_ruid;
uid_t pseudo_euid;
uid_t pseudo_suid;
uid_t pseudo_fuid;
gid_t pseudo_rgid;
gid_t pseudo_egid;
gid_t pseudo_sgid;
gid_t pseudo_fgid;

static void
pseudo_file_close(int *fd, FILE **fp) {
	if (!fp || !fd) {
		pseudo_diag("pseudo_file_close: needs valid pointers.\n");
		return;
	}
	pseudo_antimagic();
	if (*fp) {
		fclose(*fp);
		*fd = -1;
		*fp = 0;
	}
	/* this should be impossible */
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
	pseudo_magic();
}

static FILE *
pseudo_file_open(char *name, int *fd, FILE **fp) {
	if (!fp || !fd || !name) {
		pseudo_diag("pseudo_file_open: needs valid pointers.\n");
		return NULL;
	}
	pseudo_file_close(fd, fp);
	pseudo_antimagic();
	*fd = PSEUDO_ETC_FILE(name, NULL, O_RDONLY);
	if (*fd >= 0) {
		*fd = pseudo_fd(*fd, MOVE_FD);
		*fp = fdopen(*fd, "r");
		if (!*fp) {
			close(*fd);
			*fd = -1;
		}
	}
	pseudo_magic();
	return *fp;
}

/* there is no spec I know of requiring us to defend this fd
 * against being closed by the user.
 */
int
pseudo_pwd_lck_open(void) {
	if (!pseudo_pwd_lck_name) {
		pseudo_pwd_lck_name = malloc(pseudo_path_max());
		if (!pseudo_pwd_lck_name) {
			pseudo_diag("couldn't allocate space for passwd lockfile path.\n");
			return -1;
		}
	}
	pseudo_pwd_lck_fd = PSEUDO_ETC_FILE(".pwd.lock",
					pseudo_pwd_lck_name, O_RDWR | O_CREAT);
	return pseudo_pwd_lck_fd;
}

void
pseudo_pwd_lck_close(void) {
	if (pseudo_pwd_lck_fd != -1) {
		close(pseudo_pwd_lck_fd);
		if (pseudo_pwd_lck_name) {
			unlink(pseudo_pwd_lck_name);
			free(pseudo_pwd_lck_name);
			pseudo_pwd_lck_name = 0;
		}
		pseudo_pwd_lck_fd = -1;
	}
}

FILE *
pseudo_pwd_open(void) {
	return pseudo_file_open("passwd", &pseudo_pwd_fd, &pseudo_pwd);
}

void
pseudo_pwd_close(void) {
	pseudo_file_close(&pseudo_pwd_fd, &pseudo_pwd);
}

FILE *
pseudo_grp_open(void) {
	return pseudo_file_open("group", &pseudo_grp_fd, &pseudo_grp);
}

void
pseudo_grp_close(void) {
	pseudo_file_close(&pseudo_grp_fd, &pseudo_grp);
}

void
pseudo_client_touchuid(void) {
	static char uidbuf[256];
	snprintf(uidbuf, 256, "%d,%d,%d,%d",
		pseudo_ruid, pseudo_euid, pseudo_suid, pseudo_fuid);
	setenv("PSEUDO_UIDS", uidbuf, 1);
}

void
pseudo_client_touchgid(void) {
	static char gidbuf[256];
	snprintf(gidbuf, 256, "%d,%d,%d,%d",
		pseudo_rgid, pseudo_egid, pseudo_sgid, pseudo_fgid);
	setenv("PSEUDO_GIDS", gidbuf, 1);
}

int
pseudo_client_chroot(const char *path) {
	/* free old value */
	free(pseudo_chroot);

	pseudo_debug(2, "client chroot: %s\n", path);
	if (!strcmp(path, "/")) {
		pseudo_chroot_len = 0;
		pseudo_chroot = 0;
		unsetenv("PSEUDO_CHROOT");
		return 0;
	}
	/* allocate new value */
	pseudo_chroot_len = strlen(path);
	pseudo_chroot = malloc(pseudo_chroot_len + 1);
	if (!pseudo_chroot) {
		pseudo_diag("Couldn't allocate chroot directory buffer.\n");
		pseudo_chroot_len = 0;
		errno = ENOMEM;
		return -1;
	}
	memcpy(pseudo_chroot, path, pseudo_chroot_len + 1);
	setenv("PSEUDO_CHROOT", pseudo_chroot, 1);
	return 0;
}

char *
pseudo_root_path(const char *func, int line, int dirfd, const char *path, int leave_last) {
	char *rc;
	pseudo_antimagic();
	rc = base_path(dirfd, path, leave_last);
	pseudo_magic();
	if (!rc) {
		pseudo_diag("couldn't allocate absolute path for '%s'.\n",
			path);
	}
	pseudo_debug(3, "root_path [%s, %d]: '%s' from '%s'\n",
		func, line,
		rc ? rc : "<nil>",
		path ? path : "<nil>");
	return rc;
}

int
pseudo_client_getcwd(void) {
	char *cwd;
	cwd = malloc(pseudo_path_max());
	if (!cwd) {
		pseudo_diag("Can't allocate CWD buffer!\n");
		return -1;
	}
	pseudo_debug(3, "getcwd: trying to find cwd.\n");
	if (getcwd(cwd, pseudo_path_max())) {
		/* cwd now holds a canonical path to current directory */
		free(pseudo_cwd);
		pseudo_cwd = cwd;
		pseudo_cwd_len = strlen(pseudo_cwd);
		pseudo_debug(3, "getcwd okay: [%s] %d bytes\n", pseudo_cwd, (int) pseudo_cwd_len);
		if (pseudo_chroot_len &&
			pseudo_cwd_len >= pseudo_chroot_len &&
			!memcmp(pseudo_cwd, pseudo_chroot, pseudo_chroot_len) &&
			(pseudo_cwd[pseudo_chroot_len] == '\0' ||
			pseudo_cwd[pseudo_chroot_len] == '/')) {
			pseudo_cwd_rel = pseudo_cwd + pseudo_chroot_len;
		} else {
			pseudo_cwd_rel = pseudo_cwd;
		}
		pseudo_debug(4, "abscwd: <%s>\n", pseudo_cwd);
		if (pseudo_cwd_rel != pseudo_cwd) {
			pseudo_debug(4, "relcwd: <%s>\n", pseudo_cwd_rel);
		}
		return 0;
	} else {
		pseudo_diag("Can't get CWD: %s\n", strerror(errno));
		return -1;
	}
}

static char *
fd_path(int fd) {
	if (fd >= 0 && fd < nfds) {
		return fd_paths[fd];
	}
	if (fd == AT_FDCWD) {
		return pseudo_cwd;
	}
	return 0;
}

static void
pseudo_client_path(int fd, const char *path) {
	if (fd < 0)
		return;

	if (fd >= nfds) {
		int i;
		pseudo_debug(2, "expanding fds from %d to %d\n",
			nfds, fd + 1);
		fd_paths = realloc(fd_paths, (fd + 1) * sizeof(char *));
		for (i = nfds; i < fd + 1; ++i)
			fd_paths[i] = 0;
		nfds = fd + 1;
	} else {
		if (fd_paths[fd]) {
			pseudo_debug(2, "reopening fd %d [%s] -- didn't see close\n",
				fd, fd_paths[fd]);
		}
		/* yes, it is safe to free null pointers. yay for C89 */
		free(fd_paths[fd]);
		fd_paths[fd] = 0;
	}
	if (path) {
		fd_paths[fd] = strdup(path);
	}
}

static void
pseudo_client_close(int fd) {
	if (fd < 0 || fd >= nfds)
		return;

	free(fd_paths[fd]);
	fd_paths[fd] = 0;
}

void
pseudo_client_reset() {
	pseudo_antimagic();
	pseudo_new_pid();
	if (connect_fd != -1) {
		close(connect_fd);
		connect_fd = -1;
	}
	if (!pseudo_inited) {
		char *env;
		
		env = getenv("PSEUDO_UIDS");
		if (env)
			sscanf(env, "%d,%d,%d,%d",
				&pseudo_ruid, &pseudo_euid,
				&pseudo_suid, &pseudo_fuid);

		env = getenv("PSEUDO_GIDS");
		if (env)
			sscanf(env, "%d,%d,%d,%d",
				&pseudo_rgid, &pseudo_egid,
				&pseudo_sgid, &pseudo_fuid);

		env = getenv("PSEUDO_CHROOT");
		if (env) {
			pseudo_chroot = strdup(env);
			if (pseudo_chroot) {
				pseudo_chroot_len = strlen(pseudo_chroot);
			} else {
				pseudo_diag("can't store chroot path (%s)\n", env);
			}
		}

		env = getenv("PSEUDO_PASSWD");
		if (env) {
			pseudo_passwd = strdup(env);
		}

		pseudo_inited = 1;
	}
	pseudo_client_getcwd();
	pseudo_magic();
}

/* spawn server */
static int
client_spawn_server(void) {
	int status;
	FILE *fp;
	extern char **environ;
	int cwd_fd;

	if ((server_pid = fork()) != 0) {
		if (server_pid == -1) {
			pseudo_diag("couldn't fork server: %s\n", strerror(errno));
			return 1;
		}
		pseudo_debug(4, "spawned server, pid %d\n", server_pid);
		/* wait for the child process to terminate, indicating server
		 * is ready
		 */
		waitpid(server_pid, &status, 0);
		server_pid = -2;
		cwd_fd = open(".", O_RDONLY);
		if (cwd_fd == -1) {
			pseudo_diag("Couldn't stash directory before opening pidfile: %s",
				strerror(errno));
			close(connect_fd);
			connect_fd = -1;
			return 1;
		}
		if (fchdir(pseudo_dir_fd)) {
			pseudo_diag("Couldn't change to server dir [%d]: %s\n",
				pseudo_dir_fd, strerror(errno));
		}
		fp = fopen(PSEUDO_PIDFILE, "r");
		if (fchdir(cwd_fd) == -1) {
			pseudo_diag("return to previous directory failed: %s\n",
				strerror(errno));
		}
		close(cwd_fd);
		if (fp) {
			if (fscanf(fp, "%d", &server_pid) != 1) {
				pseudo_debug(1, "Opened server PID file, but didn't get a pid.\n");
			}
			fclose(fp);
		} else {
			pseudo_diag("no pid file (%s): %s\n",
				PSEUDO_PIDFILE, strerror(errno));
		}
		pseudo_debug(2, "read new pid file: %d\n", server_pid);
		/* at this point, we should have a new server_pid */
		return 0;
	} else {
		char *base_args[] = { NULL, NULL, NULL };
		char **argv;
		int args;
		int fd;

		pseudo_new_pid();
		base_args[0] = "bin/pseudo";
		base_args[1] = "-d";
		if (getenv("PSEUDO_OPTS")) {
			char *option_string = strdup(getenv("PSEUDO_OPTS"));
			char *s;
			int arg;

			/* count arguments in PSEUDO_OPTS, starting at 2
			 * for pseudo/-d/NULL, plus one for the option string.
			 * The number of additional arguments may be less
			 * than the number of spaces, but can't be more.
			 */
			args = 4;
			for (s = option_string; *s; ++s)
				if (*s == ' ')
					++args;

			argv = malloc(args * sizeof(char *));
			argv[0] = base_args[0];
			argv[1] = base_args[1];
			arg = 2;
			while ((s = strsep(&option_string, " ")) != NULL) {
				if (*s) {
					argv[arg++] = strdup(s);
				}
			}
			argv[arg] = 0;
		} else {
			argv = base_args;
		}
		pseudo_dropenv();
		pseudo_debug(4, "calling execve on %s\n", argv[0]);
		/* and now, execute the server */
		if (fchdir(pseudo_dir_fd)) {
			pseudo_diag("Couldn't change to server dir [%d]: %s\n",
				pseudo_dir_fd, strerror(errno));
		}
		/* close any higher-numbered fds which might be open,
		 * such as sockets.  We don't have to worry about 0 and 1;
		 * the server closes them already, and more importantly,
		 * they can't have been opened or closed without us already
		 * having spawned a server... The issue is just socket()
		 * calls which could result in fds being left open, and those
		 * can't overwrite fds 0-2 unless we closed them...
		 * 
		 * No, really.  It works.
		 */
		for (fd = 3; fd < 1024; ++fd) {
			if (fd != pseudo_util_debug_fd)
				close(fd);
		}
		execve(argv[0], argv, environ);
		pseudo_diag("critical failure: exec of pseudo daemon failed: %s\n", strerror(errno));
		exit(1);
	}
}

static int
client_ping(void) {
	pseudo_msg_t ping;
	pseudo_msg_t *ack;
	char *tag = getenv("PSEUDO_TAG");

	ping.type = PSEUDO_MSG_PING;
	ping.op = OP_NONE;
	if (tag) {
		ping.pathlen = strlen(tag);
	} else {
		ping.pathlen = 0;
	}
	ping.client = getpid();
	ping.result = 0;
	errno = 0;
	pseudo_debug(4, "sending ping\n");
	if (pseudo_msg_send(connect_fd, &ping, ping.pathlen, tag)) {
		pseudo_debug(3, "error pinging server: %s\n", strerror(errno));
		return 1;
	}
	ack = pseudo_msg_receive(connect_fd);
	if (!ack) {
		pseudo_debug(2, "no ping response from server: %s\n", strerror(errno));
		/* and that's not good, so... */
		server_pid = 0;
		return 1;
	}
	if (ack->type != PSEUDO_MSG_ACK) {
		pseudo_debug(1, "invalid ping response from server: expected ack, got %d\n", ack->type);
		/* and that's not good, so... */
		server_pid = 0;
		return 1;
	}
	pseudo_debug(5, "ping ok\n");
	return 0;
}

int
pseudo_fd(int fd, int how) {
	int newfd;

	if (fd < 0)
		return(-1);

	/* If already above PSEUDO_MIN_FD, no need to move */
	if ((how == MOVE_FD) && (fd >= PSEUDO_MIN_FD)) {
		newfd = fd;
	} else {
		newfd = fcntl(fd, F_DUPFD, PSEUDO_MIN_FD);

		if (how == MOVE_FD)
			close(fd);
	}

	/* Set close on exec, even if we didn't move it. */
	if ((newfd >= 0) && (fcntl(newfd, F_SETFD, FD_CLOEXEC) < 0))
		pseudo_diag("can't set close on exec flag: %s\n",
			strerror(errno));

	return(newfd);
}

static int
client_connect(void) {
	/* we have a server pid, is it responsive? */
	struct sockaddr_un sun = { AF_UNIX, PSEUDO_SOCKET };
	int cwd_fd;

	connect_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	connect_fd = pseudo_fd(connect_fd, MOVE_FD);
	if (connect_fd == -1) {
		pseudo_diag("can't create socket: %s\n", strerror(errno));
		return 1;
	}

	pseudo_debug(3, "connecting socket...\n");
	cwd_fd = open(".", O_RDONLY);
	if (cwd_fd == -1) {
		pseudo_diag("Couldn't stash directory before opening socket: %s",
			strerror(errno));
		close(connect_fd);
		connect_fd = -1;
		return 1;
	}
	if (fchdir(pseudo_dir_fd) == -1) {
		pseudo_diag("Couldn't chdir to server directory [%d]: %s\n",
			pseudo_dir_fd, strerror(errno));
		close(connect_fd);
		close(cwd_fd);
		connect_fd = -1;
		return 1;
	}
	if (connect(connect_fd, (struct sockaddr *) &sun, sizeof(sun)) == -1) {
		pseudo_debug(3, "can't connect socket to pseudo.socket: (%s)\n", strerror(errno));
		close(connect_fd);
		if (fchdir(cwd_fd) == -1) {
			pseudo_diag("return to previous directory failed: %s\n",
				strerror(errno));
		}
		close(cwd_fd);
		connect_fd = -1;
		return 1;
	}
	if (fchdir(cwd_fd) == -1) {
		pseudo_diag("return to previous directory failed: %s\n",
			strerror(errno));
	}
	close(cwd_fd);
	pseudo_debug(4, "connected socket.\n");
	return 0;
}

static int
pseudo_client_setup(void) {
	FILE *fp;
	server_pid = 0;
	int cwd_fd;

	/* avoid descriptor leak, I hope */
	if (connect_fd >= 0) {
		close(connect_fd);
		connect_fd = -1;
	}

	cwd_fd = open(".", O_RDONLY);
	if (cwd_fd == -1) {
		pseudo_diag("Couldn't stash directory before opening pidfile: %s",
			strerror(errno));
		close(connect_fd);
		connect_fd = -1;
		return 1;
	}
	if (fchdir(pseudo_dir_fd) != 1) {
		fp = fopen(PSEUDO_PIDFILE, "r");
		if (fchdir(cwd_fd) == -1) {
			pseudo_diag("return to previous directory failed: %s\n",
				strerror(errno));
		}
	} else {
		fp = NULL;
		pseudo_diag("couldn't change to pseudo working directory for pid file.\n");
	}
	close(cwd_fd);
	if (fp) {
		if (fscanf(fp, "%d", &server_pid) != 1) {
			pseudo_debug(1, "Opened server PID file, but didn't get a pid.\n");
		}
		fclose(fp);
	}
	if (server_pid) {
		if (kill(server_pid, 0) == -1) {
			pseudo_debug(1, "couldn't find server at pid %d: %s\n",
				server_pid, strerror(errno));
			server_pid = 0;
		}
	}
	if (!server_pid) {
		if (client_spawn_server()) {
			return 1;
		}
	}
	if (!client_connect() && !client_ping()) {
		return 0;
	}
	pseudo_debug(2, "server seems to be gone, trying to restart\n");
	if (client_spawn_server()) {
		pseudo_debug(1, "failed to spawn server, giving up.\n");
		return 1;
	} else {
		pseudo_debug_verbose();
		pseudo_debug(2, "restarted, new pid %d\n", server_pid);
		if (!client_connect() && !client_ping()) {
			pseudo_debug_terse();
			return 0;
		}
		pseudo_debug_terse();
	}
	pseudo_debug(1, "couldn't get a server, giving up.\n");
	return 1;
}

static pseudo_msg_t *
pseudo_client_request(pseudo_msg_t *msg, size_t len, const char *path) {
	pseudo_msg_t *response = 0;
	int tries = 0;
	int rc;

	if (!msg)
		return 0;

	do {
		do {
			pseudo_debug(4, "sending a message: ino %llu\n",
				(unsigned long long) msg->ino);
			if (connect_fd < 0) {
				pseudo_debug(2, "trying to get server\n");
				if (pseudo_client_setup()) {
					return 0;
				}
			}
			rc = pseudo_msg_send(connect_fd, msg, len, path);
			if (rc != 0) {
				pseudo_debug(2, "msg_send: %d%s\n",
					rc,
					rc == -1 ? " (sigpipe)" :
					           " (short write)");
				pseudo_client_setup();
				++tries;
				if (tries > 3) {
					pseudo_debug(1, "can't get server going again.\n");
					return 0;
				}
			}
		} while (rc != 0);
		pseudo_debug(5, "sent!\n");
		response = pseudo_msg_receive(connect_fd);
		if (!response) {
			++tries;
			if (tries > 3) {
				pseudo_debug(1, "can't get responses.\n");
				return 0;
			}
		}
	} while (response == 0);
	if (response->type != PSEUDO_MSG_ACK) {
		pseudo_debug(2, "got non-ack response %d\n", response->type);
		return 0;
	} else {
		pseudo_debug(4, "got response type %d\n", response->type);
	}
	return response;
}

int
pseudo_client_shutdown(void) {
	pseudo_msg_t msg;
	pseudo_msg_t *ack;
	char *pseudo_path;

	pseudo_path = pseudo_prefix_path(NULL);
	if (pseudo_dir_fd == -1) {
		if (pseudo_path) {
			pseudo_dir_fd = open(pseudo_path, O_RDONLY);
			pseudo_dir_fd = pseudo_fd(pseudo_dir_fd, COPY_FD);
			free(pseudo_path);
		} else {
			pseudo_diag("No prefix available to to find server.\n");
			exit(1);
		}
		if (pseudo_dir_fd == -1) {
			pseudo_diag("Can't open prefix path (%s) for server.\n",
				strerror(errno));
			exit(1);
		}
	}
	if (client_connect()) {
		pseudo_diag("Pseudo server seems to be already offline.\n");
		return 0;
	}
	memset(&msg, 0, sizeof(pseudo_msg_t));
	msg.type = PSEUDO_MSG_SHUTDOWN;
	msg.op = OP_NONE;
	msg.client = getpid();
	pseudo_debug(2, "sending shutdown request\n");
	if (pseudo_msg_send(connect_fd, &msg, 0, NULL)) {
		pseudo_debug(1, "error requesting shutdown: %s\n", strerror(errno));
		return 1;
	}
	ack = pseudo_msg_receive(connect_fd);
	if (!ack) {
		pseudo_diag("server did not respond to shutdown query.\n");
		return 1;
	}
	if (ack->type == PSEUDO_MSG_ACK) {
		return 0;
	}
	pseudo_diag("Server refused shutdown.  Remaining client fds: %d\n", ack->fd);
	pseudo_diag("Client pids: %s\n", ack->path);
	pseudo_diag("Server will shut down after all clients exit.\n");
	return 0;
}

static char *
base_path(int dirfd, const char *path, int leave_last) {
	char *basepath = 0;
	size_t baselen = 0;
	size_t minlen = 0;
	char *newpath;

	if (path[0] != '/') {
		if (dirfd != -1 && dirfd != AT_FDCWD) {
			if (dirfd >= 0) {
				basepath = fd_path(dirfd);
				baselen = strlen(basepath);
			} else {
				pseudo_diag("got *at() syscall for unknown directory, fd %d\n", dirfd);
			}
		} else {
			basepath = pseudo_cwd;
			baselen = pseudo_cwd_len;
		}
		if (!basepath) {
			pseudo_diag("unknown base path for fd %d, path %s\n", dirfd, path);
			return 0;
		}
		/* if there's a chroot path, and it's the start of basepath,
		 * flag it for pseudo_fix_path
		 */
		if (pseudo_chroot_len && baselen >= pseudo_chroot_len &&
			!memcmp(basepath, pseudo_chroot, pseudo_chroot_len) &&
			(basepath[pseudo_chroot_len] == '\0' || basepath[pseudo_chroot_len] == '/')) {

			minlen = pseudo_chroot_len;
		}
	} else if (pseudo_chroot_len) {
		/* "absolute" is really relative to chroot path */
		basepath = pseudo_chroot;
		baselen = pseudo_chroot_len;
		minlen = pseudo_chroot_len;
	}

	newpath = pseudo_fix_path(basepath, path, minlen, baselen, NULL, leave_last);
	pseudo_debug(4, "base_path: %s</>%s\n",
		basepath ? basepath : "<nil>",
		path ? path : "<nil>");
	return newpath;
}

pseudo_msg_t *
pseudo_client_op(op_id_t op, int access, int fd, int dirfd, const char *path, const struct stat64 *buf, ...) {
	pseudo_msg_t *result = 0;
	pseudo_msg_t msg = { .type = PSEUDO_MSG_OP };
	size_t pathlen = -1;
	int do_request = 0;
	char *oldpath = 0;
	char *newpath = 0;

	/* disable wrappers */
	pseudo_antimagic();

	if (op == OP_RENAME) {
		va_list ap;
		va_start(ap, buf);
		oldpath = va_arg(ap, char *);
		va_end(ap);
		/* last argument is the previous path of the file */
		if (!oldpath) {
			pseudo_diag("rename (%s) without old path.\n",
				path ? path : "<nil>");
			pseudo_magic();
			return 0;
		}
		if (!path) {
			pseudo_diag("rename (%s) without new path.\n",
				path ? path : "<nil>");
			pseudo_magic();
			return 0;
		}
	}

	if (path) {
		/* path fixup has to happen in the specific functions,
		 * because they may have to make calls which have to be
		 * fixed up for chroot stuff already.
		 */
		pathlen = strlen(path) + 1;
		if (oldpath) {
			size_t full_len = strlen(oldpath) + 1 + pathlen;
			char *both_paths = malloc(full_len);
			if (!both_paths) {
				pseudo_diag("can't allocate space for paths for a rename operation.  Sorry.\n");
				pseudo_magic();
				return 0;
			}
			snprintf(both_paths, full_len, "%s%c%s",
				path, 0, oldpath);
			pseudo_debug(2, "rename: %s -> %s [%d]\n",
				both_paths + pathlen, both_paths, (int) full_len);
			path = both_paths;
			newpath = both_paths;
			pathlen = full_len;
		}
	} else if (fd >= 0 && fd <= nfds) {
		path = fd_path(fd);
		if (!path)
			msg.pathlen = 0;
		else
			msg.pathlen = strlen(path) + 1;
	} else {
		path = 0;
		msg.pathlen = 0;
	}
	pseudo_debug(2, "%s%s", pseudo_op_name(op),
		(dirfd != -1 && dirfd != AT_FDCWD && op != OP_DUP) ? "at" : "");
	if (oldpath) {
		pseudo_debug(2, " %s ->", (char *) oldpath);
	}
	if (path) {
		pseudo_debug(2, " %s", path);
	}
	if (fd != -1) {
		pseudo_debug(2, " [fd %d]", fd);
	}
	if (buf) {
		pseudo_debug(2, " (+buf)");
		pseudo_msg_stat(&msg, buf);
		if (buf && fd != -1) {
			pseudo_debug(2, " [dev/ino: %d/%llu]",
				(int) buf->st_dev, (unsigned long long) buf->st_ino);
		}
		pseudo_debug(2, " (0%o)", (int) buf->st_mode);
	}
	pseudo_debug(2, ": ");
	msg.type = PSEUDO_MSG_OP;
	msg.op = op;
	msg.fd = fd;
	msg.access = access;
	msg.result = RESULT_NONE;
	msg.client = getpid();

	/* do stuff */
	pseudo_debug(4, "processing request [ino %llu]\n", (unsigned long long) msg.ino);
	switch (msg.op) {
	case OP_CHDIR:
		pseudo_client_getcwd();
		do_request = 0;
		break;
	case OP_CHROOT:
		if (pseudo_client_chroot(path) == 0) {
			/* return a non-zero value to show non-failure */
			result = &msg;
		}
		do_request = 0;
		break;
	case OP_OPEN:
		pseudo_client_path(fd, path);
		do_request = 1;
		break;
	case OP_CLOSE:
		/* no request needed */
		if (fd >= 0) {
			if (fd == connect_fd) {
				connect_fd = pseudo_fd(connect_fd, COPY_FD);
				if (connect_fd == -1) {
					pseudo_diag("tried to close connection, couldn't dup: %s\n", strerror(errno));
				}
			} else if (fd == pseudo_util_debug_fd) {
				pseudo_util_debug_fd = pseudo_fd(fd, COPY_FD);
			} else if (fd == pseudo_dir_fd) {
				pseudo_dir_fd = pseudo_fd(fd, COPY_FD);
			} else if (fd == pseudo_pwd_fd) {
				pseudo_pwd_fd = pseudo_fd(fd, COPY_FD);
				/* since we have a FILE * on it, we close that... */
				fclose(pseudo_pwd);
				/* and open a new one on the copy */
				pseudo_pwd = fdopen(pseudo_pwd_fd, "r");
			} else if (fd == pseudo_grp_fd) {
				pseudo_grp_fd = pseudo_fd(fd, COPY_FD);
				/* since we have a FILE * on it, we close that... */
				fclose(pseudo_grp);
				/* and open a new one on the copy */
				pseudo_grp = fdopen(pseudo_grp_fd, "r");
			}
		}
		pseudo_client_close(fd);
		do_request = 0;
		break;
	case OP_DUP:
		/* just copy the path over */
		pseudo_debug(2, "dup: fd_path(%d) = %p [%s], dup to %d\n",
			fd, fd_path(fd), fd_path(fd) ? fd_path(fd) : "<nil>",
			dirfd);
		pseudo_client_path(dirfd, fd_path(fd));
		break;
	/* operations for which we should use the magic uid/gid */
	case OP_CHMOD:		/* FALLTHROUGH */
	case OP_CREAT:		/* FALLTHROUGH */
	case OP_FCHMOD:		/* FALLTHROUGH */
	case OP_MKDIR:		/* FALLTHROUGH */
	case OP_MKNOD:		/* FALLTHROUGH */
	case OP_SYMLINK:	/* FALLTHROUGH */
		msg.uid = pseudo_fuid;
		msg.gid = pseudo_fgid;
		pseudo_debug(2, "fuid: %d ", pseudo_fuid);
		/* fallthrough.  chown/fchown uid/gid already calculated, and
		 * a link or rename should not change a file's ownership.
		 * (operations which can create should be CREAT or MKNOD
		 * or MKDIR)
		 */
	case OP_EXEC:		/* FALLTHROUGH */
	case OP_CHOWN:		/* FALLTHROUGH */
	case OP_FCHOWN:		/* FALLTHROUGH */
	case OP_FSTAT:		/* FALLTHROUGH */
	case OP_LINK:		/* FALLTHROUGH */
	case OP_RENAME:		/* FALLTHROUGH */
	case OP_STAT:		/* FALLTHROUGH */
	case OP_UNLINK:
		do_request = 1;
		break;
	default:
		pseudo_diag("error: unknown or unimplemented operator %d (%s)", op, pseudo_op_name(op));
		break;
	}
	if (do_request) {
		struct timeval tv1, tv2;
		pseudo_debug(4, "sending request [ino %llu]\n", (unsigned long long) msg.ino);
		gettimeofday(&tv1, NULL);
		result = pseudo_client_request(&msg, pathlen, path);
		gettimeofday(&tv2, NULL);
		++messages;
		message_time.tv_sec += (tv2.tv_sec - tv1.tv_sec);
		message_time.tv_usec += (tv2.tv_usec - tv1.tv_usec);
		if (message_time.tv_usec < 0) {
			message_time.tv_usec += 1000000;
			--message_time.tv_sec;
		} else while (message_time.tv_usec > 1000000) {
			message_time.tv_usec -= 1000000;
			++message_time.tv_sec;
		}
		if (result) {
			pseudo_debug(2, "(%d) %s", getpid(), pseudo_res_name(result->result));
			if (op == OP_STAT || op == OP_FSTAT) {
				pseudo_debug(2, " mode 0%o uid %d:%d",
					(int) result->mode,
					(int) result->uid,
					(int) result->gid);
			} else if (op == OP_CHMOD || op == OP_FCHMOD) {
				pseudo_debug(2, " mode 0%o",
					(int) result->mode);
			} else if (op == OP_CHOWN || op == OP_FCHOWN) {
				pseudo_debug(2, " uid %d:%d",
					(int) result->uid,
					(int) result->gid);
			}
		} else {
			pseudo_debug(2, "(%d) no answer", getpid());
		}
	} else {
		pseudo_debug(2, "(%d) (no request)", getpid());
	}
	pseudo_debug(2, "\n");

	/* if not NULL, newpath is the buffer holding both paths */
	free(newpath);

	if (do_request && (messages % 1000 == 0)) {
		pseudo_debug(2, "%d messages handled in %.4f seconds\n",
			messages,
			(double) message_time.tv_sec +
			(double) message_time.tv_usec / 1000000.0);
	}
	/* reenable wrappers */
	pseudo_magic();

	return result;
}

void
pseudo_stat32_from64(struct stat *buf32, struct stat64 *buf) {
	buf32->st_dev = buf->st_dev;
	buf32->st_ino = buf->st_ino;
	buf32->st_mode = buf->st_mode;
	buf32->st_nlink = buf->st_nlink;
	buf32->st_uid = buf->st_uid;
	buf32->st_gid = buf->st_gid;
	buf32->st_rdev = buf->st_rdev;
	buf32->st_size = buf->st_size;
	buf32->st_blksize = buf->st_blksize;
	buf32->st_blocks = buf->st_blocks;
	buf32->st_atime = buf->st_atime;
	buf32->st_mtime = buf->st_mtime;
	buf32->st_ctime = buf->st_ctime;
}

void
pseudo_stat64_from32(struct stat64 *buf64, struct stat *buf) {
	buf64->st_dev = buf->st_dev;
	buf64->st_ino = buf->st_ino;
	buf64->st_mode = buf->st_mode;
	buf64->st_nlink = buf->st_nlink;
	buf64->st_uid = buf->st_uid;
	buf64->st_gid = buf->st_gid;
	buf64->st_rdev = buf->st_rdev;
	buf64->st_size = buf->st_size;
	buf64->st_blksize = buf->st_blksize;
	buf64->st_blocks = buf->st_blocks;
	buf64->st_atime = buf->st_atime;
	buf64->st_mtime = buf->st_mtime;
	buf64->st_ctime = buf->st_ctime;
}
