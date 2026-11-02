/* Exercise deadlines without opening hardware, D-Bus or a filesystem dump. */
#define _GNU_SOURCE
#define main watchdog_program_main
#include "onekvm-watchdog.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static void check_tick(struct watchdog watch, int expected)
{
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		on_tick(NULL, 0, &watch);
		_exit(0);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

int main(void)
{
	uint64_t now = monotonic_ns();
	struct watchdog watch = {
		.hw_fd = -1, .hardware_reset = true, .timeout_sec = 1,
		.startup_timeout_sec = 1, .started_ns = now - 2000000000ull,
	};
	check_tick(watch, 4);
	watch.started_ns = now;
	check_tick(watch, 0);
	watch.armed = true;
	watch.last_feed_ns = now - 2000000000ull;
	check_tick(watch, 4);
	watch.last_feed_ns = now;
	check_tick(watch, 0);
	return 0;
}
