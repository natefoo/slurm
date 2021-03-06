/*****************************************************************************\
 *  burst_buffer_info.c - get/print the burst buffer state information
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef HAVE_SYS_SYSLOG_H
#  include <sys/syslog.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/slurm_protocol_api.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

static void _get_size_str(char *buf, size_t buf_size, uint32_t num)
{
	uint32_t tmp32;

	if ((num == NO_VAL) || (num == INFINITE)) {
		snprintf(buf, buf_size, "INFINITE");
	} else if (num == 0) {
		snprintf(buf, buf_size, "0GB");
	} else if ((num & BB_SIZE_IN_NODES) != 0) {
		tmp32 = num & (~BB_SIZE_IN_NODES);
		snprintf(buf, buf_size, "%uN", tmp32);
	} else if ((num % (1024 * 1024)) == 0) {
		tmp32 = num / (1024 * 1024);
		snprintf(buf, buf_size, "%uPB", tmp32);
	} else if ((num % 1024) == 0) {
		tmp32 = num / 1024;
		snprintf(buf, buf_size, "%uTB", tmp32);
	} else {
		tmp32 = num;
		snprintf(buf, buf_size, "%uGB", tmp32);
	}
}

/*
 * slurm_load_burst_buffer_info - issue RPC to get slurm all burst buffer plugin
 *	information
 * IN burst_buffer_info_msg_pptr - place to store a burst buffer configuration
 *	pointer
 * RET 0 or a slurm error code
 * NOTE: free the response using slurm_free_burst_buffer_info_msg
 */
extern int slurm_load_burst_buffer_info(burst_buffer_info_msg_t **
					burst_buffer_info_msg_pptr)
{
	int rc;
	slurm_msg_t req_msg;
	slurm_msg_t resp_msg;

	slurm_msg_t_init(&req_msg);
	slurm_msg_t_init(&resp_msg);
	req_msg.msg_type = REQUEST_BURST_BUFFER_INFO;
	req_msg.data     = NULL;

	if (slurm_send_recv_controller_msg(&req_msg, &resp_msg) < 0)
		return SLURM_ERROR;

	switch (resp_msg.msg_type) {
	case RESPONSE_BURST_BUFFER_INFO:
		*burst_buffer_info_msg_pptr = (burst_buffer_info_msg_t *)
					      resp_msg.data;
		break;
	case RESPONSE_SLURM_RC:
		rc = ((return_code_msg_t *) resp_msg.data)->return_code;
		slurm_free_return_code_msg(resp_msg.data);
		if (rc)
			slurm_seterrno_ret(rc);
		*burst_buffer_info_msg_pptr = NULL;
		break;
	default:
		slurm_seterrno_ret(SLURM_UNEXPECTED_MSG_ERROR);
		break;
	}

	return SLURM_PROTOCOL_SUCCESS;
}

/*
 * slurm_print_burst_buffer_info_msg - output information about burst buffers
 *	based upon message as loaded using slurm_load_burst_buffer
 * IN out - file to write to
 * IN info_ptr - burst_buffer information message pointer
 * IN one_liner - print as a single line if true
 */
extern void slurm_print_burst_buffer_info_msg(FILE *out,
		 burst_buffer_info_msg_t *info_ptr, int one_liner)
{
	int i;
	burst_buffer_info_t *burst_buffer_ptr;

	if (info_ptr->record_count == 0) {
		error("No burst buffer information available");
		return;
	}

	for (i = 0, burst_buffer_ptr = info_ptr->burst_buffer_array;
	     i < info_ptr->record_count; i++, burst_buffer_ptr++) {
		slurm_print_burst_buffer_record(out, burst_buffer_ptr,
						one_liner);
	}
}

static void _print_burst_buffer_resv(FILE *out,
				     burst_buffer_resv_t* burst_buffer_ptr,
				     int one_liner)
{
	char sz_buf[32], tmp_line[512];
	char *out_buf = NULL;

	/****** Line 1 ******/
	if (burst_buffer_ptr->name) {
		snprintf(tmp_line, sizeof(tmp_line),
			"    Name=%s ", burst_buffer_ptr->name);
	} else if (burst_buffer_ptr->array_task_id == NO_VAL) {
		snprintf(tmp_line, sizeof(tmp_line),
			"    JobID=%u ", burst_buffer_ptr->job_id);
	} else {
		snprintf(tmp_line, sizeof(tmp_line),
			"    JobID=%u.%u(%u) ",
			burst_buffer_ptr->array_job_id,
		        burst_buffer_ptr->array_task_id,
		        burst_buffer_ptr->job_id);
	}
	xstrcat(out_buf, tmp_line);
	_get_size_str(sz_buf, sizeof(sz_buf), burst_buffer_ptr->size);
	snprintf(tmp_line, sizeof(tmp_line),
		"Size=%s State=%s UserID=%s(%u)",
		sz_buf, bb_state_string(burst_buffer_ptr->state),
	        uid_to_string(burst_buffer_ptr->user_id),
	        burst_buffer_ptr->user_id);
	xstrcat(out_buf, tmp_line);

	xstrcat(out_buf, "\n");
	fprintf(out, "%s", out_buf);
	xfree(out_buf);
}

/*
 * slurm_print_burst_buffer_record - output information about a specific Slurm
 *	burst_buffer record based upon message as loaded using
 *	slurm_load_burst_buffer_info()
 * IN out - file to write to
 * IN burst_buffer_ptr - an individual burst buffer record pointer
 * IN one_liner - print as a single line if not zero
 * RET out - char * containing formatted output (must be freed after call)
 *	   NULL is returned on failure.
 */
extern void slurm_print_burst_buffer_record(FILE *out,
		burst_buffer_info_t *burst_buffer_ptr, int one_liner)
{
	char tmp_line[512], j_sz_buf[32], t_sz_buf[32], u_sz_buf[32];
	char *out_buf = NULL;
	burst_buffer_resv_t *bb_resv_ptr;
	bool has_acl = false;
	int i;

	/****** Line 1 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"Name=%s StagedInPrioBoost=%u",
		burst_buffer_ptr->name, burst_buffer_ptr->prio_boost);
	xstrcat(out_buf, tmp_line);
	if (one_liner)
		xstrcat(out_buf, " ");
	else
		xstrcat(out_buf, "\n  ");

	/****** Line 2 ******/
	_get_size_str(j_sz_buf, sizeof(j_sz_buf),
		      burst_buffer_ptr->job_size_limit);
	_get_size_str(t_sz_buf, sizeof(t_sz_buf),
		      burst_buffer_ptr->total_space);
	_get_size_str(u_sz_buf, sizeof(u_sz_buf),
		      burst_buffer_ptr->user_size_limit);
	snprintf(tmp_line, sizeof(tmp_line),
		"TotalSpace=%s JobSizeLimit=%s UserSizeLimit=%s",
		t_sz_buf, j_sz_buf, u_sz_buf);
	xstrcat(out_buf, tmp_line);
	if (one_liner)
		xstrcat(out_buf, " ");
	else
		xstrcat(out_buf, "\n  ");

	/****** Line 3 (optional) ******/
	if (burst_buffer_ptr->allow_users) {
		snprintf(tmp_line, sizeof(tmp_line),
			"AllowUsers=%s", burst_buffer_ptr->allow_users);
		xstrcat(out_buf, tmp_line);
		has_acl = true;
	} else if (burst_buffer_ptr->deny_users) {
		snprintf(tmp_line, sizeof(tmp_line),
			"DenyUsers=%s", burst_buffer_ptr->deny_users);
		xstrcat(out_buf, tmp_line);
		has_acl = true;
	}
	if (has_acl) {
		if (one_liner)
			xstrcat(out_buf, " ");
		else
			xstrcat(out_buf, "\n  ");
	}

	/****** Line 4 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"GetSysState=%s", burst_buffer_ptr->get_sys_state);
	xstrcat(out_buf, tmp_line);
	if (one_liner)
		xstrcat(out_buf, " ");
	else
		xstrcat(out_buf, "\n  ");

	/****** Line 5 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"StartStageIn=%s", burst_buffer_ptr->start_stage_in);
	xstrcat(out_buf, tmp_line);
	if (one_liner)
		xstrcat(out_buf, " ");
	else
		xstrcat(out_buf, "\n  ");

	/****** Line 6 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"StartStageIn=%s", burst_buffer_ptr->start_stage_out);
	xstrcat(out_buf, tmp_line);
	if (one_liner)
		xstrcat(out_buf, " ");
	else
		xstrcat(out_buf, "\n  ");

	/****** Line 7 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"StopStageIn=%s", burst_buffer_ptr->stop_stage_in);
	xstrcat(out_buf, tmp_line);
	if (one_liner)
		xstrcat(out_buf, " ");
	else
		xstrcat(out_buf, "\n  ");

	/****** Line 8 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"StopStageIn=%s", burst_buffer_ptr->stop_stage_out);
	xstrcat(out_buf, tmp_line);
	xstrcat(out_buf, "\n");

	fprintf(out, "%s", out_buf);
	xfree(out_buf);

	for (i = 0, bb_resv_ptr = burst_buffer_ptr->burst_buffer_resv_ptr;
	     i < burst_buffer_ptr->record_count; i++, bb_resv_ptr++) {
		 _print_burst_buffer_resv(out, bb_resv_ptr, one_liner);
	}
}
