/*****************************************************************************\
 *  openapi.c - OpenAPI plugin handler
 *****************************************************************************
 *  Copyright (C) 2019-2020 SchedMD LLC.
 *  Written by Nathan Rini <nate@schedmd.com>
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

#include "slurm/slurm.h"

#include "src/common/data.h"
#include "src/common/plugin.h"
#include "src/common/read_config.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/serializer.h"
#include "src/slurmrestd/openapi.h"

#define MAGIC_PATH 0x1121baef
#define MAGIC_OAS 0x1211be0f

typedef struct {
	int (*init)(void);
	int (*fini)(void);
	data_t *(*get_oas)(openapi_spec_flags_t *flags);
} slurm_openapi_ops_t;

/*
 * Must be synchronized with slurm_openapi_ops_t above.
 */
static const char *syms[] = {
	"slurm_openapi_p_init",
	"slurm_openapi_p_fini",
	"slurm_openapi_p_get_specification",
};

typedef enum {
	OPENAPI_PATH_ENTRY_UNKNOWN = 0,
	OPENAPI_PATH_ENTRY_MATCH_STRING,
	OPENAPI_PATH_ENTRY_MATCH_PARAMETER,
	OPENAPI_PATH_ENTRY_MAX
} entry_type_t;

/*
 * This is a simplified entry since OAS allows combos of
 * parameters but we will only honor having a single parameter
 * as an dir entry for now
 */
typedef struct {
	char *entry;
	char *name;
	entry_type_t type;
	openapi_type_t parameter;
} entry_t;

typedef struct {
	entry_t *entries;
	http_request_method_t method;
} entry_method_t;

typedef struct {
	entry_method_t *methods;
	int tag;
} path_t;

typedef struct {
	entry_method_t *method;
	entry_t *entries;
	path_t *path;
	const char *str_path;
	data_t *spec;
} populate_methods_t;

typedef struct {
	const data_t *path;        /* path requested to match */
	const data_t *path_list;   /* dictionary of all paths under server */
	const data_t *server_path; /* path from servers object */
	const data_t *found;
} match_path_string_t;

typedef struct {
	bool matched;
	const data_t *dpath;
	path_t *path;
	data_t *params;
	http_request_method_t method;
	entry_t *entry;
	int tag;
} match_path_from_data_t;

typedef struct {
	data_t *src_paths;
	data_t *dst_paths;
	openapi_spec_flags_t flags;
} merge_path_server_t;

typedef struct {
	const char *name;
	bool found;
} list_find_dict_name_t;

typedef struct {
	data_t *paths;
	data_t *server_path;
	openapi_spec_flags_t flags;
} merge_path_t;

typedef struct {
	data_t *server_path;
	char *operation;
	char *at;
	char *path;
	merge_path_t *merge_args;
} id_merge_path_t;

struct openapi_s {
	int magic;
	List paths;
	int path_tag_counter;
	data_t **spec;
	openapi_spec_flags_t *spec_flags;

	slurm_openapi_ops_t *ops;
	int context_cnt;
	plugin_context_t **context;

	plugin_handle_t *plugin_handles;
	char **plugin_types;
	size_t plugin_count;
	plugrack_t *rack;

	data_parser_t **parsers; /* symlink to parser array */
};

static char *_entry_to_string(entry_t *entry);

static const char *_get_entry_type_string(entry_type_t type)
{
	switch (type) {
	case OPENAPI_PATH_ENTRY_MATCH_STRING:
		return "string";
	case OPENAPI_PATH_ENTRY_MATCH_PARAMETER:
		return "parameter";
	default:
		return "invalid";
	}
}

static const data_t *_resolve_ref(const data_t *spec, const data_t *dref)
{
	const char *ref;

	if (!dref)
		return NULL;

	ref = data_get_string_const(dref);

	if (ref[0] == '#')
		ref = &ref[1];

	return data_resolve_dict_path_const(spec, ref);
}

static void _free_entry_list(entry_t *entry, int tag,
			     entry_method_t *method)
{
	entry_t *itr = entry;

	if (!entry)
		return;

	while (itr->type) {
		debug5("%s: remove path tag:%d method:%s entry:%s name:%s",
		       __func__, tag,
		       (method ? get_http_method_string(method->method) :
				       "N/A"),
		       itr->entry, itr->name);
		xfree(itr->entry);
		xfree(itr->name);
		itr++;
	}

	xfree(entry);
}

static void _list_delete_path_t(void *x)
{
	entry_method_t *em;

	if (!x)
		return;

	path_t *path = x;
	xassert(path->tag != -1);
	em = path->methods;

	while (em->entries) {
		debug5("%s: remove path tag:%d method:%s", __func__, path->tag,
		       get_http_method_string(em->method));

		_free_entry_list(em->entries, path->tag, em);
		em->entries = NULL;
		em++;
	}

	xfree(path->methods);
	xfree(path);
}

static entry_t *_parse_openapi_path(const char *str_path)
{
	char *save_ptr = NULL;
	char *buffer = xstrdup(str_path);
	char *token = strtok_r(buffer, "/", &save_ptr);
	entry_t *entries = NULL;
	entry_t *entry = NULL;
	int count = 0;

	/* find max bound on number of entries */
	for (const char *i = str_path; *i; i++)
		if (*i == '/')
			count++;

	if (count > 1024)
		fatal_abort("%s: url %s is way too long", str_path, __func__);

	entry = entries = xcalloc((count + 1), sizeof(entry_t));

	while (token) {
		const size_t slen = strlen(token);

		/* ignore // entries */
		if (slen <= 0)
			goto again;

		entry->entry = xstrdup(token);

		if (!xstrcmp(token, ".") || !xstrcmp(token, "..")) {
			/*
			 * there should not be a .. or . in a path
			 * definition, it just doesn't make any sense
			 */
			error("%s: invalid %s at entry",
			      __func__, token);
			goto fail;
		} else if (slen > 3 && token[0] == '{' &&
			   token[slen - 1] == '}') {
			entry->type = OPENAPI_PATH_ENTRY_MATCH_PARAMETER;
			entry->name = xstrndup(token + 1, slen - 2);

			debug5("%s: parameter %s at entry %s",
			       __func__, entry->name, token);
		} else { /* not a variable */
			entry->type = OPENAPI_PATH_ENTRY_MATCH_STRING;
			entry->name = NULL;

			debug5("%s: string match entry %s",
			       __func__, token);
		}

		entry++;
		xassert(entry <= entries + count);
again:
		token = strtok_r(NULL, "/", &save_ptr);
	}

	/* last is always NULL */
	xassert(!entry->type);
	xfree(buffer);
	return entries;

fail:
	for (entry = entries; entry <= entries + count; entry++)
		xfree(entry->entry);

	xfree(entries);
	xfree(buffer);
	return NULL;
}

static int _print_path_tag_methods(void *x, void *arg)
{
	path_t *path = (path_t *) x;
	int *tag = (int *) arg;

	if (path->tag != *tag)
		return 0;

	if (!path->methods->entries)
		debug4("%s: no methods found in path tag %d",
		       __func__, path->tag);

	for (entry_method_t *em = path->methods; em->entries; em++) {
		char *path_str = _entry_to_string(em->entries);

		debug4("%s: path tag %d entry: %s %s",
		       __func__, path->tag, get_http_method_string(em->method),
		       path_str);

		xfree(path_str);
	}

	/*
	 * We found the (unique) tag, so return -1 to exit early. The item's
	 * index returned by list_for_each_ro() will be negative.
	 */
	return -1;
}

extern void print_path_tag_methods(openapi_t *oas, int tag)
{
	if (get_log_level() < LOG_LEVEL_DEBUG4)
		return;

	xassert(oas->magic == MAGIC_OAS);

	if (list_for_each_ro(oas->paths, _print_path_tag_methods, &tag) >= 0)
		error("%s: Tag %d not found in oas->paths", __func__, tag);
}

static bool _match_server_path(const data_t *server_path, const data_t *path,
			       const data_t *match_path)
{
	bool found;
	const data_t *join[3] = {0};
	data_t *joined_path;

	join[0] = server_path;
	join[1] = path;
	joined_path = data_list_join(join, true);
	found = data_check_match(joined_path, match_path, false);

	if (get_log_level() >= LOG_LEVEL_DEBUG5) {
		char *joined_path_str = NULL, *mpath_str = NULL;

		serialize_g_data_to_string(&joined_path_str, NULL, joined_path,
					   MIME_TYPE_JSON, SER_FLAGS_COMPACT);
		serialize_g_data_to_string(&mpath_str, NULL, match_path,
					   MIME_TYPE_JSON, SER_FLAGS_COMPACT);

		debug5("%s: match:%s server_path:%s match_path:%s",
		       __func__, (found ? "T" : "F"),
		       joined_path_str, mpath_str);

		xfree(joined_path_str);
		xfree(mpath_str);
	}

	FREE_NULL_DATA(joined_path);

	return found;
}

static data_for_each_cmd_t _match_server_override(const data_t *data,
						  void *arg)
{
	const data_t **fargs = (const data_t **) arg;
	const data_t *surl;
	data_t *spath;
	data_for_each_cmd_t rc = DATA_FOR_EACH_CONT;

	surl = data_resolve_dict_path_const(data, "url");

	if (!surl) {
		char *d = NULL;

		serialize_g_data_to_string(&d, NULL, data, MIME_TYPE_JSON,
					   SER_FLAGS_COMPACT);

		fatal("%s: server %s lacks url field required per OASv3.0.3 section 4.7.5",
		      __func__, d);
	}

	spath = parse_url_path(data_get_string_const(surl), true, true);

	if (_match_server_path(spath, fargs[1], fargs[0])) {
		fargs[2] = data;
		rc = DATA_FOR_EACH_STOP;
	}

	FREE_NULL_DATA(spath);

	return rc;
}

static data_for_each_cmd_t _match_path_string(const char *key,
					      const data_t *data,
					      void *arg)
{
	match_path_string_t *args = arg;
	data_t *mpath;
	data_for_each_cmd_t rc = DATA_FOR_EACH_CONT;
	const data_t *servers = data_key_get_const(data, "servers");

	mpath = parse_url_path(key, true, true);

	if (servers) {
		/*
		 * Alternative server specified per OASv3.0.3 section 4.7.9.1
		 * which overrides the global servers settings
		 */
		const data_t *fargs[3] = {0};
		fargs[0] = args->path;
		fargs[1] = mpath;

		if (data_list_for_each_const(servers, _match_server_override,
					     &fargs) < 0)
			fatal_abort("%s: unexpected for each failure",
				    __func__);

		if (fargs[2]) {
			args->found = data;
			rc = DATA_FOR_EACH_STOP;
		}
	} else if (_match_server_path(args->server_path, mpath, args->path)) {
		args->found = data;
		rc = DATA_FOR_EACH_STOP;
	}

	FREE_NULL_DATA(mpath);
	return rc;
}

static data_for_each_cmd_t _match_server_path_string(const data_t *data,
						     void *arg)
{
	match_path_string_t *args = arg;
	const data_t *surl;
	data_t *spath = NULL;
	data_for_each_cmd_t rc = DATA_FOR_EACH_CONT;

	surl = data_resolve_dict_path_const(data, "url");

	if (!surl) {
		char *d = NULL;

		serialize_g_data_to_string(&d, NULL, data, MIME_TYPE_JSON,
					   SER_FLAGS_COMPACT);

		fatal("%s: server %s lacks url field required per OASv3.0.3 section 4.7.5",
		      __func__, d);
	}

	args->server_path = spath = parse_url_path(data_get_string_const(surl),
						   true, true);

	if ((data_dict_for_each_const(args->path_list, _match_path_string, arg)
	     < 0) || args->found)
		rc = DATA_FOR_EACH_STOP;

	FREE_NULL_DATA(spath);
	args->server_path = NULL;

	return rc;
}

static const data_t *_find_spec_path(openapi_t *oas, const char *str_path,
				     data_t **spec)
{
	match_path_string_t args = {0};
	data_t *path = parse_url_path(str_path, true, true);
	args.path = path;

	for (size_t i = 0; oas->spec[i]; i++) {
		const data_t *servers =
			data_resolve_dict_path_const(oas->spec[i], "/servers");
		args.path_list =
			data_resolve_dict_path_const(oas->spec[i], "/paths");

		if (!args.path_list ||
		    (data_get_type(args.path_list) != DATA_TYPE_DICT) ||
		    !servers)
			continue;

		if (data_list_for_each_const(servers, _match_server_path_string,
					     &args) < 0)
			continue;

		args.path_list = NULL;

		if (args.found) {
			*spec = oas->spec[i];
			break;
		}
	}

	FREE_NULL_DATA(path);
	return args.found;
}

static data_for_each_cmd_t _populate_parameters(const data_t *data, void *arg)
{
	populate_methods_t *args = arg;
	entry_t *entry;
	const char *key = NULL;
	const data_t *dname, *dref;

	if ((dref = data_key_get_const(data, "$ref")))
		data = _resolve_ref(args->spec, dref);

	dname = data_key_get_const(data, "name");

	if (!dname || !(key = data_get_string_const(dname)) || !key[0]) {
		/* parameter doesn't have a name! */
		fatal("%s: path %s parameter has invalid name",
		      __func__, args->str_path);
	}

	for (entry = args->entries; entry->type; entry++)
		if (entry->type == OPENAPI_PATH_ENTRY_MATCH_PARAMETER &&
		    !xstrcasecmp(entry->name, key)) {
			char *buffer = NULL;
			if (!data_retrieve_dict_path_string(data, "schema/type",
							    &buffer)) {
				entry->parameter =
					openapi_string_to_type(buffer);
				if (entry->parameter == OPENAPI_TYPE_INVALID)
					fatal("%s: invalid type for %s",
					      __func__, key);
			} else
				fatal("%s: missing schema type for %s",
				      __func__, key);
			xfree(buffer);
			return DATA_FOR_EACH_CONT;
		}

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _populate_methods(const char *key,
					     const data_t *data,
					     void *arg)
{
	populate_methods_t *args = arg;
	populate_methods_t nargs = *args;
	entry_method_t *method = args->method;
	const data_t *para, *ref;
	int count = 0;
	entry_t *entry;
	http_request_method_t method_type;

	if (!xstrcasecmp(key, "servers"))
		return DATA_FOR_EACH_CONT;

	if ((method_type = get_http_method(key)) == HTTP_REQUEST_INVALID)
		fatal("%s: path %s has invalid HTTP method %s",
		      __func__, args->str_path, key);

	method->method = method_type;

	if (data_get_type(data) != DATA_TYPE_DICT)
		fatal("%s: path %s has unexpected data type %s instead of dictionary",
		      __func__, args->str_path,
		      data_type_to_string(data_get_type(data)));

	for (entry = args->entries; entry->type; entry++)
		count++;

	if (!method->entries) {
		/* only add entries on first method parse */
		method->entries = xcalloc((count + 1), sizeof(entry_t));
		/* Copy spec entry list into method entry list */
		entry_t *dest = method->entries;
		for (entry_t *src = args->entries; src->type; src++) {
			dest->entry = xstrdup(src->entry);
			dest->name = xstrdup(src->name);
			dest->type = src->type;
			dest->parameter = src->parameter;
			dest++;
		}
	}

	/* point to new entries clone */
	nargs.entries = method->entries;

	para = data_key_get_const(data, "parameters");
	if (!para || ((data_get_type(para) == DATA_TYPE_DICT) &&
		      (ref = data_key_get_const(para, OPENAPI_REF_TAG)))) {
		/* increment to next method entry */
		args->method++;
		return DATA_FOR_EACH_CONT;
	}
	if (data_get_type(para) != DATA_TYPE_LIST)
		fatal("%s: path %s parameters field is unexpected type %s",
		      __func__, args->str_path,
		      data_type_to_string(data_get_type(para)));
	if (data_list_for_each_const(para, _populate_parameters, &nargs) < 0)
		fatal("%s: path %s parameters failed parsing",
		      __func__, args->str_path);

	/* increment to next method entry */
	args->method++;

	if (get_log_level() >= LOG_LEVEL_DEBUG5)
		for (entry = method->entries; entry->type; entry++) {
			debug5("%s: add path %s entry: method=%s tag=%d entry=%s name=%s parameter=%s entry_type=%s",
			       __func__, args->str_path, key, args->path->tag,
			       entry->entry, entry->name,
			       openapi_type_to_string(entry->parameter),
			       _get_entry_type_string(entry->type));
		}

	return DATA_FOR_EACH_CONT;
}

extern int register_path_tag(openapi_t *oas, const char *str_path)
{
	int rc = -1;
	path_t *path = NULL;
	const data_t *spec_entry;
	populate_methods_t args = {
		.str_path = str_path,
	};
	entry_t *entries = _parse_openapi_path(str_path);

	if (!entries) {
		debug4("%s: _parse_openapi_path(%s) failed",
		       __func__, str_path);
		goto cleanup;
	}

	spec_entry = _find_spec_path(oas, str_path, &args.spec);
	if (!spec_entry) {
		debug4("%s: _find_spec_path(%s) failed",
		       __func__, str_path);
		goto cleanup;
	}

	if (data_get_type(spec_entry) != DATA_TYPE_DICT) {
		debug4("%s: ignoring %s at %s",
		       __func__, data_type_to_string(data_get_type(spec_entry)),
		       str_path);
		goto cleanup;
	}

	path = xmalloc(sizeof(*path));
	path->tag = oas->path_tag_counter++;
	path->methods = xcalloc((data_get_dict_length(spec_entry) + 1),
				sizeof(*path->methods));

	args.method = path->methods;
	args.entries = entries;
	args.path = path;
	if (data_dict_for_each_const(spec_entry, _populate_methods, &args) < 0)
		fatal("%s: _populate_methods() failed", __func__);

	list_append(oas->paths, path);

	rc = path->tag;

cleanup:
	_free_entry_list(entries, (path ? path->tag : -1), NULL);
	entries = NULL;

	return rc;
}

static int _rm_path_by_tag(void *x, void *tptr)
{
	path_t *path = (path_t *)x;
	const int tag = *(int*)tptr;

	if (path->tag != tag)
		return 0;

	debug5("%s: removing tag %d", __func__, path->tag);

	return 1;
}

extern void unregister_path_tag(openapi_t *oas, int tag)
{
	xassert(oas->magic == MAGIC_OAS);

	list_delete_all(oas->paths, _rm_path_by_tag, &tag);
}

/*
 * Check if the entry matches based on the OAS type
 * and if it does, then add that matched parameter
 */
static bool _match_param(const data_t *data, match_path_from_data_t *args)
{
	bool matched = false;
	entry_t *entry = args->entry;
	data_t *params = args->params;
	data_t *match = data_new();

	data_copy(match, data);

	switch (entry->parameter) {
	case OPENAPI_TYPE_NUMBER:
	{
		if (data_convert_type(match, DATA_TYPE_FLOAT) ==
		    DATA_TYPE_FLOAT) {
			data_set_float(data_key_set(params, entry->name),
				       data_get_float(match));
			matched = true;
		}
		break;
	}
	case OPENAPI_TYPE_INTEGER:
	{
		if (data_convert_type(match, DATA_TYPE_INT_64) ==
		    DATA_TYPE_INT_64) {
			data_set_int(data_key_set(params, entry->name),
				     data_get_int(match));
			matched = true;
		}
		break;
	}
	default: /* assume string */
		debug("%s: unknown parameter type %s",
		      __func__, openapi_type_to_string(entry->parameter));
		/* fall through */
	case OPENAPI_TYPE_STRING:
	{
		if (data_convert_type(match, DATA_TYPE_STRING) ==
		    DATA_TYPE_STRING) {
			data_set_string(data_key_set(params, entry->name),
					data_get_string(match));
			matched = true;
		}
		break;
	}
	}

	if (get_log_level() >= LOG_LEVEL_DEBUG5) {
		char *str = NULL;
		data_get_string_converted(data, &str);

		debug5("%s: parameter %s[%s]->%s[%s] result=%s",
		       __func__, entry->name,
		       openapi_type_to_string(entry->parameter),
		       str, data_type_to_string(data_get_type(data)),
		       (matched ? "matched" : "failed"));

		xfree(str);
	}

	FREE_NULL_DATA(match);
	return matched;
}

static data_for_each_cmd_t _match_path(const data_t *data, void *y)
{
	match_path_from_data_t *args = y;
	entry_t *entry = args->entry;
	args->matched = false;

	if (!entry->type)
		return DATA_FOR_EACH_FAIL;

	if (entry->type == OPENAPI_PATH_ENTRY_MATCH_STRING) {
		bool match;

		if (data_get_type(data) != DATA_TYPE_STRING)
			return DATA_FOR_EACH_FAIL;

		match = !xstrcmp(data_get_string_const(data), entry->entry);

		debug5("%s: string attempt match %s to %s: %s",
		       __func__, entry->entry, data_get_string_const(data),
		       (match ? "SUCCESS" : "FAILURE"));

		if (!match)
			return DATA_FOR_EACH_FAIL;
	} else if (entry->type == OPENAPI_PATH_ENTRY_MATCH_PARAMETER) {
		if (!_match_param(data, args))
			return DATA_FOR_EACH_FAIL;
	} else
		fatal_abort("%s: unknown OAS path entry match type",
			    __func__);

	args->entry++;
	args->matched = true;
	return DATA_FOR_EACH_CONT;
}

static char *_entry_to_string(entry_t *entry)
{
	char *path = NULL;
	data_t *d = data_set_list(data_new());

	for (; entry->type; entry++) {
		switch (entry->type) {
		case OPENAPI_PATH_ENTRY_MATCH_STRING:
			data_set_string(data_list_append(d), entry->entry);
			break;
		case OPENAPI_PATH_ENTRY_MATCH_PARAMETER:
			data_set_string_fmt(data_list_append(d), "{%s}",
					    entry->name);
			break;
		case OPENAPI_PATH_ENTRY_UNKNOWN:
		case OPENAPI_PATH_ENTRY_MAX:
			fatal_abort("invalid entry type");
		}
	}

	serialize_g_data_to_string(&path, NULL, d, MIME_TYPE_JSON,
				   SER_FLAGS_COMPACT);

	FREE_NULL_DATA(d);
	return path;
}

static int _match_path_from_data(void *x, void *key)
{
	char *dst_path = NULL, *src_path = NULL;
	match_path_from_data_t *args = key;
	path_t *path = x;
	entry_method_t *method;

	if (get_log_level() >= LOG_LEVEL_DEBUG5) {
		serialize_g_data_to_string(&dst_path, NULL, args->dpath,
					   MIME_TYPE_JSON, SER_FLAGS_COMPACT);
	}

	args->path = path;
	for (method = path->methods; method->entries; method++) {
		if (get_log_level() >= LOG_LEVEL_DEBUG5) {
			xfree(src_path);
			src_path = _entry_to_string(method->entries);
		}

		if (args->method != method->method) {
			debug5("%s: method skip for %s(%d, %s != %s) to %s(0x%"PRIXPTR")",
			       __func__, src_path, args->path->tag,
			       get_http_method_string(args->method),
			       get_http_method_string(method->method),
			       dst_path, (uintptr_t) args->dpath);
			continue;
		}

		args->entry = method->entries;
		data_list_for_each_const(args->dpath, _match_path, args);

		if (args->matched)
			break;
	}

	if (args->matched)
		debug5("%s: match successful for %s(%d, %s) to %s(0x%"PRIXPTR")",
		       __func__, src_path, args->path->tag,
		       get_http_method_string(args->method), dst_path,
		       (uintptr_t) args->dpath);
	else
		debug5("%s: match failed for %s(%d, %s) to %s(0x%"PRIXPTR")",
		       __func__, src_path, args->path->tag,
		       get_http_method_string(args->method), dst_path,
		       (uintptr_t) args->dpath);

	xfree(src_path);
	xfree(dst_path);

	if (args->matched) {
		args->tag = path->tag;
		return 1;
	} else {
		return 0;
	}
}

extern int find_path_tag(openapi_t *oas, const data_t *dpath, data_t *params,
			 http_request_method_t method)
{
	match_path_from_data_t args = {
		.params = params,
		.dpath = dpath,
		.method = method,
		.tag = -1,
	};

	xassert(oas->magic == MAGIC_OAS);
	xassert(data_get_type(params) == DATA_TYPE_DICT);

	(void) list_find_first(oas->paths, _match_path_from_data, &args);

	return args.tag;
}

static void _oas_plugrack_foreach(const char *full_type, const char *fq_path,
				  const plugin_handle_t id, void *arg)
{
	openapi_t *oas = arg;
	xassert(oas->magic == MAGIC_OAS);

	oas->plugin_count += 1;
	xrecalloc(oas->plugin_handles, oas->plugin_count,
		  sizeof(*oas->plugin_handles));
	xrecalloc(oas->plugin_types, oas->plugin_count,
		  sizeof(*oas->plugin_types));

	oas->plugin_types[oas->plugin_count - 1] = xstrdup(full_type);
	oas->plugin_handles[oas->plugin_count - 1] = id;

	debug5("%s: OAS plugin type:%s path:%s",
	       __func__, full_type, fq_path);
}

static data_for_each_cmd_t _foreach_remove_template(const char *key,
						    data_t *data, void *arg)
{
	/* remove every path with {data_parser} */

	if (!xstrstr(key, OPENAPI_DATA_PARSER_PARAM))
		return DATA_FOR_EACH_CONT;
	else
		return DATA_FOR_EACH_DELETE;
}

static int _apply_data_parser_specs(openapi_t *oas, int plugin_id)
{
	data_parser_t **parsers = oas->parsers;
	data_t *paths, *spec = oas->spec[plugin_id];

	for (int i = 0; parsers[i]; i++) {
		int rc;

		if ((rc = data_parser_g_specify(parsers[i], spec)) &&
		    (rc != ESLURM_NOT_SUPPORTED)) {
			error("%s: parser specification failed: %s",
			      __func__, slurm_strerror(rc));
			return rc;
		}
	}

	/* scrub the paths with {data_parser} */
	paths = data_resolve_dict_path(spec, OPENAPI_PATHS_PATH);
	(void) data_dict_for_each(paths, _foreach_remove_template, NULL);

	return SLURM_SUCCESS;
}

extern int init_openapi(openapi_t **oas, const char *plugins,
			plugrack_foreach_t listf, data_parser_t **parsers)
{

	openapi_t *t = NULL;
	int rc = SLURM_SUCCESS;

	xassert(!*oas);
	destroy_openapi(*oas);

	if ((rc = data_init()))
		return rc;

	/* must have JSON plugin to parse the openapi.json */
	if ((rc = serializer_g_init(MIME_TYPE_JSON_PLUGIN, NULL)))
		return rc;

	*oas = t = xmalloc(sizeof(*t));
	t->magic = MAGIC_OAS;
	t->paths = list_create(_list_delete_path_t);
	t->parsers = parsers;

	t->rack = plugrack_create("openapi");
	plugrack_read_dir(t->rack, slurm_conf.plugindir);

	if (plugins && !xstrcasecmp(plugins, "list")) {
		plugrack_foreach(t->rack, listf, oas);
		return SLURM_SUCCESS;
	} else if (plugins) {
		/* User provide which plugins they want */
		char *type, *last = NULL;
		char *pbuf = xstrdup(plugins);

		type = strtok_r(pbuf, ",", &last);
		while (type) {
			xstrtrim(type);

			/* Permit both prefix and no-prefix for plugin names. */
			if (xstrncmp(type, "openapi/", 8) == 0)
				type += 8;
			type = xstrdup_printf("openapi/%s", type);
			xstrtrim(type);

			_oas_plugrack_foreach(type, NULL, PLUGIN_INVALID_HANDLE,
					      t);

			xfree(type);
			type = strtok_r(NULL, ",", &last);
		}

		xfree(pbuf);
	} else /* Add all possible */
		plugrack_foreach(t->rack, _oas_plugrack_foreach, t);

	if (!t->plugin_count) {
		error("No OAS plugins to load. Nothing to do.");
		rc = SLURM_PLUGIN_NAME_INVALID;
	}

	for (size_t i = 0; i < t->plugin_count; i++) {
		if ((t->plugin_handles[i] == PLUGIN_INVALID_HANDLE) &&
		    (t->plugin_handles[i] =
		     plugrack_use_by_type(t->rack, t->plugin_types[i])) ==
		    PLUGIN_INVALID_HANDLE)
				fatal("Unable to find plugin: %s",
				      t->plugin_types[i]);
	}

	t->ops = xcalloc((t->plugin_count + 1), sizeof(*t->ops));
	t->context = xcalloc((t->plugin_count + 1), sizeof(*t->context));
	t->spec = xcalloc((t->plugin_count + 1), sizeof(*t->spec));
	t->spec_flags = xcalloc((t->plugin_count + 1), sizeof(*t->spec_flags));

	for (size_t i = 0; (i < t->plugin_count); i++) {
		openapi_spec_flags_t flags = OAS_FLAG_NONE;

		if (t->plugin_handles[i] == PLUGIN_INVALID_HANDLE) {
			error("Invalid plugin to load?");
			rc = ESLURM_PLUGIN_INVALID;
			break;
		}

		if (plugin_get_syms(t->plugin_handles[i], ARRAY_SIZE(syms), syms,
				    (void **)&t->ops[t->context_cnt]) <
		    ARRAY_SIZE(syms)) {
			error("Incomplete plugin detected");
			rc = ESLURM_PLUGIN_INCOMPLETE;
			break;
		}

		t->spec[t->context_cnt] =
			(*(t->ops[t->context_cnt].get_oas))(&flags);
		t->spec_flags[t->context_cnt] = flags;
		if (!t->spec[t->context_cnt]) {
			error("unable to load OpenAPI spec");
			rc = ESLURM_PLUGIN_INCOMPLETE;
			break;
		}

		debug2("%s: loaded plugin %s with flags 0x%"PRIx64,
		       __func__, t->plugin_types[i], flags);

		if (flags & OAS_FLAG_SET_DATA_PARSER_SPEC)
			_apply_data_parser_specs(t, t->context_cnt);

		t->context_cnt++;
	}

	for (size_t i = 0; !rc && (t->context_cnt > 0) && (i < t->context_cnt);
	     i++)
		(*(t->ops[i].init))();

	return rc;
}

extern void destroy_openapi(openapi_t *oas)
{
	int rc;

	if (!oas)
		return;

	xassert(oas->magic == MAGIC_OAS);

	for (size_t i = 0; (oas->context_cnt > 0) && (i < oas->context_cnt);
	     i++) {
		(*(oas->ops[i].fini))();

		if (oas->context[i] && plugin_context_destroy(oas->context[i]))
			fatal_abort("%s: unable to unload plugin", __func__);
	}
	xfree(oas->context);

	FREE_NULL_LIST(oas->paths);

	for (size_t i = 0; oas->spec[i]; i++)
		FREE_NULL_DATA(oas->spec[i]);
	xfree(oas->spec);
	xfree(oas->spec_flags);
	xfree(oas->ops);

	for (size_t i = 0; i < oas->plugin_count; i++) {
		plugrack_release_by_type(oas->rack, oas->plugin_types[i]);
		xfree(oas->plugin_types[i]);
	}
	xfree(oas->plugin_types);
	xfree(oas->plugin_handles);
	if ((rc = plugrack_destroy(oas->rack)))
		fatal_abort("unable to clean up plugrack: %s",
			    slurm_strerror(rc));
	oas->rack = NULL;

	oas->magic = ~MAGIC_OAS;
	xfree(oas);
}

static data_for_each_cmd_t _merge_schema(const char *key, data_t *data,
					 void *arg)
{
	data_t *cs = arg;
	data_t *e;

	if (data_get_type(data) != DATA_TYPE_DICT) {
		error("%s: expected schema[%s] as type dictionary but got type %s",
		      __func__, key, data_type_to_string(data_get_type(data)));
		return DATA_FOR_EACH_FAIL;
	}

	xassert(cs && (data_get_type(cs) == DATA_TYPE_DICT));
	e = data_key_set(cs, key);

	if (data_get_type(e) != DATA_TYPE_NULL)
		debug("%s: overwriting component schema %s",
		      __func__, key);

	(void) data_copy(e, data);

	return DATA_FOR_EACH_CONT;
}

/* find matching value of name in list of dictionary with "name" entry */
static data_for_each_cmd_t _list_find_dict_name(data_t *data, void *arg)
{
	list_find_dict_name_t *args = arg;
	data_t *name;

	if (data_get_type(data) != DATA_TYPE_DICT)
		return DATA_FOR_EACH_FAIL;

	if (!(name = data_key_get(data, "name")))
		return DATA_FOR_EACH_FAIL;

	if (data_convert_type(name, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return DATA_FOR_EACH_FAIL;

	if (!xstrcmp(args->name, data_get_string(name))) {
		args->found = true;
		return DATA_FOR_EACH_STOP;
	}

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _merge_tag(data_t *data, void *arg)
{
	data_t *tags = arg;
	data_t *name, *desc, *e;
	list_find_dict_name_t tag_name_args = { 0 };

	if (data_get_type(data) != DATA_TYPE_DICT)
		return DATA_FOR_EACH_FAIL;

	name = data_key_get(data, "name");
	desc = data_key_get(data, "description");

	if (data_convert_type(name, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return DATA_FOR_EACH_FAIL;
	if (data_convert_type(desc, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return DATA_FOR_EACH_FAIL;

	/* only add if not already defined */
	tag_name_args.name = data_get_string(name);
	if (data_list_for_each(tags, _list_find_dict_name, &tag_name_args) < 0)
		return DATA_FOR_EACH_FAIL;

	if (tag_name_args.found)
		return DATA_FOR_EACH_CONT;

	e = data_set_dict(data_list_append(tags));
	data_copy(data_key_set(e, "name"), name);
	data_copy(data_key_set(e, "description"), desc);

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _merge_operationId_strings(data_t *data, void *arg)
{
	id_merge_path_t *args = arg;
	char *p;

	if (data_convert_type(data, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return DATA_FOR_EACH_FAIL;

	p = data_get_string(data);

	/* sub out '.' for '_' to avoid breaking compilers */
	for (int s = strlen(p), i = 0; i < s; i++)
		if ((p[i] == '.') || (p[i] == '{') || (p[i] == '}'))
			p[i] = '_';

	xstrfmtcatat(args->operation, &args->at, "%s%s",
		     (args->operation ? "_" : ""), data_get_string(data));

	return DATA_FOR_EACH_CONT;
}

/*
 * Merge plugin id with operationIds in paths.
 * All operationIds must be globaly unique.
 */
static data_for_each_cmd_t _differentiate_path_operationId(const char *key,
							   data_t *data,
							   void *arg)
{
	data_t *merge[4] = {0}, *merged = NULL;
	id_merge_path_t *args = arg;
	data_t *op = NULL;

	if (data_get_type(data) != DATA_TYPE_DICT)
		return DATA_FOR_EACH_CONT;

	if (args->merge_args->flags & OAS_FLAG_MANGLE_OPID) {
		if (!(op = data_key_get(data, "operationId"))) {
			debug2("%s: [%s %s] unexpected missing operationId",
			       __func__, key, args->path);
			return DATA_FOR_EACH_CONT;
		}

		/* force operationId to be a string */
		if (data_convert_type(op, DATA_TYPE_STRING) !=
		    DATA_TYPE_STRING) {
			error("%s: [%s %s] unexpected type for operationId: %s",
			      __func__, key, args->path,
			      data_type_to_string(data_get_type(op)));
			return DATA_FOR_EACH_FAIL;
		}

		merge[0] = args->server_path;
		merge[1] =
			parse_url_path(data_get_string_const(op), false, true);
	} else if (args->merge_args->flags & OAS_FLAG_SET_OPID) {
		op = data_key_set(data, "operationId");

		merge[0] = data_set_string(data_new(), key);
		merge[1] = args->server_path;
		merge[2] = parse_url_path(args->path, false, true);
	}

	merged = data_list_join((const data_t **) merge, true);
	if (data_list_for_each(merged, _merge_operationId_strings, args) < 0) {
		FREE_NULL_DATA(merged);
		return DATA_FOR_EACH_FAIL;
	}

	if (args->merge_args->flags & OAS_FLAG_MANGLE_OPID) {
		xfree(merge[1]);
	} else if (args->merge_args->flags & OAS_FLAG_SET_OPID) {
		xfree(merge[0]);
		xfree(merge[2]);
	}

	debug5("%s: [%s %s] setting OperationId %s -> %s",
	       __func__, key, args->path, (op && (data_get_type(op) ==
					    DATA_TYPE_STRING) ?
					   data_get_string(op) : "\"\""),
	       args->operation);

	data_set_string_own(op, args->operation);
	args->operation = NULL;
	FREE_NULL_DATA(merged);

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _find_first_server(data_t *data, void *arg)
{
	data_t **srv = arg;
	data_t *url;

	if (data_get_type(data) != DATA_TYPE_DICT)
		return DATA_FOR_EACH_FAIL;

	url = data_key_get(data, "url");

	if (data_convert_type(url, DATA_TYPE_STRING) == DATA_TYPE_STRING) {
		*srv = parse_url_path(data_get_string(url), false, false);
		return DATA_FOR_EACH_STOP;
	}

	return DATA_FOR_EACH_FAIL;
}

static data_for_each_cmd_t _merge_path(const char *key, data_t *data, void *arg)
{
	merge_path_t *args = arg;
	data_t *e, *servers;
	data_t *merge[3] = { 0 }, *merged = NULL;
	data_for_each_cmd_t rc = DATA_FOR_EACH_CONT;
	id_merge_path_t id_merge = {
		.merge_args = args,
	};
	bool free_0 = false; /* free merge[0] ? */
	char *path = NULL;

	if (data_get_type(data) != DATA_TYPE_DICT) {
		rc = DATA_FOR_EACH_FAIL;
		goto cleanup;
	}

	/* merge the paths together cleanly */
	if (!(servers = data_key_get(data, "servers"))) {
		merge[0] = id_merge.server_path = args->server_path;
		merge[1] = parse_url_path(key, false, true);
	} else {
		/* servers is specified: only cleanup the path */
		/* only handling 1 server for now */
		xassert(data_get_list_length(servers) == 1);

		(void) data_list_for_each(servers, _find_first_server,
					  &merge[0]);
		id_merge.server_path = merge[0];
		free_0 = true;
		xassert(merge[0]);

		merge[1] = parse_url_path(key, false, true);
	}

	merged = data_list_join((const data_t **)merge, true);

	if (data_list_join_str(&path, merged, "/")) {
		rc = DATA_FOR_EACH_FAIL;
		goto cleanup;
	}

	id_merge.path = path;

	e = data_key_set(args->paths, path);
	if (data_get_type(e) != DATA_TYPE_NULL) {
		/*
		 * path is going to be overwritten which should only happen for
		 * /openapi/ paths which is fully expected.
		 */
		debug("%s: overwriting path %s", __func__, path);
	}

	data_set_dict(e);
	(void) data_copy(e, data);

	if ((args->flags & (OAS_FLAG_SET_OPID | OAS_FLAG_MANGLE_OPID)) &&
	    data_dict_for_each(e, _differentiate_path_operationId, &id_merge) <
		    0) {
		rc = DATA_FOR_EACH_FAIL;
		goto cleanup;
	}

cleanup:
	if (free_0)
		FREE_NULL_DATA(merge[0]);
	FREE_NULL_DATA(merge[1]);
	FREE_NULL_DATA(merged);
	xfree(path);

	return rc;
}

static data_for_each_cmd_t _merge_path_server(data_t *data, void *arg)
{
	merge_path_server_t *args = arg;
	merge_path_t p_args = {
		.paths = args->dst_paths,
		.flags = args->flags,
	};
	data_t *url;

	if (data_get_type(data) != DATA_TYPE_DICT)
		return DATA_FOR_EACH_FAIL;

	if (!(url = data_key_get(data, "url")))
		return DATA_FOR_EACH_FAIL;

	if (data_convert_type(url, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return DATA_FOR_EACH_FAIL;

	p_args.server_path = parse_url_path(data_get_string_const(url),
					    false, false);

	if (args->src_paths &&
	    (data_dict_for_each(args->src_paths, _merge_path, &p_args) < 0))
		fatal("%s: unable to merge paths", __func__);

	return DATA_FOR_EACH_CONT;
}

extern int get_openapi_specification(openapi_t *oas, data_t *resp)
{
	data_t *j = data_set_dict(resp);
	data_t *tags = data_set_list(data_key_set(j, "tags"));
	data_t *paths = data_set_dict(data_key_set(j, "paths"));
	data_t *components = data_set_dict(data_key_set(j, "components"));
	data_t *components_schemas = data_set_dict(
		data_key_set(components, "schemas"));
	char *version_at = NULL;
	char *version = xstrdup_printf("Slurm-%s", SLURM_VERSION_STRING);

	/* copy the generic info from the first spec with defined */
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src = data_key_get(oas->spec[i], "openapi");

		if (!src)
			continue;

		data_copy(data_key_set(j, "openapi"), src);
		break;
	}
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src = data_key_get(oas->spec[i], "info");

		if (!src)
			continue;

		data_copy(data_key_set(j, "info"), src);
		break;
	}
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src = data_key_get(oas->spec[i], "security");

		if (!src)
			continue;

		(void) data_copy(data_key_set(j, "security"), src);
		break;
	}
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src = data_resolve_dict_path(
			oas->spec[i], "/components/securitySchemes");

		if (!src)
			continue;

		data_copy(data_set_dict(
				  data_key_set(components, "securitySchemes")),
			  src);
		break;
	}

	/* Populate OAS version */
	for (int i = 0; oas->spec[i]; i++)
		xstrfmtcatat(version, &version_at, "&%s", oas->plugin_types[i]);
	data_set_string_own(data_define_dict_path(j, "/info/version"), version);

	/* set single server at "/" */
	data_set_string(
		data_key_set(data_set_dict(data_list_append(data_set_list(
				     data_key_set(j, "servers")))),
			     "url"),
		"/");

	/* merge all the unique tags together */
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src_tags = data_key_get(oas->spec[i], "tags");
		if (src_tags &&
		    (data_list_for_each(src_tags, _merge_tag, tags) < 0))
			fatal("%s: unable to merge tags", __func__);
	}

	/* merge all the unique paths together */
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src_srvs = data_key_get(oas->spec[i], "servers");

		if (src_srvs) {
			merge_path_server_t p_args = {
				.dst_paths = paths,
				.src_paths = data_key_get(oas->spec[i],
							  "paths"),
				.flags = oas->spec_flags[i],
			};

			if (data_list_for_each(src_srvs, _merge_path_server,
					       &p_args) < 0)
				fatal("%s: unable to merge server paths",
				      __func__);
		} else {
			/* servers is not populated, default to '/' */
			merge_path_t p_args = {
				.server_path = NULL,
				.paths = paths,
				.flags = oas->spec_flags[i],
			};
			data_t *src_paths = data_key_get(oas->spec[i], "paths");

			if (src_paths &&
			    (data_dict_for_each(src_paths, _merge_path,
						&p_args) < 0))
				fatal("%s: unable to merge paths", __func__);
		}
	}

	/* merge all the unique component schemas together */
	for (int i = 0; oas->spec[i]; i++) {
		data_t *src = data_resolve_dict_path(oas->spec[i],
						     "/components/schemas");

		if (src && (data_dict_for_each(src, _merge_schema,
					       components_schemas) < 0)) {
			fatal("%s: unable to merge components schemas",
			      __func__);
		}
	}

	/*
	 * We currently fatal instead of returning failure since openapi are
	 * compile time static and we should not be failing to serve the specs
	 * out
	 */
	return SLURM_SUCCESS;
}

static bool _on_error(void *arg, data_parser_type_t type, int error_code,
		      const char *source, const char *why, ...)
{
	va_list ap;
	char *str;
	openapi_ctxt_t *ctxt = arg;

	va_start(ap, why);
	str = vxstrfmt(why, ap);
	va_end(ap);

	openapi_resp_error(ctxt, error_code, source, "%s", str);

	return false;
}

static void _on_warn(void *arg, data_parser_type_t type, const char *source,
		     const char *why, ...)
{
	va_list ap;
	char *str;
	openapi_ctxt_t *ctxt = arg;

	va_start(ap, why);
	str = vxstrfmt(why, ap);
	va_end(ap);

	openapi_resp_warn(ctxt, source, "%s", str);

	xfree(str);
}

extern int openapi_resp_error(openapi_ctxt_t *ctxt, int error_code,
			      const char *source, const char *why, ...)
{
	openapi_resp_error_t *e;

	xassert(ctxt->errors);

	if (!ctxt->errors)
		return error_code;

	e = xmalloc(sizeof(*e));

	if (why) {
		va_list ap;
		char *str;

		va_start(ap, why);
		str = vxstrfmt(why, ap);
		va_end(ap);

		error("%s: [%s] parser=%s rc[%d]=%s -> %s",
		      (source ? source : __func__), ctxt->id,
		      data_parser_get_plugin(ctxt->parser), error_code,
		      slurm_strerror(error_code), str);

		e->description = str;
	}

	if (error_code) {
		e->num = error_code;

		if (!ctxt->rc)
			ctxt->rc = error_code;
	}

	if (source)
		e->source = xstrdup(source);

	list_append(ctxt->errors, e);

	return error_code;
}

extern void openapi_resp_warn(openapi_ctxt_t *ctxt, const char *source,
			      const char *why, ...)
{
	openapi_resp_warning_t *w;

	xassert(ctxt->warnings);

	if (!ctxt->warnings)
		return;

	w = xmalloc(sizeof(*w));

	if (why) {
		va_list ap;
		char *str;

		va_start(ap, why);
		str = vxstrfmt(why, ap);
		va_end(ap);

		debug("%s: [%s] parser=%s WARNING: %s",
		      (source ? source : __func__), ctxt->id,
		      data_parser_get_plugin(ctxt->parser), str);

		w->description = str;
	}

	if (source)
		w->source = xstrdup(source);

	list_append(ctxt->warnings, w);
}

extern int wrap_openapi_ctxt_callback(const char *context_id,
				      http_request_method_t method,
				      data_t *parameters, data_t *query,
				      int tag, data_t *resp, void *auth,
				      data_parser_t *parser,
				      openapi_ctxt_handler_t callback,
				      const openapi_resp_meta_t *plugin_meta)
{
	int rc;
	data_t *errors, *warnings, *meta;
	openapi_ctxt_t ctxt = {
		.id = context_id,
		.method = method,
		.parameters = parameters,
		.query = query,
		.resp = resp,
		.tag = tag,
	};
	openapi_resp_meta_t query_meta = *plugin_meta;

	query_meta.plugin.data_parser = (char *) data_parser_get_plugin(parser);
	query_meta.client.source = (char *) context_id;

	ctxt.parent_path = data_set_list(data_new());
	ctxt.errors = list_create(free_openapi_resp_error);
	ctxt.warnings = list_create(free_openapi_resp_warning);
	ctxt.parser = data_parser_g_new(_on_error, _on_error, _on_error, &ctxt,
					_on_warn, _on_warn, _on_warn, &ctxt,
					data_parser_get_plugin(parser), NULL,
					true);

	debug("%s: [%s] %s using %s",
	      __func__, context_id, get_http_method_string(method),
	      data_parser_get_plugin(ctxt.parser));

	if (!(ctxt.db_conn = openapi_get_db_conn(auth))) {
		openapi_resp_error(&ctxt, (rc = ESLURM_DB_CONNECTION), __func__,
				   "openapi_get_db_conn() failed to open slurmdb connection");
	} else {
		rc = data_parser_g_assign(ctxt.parser,
					  DATA_PARSER_ATTR_DBCONN_PTR,
					  ctxt.db_conn);
	}

	if (!rc)
		rc = callback(&ctxt);

	if (data_get_type(ctxt.resp) == DATA_TYPE_NULL)
		data_set_dict(ctxt.resp);

	/* need to populate meta, errors and warnings */

	errors = data_key_set(ctxt.resp, OPENAPI_RESP_STRUCT_ERRORS_FIELD_NAME);
	warnings = data_key_set(ctxt.resp,
				OPENAPI_RESP_STRUCT_WARNINGS_FIELD_NAME);
	meta = data_key_set(ctxt.resp, OPENAPI_RESP_STRUCT_META_FIELD_NAME);

	if (data_get_type(meta) == DATA_TYPE_NULL) {
		/* cast to remove const */
		void *ptr = (void *) &query_meta;
		DATA_DUMP(ctxt.parser, OPENAPI_META_PTR, ptr, meta);
	}

	if ((data_get_type(errors) == DATA_TYPE_NULL) &&
	    ((rc = DATA_DUMP(ctxt.parser, OPENAPI_ERRORS, ctxt.errors,
			     errors)))) {
		/* data_parser doesn't support OPENAPI_ERRORS parser */
		data_t *e =
			data_set_dict(data_list_append(data_set_list(errors)));
		data_set_string(data_key_set(e, "description"),
				"Requested data_parser plugin does not support OpenAPI plugin");
		data_set_int(data_key_set(e, "error_number"),
			     ESLURM_NOT_SUPPORTED);
		data_set_string(data_key_set(e, "error"),
				slurm_strerror(ESLURM_NOT_SUPPORTED));
	}
	if (data_get_type(warnings) == DATA_TYPE_NULL)
		DATA_DUMP(ctxt.parser, OPENAPI_WARNINGS, ctxt.warnings,
			  warnings);

	if (!rc)
		rc = ctxt.rc;

	FREE_NULL_LIST(ctxt.errors);
	FREE_NULL_LIST(ctxt.warnings);
	FREE_NULL_DATA_PARSER(ctxt.parser);
	FREE_NULL_DATA(ctxt.parent_path);

	return rc;
}

extern data_t *openapi_get_param(openapi_ctxt_t *ctxt, bool required,
				 const char *name, const char *caller)
{
	data_t *dbuf = NULL;

	if ((!ctxt->parameters ||
	     !(dbuf = data_key_get(ctxt->parameters, name))) &&
	    required) {
		openapi_resp_error(ctxt, ESLURM_DATA_PATH_NOT_FOUND, caller,
				   "Required parameter \"%s\" not found", name);
	}

	return dbuf;
}

extern char *openapi_get_str_param(openapi_ctxt_t *ctxt, bool required,
				   const char *name, const char *caller)
{
	char *str = NULL;
	data_t *dbuf = openapi_get_param(ctxt, required, name, caller);

	if (!dbuf)
		return NULL;

	if (data_convert_type(dbuf, DATA_TYPE_STRING) != DATA_TYPE_STRING) {
		if (required)
			openapi_resp_error(ctxt, ESLURM_DATA_CONV_FAILED,
					   caller,
					   "Rejecting required parameter \"%s\" provided with format %s which was unable to be converted to string.",
					   name, data_type_to_string(
						data_get_type(dbuf)));
		else
			openapi_resp_warn(ctxt, caller,
					  "Ignoring parameter \"%s\" provided with format %s which was unable to be converted to string.",
					  name, data_type_to_string(
						data_get_type(dbuf)));
	} else if (!(str = data_get_string(dbuf)) || !str[0]) {
		if (required)
			openapi_resp_error(ctxt, ESLURM_DATA_PARSE_NOTHING,
					   caller, "Rejecting empty required parameter \"%s\"",
					   name);
		else
			openapi_resp_warn(ctxt, caller,
					  "Ignoring empty parameter \"%s\"",
					  name);

		str = NULL;
	}

	return str;
}

extern int openapi_get_date_param(openapi_ctxt_t *ctxt, bool required,
				  const char *name, time_t *time_ptr,
				  const char *caller)
{
	int rc;
	time_t t;
	data_t *dbuf = openapi_get_param(ctxt, required, name, caller);

	if (!dbuf)
		return ESLURM_REST_EMPTY_RESULT;

	rc = DATA_PARSE(ctxt->parser, TIMESTAMP, t, dbuf, ctxt->parent_path);

	if (!rc) {
		*time_ptr = t;
	} else if (required) {
		openapi_resp_error(ctxt, ESLURM_DATA_CONV_FAILED,
				   caller, "Rejecting invalid required timestamp parameter \"%s\"",
				   name);
	} else {
		openapi_resp_warn(ctxt, caller,
				  "Ignoring invalid timestamp parameter \"%s\"",
				  name);
	}

	return rc;
}
