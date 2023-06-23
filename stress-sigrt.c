/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-builtin.h"

static const stress_help_t help[] = {
	{ NULL,	"sigrt N",	"start N workers sending real time signals" },
	{ NULL,	"sigrt-ops N",	"stop after N real time signal bogo operations" },
	{ NULL,	NULL,		NULL }
};

#if defined(HAVE_SIGQUEUE) &&		\
    defined(HAVE_SIGWAITINFO) &&	\
    defined(SIGRTMIN) &&		\
    defined(SIGRTMAX)

#define MAX_RTPIDS (SIGRTMAX - SIGRTMIN + 1)

/*
 *  stress_sigrt
 *	stress by heavy real time sigqueue message sending
 */
static int stress_sigrt(const stress_args_t *args)
{
	pid_t *pids;
	union sigval s ALIGN64;
	int i, status, rc = EXIT_SUCCESS;
	stress_metrics_t *stress_sigrt_metrics;
	size_t stress_sigrt_metrics_size = sizeof(*stress_sigrt_metrics) * MAX_RTPIDS;
	double count, duration, rate;

	stress_sigrt_metrics = (stress_metrics_t *)
		mmap(NULL, stress_sigrt_metrics_size,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (stress_sigrt_metrics == MAP_FAILED) {
		pr_inf("%s: failed to mmap %zu bytes, skipping stressor\n",
			args->name, stress_sigrt_metrics_size);
		return EXIT_NO_RESOURCE;
	}
	pids = calloc((size_t)MAX_RTPIDS, sizeof(*pids));
	if (!pids) {
		pr_inf_skip("%s: cannot allocate array of %zd pids, skipping stressor\n",
			args->name, (size_t)MAX_RTPIDS);
		(void)munmap((void *)stress_sigrt_metrics, stress_sigrt_metrics_size);
		return EXIT_NO_RESOURCE;
	}

	for (i = 0; i < MAX_RTPIDS; i++) {
		stress_sigrt_metrics[i].duration = 0.0;
		stress_sigrt_metrics[i].count = 0.0;

		if (stress_sighandler(args->name, i + SIGRTMIN, stress_sighandler_nop, NULL) < 0) {
			free(pids);
			(void)munmap((void *)stress_sigrt_metrics, stress_sigrt_metrics_size);
			return EXIT_FAILURE;
		}
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	for (i = 0; i < MAX_RTPIDS; i++) {
again:
		pids[i] = fork();
		if (pids[i] < 0) {
			if (stress_redo_fork(errno))
				goto again;
			if (!keep_stressing(args))
				goto reap;
			pr_err("%s: fork failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto reap;
		} else if (pids[i] == 0) {
			sigset_t mask;
			siginfo_t info ALIGN64;
			int index;

			stress_parent_died_alarm();
			(void)sched_settings_apply(true);

			(void)sigemptyset(&mask);
			for (i = 0; i < MAX_RTPIDS; i++)
				(void)sigaddset(&mask, i + SIGRTMIN);

			(void)shim_memset(&info, 0, sizeof info);

			while (keep_stressing_flag()) {

				if (UNLIKELY(sigwaitinfo(&mask, &info) < 0)) {
					if (errno == EINTR)
						continue;
					break;
				}

				index = info.si_signo - SIGRTMIN;
				if ((index >= 0) && (index < SIGRTMIN)) {
					const double delta = stress_time_now() - stress_sigrt_metrics[index].t_start;
					if (delta > 0.0) {
						stress_sigrt_metrics[index].duration += delta;
						stress_sigrt_metrics[index].count += 1.0;
					}
				}
				if (UNLIKELY(info.si_value.sival_int == 0))
					break;

				if (info.si_value.sival_int != -1) {
					(void)shim_memset(&s, 0, sizeof(s));
					s.sival_int = -1;
					(void)sigqueue(info.si_value.sival_int, SIGRTMIN, s);
				}
			}
			_exit(0);
		}
	}

	/* Parent */
	do {
		(void)shim_memset(&s, 0, sizeof(s));

		for (i = 0; i < MAX_RTPIDS; i++) {
			const int pid = pids[i];

			/* Inform child which pid to queue a signal to */
			s.sival_int = pid;
			stress_sigrt_metrics[i].t_start = stress_time_now();

			if (UNLIKELY(sigqueue(pids[i], i + SIGRTMIN, s) < 0)) {
				if ((errno != EAGAIN) && (errno != EINTR)) {
					pr_fail("%s: sigqueue on signal %d failed, "
						"errno=%d (%s)\n",
						args->name, i + SIGRTMIN,
						errno, strerror(errno));
					rc = EXIT_FAILURE;
					break;
				}
			}
			inc_counter(args);
		}
	} while (keep_stressing(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	(void)shim_memset(&s, 0, sizeof(s));
	for (i = 0; i < MAX_RTPIDS; i++) {
		if (pids[i] > 0) {
			s.sival_int = 0;
			(void)sigqueue(pids[i], i + SIGRTMIN, s);
		}
	}
	(void)shim_usleep(250);
reap:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	for (i = 0; i < MAX_RTPIDS; i++) {
		if (pids[i] > 0) {
			/* And ensure child is really dead */
			(void)shim_kill(pids[i], SIGALRM);
			(void)shim_waitpid(pids[i], &status, 0);
		}
	}

	duration = 0.0;
	count = 0.0;
	for (i = 0; i < MAX_RTPIDS; i++) {
		duration += stress_sigrt_metrics[i].duration;
		count += stress_sigrt_metrics[i].count;
	}
	rate = (count > 0.0) ? duration / count : 0.0;
	stress_metrics_set(args, 0, "nanosecs between sigqueue and sigwaitinfo completion", rate * STRESS_DBL_NANOSECOND);

	free(pids);
	(void)munmap((void *)stress_sigrt_metrics, stress_sigrt_metrics_size);

	return rc;
}

stressor_info_t stress_sigrt_info = {
	.stressor = stress_sigrt,
	.class = CLASS_INTERRUPT | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_sigrt_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_INTERRUPT | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help,
	.unimplemented_reason = "built without sigqueue() or sigwaitinfo() or defined SIGRTMIN or SIGRTMAX"
};
#endif
