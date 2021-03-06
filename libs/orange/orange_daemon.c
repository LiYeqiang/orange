#include "orange_daemon.h"
#include "orange_log.h"

#if 0
#define DEBUGP printf
#else
#define DEBUGP(format, args...)
#endif

#define FILE_PATH_LEN_MAX 128

static int terminate = -1;

static void __orange_daemon_init(void)
{
	int				 i, fd0, fd1, fd2;
	pid_t			 pid;
	struct rlimit	rl;
	struct sigaction sa;

	umask(0);
	if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
		exit(1);
	}

	if ((pid = fork()) < 0) {
		exit(1);
	} else if (pid != 0) {
		exit(0);
	}

	setsid();

	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGHUP, &sa, NULL) < 0) {
		exit(1);
	}

	if ((pid = fork()) < 0) {
		exit(1);
	} else if (pid != 0) {
		exit(0);
	}

	if (chdir("/") < 0) {
		exit(1);
	}

	if (rl.rlim_max == RLIM_INFINITY) {
		rl.rlim_max = 1024;
	}

	for (i = 0; i < rl.rlim_max; i++) {
		close(i);
	}

	fd0 = open("/dev/null", O_RDWR);
	fd1 = dup(0);
	fd2 = dup(0);

	if (fd0 != 0 || fd1 != 1 || fd2 != 2) {
		exit(1);
	}

	return;
}

static int __orange_daemon_lockfile(int fd)
{
	struct flock fl;

	fl.l_type   = F_WRLCK;
	fl.l_start  = 0;
	fl.l_whence = SEEK_SET;
	fl.l_len	= 0;
	return (fcntl(fd, F_SETLK, &fl));
}

static int __orange_daemon_already_running(char* daemon_name)
{
	int __attribute__((unused)) ret = 0;
	char lockfile_name[FILE_PATH_LEN_MAX];
	char buf[16];
	int  fd;

    orange_log_error("%s:%d begin daemon name: %s\n", __func__, __LINE__, daemon_name);

	snprintf(lockfile_name, FILE_PATH_LEN_MAX, "/var/run/%s.pid", daemon_name);

	fd = open(lockfile_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		exit(1);
	}

	if (__orange_daemon_lockfile(fd) < 0) {
		if (errno == EACCES || errno == EAGAIN) {
			close(fd);
			return 1;
		}
		exit(1);
	}

	ret = ftruncate(fd, 0);
	snprintf(buf, 16, "%ld", (long) getpid());
	ret = write(fd, buf, strlen(buf) + 1);

	return 0;
}

static void __orange_daemon_reread(void)
{
	return;
}

static void __orange_daemon_sigterm(int signo)
{
	exit(0);
}

static void __orange_daemon_sighup(int signo)
{
	return __orange_daemon_reread();
}

static void __orange_detach_terminal(void)
{
	int handle;
	int pgrp_id;

	orange_log(ORANGE_LOG_DEBUG, "detaching terminal...\n");

	orange_log_close();

	for (handle = 3; handle >= 0; handle--) {
		close(handle);
	}

	errno = 0;

	orange_log_open();

	if (getpgrp() == getpid()) {
		orange_log(ORANGE_LOG_DEBUG, "already process group leader.\n");
	} else {
		orange_log(ORANGE_LOG_DEBUG, "moving to new session group\n");
		orange_log_close();

		pgrp_id = setsid();

		orange_log_open();

		if (pgrp_id == -1) {
			orange_log(ORANGE_LOG_ERR, "setsid() failed. error: %d\n", errno);
		}
	}

	return;
}

static void __orange_daemon_on_signal(int signal_code)
{
    switch(signal_code){
        case SIGHUP:
            terminate = 1;
            break;
        case SIGTERM:
        case SIGKILL:
            terminate = 1;
            break;
        case SIGINT:
            signal(SIGINT, SIG_IGN);
            terminate = 1;
            break;
        case SIGXFSZ:
            break;
        default:
            break;
    }

    return;
}

void orange_daemon_create(char* daemon_name)
{
	struct sigaction sa;

    signal(SIGHUP, __orange_daemon_on_signal);
    signal(SIGTERM, __orange_daemon_on_signal);
    signal(SIGKILL, __orange_daemon_on_signal);
    signal(SIGINT, __orange_daemon_on_signal);
    signal(SIGXFSZ, __orange_daemon_on_signal);

	orange_log_init(daemon_name, 1, 1, -1);
	__orange_daemon_init();

	if (__orange_daemon_already_running(daemon_name)) {
        orange_log(ORANGE_LOG_EMERG, "%s already running...\n", daemon_name);
		exit(1);
	}

	__orange_detach_terminal();

	sa.sa_handler = __orange_daemon_sigterm;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGHUP);
	sa.sa_flags = 0;

	if (sigaction(SIGTERM, &sa, NULL) < 0) {
		exit(1);
	}

	sa.sa_handler = __orange_daemon_sighup;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGTERM);
	sa.sa_flags = 0;

	if (sigaction(SIGHUP, &sa, NULL) < 0) {
		exit(1);
	}
}

int orange_daemon_is_terminated(void)
{
    return terminate;
}
