/*****************************************************************************\
 *  burst_buffer.c - driver for burst buffer infrastructure and plugin
 *****************************************************************************
 *  Copyright (C) 2014 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#  if STDC_HEADERS
#    include <string.h>
#  endif
#  if HAVE_SYS_TYPES_H
#    include <sys/types.h>
#  endif /* HAVE_SYS_TYPES_H */
#  if HAVE_UNISTD_H
#    include <unistd.h>
#  endif
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  else /* ! HAVE_INTTYPES_H */
#    if HAVE_STDINT_H
#      include <stdint.h>
#    endif
#  endif /* HAVE_INTTYPES_H */
#else /* ! HAVE_CONFIG_H */
#  include <sys/types.h>
#  include <unistd.h>
#  include <stdint.h>
#  include <string.h>
#endif /* HAVE_CONFIG_H */
#include <stdio.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/pack.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/burst_buffer.h"
#include "src/slurmctld/slurmctld.h"

typedef struct slurm_bb_ops {
	int		(*load_state)	(bool init_config);
	int		(*state_pack)	(Buf buffer, uint16_t protocol_version);
	int		(*reconfig)	(void);
	int		(*job_validate)	(struct job_descriptor *job_desc,
					 uid_t submit_uid);
	int		(*job_try_stage_in) (List job_queue);
	int		(*job_test_stage_in) (struct job_record *job_ptr);
	int		(*job_start_stage_out) (struct job_record *job_ptr);
	int		(*job_test_stage_out) (struct job_record *job_ptr);
} slurm_bb_ops_t;

/*
 * Must be synchronized with slurm_bb_ops_t above.
 */
static const char *syms[] = {
	"bb_p_load_state",
	"bb_p_state_pack",
	"bb_p_reconfig",
	"bb_p_job_validate",
	"bb_p_job_try_stage_in",
	"bb_p_job_test_stage_in",
	"bb_p_job_start_stage_out",
	"bb_p_job_test_stage_out"
};

static int g_context_cnt = -1;
static slurm_bb_ops_t *ops = NULL;
static plugin_context_t **g_context = NULL;
static char *bb_plugin_list = NULL;
static pthread_mutex_t g_context_lock = PTHREAD_MUTEX_INITIALIZER;
static bool init_run = false;

/*
 * Initialize the burst buffer infrastructure.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_init(void)
{
	int rc = SLURM_SUCCESS;
	char *last = NULL, *names;
	char *plugin_type = "burst_buffer";
	char *type;

	if (init_run && (g_context_cnt >= 0))
		return rc;

	slurm_mutex_lock(&g_context_lock);
	if (g_context_cnt >= 0)
		goto fini;

	bb_plugin_list = slurm_get_bb_type();
	g_context_cnt = 0;
	if ((bb_plugin_list == NULL) || (bb_plugin_list[0] == '\0'))
		goto fini;

	names = bb_plugin_list;
	while ((type = strtok_r(names, ",", &last))) {
		xrealloc(ops, (sizeof(slurm_bb_ops_t) * (g_context_cnt + 1)));
		xrealloc(g_context,
			 (sizeof(plugin_context_t *) * (g_context_cnt + 1)));
		if (strncmp(type, "burst_buffer/", 13) == 0)
			type += 13; /* backward compatibility */
		type = xstrdup_printf("burst_buffer/%s", type);
		g_context[g_context_cnt] = plugin_context_create(
			plugin_type, type, (void **)&ops[g_context_cnt],
			syms, sizeof(syms));
		if (!g_context[g_context_cnt]) {
			error("cannot create %s context for %s",
			      plugin_type, type);
			rc = SLURM_ERROR;
			xfree(type);
			break;
		}

		xfree(type);
		g_context_cnt++;
		names = NULL; /* for next iteration */
	}
	init_run = true;

fini:
	slurm_mutex_unlock(&g_context_lock);

	if (rc != SLURM_SUCCESS)
		bb_g_fini();

	return rc;
}

/*
 * Terminate the burst buffer infrastructure. Free memory.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_fini(void)
{
	int i, j, rc = SLURM_SUCCESS;

	slurm_mutex_lock(&g_context_lock);
	if (g_context_cnt < 0)
		goto fini;

	init_run = false;
	for (i = 0; i < g_context_cnt; i++) {
		if (g_context[i]) {
			j = plugin_context_destroy(g_context[i]);
			if (j != SLURM_SUCCESS)
				rc = j;
		}
	}
	xfree(ops);
	xfree(g_context);
	xfree(bb_plugin_list);
	g_context_cnt = -1;

fini:	slurm_mutex_unlock(&g_context_lock);
	return rc;
}

/*
 **************************************************************************
 *                          P L U G I N   C A L L S                       *
 **************************************************************************
 */

/*
 * Load the current burst buffer state (e.g. how much space is available now).
 * Run at the beginning of each scheduling cycle in order to recognize external
 * changes to the burst buffer state (e.g. capacity is added, removed, fails,
 * etc.).
 *
 * init_config IN - true if called as part of slurmctld initialization
 * Returns a SLURM errno.
 */
extern int bb_g_load_state(bool init_config)
{
	DEF_TIMERS;
	int i, rc, rc2;

	START_TIMER;
	rc = bb_g_init();
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; ((i < g_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		rc2 = (*(ops[i].load_state))(init_config);
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	END_TIMER2(__func__);

	return rc;
}

/*
 * Pack current burst buffer state information for network transmission to
 * user (e.g. "scontrol show burst")
 *
 * Returns a SLURM errno.
 */
extern int bb_g_state_pack(Buf buffer, uint16_t protocol_version)
{
	DEF_TIMERS;
	int i, rc, rc2;
	uint32_t rec_count = 0;
	int eof, last_offset, offset;

	START_TIMER;
	offset = get_buf_offset(buffer);
	pack32(rec_count, buffer);
	rc = bb_g_init();
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; i < g_context_cnt; i++) {
		last_offset = get_buf_offset(buffer);
		rc2 = (*(ops[i].state_pack))(buffer, protocol_version);
		if (last_offset != get_buf_offset(buffer))
			rec_count++;
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	if (rec_count != 0) {
		eof = get_buf_offset(buffer);
		set_buf_offset(buffer, offset);
		pack32(rec_count, buffer);
		set_buf_offset(buffer, eof);
	}
	END_TIMER2(__func__);

	return rc;
}

/*
 * Note configuration may have changed. Handle changes in BurstBufferParameters.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_reconfig(void)
{
	DEF_TIMERS;
	int i, rc, rc2;

	START_TIMER;
	rc = bb_g_init();
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; ((i < g_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		rc2 = (*(ops[i].reconfig))();
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	END_TIMER2(__func__);

	return rc;
}

/*
 * Validate a job submit request with respect to burst buffer options.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_job_validate(struct job_descriptor *job_desc,
			     uid_t submit_uid)
{
	DEF_TIMERS;
	int i, rc, rc2;

	START_TIMER;
	rc = bb_g_init();
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; i < g_context_cnt; i++) {
		rc2 = (*(ops[i].job_validate))(job_desc, submit_uid);
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	END_TIMER2(__func__);

	return rc;
}

/* sort jobs by expected start time */
extern int _sort_job_queue(void *x, void *y)
{
	struct job_record *job_ptr1 = x;
	struct job_record *job_ptr2 = y;
	time_t t1, t2;

	t1 = job_ptr1->start_time;
	t2 = job_ptr2->start_time;
	if (t1 > t2)
		return 1;
	if (t1 < t2)
		return -1;
	return 0;
}

/*
 * Allocate burst buffers to jobs expected to start soonest
 * Job records must be read locked
 *
 * Returns a SLURM errno.
 */
extern int bb_g_job_try_stage_in(void)
{
	DEF_TIMERS;
	int i, rc = 1, rc2;
	ListIterator job_iterator;
	struct job_record *job_ptr;
	time_t now = time(NULL);
	List job_queue;

	START_TIMER;
	job_queue = list_create(NULL);
	job_iterator = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (!IS_JOB_PENDING(job_ptr))
			continue;
		if ((job_ptr->burst_buffer == NULL) ||
		    (job_ptr->burst_buffer[0] == '\0'))
			continue;
		if ((job_ptr->start_time == 0) ||
		    (job_ptr->start_time > now + 10 * 60 * 60))	/* ten hours */
			continue;
		list_push(job_queue, job_ptr);
	}
	list_iterator_destroy(job_iterator);
	list_sort(job_queue, _sort_job_queue);

	rc = bb_g_init();
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; i < g_context_cnt; i++) {
		rc2 = (*(ops[i].job_try_stage_in))(job_queue);
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	list_destroy(job_queue);
	END_TIMER2(__func__);

	return rc;
}

/*
 * Determine if a job's burst buffer stage-in is complete
 *
 * RET: 0 - stage-in is underway
 *      1 - stage-in complete
 *     -1 - fatal error
 */
extern int bb_g_job_test_stage_in(struct job_record *job_ptr)
{
	DEF_TIMERS;
	int i, rc = 1, rc2;

	START_TIMER;
	if (bb_g_init() != SLURM_SUCCESS)
		rc = -1;
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; i < g_context_cnt; i++) {
		rc2 = (*(ops[i].job_test_stage_in))(job_ptr);
		rc = MIN(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	END_TIMER2(__func__);

	return rc;
}

/*
 * Trigger a job's burst buffer stage-out to begin
 *
 * Returns a SLURM errno.
 */
extern int bb_g_job_start_stage_out(struct job_record *job_ptr)
{
	DEF_TIMERS;
	int i, rc, rc2;

	START_TIMER;
	rc = bb_g_init();
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; i < g_context_cnt; i++) {
		rc2 = (*(ops[i].job_start_stage_out))(job_ptr);
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	END_TIMER2(__func__);

	return rc;
}

/*
 * Determine if a job's burst buffer stage-out is complete
 *
 * RET: 0 - stage-out is underway
 *      1 - stage-out complete
 *     -1 - fatal error
 */
extern int bb_g_job_test_stage_out(struct job_record *job_ptr)
{
	DEF_TIMERS;
	int i, rc = 1, rc2;

	START_TIMER;
	if (bb_g_init() != SLURM_SUCCESS)
		rc = -1;
	slurm_mutex_lock(&g_context_lock);
	for (i = 0; i < g_context_cnt; i++) {
		rc2 = (*(ops[i].job_test_stage_out))(job_ptr);
		rc = MIN(rc, rc2);
	}
	slurm_mutex_unlock(&g_context_lock);
	END_TIMER2(__func__);

	return rc;
}
