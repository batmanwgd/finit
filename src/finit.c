/* Finit - Fast /sbin/init replacement w/ I/O, hook & service plugins
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <ctype.h>
#include <dirent.h>
#ifdef HAVE_FSTAB_H
#include <fstab.h>
#endif
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>		/* umask(), mkdir() */
#include <sys/wait.h>
#include <lite/lite.h>

#include "finit.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "service.h"
#include "sig.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "watchdog.h"

int   wdogpid   = 0;		/* No watchdog by default */
int   runlevel  = 0;		/* Bootstrap 'S' */
int   cfglevel  = RUNLEVEL;	/* Fallback if no configured runlevel */
int   prevlevel = -1;
char *sdown     = NULL;
char *network   = NULL;
char *hostname  = NULL;
char *rcsd      = FINIT_RCSD;
char *runparts  = NULL;

uev_ctx_t *ctx  = NULL;		/* Main loop context */

/*
 * Show user configured banner before service bootstrap progress
 */
static void banner(void)
{
	char *buf = INIT_HEADING;
	char separator[SCREEN_WIDTH];

	if (plugin_exists(HOOK_BANNER)) {
		plugin_run_hooks(HOOK_BANNER);
		return;
	}

	if (log_is_silent())
		return;

	memset(separator, '=', sizeof(separator));
	fprintf(stderr, "\e[2K\e[1m%s %.*s\e[0m\n", buf, SCREEN_WIDTH - (int)strlen(buf) - 2, separator);
}

static int ismnt(char *file, char *dir)
{
	FILE *fp;
	int found = 0;
	struct mntent *mnt;

	fp = setmntent(file, "r");
	if (!fp)
		return 0;	/* Dunno, maybe not */

	while ((mnt = getmntent(fp))) {
		if (!strcmp(mnt->mnt_dir, dir)) {
			found = 1;
			break;
		}
	}
	endmntent(fp);

	return found;
}

/* Requires /proc to be mounted */
static int fismnt(char *dir)
{
	return ismnt("/proc/mounts", dir);
}

/*
 * Check all filesystems in /etc/fstab with a fs_passno > 0
 */
static int fsck(int pass)
{
//	int save;
	struct fstab *fs;

	if (!setfsent()) {
		_pe("Failed opening fstab");
		return 1;
	}

//	if ((save = log_is_debug()))
//		log_debug();

	while ((fs = getfsent())) {
		char cmd[80];
		struct stat st;

		if (fs->fs_passno != pass)
			continue;

		errno = 0;
		if (stat(fs->fs_spec, &st) || !S_ISBLK(st.st_mode)) {
			if (!string_match(fs->fs_spec, "UUID=") && !string_match(fs->fs_spec, "LABEL=")) {
				_d("Cannot fsck %s, not a block device: %s", fs->fs_spec, strerror(errno));
				continue;
			}
		}

		if (fismnt(fs->fs_file)) {
			_d("Skipping fsck of %s, already mounted on %s.", fs->fs_spec, fs->fs_file);
			continue;
		}

		snprintf(cmd, sizeof(cmd), "fsck -a %s", fs->fs_spec);
		run_interactive(cmd, "Checking filesystem %.13s", fs->fs_spec);
	}

//	if (save)
//		log_debug();
	endfsent();

	return 0;
}

static void networking(void)
{
	FILE *fp;

	/* Run user network start script if enabled */
	if (network) {
		run_interactive(network, "Starting networking: %s", network);
		goto done;
	}

	/* Debian/Ubuntu/Busybox/RH/Suse */
	if (!whichp("ifup"))
		goto done;

	fp = fopen("/etc/network/interfaces", "r");
	if (fp) {
		int i = 0;
		char buf[160];

		/* Bring up all 'auto' interfaces */
		while (fgets(buf, sizeof(buf), fp)) {
			char cmd[80];
			char *line, *ifname = NULL;

			chomp(buf);
			line = strip_line(buf);

			if (!strncmp(line, "auto", 4))
				ifname = &line[5];
			if (!strncmp(line, "allow-hotplug", 13))
				ifname = &line[14];

			if (!ifname)
				continue;

			snprintf(cmd, 80, "ifup %s", ifname);
			run_interactive(cmd, "Bringing up interface %s", ifname);
			i++;
		}

		fclose(fp);
	}

done:
	/* Fall back to bring up at least loopback */
	ifconfig("lo", "127.0.0.1", "255.0.0.0", 1);
}

/*
 * If everything goes south we can use this to give the operator an
 * emergency shell to debug the problem -- Finit should not crash!
 *
 * Note: Only use this for debugging a new Finit setup, don't use
 *       this in production since it gives a root shell to anyone
 *       if Finit crashes.
 *
 * This emergency shell steps in to prevent "Aieee, PID 1 crashed"
 * messages from the kernel, which usually results in a reboot, so
 * that the operator instead can debug the problem.
 */
static void emergency_shell(void)
{
#ifdef EMERGENCY_SHELL
	pid_t pid;

	pid = fork();
	if (pid) {
		while (1) {
			pid_t id;

			/* Reap 'em (prevents Zombies) */
			id = waitpid(-1, NULL, WNOHANG);
			if (id == pid)
				break;
		}

		fprintf(stderr, "\n=> Embarrassingly, Finit has crashed.  Check /dev/kmsg for details.\n");
		fprintf(stderr,   "=> To debug, add '--debug' to the kernel command line.\n\n");

		/*
		 * Become session leader and set controlling TTY
		 * to enable Ctrl-C and job control in shell.
		 */
		setsid();
		ioctl(STDIN_FILENO, TIOCSCTTY, 1);

		execl(_PATH_BSHELL, _PATH_BSHELL, NULL);
	}
#endif /* EMERGENCY_SHELL */
}

/*
 * Handle bootstrap transition to configured runlevel, start TTYs
 *
 * This is the final stage of bootstrap.  It changes to the default
 * (configured) runlevel, calls all external start scripts and final
 * bootstrap hooks before bringing up TTYs.
 *
 * We must ensure that all declared `task [S]` and `run [S]` jobs in
 * finit.conf, or *.conf in finit.d/, run to completion before we
 * finalize the bootstrap process by calling this function.
 */
static void finalize(void)
{
	/*
	 * Network stuff
	 */
	_d("Setting up networking ...");
	networking();
	umask(022);

	/* Hooks that rely on loopback, or basic networking being up. */
	_d("Calling all network up hooks ...");
	plugin_run_hooks(HOOK_NETWORK_UP);

	/*
	 * Start all tasks/services in the configured runlevel
	 */
	_d("Change to default runlevel, start all services ...");
	service_runlevel(cfglevel);

	/* Clean up bootstrap-only tasks/services that never started */
	_d("Clean up all bootstrap-only tasks/services ...");
	svc_prune_bootstrap();

	/* All services/tasks/inetd/etc. in configure runlevel have started */
	_d("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);
	service_step_all(SVC_TYPE_ANY);

	/*
	 * Run startup scripts in the runparts directory, if any.
	 */
	if (runparts && fisdir(runparts)) {
		_d("Running startup scripts in %s ...", runparts);
		run_parts(runparts, NULL);
		service_reload_dynamic();
	}

	/* Convenient SysV compat for when you just don't care ... */
	if (!access(FINIT_RC_LOCAL, X_OK)) {
		run_interactive(FINIT_RC_LOCAL, "Calling %s", FINIT_RC_LOCAL);
		service_reload_dynamic();
	}

	/* Hooks that should run at the very end */
	_d("Calling all system up hooks ...");
	plugin_run_hooks(HOOK_SYSTEM_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Enable silent mode before starting TTYs */
	_d("Going silent ...");
	log_silent();

	/* Delayed start of TTYs at bootstrap */
	_d("Launching all getty services ...");
	tty_runlevel();
}

int main(int argc, char* argv[])
{
	char *path;
	char cmd[256];
	int udev = 0;
	uev_t timer;	       /* Bootstrap timer, on timeout call finalize() */
	uev_ctx_t loop;

	/*
	 * finit/init/telinit client tool uses /dev/initctl pipe
	 * for compatibility but initctl client tool uses socket
	 */
	if (getpid() != 1)
		return client(argc, argv);

	/* Set up canvas */
	screen_init();

	/*
	 * In case of emergency.
	 */
	emergency_shell();

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Initalize event context.
	 */
	uev_init(&loop);
	ctx = &loop;

	/*
	 * Set the PATH early to something sane
	 */
	setenv("PATH", _PATH_STDPATH, 1);

	/*
	 * Mount base file system, kernel is assumed to run devtmpfs for /dev
	 */
	chdir("/");
	umask(0);
	mount("none", "/proc", "proc", 0, NULL);
	mount("none", "/sys", "sysfs", 0, NULL);
	if (fisdir("/proc/bus/usb"))
		mount("none", "/proc/bus/usb", "usbfs", 0, NULL);

	/*
	 * Parse kernel parameters, including log_init()
	 */
	conf_parse_cmdline();

	/*
	 * Load plugins early, finit.conf may contain references to
	 * features implemented by plugins.
	 */
	plugin_init(&loop, PLUGIN_PATH);

	/*
	 * Hello world.
	 */
	banner();

	/*
	 * Check file filesystems in /etc/fstab
	 */
	for (int pass = 1; pass < 10; pass++) {
		if (fsck(pass))
			break;
	}

	/*
	 * Initialize .conf system and load static /etc/finit.conf
	 * Also initializes global_rlimit[] for udevd, below.
	 */
	conf_init();

	/*
	 * Some non-embedded systems without an initramfs may not have /dev mounted yet
	 * If they do, check if system has udevadm and perform cleanup from initramfs
	 */
	if (!fismnt("/dev"))
		mount("udev", "/dev", "devtmpfs", MS_RELATIME, "size=10%,nr_inodes=61156,mode=755");
	else if (whichp("udevadm"))
		run_interactive("udevadm info --cleanup-db", "Cleaning up udev db");

	/* Some systems use /dev/pts */
	makedir("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620");

	/*
	 * Some systems rely on us to both create /dev/shm and, to mount
	 * a tmpfs there.  Any system with dbus needs shared memory, so
	 * mount it, unless its already mounted, but not if listed in
	 * the /etc/fstab file already.
	 */
	makedir("/dev/shm", 0755);
	if (!fismnt("/dev/shm") && !ismnt("/etc/fstab", "/dev/shm"))
		mount("shm", "/dev/shm", "tmpfs", 0, NULL);

	/*
	 * New tmpfs based /run for volatile runtime data
	 * For details, see http://lwn.net/Articles/436012/
	 */
	if (fisdir("/run") && !fismnt("/run"))
		mount("tmpfs", "/run", "tmpfs", MS_NODEV, "mode=0755,size=10%");
	umask(022);

	/*
	 * Populate /dev and prepare for runtime events from kernel.
	 */
	path = which("mdev");
	if (path) {
		/* Embedded Linux systems usually have BusyBox mdev */
		if (log_is_debug())
			touch("/dev/mdev.log");

		snprintf(cmd, sizeof(cmd), "%s -s", path);
	} else {
		/* Desktop and server distros usually have a variant of udev */
		path = which("udevd");
		if (!path)
			path = which("/lib/systemd/systemd-udevd");
		if (path) {
			udev = 1;

			/* Register udevd as a monitored service, started much later */
			snprintf(cmd, sizeof(cmd), "[12345] %s -- Device event manager daemon", path);
			if (service_register(SVC_TYPE_SERVICE, cmd, global_rlimit, NULL)) {
				_pe("Failed registering %s", path);
				udev = 0;
			}

			/* Start a temporary udevd instance to populate /dev  */
			snprintf(cmd, sizeof(cmd), "%s --daemon", path);
		}
	}

	if (path) {
		free(path);

		run_interactive(cmd, "Populating device tree");
		if (udev && whichp("udevadm")) {
			run("udevadm trigger --action=add --type=subsystems");
			run("udevadm trigger --action=add --type=devices");
			run("udevadm settle --timeout=120");

			/* Tell temporary udevd to exit, we'll start a monitored instance later */
			run("udevadm control --exit");
		}
	}

	/*
	 * Start built-in watchdog as soon as possible, if enabled
	 */
	wdogpid = watchdog(argv[0]);

	/*
	 * Mount filesystems
	 */
#ifdef REMOUNT_ROOTFS
	run("mount -n -o remount,rw /");
#endif
#ifdef SYSROOT
	mount(SYSROOT, "/", NULL, MS_MOVE, NULL);
#endif

	/* Debian has this little script to copy generated rules while the system was read-only */
	if (fexist("/lib/udev/udev-finish"))
		run_interactive("/lib/udev/udev-finish", "Finalizing udev");

	/* Bootstrap conditions, needed for hooks */
	cond_init();

	_d("Root FS up, calling hooks ...");
	plugin_run_hooks(HOOK_ROOTFS_UP);

	umask(0);
	if (run_interactive("mount -na", "Mounting filesystems"))
		plugin_run_hooks(HOOK_MOUNT_ERROR);

	run("swapon -ea");
	umask(0022);

	/* Base FS up, enable standard SysV init signals */
	sig_setup(&loop);

	/*
	 * Set up inotify watcher for /etc/finit.d and read all .conf
	 * files to figure out how to bootstrap the system.
	 */
	conf_monitor(&loop);

	_d("Base FS up, calling hooks ...");
	plugin_run_hooks(HOOK_BASEFS_UP);

	/*
	 * Initalize state machine and start all bootstrap tasks
	 * NOTE: no network available!
	 */
	sm_init(&sm);
	sm_step(&sm);

	/* Start new initctl API responder */
	api_init(&loop);

	/*
	 * Wait for all SVC_TYPE_RUNTASK to have completed their work in
	 * [S], or timeout, before calling finalize()
	 */
	_d("Starting bootstrap finalize timer ...");
	uev_timer_init(&loop, &timer, service_bootstrap_cb, finalize, 1000, 1000);

	/*
	 * Enter main loop to monior /dev/initctl and services
	 */
	_d("Entering main loop ...");
	return uev_run(&loop, 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
