/*****************************************************************************\
 *  wckey_functions.c - functions dealing with wckeys in the accounting system.
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
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

#include "src/sacctmgr/sacctmgr.h"
#include "src/common/uid.h"
#include "src/interfaces/data_parser.h"

static int _set_cond(int *start, int argc, char **argv,
		     slurmdb_wckey_cond_t *wckey_cond,
		     List format_list)
{
	int i;
	int set = 0;
	int end = 0;
	int command_len = 0;

	if (!wckey_cond) {
		error("No wckey_cond given");
		return -1;
	}

	for (i=(*start); i<argc; i++) {
		end = parse_option_end(argv[i]);
		if (!end)
			command_len=strlen(argv[i]);
		else {
			command_len=end-1;
			if (argv[i][end] == '=') {
				end++;
			}
		}

		if (!end && !xstrncasecmp(argv[i], "where",
					MAX(command_len, 5))) {
			continue;
		} else if (!end && !xstrncasecmp(argv[i], "withdeleted",
					  MAX(command_len, 5))) {
			wckey_cond->with_deleted = 1;
			set = 1;
		} else if (!end
			  || !xstrncasecmp(argv[i], "WCKeys",
					   MAX(command_len, 3))
			  || !xstrncasecmp(argv[i], "Names",
					   MAX(command_len, 3))) {
			if (!wckey_cond->name_list)
				wckey_cond->name_list = list_create(xfree_ptr);
			if (slurm_addto_char_list(wckey_cond->name_list,
						 argv[i]+end))
				set = 1;
		} else if (!xstrncasecmp(argv[i], "Ids",
					 MAX(command_len, 1))) {
			ListIterator itr = NULL;
			char *temp = NULL;
			uint32_t id = 0;

			if (!wckey_cond->id_list)
				wckey_cond->id_list = list_create(xfree_ptr);

			if (slurm_addto_char_list(wckey_cond->id_list,
						 argv[i]+end))
				set = 1;

			/* check to make sure user gave ints here */
			itr = list_iterator_create(wckey_cond->id_list);
			while ((temp = list_next(itr))) {
				if (get_uint(temp, &id, "WCKeyID")
				    != SLURM_SUCCESS) {
					exit_code = 1;
					list_delete_item(itr);
				}
			}
			list_iterator_destroy(itr);
		} else if (!xstrncasecmp(argv[i], "Clusters",
					 MAX(command_len, 3))) {
			if (!wckey_cond->cluster_list)
				wckey_cond->cluster_list =
					list_create(xfree_ptr);
			if (slurm_addto_char_list(wckey_cond->cluster_list,
						 argv[i]+end))
				set = 1;
		} else if (!xstrncasecmp(argv[i], "End", MAX(command_len, 1))) {
			wckey_cond->usage_end = parse_time(argv[i]+end, 1);
			set = 1;
		} else if (!xstrncasecmp(argv[i], "Format",
					 MAX(command_len, 1))) {
			if (format_list)
				slurm_addto_char_list(format_list, argv[i]+end);
		} else if (!xstrncasecmp(argv[i], "Start",
					 MAX(command_len, 1))) {
			wckey_cond->usage_start = parse_time(argv[i]+end, 1);
			set = 1;
		} else if (!xstrncasecmp(argv[i], "Users",
					 MAX(command_len, 1))) {
			if (!wckey_cond->user_list)
				wckey_cond->user_list = list_create(xfree_ptr);
			if (slurm_addto_char_list_with_case(wckey_cond->user_list,
							    argv[i]+end,
							    user_case_norm))
				set = 1;
		} else {
			exit_code=1;
			fprintf(stderr, " Unknown condition: %s\n", argv[i]);
		}
	}

	(*start) = i;

	return set;
}

extern int sacctmgr_list_wckey(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	slurmdb_wckey_cond_t *wckey_cond =
		xmalloc(sizeof(slurmdb_wckey_cond_t));
	List wckey_list = NULL;
	int i=0;
	ListIterator itr = NULL;
	ListIterator itr2 = NULL;
	slurmdb_wckey_rec_t *wckey = NULL;
	char *object;

	print_field_t *field = NULL;
	int field_count = 0;

	List format_list = list_create(xfree_ptr);
	List print_fields_list; /* types are of print_field_t */

	enum {
		PRINT_CLUSTER,
		PRINT_ID,
		PRINT_NAME,
		PRINT_USER
	};

	for (i=0; i<argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp (argv[i], "Where", MAX(command_len, 5))
		    || !xstrncasecmp (argv[i], "Set", MAX(command_len, 3)))
			i++;
		_set_cond(&i, argc, argv, wckey_cond, format_list);
	}

	if (exit_code) {
		slurmdb_destroy_wckey_cond(wckey_cond);
		FREE_NULL_LIST(format_list);
		return SLURM_ERROR;
	}

	if (!list_count(format_list)) {
		slurm_addto_char_list(format_list,
				      "Name,Cluster,User");
	}

	print_fields_list = list_create(destroy_print_field);

	itr = list_iterator_create(format_list);
	while((object = list_next(itr))) {
		char *tmp_char = NULL;
		int command_len = 0;
		int newlen = 0;

		if ((tmp_char = strstr(object, "\%"))) {
			newlen = atoi(tmp_char+1);
			tmp_char[0] = '\0';
		}

		command_len = strlen(object);

		field = xmalloc(sizeof(print_field_t));
		if (!xstrncasecmp("WCKeys", object, MAX(command_len, 1))
		   || !xstrncasecmp("Names", object, MAX(command_len, 1))) {
			field->type = PRINT_NAME;
			field->name = xstrdup("WCKey");
			field->len = 10;
			field->print_routine = print_fields_str;
		} else if (!xstrncasecmp("Clusters", object,
					 MAX(command_len, 2))) {
			field->type = PRINT_CLUSTER;
			field->name = xstrdup("Cluster");
			field->len = 10;
			field->print_routine = print_fields_str;
		} else if (!xstrncasecmp("IDs", object, MAX(command_len, 1))) {
			field->type = PRINT_ID;
			field->name = xstrdup("ID");
			field->len = 6;
			field->print_routine = print_fields_uint;
		} else if (!xstrncasecmp("Users", object, MAX(command_len, 1))) {
			field->type = PRINT_USER;
			field->name = xstrdup("User");
			field->len = 10;
			field->print_routine = print_fields_str;
		} else {
			exit_code=1;
			fprintf(stderr, "Unknown field '%s'\n", object);
			xfree(field);
			continue;
		}

		if (newlen)
			field->len = newlen;

		list_append(print_fields_list, field);
	}
	list_iterator_destroy(itr);
	FREE_NULL_LIST(format_list);

	if (exit_code) {
		slurmdb_destroy_wckey_cond(wckey_cond);
		FREE_NULL_LIST(print_fields_list);
		return SLURM_ERROR;
	}

	wckey_list = slurmdb_wckeys_get(db_conn, wckey_cond);
	slurmdb_destroy_wckey_cond(wckey_cond);

	if (mime_type) {
		rc = DATA_DUMP_CLI(WCKEY_LIST, wckey_list, "wckeys", argc, argv,
				   db_conn, mime_type, NULL);
		FREE_NULL_LIST(print_fields_list);
		FREE_NULL_LIST(wckey_list);
		return rc;
	}

	if (!wckey_list) {
		exit_code=1;
		fprintf(stderr, " Error with request: %s\n",
			slurm_strerror(errno));
		FREE_NULL_LIST(print_fields_list);
		return SLURM_ERROR;
	}

	itr = list_iterator_create(wckey_list);
	itr2 = list_iterator_create(print_fields_list);
	print_fields_header(print_fields_list);

	field_count = list_count(print_fields_list);

	while((wckey = list_next(itr))) {
		int curr_inx = 1;
		while((field = list_next(itr2))) {
			switch(field->type) {
				/* All the association stuff */
			case PRINT_CLUSTER:
				field->print_routine(
					field,
					wckey->cluster,
					(curr_inx == field_count));
				break;
			case PRINT_ID:
				field->print_routine(
					field,
					&wckey->id,
					(curr_inx == field_count));
				break;
			case PRINT_NAME:
				field->print_routine(
					field,
					wckey->name,
					(curr_inx == field_count));
				break;
			case PRINT_USER:
				field->print_routine(
					field,
					wckey->user,
					(curr_inx == field_count));
				break;
			default:
				field->print_routine(
					field, NULL,
					(curr_inx == field_count));
				break;
			}
			curr_inx++;
		}
		list_iterator_reset(itr2);
		printf("\n");
	}


	list_iterator_destroy(itr2);
	list_iterator_destroy(itr);
	FREE_NULL_LIST(wckey_list);
	FREE_NULL_LIST(print_fields_list);

	return rc;
}
