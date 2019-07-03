/*****************************************************************************\
 *  select_cons_res.c - node selection plugin supporting consumable
 *  resources policies.
 *****************************************************************************\
 *
 *  The following example below illustrates how four jobs are allocated
 *  across a cluster using when a processor consumable resource approach.
 *
 *  The example cluster is composed of 4 nodes (10 cpus in total):
 *  linux01 (with 2 processors),
 *  linux02 (with 2 processors),
 *  linux03 (with 2 processors), and
 *  linux04 (with 4 processors).
 *
 *  The four jobs are the following:
 *  1. srun -n 4 -N 4  sleep 120 &
 *  2. srun -n 3 -N 3 sleep 120 &
 *  3. srun -n 1 sleep 120 &
 *  4. srun -n 3 sleep 120 &
 *  The user launches them in the same order as listed above.
 *
 *  Using a processor consumable resource approach we get the following
 *  job allocation and scheduling:
 *
 *  The output of squeue shows that we have 3 out of the 4 jobs allocated
 *  and running. This is a 2 running job increase over the default Slurm
 *  approach.
 *
 *  Job 2, Job 3, and Job 4 are now running concurrently on the cluster.
 *
 *  [<snip>]# squeue
 *  JOBID PARTITION     NAME     USER  ST       TIME  NODES NODELIST(REASON)
 *     5        lsf    sleep     root  PD       0:00      1 (Resources)
 *     2        lsf    sleep     root   R       0:13      4 linux[01-04]
 *     3        lsf    sleep     root   R       0:09      3 linux[01-03]
 *     4        lsf    sleep     root   R       0:05      1 linux04
 *  [<snip>]#
 *
 *  Once Job 2 finishes, Job 5, which was pending, is allocated
 *  available resources and is then running as illustrated below:
 *
 *  [<snip>]# squeue
 *   JOBID PARTITION    NAME     USER  ST       TIME  NODES NODELIST(REASON)
 *     3        lsf    sleep     root   R       1:58      3 linux[01-03]
 *     4        lsf    sleep     root   R       1:54      1 linux04
 *     5        lsf    sleep     root   R       0:02      3 linux[01-03]
 *  [<snip>]#
 *
 *  Job 3, Job 4, and Job 5 are now running concurrently on the cluster.
 *
 *  [<snip>]#  squeue
 *  JOBID PARTITION     NAME     USER  ST       TIME  NODES NODELIST(REASON)
 *     5        lsf    sleep     root   R       1:52      3 linux[01-03]
 *  [<snip>]#
 *
 * The advantage of the consumable resource scheduling policy is that
 * the job throughput can increase dramatically.
 *
 *****************************************************************************
 *  Copyright (C) 2005-2008 Hewlett-Packard Development Company, L.P.
 *  Written by Susanne M. Balle <susanne.balle@hp.com>, who borrowed heavily
 *  from select/linear
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
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
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE

#include <inttypes.h>
#include <string.h>

#include "src/common/slurm_xlator.h"
#include "src/common/slurm_selecttype_info.h"
#include "src/common/assoc_mgr.h"
#include "src/common/xstring.h"
#include "select_cons_res.h"

#include "dist_tasks.h"
#include "job_test.h"

#define NODEINFO_MAGIC 0x82aa

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
extern slurm_ctl_conf_t slurmctld_conf __attribute__((weak_import));
extern struct node_record *node_record_table_ptr __attribute__((weak_import));
extern List part_list __attribute__((weak_import));
extern List job_list __attribute__((weak_import));
extern int node_record_count __attribute__((weak_import));
extern time_t last_node_update __attribute__((weak_import));
extern struct switch_record *switch_record_table __attribute__((weak_import));
extern int switch_record_cnt __attribute__((weak_import));
extern bitstr_t *avail_node_bitmap __attribute__((weak_import));
extern uint16_t *cr_node_num_cores __attribute__((weak_import));
extern uint32_t *cr_node_cores_offset __attribute__((weak_import));
extern int slurmctld_tres_cnt __attribute__((weak_import));
extern slurmctld_config_t slurmctld_config __attribute__((weak_import));
#else
slurm_ctl_conf_t slurmctld_conf;
struct node_record *node_record_table_ptr;
List part_list;
List job_list;
int node_record_count;
time_t last_node_update;
struct switch_record *switch_record_table;
int switch_record_cnt;
bitstr_t *avail_node_bitmap;
uint16_t *cr_node_num_cores;
uint32_t *cr_node_cores_offset;
int slurmctld_tres_cnt = 0;
slurmctld_config_t slurmctld_config;
#endif

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "select" for Slurm node selection) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load select plugins if the plugin_type string has a
 * prefix of "select/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "Consumable Resources (CR) Node Selection plugin";
const char *plugin_type = "select/cons_res";
const uint32_t plugin_id      = SELECT_PLUGIN_CONS_RES;
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
const uint32_t pstate_version = 7;	/* version control on saved state */

struct select_nodeinfo {
	uint16_t magic;		/* magic number */
	uint16_t alloc_cpus;
	uint64_t alloc_memory;
	uint64_t *tres_alloc_cnt;	/* array of tres counts allocated.
					   NOT PACKED */
	char     *tres_alloc_fmt_str;	/* formatted str of allocated tres */
	double    tres_alloc_weighted;	/* weighted number of tres allocated. */
};

extern select_nodeinfo_t *select_p_select_nodeinfo_alloc(void);
extern int select_p_select_nodeinfo_free(select_nodeinfo_t *nodeinfo);

/* Procedure Declarations */
static int _job_expand(struct job_record *from_job_ptr,
		       struct job_record *to_job_ptr);
static int _rm_job_from_one_node(struct job_record *job_ptr,
				 struct node_record *node_ptr);
static void _spec_core_filter(bitstr_t *node_bitmap, bitstr_t **core_bitmap);

struct sort_support {
	int jstart;
	struct job_resources *tmpjobs;
};
static int _compare_support(const void *, const void *);

static void _dump_job_res(struct job_resources *job) {
	char str[64];

	if (job->core_bitmap)
		bit_fmt(str, sizeof(str), job->core_bitmap);
	else
		sprintf(str, "[no core_bitmap]");
	info("DEBUG: Dump job_resources: nhosts %u cb %s", job->nhosts, str);
}

static void _add_job_to_cores(job_resources_t *job_resrcs_ptr,
			      struct part_row_data *r_ptr,
			      const uint16_t *bits_per_node)
{
	if (!r_ptr->row_bitmap) {
		r_ptr->row_bitmap_size = 1;
		r_ptr->row_bitmap = xcalloc(r_ptr->row_bitmap_size,
					    sizeof(bitstr_t *));
	}
	add_job_to_cores(job_resrcs_ptr, &r_ptr->row_bitmap[0], bits_per_node);
	r_ptr->first_row_bitmap = r_ptr->row_bitmap[0];
}

/* test for conflicting core_bitmap bits */
static int _can_job_fit_in_row(struct job_resources *job,
			       struct part_row_data *r_ptr)
{
	if ((r_ptr->num_jobs == 0) || !r_ptr->first_row_bitmap)
		return 1;

	return job_fits_into_cores(job, r_ptr->first_row_bitmap,
				   cr_node_num_cores);
}

/*
 * _build_row_bitmaps: A job has been removed from the given partition,
 *                     so the first_row_bitmap(s) need to be reconstructed.
 *                     Optimize the jobs into the least number of rows,
 *                     and make the lower rows as dense as possible.
 *
 * IN/OUT: p_ptr   - the partition that has jobs to be optimized
 */
static void _build_row_bitmaps(struct part_res_record *p_ptr,
			       struct job_record *job_ptr)
{
	uint32_t i, j, num_jobs, size;
	int x;
	struct part_row_data *this_row, *orig_row;
	struct sort_support *ss;

	if (!p_ptr->row)
		return;

	if (p_ptr->num_rows == 1) {
		this_row = &(p_ptr->row[0]);
		if (this_row->num_jobs == 0) {
			if (this_row->first_row_bitmap) {
				size = bit_size(this_row->first_row_bitmap);
				bit_nclear(this_row->first_row_bitmap, 0, size-1);
			}
		} else {
			if (job_ptr) { /* just remove the job */
				xassert(job_ptr->job_resrcs);
				remove_job_from_cores(job_ptr->job_resrcs,
						      &(this_row->first_row_bitmap),
						      cr_node_num_cores);
			} else { /* totally rebuild the bitmap */
				size = bit_size(this_row->first_row_bitmap);
				bit_nclear(this_row->first_row_bitmap, 0, size-1);
				for (j = 0; j < this_row->num_jobs; j++) {
					_add_job_to_cores(this_row->job_list[j],
							  this_row,
							  cr_node_num_cores);
				}
			}
		}
		return;
	}

	/* gather data */
	num_jobs = 0;
	for (i = 0; i < p_ptr->num_rows; i++) {
		num_jobs += p_ptr->row[i].num_jobs;
	}
	if (num_jobs == 0) {
		size = bit_size(p_ptr->row[0].first_row_bitmap);
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (p_ptr->row[i].first_row_bitmap) {
				bit_nclear(p_ptr->row[i].first_row_bitmap, 0,
					   size-1);
			}
		}
		return;
	}

	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		info("DEBUG: _build_row_bitmaps (before):");
		common_dump_parts(p_ptr);
	}
	debug3("cons_res: build_row_bitmaps reshuffling %u jobs", num_jobs);

	/* make a copy, in case we cannot do better than this */
	orig_row = common_dup_row_data(p_ptr->row, p_ptr->num_rows);
	if (orig_row == NULL)
		return;

	/* get row_bitmap size from first row (we can safely assume that the
	 * first row_bitmap exists because there exists at least one job. */
	size = bit_size(p_ptr->row[0].first_row_bitmap);

	/* create a master job list and clear out ALL row data */
	ss = xcalloc(num_jobs, sizeof(struct sort_support));
	x = 0;
	for (i = 0; i < p_ptr->num_rows; i++) {
		for (j = 0; j < p_ptr->row[i].num_jobs; j++) {
			ss[x].tmpjobs = p_ptr->row[i].job_list[j];
			p_ptr->row[i].job_list[j] = NULL;
			ss[x].jstart = bit_ffs(ss[x].tmpjobs->node_bitmap);
			ss[x].jstart = cr_get_coremap_offset(ss[x].jstart);
			ss[x].jstart += bit_ffs(ss[x].tmpjobs->core_bitmap);
			x++;
		}
		p_ptr->row[i].num_jobs = 0;
		if (p_ptr->row[i].first_row_bitmap) {
			bit_nclear(p_ptr->row[i].first_row_bitmap, 0, size-1);
		}
	}

	/* VERY difficult: Optimal placement of jobs in the matrix
	 * - how to order jobs to be added to the matrix?
	 *   - "by size" does not guarantee optimal placement
	 *
	 *   - for now, try sorting jobs by first bit set
	 *     - if job allocations stay "in blocks", then this should work OK
	 *     - may still get scenarios where jobs should switch rows
	 *     - fixme: JOB SHUFFLING BETWEEN ROWS NEEDS TESTING
	 */
	qsort(ss, num_jobs, sizeof(struct sort_support), _compare_support);
	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		for (i = 0; i < num_jobs; i++) {
			char cstr[64], nstr[64];
			if (ss[i].tmpjobs->core_bitmap) {
				bit_fmt(cstr, (sizeof(cstr)-1) ,
					ss[i].tmpjobs->core_bitmap);
			} else
				sprintf(cstr, "[no core_bitmap]");
			if (ss[i].tmpjobs->node_bitmap) {
				bit_fmt(nstr, (sizeof(nstr)-1),
					ss[i].tmpjobs->node_bitmap);
			} else
				sprintf(nstr, "[no node_bitmap]");
			info("DEBUG:  jstart %d job nb %s cb %s",
			     ss[i].jstart, nstr, cstr);
		}
	}

	/* add jobs to the rows */
	for (j = 0; j < num_jobs; j++) {
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (_can_job_fit_in_row(ss[j].tmpjobs,
						&(p_ptr->row[i]))) {
				/* job fits in row, so add it */
				common_add_job_to_row(ss[j].tmpjobs,
						      &(p_ptr->row[i]));
				ss[j].tmpjobs = NULL;
				break;
			}
		}
		/* job should have been added, so shuffle the rows */
		common_sort_part_rows(p_ptr);
	}

	/* test for dangling jobs */
	for (j = 0; j < num_jobs; j++) {
		if (ss[j].tmpjobs)
			break;
	}
	if (j < num_jobs) {
		/* we found a dangling job, which means our packing
		 * algorithm couldn't improve apon the existing layout.
		 * Thus, we'll restore the original layout here */
		debug3("cons_res: build_row_bitmap: dangling job found");

		if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
			info("DEBUG: _build_row_bitmaps (post-algorithm):");
			common_dump_parts(p_ptr);
		}

		common_destroy_row_data(p_ptr->row, p_ptr->num_rows);
		p_ptr->row = orig_row;
		orig_row = NULL;

		/* still need to rebuild row_bitmaps */
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (p_ptr->row[i].first_row_bitmap)
				bit_nclear(p_ptr->row[i].first_row_bitmap, 0,
					   size-1);
			if (p_ptr->row[i].num_jobs == 0)
				continue;
			for (j = 0; j < p_ptr->row[i].num_jobs; j++) {
				_add_job_to_cores(p_ptr->row[i].job_list[j],
						  &p_ptr->row[i],
						  cr_node_num_cores);
			}
		}
	}

	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		info("DEBUG: _build_row_bitmaps (after):");
		common_dump_parts(p_ptr);
	}

	if (orig_row)
		common_destroy_row_data(orig_row, p_ptr->num_rows);
	xfree(ss);

	return;

	/* LEFTOVER DESIGN THOUGHTS, PRESERVED HERE */

	/* 1. sort jobs by size
	 * 2. only load core bitmaps with largest jobs that conflict
	 * 3. sort rows by set count
	 * 4. add remaining jobs, starting with fullest rows
	 * 5. compute  set count: if disparity between rows got closer, then
	 *    switch non-conflicting jobs that were added
	 */

	/*
	 *  Step 1: remove empty rows between non-empty rows
	 *  Step 2: try to collapse rows
	 *  Step 3: sort rows by size
	 *  Step 4: try to swap jobs from different rows to pack rows
	 */

	/* WORK IN PROGRESS - more optimization should go here, such as:
	 *
	 * - try collapsing jobs from higher rows to lower rows
	 *
	 * - produce a load array to identify cores with less load. Test
	 * to see if those cores are in the lower row. If not, try to swap
	 * those jobs with jobs in the lower row. If the job can be swapped
	 * AND the lower row set_count increases, then SUCCESS! else swap
	 * back. The goal is to pack the lower rows and "bubble up" clear
	 * bits to the higher rows.
	 */
}

static job_resources_t *_create_job_resources(int node_cnt)
{
	job_resources_t *job_resrcs_ptr;

	job_resrcs_ptr = create_job_resources();
	job_resrcs_ptr->cpu_array_reps = xcalloc(node_cnt, sizeof(uint32_t));
	job_resrcs_ptr->cpu_array_value = xcalloc(node_cnt, sizeof(uint16_t));
	job_resrcs_ptr->cpus = xcalloc(node_cnt, sizeof(uint16_t));
	job_resrcs_ptr->cpus_used = xcalloc(node_cnt, sizeof(uint16_t));
	job_resrcs_ptr->memory_allocated = xcalloc(node_cnt, sizeof(uint64_t));
	job_resrcs_ptr->memory_used = xcalloc(node_cnt, sizeof(uint64_t));
	job_resrcs_ptr->nhosts = node_cnt;
	return job_resrcs_ptr;
}

/* Move all resources from one job to another */
static int _job_expand(struct job_record *from_job_ptr,
		       struct job_record *to_job_ptr)
{
	job_resources_t *from_job_resrcs_ptr, *to_job_resrcs_ptr,
		        *new_job_resrcs_ptr;
	struct node_record *node_ptr;
	int first_bit, last_bit, i, node_cnt;
	bool from_node_used, to_node_used;
	int from_node_offset, to_node_offset, new_node_offset;
	bitstr_t *tmp_bitmap, *tmp_bitmap2;

	xassert(from_job_ptr);
	xassert(from_job_ptr->details);
	xassert(to_job_ptr);
	xassert(to_job_ptr->details);

	if (from_job_ptr->job_id == to_job_ptr->job_id) {
		error("%s: attempt to merge %pJ with self",
		      plugin_type, from_job_ptr);
		return SLURM_ERROR;
	}

	from_job_resrcs_ptr = from_job_ptr->job_resrcs;
	if ((from_job_resrcs_ptr == NULL) ||
	    (from_job_resrcs_ptr->cpus == NULL) ||
	    (from_job_resrcs_ptr->core_bitmap == NULL) ||
	    (from_job_resrcs_ptr->node_bitmap == NULL)) {
		error("%s: %pJ lacks a job_resources struct",
		      plugin_type, from_job_ptr);
		return SLURM_ERROR;
	}
	to_job_resrcs_ptr = to_job_ptr->job_resrcs;
	if ((to_job_resrcs_ptr == NULL) ||
	    (to_job_resrcs_ptr->cpus == NULL) ||
	    (to_job_resrcs_ptr->core_bitmap == NULL) ||
	    (to_job_resrcs_ptr->node_bitmap == NULL)) {
		error("%s: %pJ lacks a job_resources struct",
		      plugin_type, to_job_ptr);
		return SLURM_ERROR;
	}

	(void) common_rm_job_res(select_part_record, select_node_usage,
				from_job_ptr, 0, true);
	(void) common_rm_job_res(select_part_record, select_node_usage,
				to_job_ptr, 0, true);

	if (to_job_resrcs_ptr->core_bitmap_used) {
		i = bit_size(to_job_resrcs_ptr->core_bitmap_used);
		bit_nclear(to_job_resrcs_ptr->core_bitmap_used, 0, i-1);
	}

	tmp_bitmap = bit_copy(to_job_resrcs_ptr->node_bitmap);
	bit_or(tmp_bitmap, from_job_resrcs_ptr->node_bitmap);
	tmp_bitmap2 = bit_copy(to_job_ptr->node_bitmap);
	bit_or(tmp_bitmap2, from_job_ptr->node_bitmap);
	bit_and(tmp_bitmap, tmp_bitmap2);
	bit_free(tmp_bitmap2);
	node_cnt = bit_set_count(tmp_bitmap);
	new_job_resrcs_ptr = _create_job_resources(node_cnt);
	new_job_resrcs_ptr->ncpus = from_job_resrcs_ptr->ncpus +
				    to_job_resrcs_ptr->ncpus;
	new_job_resrcs_ptr->node_req = to_job_resrcs_ptr->node_req;
	new_job_resrcs_ptr->node_bitmap = tmp_bitmap;
	new_job_resrcs_ptr->nodes = bitmap2node_name(new_job_resrcs_ptr->
						     node_bitmap);
	new_job_resrcs_ptr->whole_node = to_job_resrcs_ptr->whole_node;
	build_job_resources(new_job_resrcs_ptr, node_record_table_ptr,
			    select_fast_schedule);
	xfree(to_job_ptr->node_addr);
	to_job_ptr->node_addr = xcalloc(node_cnt, sizeof(slurm_addr_t));
	to_job_ptr->total_cpus = 0;

	first_bit = MIN(bit_ffs(from_job_resrcs_ptr->node_bitmap),
			bit_ffs(to_job_resrcs_ptr->node_bitmap));
	last_bit =  MAX(bit_fls(from_job_resrcs_ptr->node_bitmap),
			bit_fls(to_job_resrcs_ptr->node_bitmap));
	from_node_offset = to_node_offset = new_node_offset = -1;
	for (i = first_bit; i <= last_bit; i++) {
		from_node_used = to_node_used = false;
		if (bit_test(from_job_resrcs_ptr->node_bitmap, i)) {
			from_node_used = bit_test(from_job_ptr->node_bitmap,i);
			from_node_offset++;
		}
		if (bit_test(to_job_resrcs_ptr->node_bitmap, i)) {
			to_node_used = bit_test(to_job_ptr->node_bitmap, i);
			to_node_offset++;
		}
		if (!from_node_used && !to_node_used)
			continue;
		new_node_offset++;
		node_ptr = node_record_table_ptr + i;
		memcpy(&to_job_ptr->node_addr[new_node_offset],
                       &node_ptr->slurm_addr, sizeof(slurm_addr_t));
		if (from_node_used) {
			/* Merge alloc info from both "from" and "to" jobs,
			 * leave "from" job with no allocated CPUs or memory */
			new_job_resrcs_ptr->cpus[new_node_offset] =
				from_job_resrcs_ptr->cpus[from_node_offset];
			from_job_resrcs_ptr->cpus[from_node_offset] = 0;
			/* new_job_resrcs_ptr->cpus_used[new_node_offset] =
				from_job_resrcs_ptr->
				cpus_used[from_node_offset]; Should be 0 */
			new_job_resrcs_ptr->memory_allocated[new_node_offset] =
				from_job_resrcs_ptr->
				memory_allocated[from_node_offset];
			/* new_job_resrcs_ptr->memory_used[new_node_offset] =
				from_job_resrcs_ptr->
				memory_used[from_node_offset]; Should be 0 */
			job_resources_bits_copy(new_job_resrcs_ptr,
						new_node_offset,
						from_job_resrcs_ptr,
						from_node_offset);
		}
		if (to_node_used) {
			/* Merge alloc info from both "from" and "to" jobs */

			/* DO NOT double count the allocated CPUs in partition
			 * with Shared nodes */
			new_job_resrcs_ptr->cpus[new_node_offset] +=
				to_job_resrcs_ptr->cpus[to_node_offset];
			new_job_resrcs_ptr->cpus_used[new_node_offset] +=
				to_job_resrcs_ptr->cpus_used[to_node_offset];
			new_job_resrcs_ptr->memory_allocated[new_node_offset]+=
				to_job_resrcs_ptr->
				memory_allocated[to_node_offset];
			new_job_resrcs_ptr->memory_used[new_node_offset] +=
				to_job_resrcs_ptr->memory_used[to_node_offset];
			job_resources_bits_copy(new_job_resrcs_ptr,
						new_node_offset,
						to_job_resrcs_ptr,
						to_node_offset);
			if (from_node_used) {
				/* Adjust CPU count for shared CPUs */
				int from_core_cnt, to_core_cnt, new_core_cnt;
				from_core_cnt = count_job_resources_node(
							from_job_resrcs_ptr,
							from_node_offset);
				to_core_cnt = count_job_resources_node(
							to_job_resrcs_ptr,
							to_node_offset);
				new_core_cnt = count_job_resources_node(
							new_job_resrcs_ptr,
							new_node_offset);
				if ((from_core_cnt + to_core_cnt) !=
				    new_core_cnt) {
					new_job_resrcs_ptr->
						cpus[new_node_offset] *=
						new_core_cnt;
					new_job_resrcs_ptr->
						cpus[new_node_offset] /=
						(from_core_cnt + to_core_cnt);
				}
			}
		}
		if (to_job_ptr->details->whole_node == 1) {
			to_job_ptr->total_cpus += select_node_record[i].cpus;
		} else {
			to_job_ptr->total_cpus += new_job_resrcs_ptr->
						  cpus[new_node_offset];
		}
	}
	build_job_resources_cpu_array(new_job_resrcs_ptr);
	gres_plugin_job_merge(from_job_ptr->gres_list,
			      from_job_resrcs_ptr->node_bitmap,
			      to_job_ptr->gres_list,
			      to_job_resrcs_ptr->node_bitmap);

	/* Now swap data: "new" -> "to" and clear "from" */
	free_job_resources(&to_job_ptr->job_resrcs);
	to_job_ptr->job_resrcs = new_job_resrcs_ptr;

	to_job_ptr->cpu_cnt = to_job_ptr->total_cpus;
	to_job_ptr->details->min_cpus = to_job_ptr->total_cpus;
	to_job_ptr->details->max_cpus = to_job_ptr->total_cpus;
	from_job_ptr->total_cpus   = 0;
	from_job_resrcs_ptr->ncpus = 0;
	from_job_ptr->details->min_cpus = 0;
	from_job_ptr->details->max_cpus = 0;

	from_job_ptr->total_nodes   = 0;
	from_job_resrcs_ptr->nhosts = 0;
	from_job_ptr->node_cnt      = 0;
	from_job_ptr->details->min_nodes = 0;
	to_job_ptr->total_nodes     = new_job_resrcs_ptr->nhosts;
	to_job_ptr->node_cnt        = new_job_resrcs_ptr->nhosts;

	bit_or(to_job_ptr->node_bitmap, from_job_ptr->node_bitmap);
	bit_nclear(from_job_ptr->node_bitmap, 0, (node_record_count - 1));
	bit_nclear(from_job_resrcs_ptr->node_bitmap, 0,
		   (node_record_count - 1));

	xfree(to_job_ptr->nodes);
	to_job_ptr->nodes = xstrdup(new_job_resrcs_ptr->nodes);
	xfree(from_job_ptr->nodes);
	from_job_ptr->nodes = xstrdup("");
	xfree(from_job_resrcs_ptr->nodes);
	from_job_resrcs_ptr->nodes = xstrdup("");

	(void) common_add_job_to_res(to_job_ptr, 0);

	return SLURM_SUCCESS;
}

static int _rm_job_from_one_node(struct job_record *job_ptr,
				 struct node_record *node_ptr)
{
	struct part_res_record *part_record_ptr = select_part_record;
	struct node_use_record *node_usage = select_node_usage;
	struct job_resources *job = job_ptr->job_resrcs;
	struct part_res_record *p_ptr;
	int first_bit, last_bit;
	int i, node_inx, n;
	List gres_list;
	bool old_job = false;

	if (!job || !job->core_bitmap) {
		error("%s: %s: %pJ has no job_resrcs info",
		      plugin_type, __func__, job_ptr);
		return SLURM_ERROR;
	}

	debug3("%s: %s: %pJ node %s",
	       plugin_type, __func__, job_ptr, node_ptr->name);
	if (job_ptr->start_time < slurmctld_config.boot_time)
		old_job = true;
	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		_dump_job_res(job);

	/* subtract memory */
	node_inx  = node_ptr - node_record_table_ptr;
	first_bit = bit_ffs(job->node_bitmap);
	last_bit  = bit_fls(job->node_bitmap);
	for (i = first_bit, n = 0; i <= last_bit; i++) {
		if (!bit_test(job->node_bitmap, i))
			continue;
		if (i != node_inx) {
			n++;
			continue;
		}

		if (job->cpus[n] == 0) {
			info("attempt to remove node %s from %pJ again",
			     node_ptr->name, job_ptr);
			return SLURM_SUCCESS;
		}

		if (node_usage[i].gres_list)
			gres_list = node_usage[i].gres_list;
		else
			gres_list = node_ptr->gres_list;
		gres_plugin_job_dealloc(job_ptr->gres_list, gres_list, n,
					job_ptr->job_id, node_ptr->name,
					old_job, job_ptr->user_id, true);
		gres_plugin_node_state_log(gres_list, node_ptr->name);

		if (node_usage[i].alloc_memory < job->memory_allocated[n]) {
			error("%s: node %s memory is underallocated (%"PRIu64"-%"PRIu64") for %pJ",
			      plugin_type, node_ptr->name,
			      node_usage[i].alloc_memory,
			      job->memory_allocated[n], job_ptr);
			node_usage[i].alloc_memory = 0;
		} else
			node_usage[i].alloc_memory -= job->memory_allocated[n];

		extract_job_resources_node(job, n);

		break;
	}

	if (IS_JOB_SUSPENDED(job_ptr))
		return SLURM_SUCCESS;	/* No cores allocated to the job now */

	/* subtract cores, reconstruct rows with remaining jobs */
	if (!job_ptr->part_ptr) {
		error("%s: removed %pJ does not have a partition assigned",
		      plugin_type, job_ptr);
		return SLURM_ERROR;
	}

	for (p_ptr = part_record_ptr; p_ptr; p_ptr = p_ptr->next) {
		if (p_ptr->part_ptr == job_ptr->part_ptr)
			break;
	}
	if (!p_ptr) {
		error("%s: removed %pJ could not find part %s",
		      plugin_type, job_ptr, job_ptr->part_ptr->name);
		return SLURM_ERROR;
	}

	if (!p_ptr->row)
		return SLURM_SUCCESS;

	/* look for the job in the partition's job_list */
	n = 0;
	for (i = 0; i < p_ptr->num_rows; i++) {
		uint32_t j;
		for (j = 0; j < p_ptr->row[i].num_jobs; j++) {
			if (p_ptr->row[i].job_list[j] != job)
				continue;
			debug3("%s: found %pJ in part %s row %u",
			       plugin_type, job_ptr, p_ptr->part_ptr->name, i);
			/* found job - we're done, don't actually remove */
			n = 1;
			i = p_ptr->num_rows;
			break;
		}
	}
	if (n == 0) {
		error("%s: could not find %pJ in partition %s",
		      plugin_type, job_ptr, p_ptr->part_ptr->name);
		return SLURM_ERROR;
	}


	/* some node of job removed from core-bitmap, so refresh CR bitmaps */
	_build_row_bitmaps(p_ptr, NULL);

	/* Adjust the node_state of the node removed from this job.
	 * If all cores are now available, set node_state = NODE_CR_AVAILABLE */
	if (node_usage[node_inx].node_state >= job->node_req) {
		node_usage[node_inx].node_state -= job->node_req;
	} else {
		error("cons_res:_rm_job_from_one_node: node_state miscount");
		node_usage[node_inx].node_state = NODE_CR_AVAILABLE;
	}

	return SLURM_SUCCESS;
}

static int
_compare_support(const void *v, const void *v1)
{
	struct sort_support *s;
	struct sort_support *s1;

	s = (struct sort_support *)v;
	s1 = (struct sort_support *)v1;

	if (s->jstart > s1->jstart
		|| (s->jstart == s1->jstart
			&& s->tmpjobs->ncpus > s1->tmpjobs->ncpus))
		return 1;

	return 0;
}
/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	common_init();

	cons_common_callbacks.add_job_to_res = _add_job_to_cores;
	cons_common_callbacks.can_job_fit_in_row = _can_job_fit_in_row;
	cons_common_callbacks.can_job_run_on_node = can_job_run_on_node;
	cons_common_callbacks.choose_nodes = choose_nodes;
	cons_common_callbacks.verify_node_state = verify_node_state;
	cons_common_callbacks.mark_avail_cores = make_core_bitmap;
	cons_common_callbacks.build_row_bitmaps = _build_row_bitmaps;
	cons_common_callbacks.dist_tasks_compute_c_b = dist_tasks_compute_c_b;

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	common_fini();

	return SLURM_SUCCESS;
}

/*
 * The remainder of this file implements the standard Slurm
 * node selection API.
 */

extern int select_p_state_save(char *dir_name)
{
	/* nothing to save */
	return SLURM_SUCCESS;
}

/* This is Part 2 of a 4-part procedure which can be found in
 * src/slurmctld/read_config.c. See select_p_node_init for the
 * whole story.
 */
extern int select_p_state_restore(char *dir_name)
{
	/* nothing to restore */
	return SLURM_SUCCESS;
}

/* This is Part 3 of a 4-part procedure which can be found in
 * src/slurmctld/read_config.c. See select_p_node_init for the
 * whole story.
 */
extern int select_p_job_init(List job_list)
{
	/* nothing to initialize for jobs */
	return SLURM_SUCCESS;
}

/* This plugin does not generate a node ranking. */
extern bool select_p_node_ranking(struct node_record *node_ptr, int node_cnt)
{
	return false;
}

/* This is Part 1 of a 4-part procedure which can be found in
 * src/slurmctld/read_config.c. The whole story goes like this:
 *
 * Step 1: select_g_node_init          : initializes the global node arrays
 * Step 2: select_g_state_restore      : NO-OP - nothing to restore
 * Step 3: select_g_job_init           : NO-OP - nothing to initialize
 * Step 4: select_g_select_nodeinfo_set: called from reset_job_bitmaps() with
 *                                       each valid recovered job_ptr AND from
 *                                       select_nodes(), this procedure adds
 *                                       job data to the 'select_part_record'
 *                                       global array
 */
extern int select_p_node_init(struct node_record *node_ptr, int node_cnt)
{
	return common_node_init(node_ptr, node_cnt);
}

extern int select_p_block_init(List part_list)
{
	return SLURM_SUCCESS;
}


/*
 * select_p_job_test - Given a specification of scheduling requirements,
 *	identify the nodes which "best" satisfy the request.
 * 	"best" is defined as either a minimal number of consecutive nodes
 *	or if sharing resources then sharing them with a job of similar size.
 * IN/OUT job_ptr - pointer to job being considered for initiation,
 *                  set's start_time when job expected to start
 * IN/OUT bitmap - usable nodes are set on input, nodes not required to
 *	satisfy the request are cleared, other left set
 * IN min_nodes - minimum count of nodes
 * IN req_nodes - requested (or desired) count of nodes
 * IN max_nodes - maximum count of nodes (0==don't care)
 * IN mode - SELECT_MODE_RUN_NOW   (0): try to schedule job now
 *           SELECT_MODE_TEST_ONLY (1): test if job can ever run
 *           SELECT_MODE_WILL_RUN  (2): determine when and where job can run
 * IN preemptee_candidates - List of pointers to jobs which can be preempted.
 * IN/OUT preemptee_job_list - Pointer to list of job pointers. These are the
 *		jobs to be preempted to initiate the pending job. Not set
 *		if mode=SELECT_MODE_TEST_ONLY or input pointer is NULL.
 * RET zero on success, EINVAL otherwise
 * globals (passed via select_p_node_init):
 *	node_record_count - count of nodes configured
 *	node_record_table_ptr - pointer to global node table
 * NOTE: the job information that is considered for scheduling includes:
 *	req_node_bitmap: bitmap of specific nodes required by the job
 *	contiguous: allocated nodes must be sequentially located
 *	num_cpus: minimum number of processors required by the job
 * NOTE: bitmap must be a superset of req_nodes at the time that
 *	select_p_job_test is called
 */
extern int select_p_job_test(struct job_record *job_ptr, bitstr_t * bitmap,
			     uint32_t min_nodes, uint32_t max_nodes,
			     uint32_t req_nodes, uint16_t mode,
			     List preemptee_candidates,
			     List *preemptee_job_list,
			     bitstr_t *exc_core_bitmap)
{
	xassert(bitmap);

	debug2("%s for %pJ", __func__, job_ptr);

	if (!job_ptr->details)
		return EINVAL;

	return common_job_test(job_ptr, bitmap, min_nodes, max_nodes,
			       req_nodes, mode, preemptee_candidates,
			       preemptee_job_list, &exc_core_bitmap);
}

extern int select_p_job_begin(struct job_record *job_ptr)
{
	return SLURM_SUCCESS;
}

/* Determine if allocated nodes are usable (powered up) */
extern int select_p_job_ready(struct job_record *job_ptr)
{
	int i, i_first, i_last;
	struct node_record *node_ptr;

	if (!IS_JOB_RUNNING(job_ptr) && !IS_JOB_SUSPENDED(job_ptr)) {
		/* Gang scheduling might suspend job immediately */
		return 0;
	}

	if ((job_ptr->node_bitmap == NULL) ||
	    ((i_first = bit_ffs(job_ptr->node_bitmap)) == -1))
		return READY_NODE_STATE;
	i_last  = bit_fls(job_ptr->node_bitmap);

	for (i = i_first; i <= i_last; i++) {
		if (bit_test(job_ptr->node_bitmap, i) == 0)
			continue;
		node_ptr = node_record_table_ptr + i;
		if (IS_NODE_POWER_SAVE(node_ptr) || IS_NODE_POWER_UP(node_ptr))
			return 0;
	}

	return READY_NODE_STATE;
}

extern int select_p_job_resized(struct job_record *job_ptr,
				struct node_record *node_ptr)
{
	xassert(job_ptr);
	xassert(job_ptr->magic == JOB_MAGIC);

	_rm_job_from_one_node(job_ptr, node_ptr);
	return SLURM_SUCCESS;
}

extern int select_p_job_expand(struct job_record *from_job_ptr,
			       struct job_record *to_job_ptr)
{
	xassert(from_job_ptr);
	xassert(from_job_ptr->magic == JOB_MAGIC);
	xassert(to_job_ptr);
	xassert(to_job_ptr->magic == JOB_MAGIC);

	return _job_expand(from_job_ptr, to_job_ptr);
}

extern int select_p_job_signal(struct job_record *job_ptr, int signal)
{
	xassert(job_ptr);
	xassert(job_ptr->magic == JOB_MAGIC);

	return SLURM_SUCCESS;
}

extern int select_p_job_mem_confirm(struct job_record *job_ptr)
{
	int i_first, i_last, i, offset;
	uint64_t avail_mem, lowest_mem = 0;

	xassert(job_ptr);

	if (((job_ptr->bit_flags & NODE_MEM_CALC) == 0) ||
	    (select_fast_schedule != 0))
		return SLURM_SUCCESS;
	if ((job_ptr->details == NULL) ||
	    (job_ptr->job_resrcs == NULL) ||
	    (job_ptr->job_resrcs->node_bitmap == NULL) ||
	    (job_ptr->job_resrcs->memory_allocated == NULL))
		return SLURM_ERROR;
	i_first = bit_ffs(job_ptr->job_resrcs->node_bitmap);
	if (i_first >= 0)
		i_last = bit_fls(job_ptr->job_resrcs->node_bitmap);
	else
		i_last = i_first - 1;
	for (i = i_first, offset = 0; i <= i_last; i++) {
		if (!bit_test(job_ptr->job_resrcs->node_bitmap, i))
			continue;
		avail_mem = select_node_record[i].real_memory -
			    select_node_record[i].mem_spec_limit;
		job_ptr->job_resrcs->memory_allocated[offset] = avail_mem;
		select_node_usage[i].alloc_memory = avail_mem;
		if ((offset == 0) || (lowest_mem > avail_mem))
			lowest_mem = avail_mem;
		offset++;
	}
	job_ptr->details->pn_min_memory = lowest_mem;

	return SLURM_SUCCESS;
}

extern int select_p_job_fini(struct job_record *job_ptr)
{
	xassert(job_ptr);
	xassert(job_ptr->magic == JOB_MAGIC);

	common_rm_job_res(select_part_record, select_node_usage, job_ptr, 0,
			  true);

	return SLURM_SUCCESS;
}

/* NOTE: This function is not called with gang scheduling because it
 * needs to track how many jobs are running or suspended on each node.
 * This sum is compared with the partition's Shared parameter */
extern int select_p_job_suspend(struct job_record *job_ptr, bool indf_susp)
{
	xassert(job_ptr);

	if (!indf_susp)
		return SLURM_SUCCESS;

	return common_rm_job_res(select_part_record, select_node_usage,
				 job_ptr, 2, false);
}

/* See NOTE with select_p_job_suspend above */
extern int select_p_job_resume(struct job_record *job_ptr, bool indf_susp)
{
	xassert(job_ptr);

	if (!indf_susp)
		return SLURM_SUCCESS;

	return common_add_job_to_res(job_ptr, 2);
}


extern bitstr_t *select_p_step_pick_nodes(struct job_record *job_ptr,
					  select_jobinfo_t *jobinfo,
					  uint32_t node_count,
					  bitstr_t **avail_nodes)
{
	return NULL;
}

extern int select_p_step_start(struct step_record *step_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_step_finish(struct step_record *step_ptr, bool killing_step)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_pack(select_nodeinfo_t *nodeinfo,
					 Buf buffer,
					 uint16_t protocol_version)
{
	select_nodeinfo_t *nodeinfo_empty = NULL;

	if (!nodeinfo) {
		/*
		 * We should never get here,
		 * but avoid abort with bad data structures
		 */
		error("%s: nodeinfo is NULL", __func__);
		nodeinfo_empty = xmalloc(sizeof(select_nodeinfo_t));
		nodeinfo = nodeinfo_empty;
	}

	if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		pack16(nodeinfo->alloc_cpus, buffer);
		pack64(nodeinfo->alloc_memory, buffer);
		packstr(nodeinfo->tres_alloc_fmt_str, buffer);
		packdouble(nodeinfo->tres_alloc_weighted, buffer);
	}
	xfree(nodeinfo_empty);

	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_unpack(select_nodeinfo_t **nodeinfo,
					   Buf buffer,
					   uint16_t protocol_version)
{
	uint32_t uint32_tmp;
	select_nodeinfo_t *nodeinfo_ptr = NULL;

	nodeinfo_ptr = select_p_select_nodeinfo_alloc();
	*nodeinfo = nodeinfo_ptr;

	if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		safe_unpack16(&nodeinfo_ptr->alloc_cpus, buffer);
		safe_unpack64(&nodeinfo_ptr->alloc_memory, buffer);
		safe_unpackstr_xmalloc(&nodeinfo_ptr->tres_alloc_fmt_str,
				       &uint32_tmp, buffer);
		safe_unpackdouble(&nodeinfo_ptr->tres_alloc_weighted, buffer);
	}

	return SLURM_SUCCESS;

unpack_error:
	error("select_nodeinfo_unpack: error unpacking here");
	select_p_select_nodeinfo_free(nodeinfo_ptr);
	*nodeinfo = NULL;

	return SLURM_ERROR;
}

extern select_nodeinfo_t *select_p_select_nodeinfo_alloc(void)
{
	select_nodeinfo_t *nodeinfo = xmalloc(sizeof(struct select_nodeinfo));

	nodeinfo->magic = NODEINFO_MAGIC;

	return nodeinfo;
}

extern int select_p_select_nodeinfo_free(select_nodeinfo_t *nodeinfo)
{
	if (nodeinfo) {
		if (nodeinfo->magic != NODEINFO_MAGIC) {
			error("select_p_select_nodeinfo_free: "
			      "nodeinfo magic bad");
			return EINVAL;
		}
		nodeinfo->magic = 0;
		xfree(nodeinfo->tres_alloc_cnt);
		xfree(nodeinfo->tres_alloc_fmt_str);
		xfree(nodeinfo);
	}
	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_set_all(void)
{
	struct part_res_record *p_ptr;
	struct node_record *node_ptr = NULL;
	int i, n, start, end;
	static time_t last_set_all = 0;
	uint32_t alloc_cpus, node_cores, node_cpus, node_threads;
	bitstr_t *alloc_core_bitmap = NULL;
	List gres_list;

	/* only set this once when the last_node_update is newer than
	 * the last time we set things up. */
	if (last_set_all && (last_node_update < last_set_all)) {
		debug2("Node select info for set all hasn't "
		       "changed since %ld",
		       (long)last_set_all);
		return SLURM_NO_CHANGE_IN_DATA;
	}
	last_set_all = last_node_update;

	/* Build bitmap representing all cores allocated to all active jobs
	 * (running or preempted jobs) */
	for (p_ptr = select_part_record; p_ptr; p_ptr = p_ptr->next) {
		if (!p_ptr->row)
			continue;
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (!p_ptr->row[i].first_row_bitmap)
				continue;
			if (!alloc_core_bitmap) {
				alloc_core_bitmap =
					bit_copy(p_ptr->row[i].first_row_bitmap);
			} else if (bit_size(alloc_core_bitmap) ==
				   bit_size(p_ptr->row[i].first_row_bitmap)) {
				bit_or(alloc_core_bitmap,
				       p_ptr->row[i].first_row_bitmap);
			}
		}
	}

	for (n = 0, node_ptr = node_record_table_ptr;
	     n < select_node_cnt; n++, node_ptr++) {
		select_nodeinfo_t *nodeinfo = NULL;
		/* We have to use the '_g_' here to make sure we get the
		 * correct data to work on.  i.e. cray calls this plugin
		 * from within select/cray which has it's own struct. */
		select_g_select_nodeinfo_get(node_ptr->select_nodeinfo,
					     SELECT_NODEDATA_PTR, 0,
					     (void *)&nodeinfo);
		if (!nodeinfo) {
			error("no nodeinfo returned from structure");
			continue;
		}

		if (slurmctld_conf.fast_schedule) {
			node_cpus    = node_ptr->config_ptr->cpus;
			node_threads = node_ptr->config_ptr->threads;
		} else {
			node_cpus    = node_ptr->cpus;
			node_threads = node_ptr->threads;
		}

		start = cr_get_coremap_offset(n);
		end = cr_get_coremap_offset(n + 1);
		if (alloc_core_bitmap) {
			alloc_cpus = bit_set_count_range(alloc_core_bitmap,
							 start, end);
		} else {
			alloc_cpus = 0;
		}
		node_cores = end - start;

		/* Administrator could resume suspended jobs and oversubscribe
		 * cores, avoid reporting more cores in use than configured */
		if (alloc_cpus > node_cores)
			alloc_cpus = node_cores;

		/* The minimum allocatable unit may a core, so scale by thread
		 * count up to the proper CPU count as needed */
		if (node_cores < node_cpus)
			alloc_cpus *= node_threads;
		nodeinfo->alloc_cpus = alloc_cpus;

		if (select_node_record) {
			nodeinfo->alloc_memory =
				select_node_usage[n].alloc_memory;
		} else {
			nodeinfo->alloc_memory = 0;
		}

		/* Build allocated tres */
		if (!nodeinfo->tres_alloc_cnt)
			nodeinfo->tres_alloc_cnt = xcalloc(slurmctld_tres_cnt,
							   sizeof(uint64_t));
		nodeinfo->tres_alloc_cnt[TRES_ARRAY_CPU] = alloc_cpus;
		nodeinfo->tres_alloc_cnt[TRES_ARRAY_MEM] =
			nodeinfo->alloc_memory;
		if (select_node_usage[n].gres_list)
			gres_list = select_node_usage[n].gres_list;
		else
			gres_list = node_ptr->gres_list;
		gres_set_node_tres_cnt(gres_list, nodeinfo->tres_alloc_cnt,
				       false);

		xfree(nodeinfo->tres_alloc_fmt_str);
		nodeinfo->tres_alloc_fmt_str =
			assoc_mgr_make_tres_str_from_array(
						nodeinfo->tres_alloc_cnt,
						TRES_STR_CONVERT_UNITS, false);
		nodeinfo->tres_alloc_weighted =
			assoc_mgr_tres_weighted(nodeinfo->tres_alloc_cnt,
					node_ptr->config_ptr->tres_weights,
					priority_flags, false);
	}
	FREE_NULL_BITMAP(alloc_core_bitmap);

	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_set(struct job_record *job_ptr)
{
	int rc;
	xassert(job_ptr);
	xassert(job_ptr->magic == JOB_MAGIC);

	if (IS_JOB_RUNNING(job_ptr))
		rc = common_add_job_to_res(job_ptr, 0);
	else if (IS_JOB_SUSPENDED(job_ptr)) {
		if (job_ptr->priority == 0)
			rc = common_add_job_to_res(job_ptr, 1);
		else	/* Gang schedule suspend */
			rc = common_add_job_to_res(job_ptr, 0);
	} else
		return SLURM_SUCCESS;
	gres_plugin_job_state_log(job_ptr->gres_list, job_ptr->job_id);

	return rc;
}

extern int select_p_select_nodeinfo_get(select_nodeinfo_t *nodeinfo,
					enum select_nodedata_type dinfo,
					enum node_states state,
					void *data)
{
	int rc = SLURM_SUCCESS;
	uint16_t *uint16 = (uint16_t *) data;
	uint64_t *uint64 = (uint64_t *) data;
	char **tmp_char = (char **) data;
	double *tmp_double = (double *) data;
	select_nodeinfo_t **select_nodeinfo = (select_nodeinfo_t **) data;

	if (nodeinfo == NULL) {
		error("get_nodeinfo: nodeinfo not set");
		return SLURM_ERROR;
	}

	if (nodeinfo->magic != NODEINFO_MAGIC) {
		error("get_nodeinfo: jobinfo magic bad");
		return SLURM_ERROR;
	}

	switch (dinfo) {
	case SELECT_NODEDATA_SUBCNT:
		if (state == NODE_STATE_ALLOCATED)
			*uint16 = nodeinfo->alloc_cpus;
		else
			*uint16 = 0;
		break;
	case SELECT_NODEDATA_PTR:
		*select_nodeinfo = nodeinfo;
		break;
	case SELECT_NODEDATA_MEM_ALLOC:
		*uint64 = nodeinfo->alloc_memory;
		break;
	case SELECT_NODEDATA_TRES_ALLOC_FMT_STR:
		*tmp_char = xstrdup(nodeinfo->tres_alloc_fmt_str);
		break;
	case SELECT_NODEDATA_TRES_ALLOC_WEIGHTED:
		*tmp_double = nodeinfo->tres_alloc_weighted;
		break;
	default:
		error("Unsupported option %d for get_nodeinfo.", dinfo);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern int select_p_select_jobinfo_alloc(void)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_jobinfo_free(select_jobinfo_t *jobinfo)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_jobinfo_set(select_jobinfo_t *jobinfo,
				       enum select_jobdata_type data_type,
				       void *data)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_jobinfo_get(select_jobinfo_t *jobinfo,
				       enum select_jobdata_type data_type,
				       void *data)
{
	return SLURM_ERROR;
}

extern select_jobinfo_t *select_p_select_jobinfo_copy(
	select_jobinfo_t *jobinfo)
{
	return NULL;
}

extern int select_p_select_jobinfo_pack(select_jobinfo_t *jobinfo, Buf buffer,
					uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_jobinfo_unpack(select_jobinfo_t *jobinfo,
					  Buf buffer,
					  uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}

extern char *select_p_select_jobinfo_sprint(select_jobinfo_t *jobinfo,
					    char *buf, size_t size, int mode)
{
	if (buf && size) {
		buf[0] = '\0';
		return buf;
	} else
		return NULL;
}

extern char *select_p_select_jobinfo_xstrdup(
	select_jobinfo_t *jobinfo, int mode)
{
	return NULL;
}

extern int select_p_get_info_from_plugin(enum select_plugindata_info info,
					 struct job_record *job_ptr,
					 void *data)
{
	int rc = SLURM_SUCCESS;
	uint32_t *tmp_32 = (uint32_t *) data;
	List *tmp_list = (List *) data;

	switch (info) {
	case SELECT_CR_PLUGIN:
		*tmp_32 = SELECT_TYPE_CONS_RES;
		break;
	case SELECT_CONFIG_INFO:
		*tmp_list = NULL;
		break;
	case SELECT_SINGLE_JOB_TEST:
		*tmp_32 = 0;
		break;
	default:
		error("%s: info type %d invalid", __func__, info);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

/* For right now, we just update the node's memory size. In order to update
 * socket, core, thread or cpu count, we would need to rebuild many bitmaps. */
extern int select_p_update_node_config (int index)
{
	if (index >= select_node_cnt) {
		error("select_p_update_node_config: index too large %d>%d",
		      index, select_node_cnt);
		return SLURM_ERROR;
	}

	/* Socket and core count can be changed when KNL node reboots in a
	 * different NUMA configuration */
	if ((select_fast_schedule == 1) &&
	    (select_node_record[index].sockets !=
	     select_node_record[index].node_ptr->config_ptr->sockets) &&
	    (select_node_record[index].cores !=
	     select_node_record[index].node_ptr->config_ptr->cores) &&
	    ((select_node_record[index].sockets *
	      select_node_record[index].cores) ==
	     (select_node_record[index].node_ptr->sockets *
	      select_node_record[index].node_ptr->cores))) {
		select_node_record[index].sockets =
			select_node_record[index].node_ptr->config_ptr->sockets;
		select_node_record[index].cores =
			select_node_record[index].node_ptr->config_ptr->cores;
	}

	if (select_fast_schedule)
		return SLURM_SUCCESS;

	select_node_record[index].real_memory =
		select_node_record[index].node_ptr->real_memory;
	select_node_record[index].mem_spec_limit =
		select_node_record[index].node_ptr->mem_spec_limit;

	return SLURM_SUCCESS;
}

extern int select_p_update_node_state(struct node_record *node_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_reconfigure(void)
{
	return common_reconfig();
}

/* given an "avail" node_bitmap, return a corresponding "avail" core_bitmap */
/* DUPLICATE CODE: see job_test.c */
/* Adding a filter for setting cores based on avail bitmap */
bitstr_t *_make_core_bitmap_filtered(bitstr_t *node_map, int filter)
{
	uint32_t c, size;
	uint32_t coff;
	int n, n_first, n_last, nodes;

	nodes = bit_size(node_map);
	size = cr_get_coremap_offset(nodes);
	bitstr_t *core_map = bit_alloc(size);
	if (!core_map)
		return NULL;

	if (!filter)
		return core_map;

	n_first = bit_ffs(node_map);
	if (n_first == -1)
		n_last = -2;
	else
		n_last = bit_fls(node_map);
	for (n = n_first; n <= n_last; n++) {
		if (bit_test(node_map, n)) {
			c = cr_get_coremap_offset(n);
			coff = cr_get_coremap_offset(n + 1);
			while (c < coff) {
				bit_set(core_map, c++);
			}
		}
	}
	return core_map;
}

/* Once here, if core_cnt is NULL, avail_bitmap has nodes not used by any job or
 * reservation */
bitstr_t *_sequential_pick(bitstr_t *avail_bitmap, uint32_t node_cnt,
			   uint32_t *core_cnt, bitstr_t **core_bitmap)
{
	bitstr_t *sp_avail_bitmap;
	char str[300];
	uint32_t cores_per_node = 0, extra_cores_needed = 0;
	bitstr_t *tmpcore;
	int total_core_cnt = 0;

	/* We have these cases here:
	 *	1) Reservation requests using just number of nodes
	 *		- core_cnt is null
	 *	2) Reservations request using number of nodes + number of cores
	 *	3) Reservations request using node list
	 *		- node_cnt is 0
	 *		- core_cnt is null
	 *	4) Reservation request using node list + number of cores list
	 *		- node_cnt is 0
	 */

	if ((node_cnt) && (core_cnt)) {
		total_core_cnt = core_cnt[0];
		cores_per_node = core_cnt[0] / MAX(node_cnt, 1);
		debug2("Reserving %u cores across %d nodes",
			total_core_cnt, node_cnt);
		extra_cores_needed = total_core_cnt -
				     (cores_per_node * node_cnt);
	}
	if ((!node_cnt) && (core_cnt)) {
		int num_nodes = bit_set_count(avail_bitmap);
		int i;
		bit_fmt(str, (sizeof(str) - 1), avail_bitmap);
		debug2("Reserving cores from nodes: %s", str);
		for (i = 0; (i < num_nodes) && core_cnt[i]; i++)
			total_core_cnt += core_cnt[i];
	}

	debug2("Reservations requires %d cores (%u each on %d nodes, plus %u)",
	       total_core_cnt, cores_per_node, node_cnt, extra_cores_needed);

	sp_avail_bitmap = bit_alloc(bit_size(avail_bitmap));
	bit_fmt(str, (sizeof(str) - 1), avail_bitmap);
	bit_fmt(str, (sizeof(str) - 1), sp_avail_bitmap);

	if (core_cnt) { /* Reservation is using partial nodes */
		int node_list_inx = 0;

		debug2("Reservation is using partial nodes");

		_spec_core_filter(avail_bitmap, core_bitmap);
		tmpcore = bit_copy(*core_bitmap);

		bit_not(tmpcore); /* tmpcore contains now current free cores */
		bit_fmt(str, (sizeof(str) - 1), tmpcore);
		debug2("tmpcore contains just current free cores: %s", str);
		bit_and(*core_bitmap, tmpcore);	/* clear core_bitmap */

		while (total_core_cnt) {
			int inx, coff, coff2;
			int i;
			int cores_in_node;
			int local_cores;

			if (node_cnt == 0) {
				cores_per_node = core_cnt[node_list_inx];
				if (cores_per_node == 0)
					break;
			}

			inx = bit_ffs(avail_bitmap);
			if (inx < 0)
				break;
			debug2("Using node %d", inx);

			coff = cr_get_coremap_offset(inx);
			coff2 = cr_get_coremap_offset(inx + 1);
			local_cores = coff2 - coff;

			bit_clear(avail_bitmap, inx);

			if (local_cores < cores_per_node) {
				debug2("Skip node %d (local: %d, needed: %d)",
					inx, local_cores, cores_per_node);
				continue;
			}

			cores_in_node = 0;

			/* First let's see in there are enough cores in
			 * this node */
			for (i = 0; i < local_cores; i++) {
				if (bit_test(tmpcore, coff + i))
					cores_in_node++;
			}
			if (cores_in_node < cores_per_node) {
				debug2("Skip node %d (avail: %d, needed: %d)",
					inx, cores_in_node, cores_per_node);
				continue;
			}

			debug2("Using node %d (avail: %d, needed: %d)",
				inx, cores_in_node, cores_per_node);

			cores_in_node = 0;
			for (i = 0; i < local_cores; i++) {
				if (bit_test(tmpcore, coff + i)) {
					bit_set(*core_bitmap, coff + i);
					total_core_cnt--;
					cores_in_node++;
					if (cores_in_node > cores_per_node)
						extra_cores_needed--;
					if ((total_core_cnt == 0) ||
					    ((extra_cores_needed == 0) &&
					     (cores_in_node >= cores_per_node)))
						break;
				}
			}

			if (cores_in_node) {
				/* Add this node to the final node bitmap */
				debug2("Reservation using %d cores in node %d",
				       cores_in_node, inx);
				bit_set(sp_avail_bitmap, inx);
			} else {
				debug2("Reservation NOT using node %d", inx);
			}
			node_list_inx++;
		}
		FREE_NULL_BITMAP(tmpcore);

		if (total_core_cnt) {
			info("reservation request can not be satisfied");
			FREE_NULL_BITMAP(sp_avail_bitmap);
			return NULL;
		}

		bit_fmt(str, (sizeof(str) - 1), *core_bitmap);
		debug2("sequential pick using coremap: %s", str);

	} else { /* Reservation is using full nodes */
		while (node_cnt) {
			int inx;

			inx = bit_ffs(avail_bitmap);
			if (inx < 0)
				break;

			/* Add this node to the final node bitmap */
			bit_set(sp_avail_bitmap, inx);
			node_cnt--;

			/* Clear this node from the initial available bitmap */
			bit_clear(avail_bitmap, inx);
		}

		if (node_cnt) {
			info("reservation request can not be satisfied");
			FREE_NULL_BITMAP(sp_avail_bitmap);
			return NULL;
		}

		bit_fmt(str, (sizeof(str) - 1), sp_avail_bitmap);
		debug2("sequential pick using nodemap: %s", str);
	}

	return sp_avail_bitmap;
}

bitstr_t *_pick_first_cores(bitstr_t *avail_bitmap, uint32_t node_cnt,
			    uint32_t *core_cnt, bitstr_t **core_bitmap)
{
	bitstr_t *sp_avail_bitmap;
	bitstr_t *tmpcore;
	int inx, jnx, first_node, last_node;
	int node_offset = 0;
	int coff, coff2, local_cores;

	if (!core_cnt || (core_cnt[0] == 0))
		return NULL;

	sp_avail_bitmap = bit_alloc(bit_size(avail_bitmap));

	_spec_core_filter(avail_bitmap, core_bitmap);
	tmpcore = bit_copy(*core_bitmap);
	bit_not(tmpcore); /* tmpcore contains now current free cores */
	bit_and(*core_bitmap, tmpcore);	/* clear core_bitmap */

	first_node = bit_ffs(avail_bitmap);
	if (first_node >= 0)
		last_node  = bit_fls(avail_bitmap);
	else
		last_node = first_node - 1;
	for (inx = first_node; inx <= last_node; inx++) {
		coff = cr_get_coremap_offset(inx);
		coff2 = cr_get_coremap_offset(inx + 1);
		local_cores = coff2 - coff;

		bit_clear(avail_bitmap, inx);
		if (local_cores < core_cnt[node_offset])
			local_cores = -1;
		else
			local_cores = core_cnt[node_offset];
		for (jnx = 0; jnx < local_cores; jnx++) {
			if (!bit_test(tmpcore, coff + jnx))
				break;
			bit_set(*core_bitmap, coff + jnx);
		}
		if (jnx < core_cnt[node_offset])
			continue;
		local_cores = coff2 - coff;
		for (jnx = core_cnt[node_offset]; jnx < local_cores; jnx++) {
			bit_clear(tmpcore, coff + jnx);
		}
		bit_set(sp_avail_bitmap, inx);
		if (core_cnt[++node_offset] == 0)
			break;
	}

	FREE_NULL_BITMAP(tmpcore);
	if (core_cnt[node_offset]) {
		info("reservation request can not be satisfied");
		FREE_NULL_BITMAP(sp_avail_bitmap);
	}

	return sp_avail_bitmap;
}

/* Test that sufficient cores are available on the specified node for use
 * IN/OUT core_bitmap - cores which are NOT available for use (i.e. specialized
 *		cores or those already reserved), all cores/bits for the
 *		specified node will be cleared if the available count is too low
 * IN node - index of node to test
 * IN cores_per_node - minimum number of nodes which should be available
 * RET count of cores available on this node
 */
static int _get_avail_core_in_node(bitstr_t *core_bitmap, int node,
				   int cores_per_node)
{
	int coff;
	int total_cores;
	int i;
	int avail = 0;

	coff = cr_get_coremap_offset(node);
	total_cores = cr_node_num_cores[node];

	if (!core_bitmap)
		return total_cores;

	for (i = 0; i < total_cores; i++) {
		if (!bit_test(core_bitmap, coff + i))
			avail++;
	}

	if (avail >= cores_per_node)
		return avail;

	bit_nclear(core_bitmap, coff, coff + total_cores - 1);
	return 0;
}

/* Given available node and core bitmaps, remove all specialized cores
 * node_bitmap IN - Nodes available for use
 * core_bitmap IN/OUT - Cores currently NOT available for use */
static void _spec_core_filter(bitstr_t *node_bitmap, bitstr_t **core_bitmap)
{
	bitstr_t **p_spec_core_map = make_core_bitmap(node_bitmap, NO_VAL16);
	bitstr_t *spec_core_map = *p_spec_core_map;

	*p_spec_core_map = NULL;
	free_core_array(&p_spec_core_map);

	bit_not(spec_core_map);

	xassert(core_bitmap);
	if (*core_bitmap) {
		bit_or(*core_bitmap, spec_core_map);
		bit_free(spec_core_map);
	} else {
		*core_bitmap = spec_core_map;
	}
}

extern bitstr_t * select_p_resv_test(resv_desc_msg_t *resv_desc_ptr,
				     uint32_t node_cnt,
				     bitstr_t *avail_bitmap,
				     bitstr_t **core_bitmap)
{
	bitstr_t **switches_bitmap;		/* nodes on this switch */
	bitstr_t **switches_core_bitmap;	/* cores on this switch */
	int       *switches_cpu_cnt;		/* total CPUs on switch */
	int       *switches_node_cnt;		/* total nodes on switch */
	int       *switches_required;		/* set if has required node */

	bitstr_t  *avail_nodes_bitmap = NULL;	/* nodes on any switch */
	bitstr_t *sp_avail_bitmap;
	int rem_nodes, rem_cores = 0;		/* remaining resources desired */
	int c, i, j, k, n, prev_rem_cores;
	int best_fit_inx, first, last;
	int best_fit_nodes;
	int best_fit_location = 0, best_fit_sufficient;
	bool sufficient;
	int cores_per_node = 1;	/* Minimum cores per node to consider */
	uint32_t *core_cnt, flags, rem_cores_save;
	bool aggr_core_cnt = false, clear_core;

	xassert(avail_bitmap);
	xassert(resv_desc_ptr);

	core_cnt = resv_desc_ptr->core_cnt;
	flags = resv_desc_ptr->flags;

	if ((flags & RESERVE_FLAG_FIRST_CORES) && core_cnt) {
		return _pick_first_cores(avail_bitmap, node_cnt, core_cnt,
					 core_bitmap);
	}

	/* When reservation includes a nodelist we use _sequential_pick code */
	if (!switch_record_cnt || !switch_record_table || !node_cnt)  {
		return _sequential_pick(avail_bitmap, node_cnt, core_cnt,
					core_bitmap);
	}

	/* Use topology state information */
	if (bit_set_count(avail_bitmap) < node_cnt)
		return avail_nodes_bitmap;

	if (core_cnt)
		_spec_core_filter(avail_bitmap, core_bitmap);

	rem_nodes = node_cnt;
	if (core_cnt && core_cnt[1]) {	/* Array of core counts */
		for (j = 0; core_cnt[j]; j++) {
			rem_cores += core_cnt[j];
			if (j == 0)
				cores_per_node = core_cnt[j];
			else if (cores_per_node > core_cnt[j])
				cores_per_node = core_cnt[j];
		}
	} else if (core_cnt) {		/* Aggregate core count */
		rem_cores = core_cnt[0];
		cores_per_node = core_cnt[0] / MAX(node_cnt, 1);
		aggr_core_cnt = true;
	} else if (cr_node_num_cores)
		cores_per_node = cr_node_num_cores[0];
	else
		cores_per_node = 1;
	rem_cores_save = rem_cores;

	/* Construct a set of switch array entries,
	 * use the same indexes as switch_record_table in slurmctld */
	switches_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	switches_core_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	switches_cpu_cnt = xcalloc(switch_record_cnt, sizeof(int));
	switches_node_cnt = xcalloc(switch_record_cnt, sizeof(int));
	switches_required = xcalloc(switch_record_cnt, sizeof(int));

	for (i = 0; i < switch_record_cnt; i++) {
		char str[100];
		switches_bitmap[i] = bit_copy(switch_record_table[i].
					      node_bitmap);
		bit_and(switches_bitmap[i], avail_bitmap);
		switches_node_cnt[i] = bit_set_count(switches_bitmap[i]);

		switches_core_bitmap[i] =
			_make_core_bitmap_filtered(switches_bitmap[i], 1);

		if (*core_bitmap) {
			bit_and_not(switches_core_bitmap[i], *core_bitmap);
		}
		bit_fmt(str, sizeof(str), switches_core_bitmap[i]);
		switches_cpu_cnt[i] = bit_set_count(switches_core_bitmap[i]);
		debug2("switch:%d nodes:%d cores:%d:%s",
		       i, switches_node_cnt[i], switches_cpu_cnt[i], str);
	}

	/* Remove nodes with less available cores than needed */
	if (core_cnt) {
		n = 0;
		for (j = 0; j < switch_record_cnt; j++) {
			first = bit_ffs(switches_bitmap[j]);
			if (first >= 0)
				last = bit_fls(switches_bitmap[j]);
			else
				last = first - 1;
			for (i = first; i <= last; i++) {
				if (!bit_test(switches_bitmap[j], i))
					continue;

				c = _get_avail_core_in_node(*core_bitmap, i,
							    cores_per_node);
				clear_core = false;
				if (aggr_core_cnt && (c < cores_per_node)) {
					clear_core = true;
				} else if (aggr_core_cnt) {
					;
				} else if (c < core_cnt[n]) {
					clear_core = true;
				} else if (core_cnt[n]) {
					n++;
				}
				if (!clear_core)
					continue;
				for (k = 0; k < switch_record_cnt; k++) {
					if (!switches_bitmap[k] ||
					    !bit_test(switches_bitmap[k], i))
						continue;
					bit_clear(switches_bitmap[k], i);
					switches_node_cnt[k]--;
					switches_cpu_cnt[k] -= c;
				}
			}
		}
	}

#if SELECT_DEBUG
	/* Don't compile this, it slows things down too much */
	for (i = 0; i < switch_record_cnt; i++) {
		char *node_names = NULL;
		if (switches_node_cnt[i])
			node_names = bitmap2node_name(switches_bitmap[i]);
		info("switch=%s nodes=%u:%s cpus:%d required:%u speed=%u",
		     switch_record_table[i].name,
		     switches_node_cnt[i], node_names,
		     switches_cpu_cnt[i], switches_required[i],
		     switch_record_table[i].link_speed);
		xfree(node_names);
	}
#endif

	/* Determine lowest level switch satisfying request with best fit */
	best_fit_inx = -1;
	for (j = 0; j < switch_record_cnt; j++) {
		if ((switches_node_cnt[j] < rem_nodes) ||
		    (core_cnt && (switches_cpu_cnt[j] < rem_cores)))
			continue;
		if ((best_fit_inx == -1) ||
		    (switch_record_table[j].level <
		     switch_record_table[best_fit_inx].level) ||
		    ((switch_record_table[j].level ==
		      switch_record_table[best_fit_inx].level) &&
		     (switches_node_cnt[j] < switches_node_cnt[best_fit_inx])))
			/* We should use core count by switch here as well */
			best_fit_inx = j;
	}
	if (best_fit_inx == -1) {
		debug("select_p_resv_test: could not find resources for "
		      "reservation");
		goto fini;
	}

	/* Identify usable leafs (within higher switch having best fit) */
	for (j = 0; j < switch_record_cnt; j++) {
		if ((switch_record_table[j].level != 0) ||
		    (!bit_super_set(switches_bitmap[j],
				    switches_bitmap[best_fit_inx]))) {
			switches_node_cnt[j] = 0;
		}
	}

	/* Select resources from these leafs on a best-fit basis */
	avail_nodes_bitmap = bit_alloc(node_record_count);
	while (rem_nodes > 0) {
		int avail_cores_in_node;
		best_fit_nodes = best_fit_sufficient = 0;
		for (j = 0; j < switch_record_cnt; j++) {
			if (switches_node_cnt[j] == 0)
				continue;
			if (core_cnt) {
				sufficient =
					(switches_node_cnt[j] >= rem_nodes) &&
					(switches_cpu_cnt[j] >= rem_cores);
			} else
				sufficient = switches_node_cnt[j] >= rem_nodes;
			/* If first possibility OR */
			/* first set large enough for request OR */
			/* tightest fit (less resource waste) OR */
			/* nothing yet large enough, but this is biggest */
			if ((best_fit_nodes == 0) ||
			    (sufficient && (best_fit_sufficient == 0)) ||
			    (sufficient &&
			     (switches_node_cnt[j] < best_fit_nodes)) ||
			    ((sufficient == 0) &&
			     (switches_node_cnt[j] > best_fit_nodes))) {
				best_fit_nodes = switches_node_cnt[j];
				best_fit_location = j;
				best_fit_sufficient = sufficient;
			}
		}
		if (best_fit_nodes == 0)
			break;
		/* Use select nodes from this leaf */
		first = bit_ffs(switches_bitmap[best_fit_location]);
		if (first >= 0)
			last  = bit_fls(switches_bitmap[best_fit_location]);
		else
			last = first - 1;
		for (i = first; i <= last; i++) {
			if (!bit_test(switches_bitmap[best_fit_location], i))
				continue;
			bit_clear(switches_bitmap[best_fit_location], i);
			switches_node_cnt[best_fit_location]--;

			if (bit_test(avail_nodes_bitmap, i)) {
				/* node on multiple leaf switches
				 * and already selected */
				continue;
			}

			avail_cores_in_node = 0;
			if (*core_bitmap) {
				int coff;
				coff = cr_get_coremap_offset(i);
				debug2("Testing node %d, core offset %d",
				       i, coff);
				for (j = 0; j < cr_node_num_cores[i]; j++) {
					if (!bit_test(*core_bitmap, coff + j))
						avail_cores_in_node++;
				}
				if (avail_cores_in_node < cores_per_node)
					continue;

				debug2("Using node %d with %d cores available",
				       i, avail_cores_in_node);
			}

			bit_set(avail_nodes_bitmap, i);
			rem_cores -= avail_cores_in_node;
			if (--rem_nodes <= 0)
				break;
		}
		switches_node_cnt[best_fit_location] = 0;
	}
	if ((rem_nodes > 0) || (rem_cores > 0))	/* insufficient resources */
		FREE_NULL_BITMAP(avail_nodes_bitmap);

fini:	for (i = 0; i < switch_record_cnt; i++) {
		FREE_NULL_BITMAP(switches_bitmap[i]);
		FREE_NULL_BITMAP(switches_core_bitmap[i]);
	}

	xfree(switches_bitmap);
	xfree(switches_core_bitmap);
	xfree(switches_cpu_cnt);
	xfree(switches_node_cnt);
	xfree(switches_required);

	if (avail_nodes_bitmap && core_cnt) {
		/* Reservation is using partial nodes */
		bitstr_t *exc_core_bitmap = NULL;

		sp_avail_bitmap = bit_alloc(bit_size(avail_bitmap));
		if (*core_bitmap) {
			exc_core_bitmap = *core_bitmap;
			*core_bitmap = bit_alloc(bit_size(exc_core_bitmap));
		}

		rem_cores = rem_cores_save;
		n = 0;
		prev_rem_cores = -1;
		while (rem_cores) {
			uint32_t coff;
			int inx, i;
			int avail_cores_in_node;

			inx = bit_ffs(avail_nodes_bitmap);
			if ((inx < 0) && aggr_core_cnt && (rem_cores > 0) &&
			    (rem_cores != prev_rem_cores)) {
				/* Make another pass over nodes to reach
				 * requested aggregate core count */
				bit_or(avail_nodes_bitmap, sp_avail_bitmap);
				inx = bit_ffs(avail_nodes_bitmap);
				prev_rem_cores = rem_cores;
				cores_per_node = 1;
			}
			if (inx < 0)
				break;

			debug2("Using node inx %d cores_per_node %d "
			       "rem_cores %u", inx, cores_per_node, rem_cores);
			coff = cr_get_coremap_offset(inx);

			/* Clear this node from the initial available bitmap */
			bit_clear(avail_nodes_bitmap, inx);

			if (cr_node_num_cores[inx] < cores_per_node)
				continue;

			avail_cores_in_node = 0;
			for (i = 0; i < cr_node_num_cores[inx]; i++) {
				if (!bit_test(exc_core_bitmap, coff + i)) {
					avail_cores_in_node++;
				}
			}

			debug2("Node %d has %d available cores", inx,
			       avail_cores_in_node);

			if (avail_cores_in_node < cores_per_node)
				continue;

			avail_cores_in_node = 0;
			for (i = 0; i < cr_node_num_cores[inx]; i++) {
				if (!bit_test(exc_core_bitmap, coff + i)) {
					// info("PICK NODE:%u BIT:%u", inx, i);
					bit_set(*core_bitmap, coff + i);
					bit_set(exc_core_bitmap, coff + i);
					rem_cores--;
					avail_cores_in_node++;
				}

				if (rem_cores == 0)
					break;
				if (aggr_core_cnt &&
				    (avail_cores_in_node >= cores_per_node))
					break;
				if (!aggr_core_cnt &&
				    (avail_cores_in_node >= core_cnt[n]))
					break;
			}

			/* Add this node to the final node bitmap */
			bit_set(sp_avail_bitmap, inx);
			n++;
		}
		FREE_NULL_BITMAP(avail_nodes_bitmap);
		FREE_NULL_BITMAP(exc_core_bitmap);

		if (rem_cores) {
			info("reservation request can not be satisfied");
			FREE_NULL_BITMAP(sp_avail_bitmap);
			return NULL;
		}
		return sp_avail_bitmap;
	}

	return avail_nodes_bitmap;
}
