#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_MATCH \
	"type='signal',sender='org.onekvm.Core1'," \
	"path='/org/onekvm/Core1/Watchdog'," \
	"interface='org.onekvm.Core1.Watchdog',member='Heartbeat'"

#define DEFAULT_TIMEOUT_SEC 60
#define DEFAULT_HW_TIMEOUT_SEC 60
#define DEFAULT_TICK_SEC 5
#define DEFAULT_REBOOT_WINDOW_SEC 900
#define DEFAULT_REBOOT_LIMIT 3
#define DEFAULT_DUMP_PATH "/var/log/onekvm/watchdog-last.dump"
#define DEFAULT_BUDGET_PATH "/var/lib/onekvm/watchdog-reboots"

struct watchdog {
	sd_event *event;
	sd_bus *bus;
	bool armed;
	bool hardware_reset;
	unsigned startup_timeout_sec;
	uint64_t started_ns;
	bool dump_stack;
	bool reboot;
	bool disarm_hw_only;
	int hw_fd;
	unsigned timeout_sec;
	unsigned hw_timeout_sec;
	unsigned reboot_window_sec;
	unsigned reboot_limit;
	uint64_t last_feed_ns;
	uint64_t healthy_since_ns;
	uint64_t last_sequence;
	bool budget_cleared;
	char dump_path[PATH_MAX];
	char budget_path[PATH_MAX];
};

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void log_line(const char *message)
{
	fprintf(stderr, "onekvm-watchdog: %s\n", message);
}

static int ping_hw(struct watchdog *watch)
{
	int dummy = 0;

	if (watch == NULL || watch->hw_fd < 0)
		return 0;
	if (ioctl(watch->hw_fd, WDIOC_KEEPALIVE, &dummy) < 0) {
		log_line("hw keepalive failed");
		return -1;
	}
	return 0;
}

static void close_hw(struct watchdog *watch, bool magic)
{
	int flags = WDIOS_DISABLECARD;

	if (watch == NULL || watch->hw_fd < 0)
		return;
	if (magic) {
		if (ioctl(watch->hw_fd, WDIOC_SETOPTIONS, &flags) < 0)
			log_line("WDIOC_SETOPTIONS WDIOS_DISABLECARD failed");
		if (write(watch->hw_fd, "V", 1) != 1)
			log_line("magic close write failed");
	}
	if (close(watch->hw_fd) != 0)
		log_line("close /dev/watchdog failed");
	watch->hw_fd = -1;
	if (magic)
		log_line("hardware watchdog disarmed");
}

static int arm_hw(struct watchdog *watch)
{
	static const char *const nodes[] = { "/dev/watchdog0", "/dev/watchdog" };
	unsigned int timeout;
	unsigned int i;
	char line[96];

	if (watch == NULL || watch->hw_fd >= 0)
		return 0;
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		watch->hw_fd = open(nodes[i], O_WRONLY | O_CLOEXEC);
		if (watch->hw_fd >= 0)
			break;
		if (errno == ENOENT || errno == ENODEV)
			continue;
		if (errno == EBUSY) {
			log_line("hw watchdog busy (already owned)");
			watch->hw_fd = -1;
			return 0;
		}
		log_line("open /dev/watchdog failed");
		watch->hw_fd = -1;
		return -1;
	}
	if (watch->hw_fd < 0) {
		log_line("no /dev/watchdog; hardware feeding disabled");
		return 0;
	}

	timeout = watch->hw_timeout_sec;
	if (ioctl(watch->hw_fd, WDIOC_SETTIMEOUT, &timeout) < 0)
		log_line("WDIOC_SETTIMEOUT failed; using device default");
	timeout = 0;
	if (ioctl(watch->hw_fd, WDIOC_GETTIMEOUT, &timeout) == 0) {
		if (timeout != watch->hw_timeout_sec) {
			snprintf(line, sizeof(line),
				 "hardware watchdog rounded timeout %u s -> %u s",
				 watch->hw_timeout_sec, timeout);
			log_line(line);
		}
		watch->hw_timeout_sec = timeout;
		snprintf(line, sizeof(line), "feeding hardware watchdog timeout=%u s",
			 timeout);
		log_line(line);
	} else {
		log_line("feeding hardware watchdog");
	}
	return ping_hw(watch);
}

static int copy_proc_file(FILE *out, pid_t pid, const char *name)
{
	char path[64];
	char buffer[512];
	FILE *in;
	size_t n;

	if (snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, name) >= (int)sizeof(path))
		return -1;
	in = fopen(path, "r");
	if (in == NULL) {
		fprintf(out, "--- %s: %s ---\n", name, strerror(errno));
		return -1;
	}
	fprintf(out, "--- /proc/%d/%s ---\n", (int)pid, name);
	while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
		if (fwrite(buffer, 1, n, out) != n)
			break;
	}
	if (n == 0 || buffer[n - 1] != '\n')
		fputc('\n', out);
	fclose(in);
	return 0;
}

static void dump_one_process(FILE *out, pid_t pid, const char *banner)
{
	char task_dir[64];
	DIR *tasks;
	struct dirent *entry;

	fprintf(out, "===== %s pid %d =====\n", banner, (int)pid);
	copy_proc_file(out, pid, "status");
	copy_proc_file(out, pid, "wchan");
	copy_proc_file(out, pid, "syscall");
	copy_proc_file(out, pid, "stack");
	if (snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", (int)pid) >= (int)sizeof(task_dir))
		return;
	tasks = opendir(task_dir);
	if (tasks == NULL)
		return;
	while ((entry = readdir(tasks)) != NULL) {
		char stack_rel[64];
		pid_t tid;

		if (entry->d_name[0] == '.')
			continue;
		tid = (pid_t)atoi(entry->d_name);
		if (tid <= 0)
			continue;
		if (snprintf(stack_rel, sizeof(stack_rel), "task/%s/stack", entry->d_name)
		    >= (int)sizeof(stack_rel))
			continue;
		copy_proc_file(out, pid, stack_rel);
		(void)tid;
	}
	closedir(tasks);
}

static int task_is_disk_sleep(pid_t pid)
{
	char path[64];
	char line[256];
	FILE *file;
	int disk = 0;

	if (snprintf(path, sizeof(path), "/proc/%d/status", (int)pid) >= (int)sizeof(path))
		return 0;
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "State:", 6) == 0) {
			/* "State:\tD (disk sleep)". The word "disk" is lowercase. */
			if (strchr(line, 'D') != NULL)
				disk = 1;
			break;
		}
	}
	fclose(file);
	return disk;
}

static void dump_disk_sleep(FILE *out)
{
	DIR *proc;
	struct dirent *entry;
	unsigned count = 0;

	proc = opendir("/proc");
	if (proc == NULL)
		return;
	fprintf(out, "===== D-state processes =====\n");
	while ((entry = readdir(proc)) != NULL && count < 40) {
		char comm_path[64];
		char comm[64];
		FILE *comm_file;
		pid_t pid;

		if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
			continue;
		pid = (pid_t)atoi(entry->d_name);
		if (pid <= 0 || !task_is_disk_sleep(pid))
			continue;
		if (snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid)
		    >= (int)sizeof(comm_path))
			continue;
		comm_file = fopen(comm_path, "r");
		if (comm_file == NULL)
			continue;
		if (fgets(comm, sizeof(comm), comm_file) == NULL) {
			fclose(comm_file);
			continue;
		}
		fclose(comm_file);
		comm[strcspn(comm, "\n")] = '\0';
		if (strcmp(comm, "onekvm-server") == 0)
			continue;
		dump_one_process(out, pid, comm);
		count++;
	}
	closedir(proc);
	if (count == 0)
		fprintf(out, "none\n");
}

static void write_stamp(FILE *out, const char *reason)
{
	struct timespec real;
	FILE *uptime;
	char buf[80];

	fprintf(out, "onekvm-watchdog: %s\n", reason != NULL ? reason : "heartbeat timeout");
	fprintf(out,
		"Replaced only while this process still runs. "
		"A CPU stuck in one MMIO load does not update this file; "
		"the hardware watchdog resets the board instead.\n");
	if (clock_gettime(CLOCK_REALTIME, &real) == 0) {
		fprintf(out, "realtime: %lld.%09ld\n",
			(long long)real.tv_sec, real.tv_nsec);
	}
	uptime = fopen("/proc/uptime", "r");
	if (uptime != NULL) {
		if (fgets(buf, sizeof(buf), uptime) != NULL)
			fprintf(out, "uptime: %s", buf);
		fclose(uptime);
	}
}

static int dump_server_stacks(const char *path, const char *reason)
{
	DIR *proc;
	struct dirent *entry;
	FILE *out;
	char dir[PATH_MAX];
	char *slash;
	int found = 0;

	if (path == NULL || path[0] == '\0')
		return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		if (dir[0] != '\0' && mkdir(dir, 0755) != 0 && errno != EEXIST)
			return -1;
	}
	out = fopen(path, "w");
	if (out == NULL)
		return -1;
	write_stamp(out, reason);
	proc = opendir("/proc");
	if (proc == NULL) {
		fclose(out);
		return -1;
	}
	while ((entry = readdir(proc)) != NULL) {
		char comm_path[64];
		char comm[32];
		FILE *comm_file;
		pid_t pid;

		if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
			continue;
		pid = (pid_t)atoi(entry->d_name);
		if (pid <= 0)
			continue;
		if (snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid)
		    >= (int)sizeof(comm_path))
			continue;
		comm_file = fopen(comm_path, "r");
		if (comm_file == NULL)
			continue;
		if (fgets(comm, sizeof(comm), comm_file) == NULL) {
			fclose(comm_file);
			continue;
		}
		fclose(comm_file);
		if (strncmp(comm, "onekvm-server", 13) != 0)
			continue;
		dump_one_process(out, pid, "onekvm-server");
		found++;
	}
	closedir(proc);
	if (found == 0)
		fprintf(out, "no onekvm-server process found\n");
	dump_disk_sleep(out);
	if (fflush(out) != 0) {
		fclose(out);
		return -1;
	}
	{
		int fd = fileno(out);

		if (fd >= 0 && fsync(fd) != 0) {
			fclose(out);
			return -1;
		}
	}
	if (fclose(out) != 0)
		return -1;
	return 0;
}

static int ensure_parent_dir(const char *path)
{
	char dir[PATH_MAX];
	char *slash;

	if (path == NULL || path[0] == '\0')
		return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash == NULL)
		return 0;
	*slash = '\0';
	if (dir[0] == '\0')
		return 0;
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* File holds one integer. Missing file is 0. Existing but unreadable is error. */
static int load_reboot_count(const char *path, unsigned *count)
{
	FILE *file;
	unsigned long value = 0;

	if (path == NULL || path[0] == '\0' || count == NULL)
		return -1;
	file = fopen(path, "r");
	if (file == NULL) {
		if (errno == ENOENT) {
			*count = 0;
			return 0;
		}
		return -1;
	}
	if (fscanf(file, "%lu", &value) != 1) {
		fclose(file);
		return -1;
	}
	fclose(file);
	if (value > 100)
		return -1;
	*count = (unsigned)value;
	return 0;
}

static int store_reboot_count(const char *path, unsigned count)
{
	char tmp[PATH_MAX];
	FILE *file;
	int fd;

	if (ensure_parent_dir(path) != 0)
		return -1;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;
	file = fopen(tmp, "w");
	if (file == NULL)
		return -1;
	if (fprintf(file, "%u\n", count) < 0) {
		fclose(file);
		unlink(tmp);
		return -1;
	}
	if (fflush(file) != 0) {
		fclose(file);
		unlink(tmp);
		return -1;
	}
	fd = fileno(file);
	if (fd >= 0 && fsync(fd) != 0) {
		fclose(file);
		unlink(tmp);
		return -1;
	}
	if (fclose(file) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void force_reboot(void)
{
	sync();
	sync();
	execl("/sbin/reboot", "reboot", "-f", (char *)NULL);
	execl("/bin/busybox", "busybox", "reboot", "-f", (char *)NULL);
	execl("/bin/reboot", "reboot", "-f", (char *)NULL);
	log_line("reboot -f exec failed");
	_exit(1);
}

static int write_text(const char *path, const char *text)
{
	int fd;
	size_t len;
	ssize_t wrote;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	len = strlen(text);
	wrote = write(fd, text, len);
	if (close(fd) != 0)
		return -1;
	if (wrote < 0 || (size_t)wrote != len)
		return -1;
	return 0;
}

static void note_kmsg(const char *message)
{
	char line[192];
	int fd;
	size_t len;
	ssize_t wrote;

	if (snprintf(line, sizeof(line), "<2>onekvm-watchdog: %s\n", message)
	    >= (int)sizeof(line))
		return;
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	len = strlen(line);
	wrote = write(fd, line, len);
	if (wrote < 0 || (size_t)wrote != len)
		log_line("kmsg write failed");
	if (close(fd) != 0)
		log_line("kmsg close failed");
}

/* sysrq is often sync-only (16). Crash, task, and blocked-task keys need bit 0x8,
 * and the value 1 enables every key. panic_sys_info makes panic() print stacks. */
static void arm_panic_sysrq(void)
{
	if (write_text("/proc/sys/kernel/sysrq", "1\n") != 0)
		log_line("failed to enable sysrq");
	if (write_text("/proc/sys/kernel/panic_sys_info",
		       "tasks,all_bt,blocked_tasks") != 0)
		log_line("failed to set panic_sys_info");
}

static void sysrq_key(char key)
{
	char text[2];

	text[0] = key;
	text[1] = '\0';
	if (write_text("/proc/sysrq-trigger", text) != 0)
		log_line("sysrq trigger failed");
}

static int append_dmesg(const char *path)
{
	FILE *out;
	FILE *in;
	char buffer[512];
	size_t n;
	int fd;

	out = fopen(path, "a");
	if (out == NULL)
		return -1;
	fprintf(out, "===== dmesg =====\n");
	in = popen("dmesg", "r");
	if (in == NULL) {
		fprintf(out, "dmesg failed: %s\n", strerror(errno));
	} else {
		while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
			if (fwrite(buffer, 1, n, out) != n)
				break;
		}
		if (pclose(in) != 0)
			fprintf(out, "dmesg exit status non-zero\n");
	}
	if (fflush(out) != 0) {
		fclose(out);
		return -1;
	}
	fd = fileno(out);
	if (fd >= 0 && fsync(fd) != 0) {
		fclose(out);
		return -1;
	}
	if (fclose(out) != 0)
		return -1;
	return 0;
}

static void trigger_kernel_panic(const char *reason)
{
	arm_panic_sysrq();
	note_kmsg(reason);
	log_line(reason);
	/* panic() does not return. Hardware watchdog stays armed. */
	sysrq_key('c');
	log_line("sysrq panic returned; reboot -f");
	force_reboot();
}

static int on_heartbeat(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
	struct watchdog *watch = userdata;
	uint64_t sequence = 0;
	int result;

	(void)error;
	if (watch == NULL)
		return 0;
	result = sd_bus_message_read(message, "t", &sequence);
	if (result < 0)
		return 0;
	if (!watch->armed)
		watch->healthy_since_ns = monotonic_ns();
	watch->armed = true;
	watch->last_sequence = sequence;
	watch->last_feed_ns = monotonic_ns();
	return 0;
}

static int arm_tick(sd_event_source *source)
{
	uint64_t next = 0;
	int result;

	result = sd_event_now(sd_event_source_get_event(source), CLOCK_MONOTONIC, &next);
	if (result < 0)
		return result;
	next += (uint64_t)DEFAULT_TICK_SEC * 1000000ull;
	result = sd_event_source_set_time(source, next);
	if (result < 0)
		return result;
	return sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
}

static int on_tick(sd_event_source *source, uint64_t usec, void *userdata)
{
	struct watchdog *watch = userdata;
	uint64_t now;
	uint64_t idle_ns;
	char line[160];

	(void)usec;
	if (source != NULL)
		(void)arm_tick(source);
	if (watch == NULL)
		return 0;
	(void)ping_hw(watch);
	if (!watch->armed)
		return 0;
	now = monotonic_ns();
	if (now < watch->last_feed_ns)
		return 0;
	idle_ns = now - watch->last_feed_ns;
	if (idle_ns < (uint64_t)watch->timeout_sec * 1000000000ull) {
		if (!watch->budget_cleared &&
		    now >= watch->healthy_since_ns &&
		    now - watch->healthy_since_ns >=
			    (uint64_t)watch->reboot_window_sec * 1000000000ull) {
			if (store_reboot_count(watch->budget_path, 0) == 0)
				watch->budget_cleared = true;
		}
		return 0;
	}

	snprintf(line, sizeof(line),
		 "heartbeat timeout after %u s (last sequence %" PRIu64 "); kernel panic",
		 watch->timeout_sec, watch->last_sequence);
	log_line(line);
	if (watch->hardware_reset) {
		/* NFS may be wedged: do not write a dump or enter software reboot. */
		close_hw(watch, false);
		_exit(4);
	}

	if (watch->dump_stack) {
		if (dump_server_stacks(watch->dump_path, line) != 0)
			log_line("failed to dump onekvm-server stacks");
		else
			log_line("wrote stack dump");
	}
	if (!watch->reboot) {
		log_line("reboot disabled; exiting after timeout");
		close_hw(watch, true);
		sd_event_exit(watch->event, 2);
		return 0;
	}
	{
		unsigned count = 0;

		if (load_reboot_count(watch->budget_path, &count) != 0 ||
		    count >= watch->reboot_limit) {
			log_line("reboot budget exceeded; leaving the system running");
			close_hw(watch, true);
			sd_event_exit(watch->event, 3);
			return 0;
		}
		if (store_reboot_count(watch->budget_path, count + 1) != 0) {
			log_line("failed to persist reboot budget; not rebooting");
			close_hw(watch, true);
			sd_event_exit(watch->event, 3);
			return 0;
		}
	}
	/* Dump file is already fsynced. panic=10 reboots after the trace.
	 * A stalled panic, or a CPU stuck in one MMIO load, still expires
	 * the hardware timer. That load cannot run this path. */
	(void)ping_hw(watch);
	if (watch->dump_stack) {
		arm_panic_sysrq();
		sysrq_key('w');
		sysrq_key('t');
		sysrq_key('l');
		if (append_dmesg(watch->dump_path) != 0)
			log_line("failed to append dmesg");
	}
	trigger_kernel_panic(line);
	return 0;
}

static int disarm_hw(void)
{
	struct watchdog watch = { .hw_fd = -1, .hw_timeout_sec = DEFAULT_HW_TIMEOUT_SEC };

	if (arm_hw(&watch) != 0)
		return 1;
	close_hw(&watch, true);
	return 0;
}

static int on_signal(sd_event_source *source, const struct signalfd_siginfo *info,
		     void *userdata)
{
	struct watchdog *watch = userdata;

	(void)source;
	(void)info;
	log_line("stop requested; disarming hardware watchdog");
	close_hw(watch, true);
	sd_event_exit(watch->event, 0);
	return 0;
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: onekvm-watchdog [options]\n"
		"  --startup-timeout-sec N  deadline for the first Core heartbeat\n"
		"  --hardware-reset    expire the hardware timer without panic or filesystem I/O\n"
		"  --timeout-sec N     Core heartbeat silence before kernel panic (default %d)\n"
		"                      HDMI off / 0 capture FPS is not a timeout.\n"
		"  --hw-timeout-sec N  DesignWare timeout while this process feeds it (default %d)\n"
		"                      Must stay <= hardware max so the kernel does not ping.\n"
		"  --no-dump           skip /proc stack dump on timeout\n"
		"  --no-reboot         do not panic; exit 2 on timeout\n"
		"  --disarm-hw         stop DesignWare WDT and exit (systemctl stop)\n"
		"  --dump-path PATH    stack dump file\n"
		"  --budget-path PATH  persistent reboot count file\n"
		"  --reboot-limit N    max reboots in the window (default %d)\n"
		"  --reboot-window N   reboot budget window seconds (default %d)\n",
		DEFAULT_TIMEOUT_SEC, DEFAULT_HW_TIMEOUT_SEC,
		DEFAULT_REBOOT_LIMIT, DEFAULT_REBOOT_WINDOW_SEC);
}

static int parse_args(struct watchdog *watch, int argc, char **argv)
{
	static const struct option options[] = {
		{"timeout-sec", required_argument, NULL, 't'},
		{"hw-timeout-sec", required_argument, NULL, 'T'},
		{"no-dump", no_argument, NULL, 'D'},
		{"no-reboot", no_argument, NULL, 'R'},
		{"disarm-hw", no_argument, NULL, 'H'},
		{"dump-path", required_argument, NULL, 'd'},
		{"budget-path", required_argument, NULL, 'b'},
		{"reboot-limit", required_argument, NULL, 'l'},
		{"reboot-window", required_argument, NULL, 'w'},
		{"startup-timeout-sec", required_argument, NULL, 'S'},
		{"hardware-reset", no_argument, NULL, 'X'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "t:T:h", options, NULL)) != -1) {
		unsigned long value;
		char *end = NULL;

		switch (opt) {
		case 'X':
			watch->hardware_reset = true;
			break;
		case 'S':
			errno = 0;
			value = strtoul(optarg, &end, 10);
			if (errno || !end || end == optarg || *end || !value || value > 3600)
				return -1;
			watch->startup_timeout_sec = (unsigned)value;
			break;
		case 't':
			value = strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0' || value == 0 || value > 3600)
				return -1;
			watch->timeout_sec = (unsigned)value;
			break;
		case 'T':
			value = strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0' || value == 0 || value > 120)
				return -1;
			watch->hw_timeout_sec = (unsigned)value;
			break;
		case 'D':
			watch->dump_stack = false;
			break;
		case 'R':
			watch->reboot = false;
			break;
		case 'H':
			watch->disarm_hw_only = true;
			break;
		case 'd':
			if (snprintf(watch->dump_path, sizeof(watch->dump_path), "%s", optarg)
			    >= (int)sizeof(watch->dump_path))
				return -1;
			break;
		case 'b':
			if (snprintf(watch->budget_path, sizeof(watch->budget_path), "%s", optarg)
			    >= (int)sizeof(watch->budget_path))
				return -1;
			break;
		case 'l':
			value = strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0' || value == 0 || value > 100)
				return -1;
			watch->reboot_limit = (unsigned)value;
			break;
		case 'w':
			value = strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0' || value == 0 || value > 86400)
				return -1;
			watch->reboot_window_sec = (unsigned)value;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		default:
			return -1;
		}
	}
	return optind == argc ? 0 : -1;
}

int main(int argc, char **argv)
{
	struct watchdog watch = {
		.dump_stack = true,
		.reboot = true,
		.timeout_sec = DEFAULT_TIMEOUT_SEC,
		.hw_timeout_sec = DEFAULT_HW_TIMEOUT_SEC,
		.hw_fd = -1,
		.reboot_window_sec = DEFAULT_REBOOT_WINDOW_SEC,
		.reboot_limit = DEFAULT_REBOOT_LIMIT,
	};
	sd_event_source *timer = NULL;
	sigset_t mask;
	int result;

	snprintf(watch.dump_path, sizeof(watch.dump_path), "%s", DEFAULT_DUMP_PATH);
	snprintf(watch.budget_path, sizeof(watch.budget_path), "%s", DEFAULT_BUDGET_PATH);
	if (parse_args(&watch, argc, argv) != 0) {
		usage(stderr);
		return 2;
	}
	if (watch.disarm_hw_only)
		return disarm_hw();
	if (setpriority(PRIO_PROCESS, 0, -10) != 0)
		log_line("setpriority -10 failed");

	result = sd_event_default(&watch.event);
	if (result < 0) {
		log_line("sd_event_default failed");
		return 1;
	}
	result = sd_bus_default_system(&watch.bus);
	if (result < 0) {
		log_line("connect system bus failed");
		return 1;
	}
	result = sd_bus_match_signal(watch.bus, NULL, "org.onekvm.Core1",
				     "/org/onekvm/Core1/Watchdog",
				     "org.onekvm.Core1.Watchdog", "Heartbeat",
				     on_heartbeat, &watch);
	if (result < 0) {
		result = sd_bus_add_match(watch.bus, NULL, HEARTBEAT_MATCH, on_heartbeat, &watch);
		if (result < 0) {
			log_line("subscribe Heartbeat failed");
			return 1;
		}
	}
	result = sd_bus_attach_event(watch.bus, watch.event, 0);
	if (result < 0) {
		log_line("attach bus to event loop failed");
		return 1;
	}
	result = sd_event_add_time_relative(watch.event, &timer, CLOCK_MONOTONIC,
					    DEFAULT_TICK_SEC * 1000000ull,
					    100 * 1000ull, on_tick, &watch);
	if (result < 0) {
		log_line("add timer failed");
		return 1;
	}
	(void)sd_event_source_set_time_accuracy(timer, 100 * 1000ull);
	result = sd_event_source_set_enabled(timer, SD_EVENT_ONESHOT);
	if (result < 0) {
		log_line("enable timer failed");
		return 1;
	}
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
		log_line("block SIGTERM/SIGINT failed");
		close_hw(&watch, true);
		return 1;
	}
	result = sd_event_add_signal(watch.event, NULL, SIGTERM, on_signal, &watch);
	if (result < 0) {
		log_line("add SIGTERM handler failed");
		close_hw(&watch, true);
		return 1;
	}
	result = sd_event_add_signal(watch.event, NULL, SIGINT, on_signal, &watch);
	if (result < 0) {
		log_line("add SIGINT handler failed");
		close_hw(&watch, true);
		return 1;
	}
	if (arm_hw(&watch) != 0) {
		close_hw(&watch, true);
		return 1;
	}
	watch.started_ns = monotonic_ns();
	log_line("waiting for org.onekvm.Core1.Watchdog.Heartbeat");
	result = sd_event_loop(watch.event);
	close_hw(&watch, true);
	sd_event_source_unref(timer);
	sd_bus_unref(watch.bus);
	sd_event_unref(watch.event);
	return result < 0 ? 1 : result;
}
