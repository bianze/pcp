/*
 * Copyright (c) 2019 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include <assert.h>
#include <ctype.h>
#include "pmapi.h"
#include "pmda.h"
#include "schema.h"
#include "util.h"
#include "load.h"
#ifdef HAVE_REGEX_H
#include <regex.h>
#endif
#include <fnmatch.h>

#define DEFAULT_TIMEOUT 2000
static unsigned int default_timeout;	/* timeout in milliseconds */

#define DEFAULT_BATCHSIZE 256
static unsigned int default_batchsize;	/* for groups of metrics */

/* constant string keys (initialized during setup) */
static sds PARAM_HOSTNAME, PARAM_HOSTSPEC, PARAM_CTXNUM, PARAM_CTXID,
           PARAM_POLLTIME, PARAM_PREFIX, PARAM_MNAME, PARAM_MNAMES,
           PARAM_PMIDS, PARAM_PMID, PARAM_INDOM, PARAM_INSTANCE,
           PARAM_INAME, PARAM_MVALUE, PARAM_TARGET, PARAM_EXPR, PARAM_MATCH;
static sds AUTH_USERNAME, AUTH_PASSWORD;
static sds EMPTYSTRING, LOCALHOST, TIMEOUT, BATCHSIZE;

enum matches { MATCH_EXACT, MATCH_GLOB, MATCH_REGEX };
enum profile { PROFILE_ADD, PROFILE_DEL };

typedef struct webgroups {
    struct dict		*contexts;
    mmv_registry_t	*metrics;
    struct dict		*config;
    uv_loop_t		*events;
} webgroups;

static struct webgroups *
webgroups_lookup(pmWebGroupModule *module)
{
    if (module->privdata == NULL)
	module->privdata = calloc(1, sizeof(struct webgroups));
    return (struct webgroups *)module->privdata;
}

static void
webgroup_destroy_context(struct context *context, struct webgroups *groups)
{
    context->garbage = 1;

    if (pmDebugOptions.http)
	fprintf(stderr, "freeing context %p\n", context);

    uv_timer_stop(&context->timer);
    if (groups)
	dictUnlink(groups->contexts, &context->randomid);
    pmwebapi_free_context(context);
    memset(context, 0, sizeof(*context));
}

static void
webgroup_timeout_context(uv_timer_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct context	*cp = (struct context *)handle->data;
    struct webgroups	*gp = (struct webgroups *)cp->privdata;

    if (pmDebugOptions.http)
	fprintf(stderr, "context %u timed out (%p)\n", cp->randomid, cp);

    webgroup_destroy_context(cp, gp);
}

static int
webgroup_access(struct context *cp, sds hostspec, dict *params,
		int *status, sds *message, void *arg)
{
    __pmHashNode	*node;
    __pmHashCtl		attrs;
    pmHostSpec		*hosts = NULL;
    char		*msg, buf[512];
    int			sts, bytes, numhosts = 0;
    sds			value;

    __pmHashInit(&attrs);
    if ((sts = __pmParseHostAttrsSpec(hostspec,
				&hosts, &numhosts, &attrs, &msg)) < 0) {
	/* may as well report the error here */
	*message = sdsnew(msg);
	*status = sts;
	free(msg);
	return sts;
    }
    if ((node = __pmHashSearch(PCP_ATTR_USERNAME, &attrs)) != NULL)
	cp->username = sdsnew((char *)node->data);
    else
    if ((node = __pmHashSearch(PCP_ATTR_PASSWORD, &attrs)) != NULL)
	cp->password = sdsnew((char *)node->data);

    /* add username from Basic Auth header if none given in hostspec */
    if (params && cp->username == NULL) {
	if ((value = dictFetchValue(params, AUTH_USERNAME)) != NULL)
	    __pmHashAdd(PCP_ATTR_USERNAME, strdup(value), &attrs);
	if ((value = dictFetchValue(params, AUTH_PASSWORD)) != NULL)
	    __pmHashAdd(PCP_ATTR_PASSWORD, strdup(value), &attrs);
    }

    bytes = __pmUnparseHostAttrsSpec(hosts, numhosts, &attrs, buf, sizeof(buf));
    if (bytes > 0 && strcmp(cp->name.sds, buf) != 0) {
	sdsfree(cp->name.sds);
	cp->name.sds = sdsnewlen(buf, bytes);
    }
    __pmFreeHostAttrsSpec(hosts, numhosts, &attrs);
    __pmHashClear(&attrs);

    return sts;
}

static struct context *
webgroup_new_context(pmWebGroupSettings *sp, dict *params,
		int *status, sds *message, void *arg)
{
    struct webgroups	*groups = webgroups_lookup(&sp->module);
    struct context	*cp;
    unsigned int	polltime = DEFAULT_TIMEOUT;
    uv_handle_t		*handle;
    pmWebAccess		access;
    double		seconds;
    char		*endptr;
    sds			hostspec = NULL, timeout;

    if (params) {
	if ((hostspec = dictFetchValue(params, PARAM_HOSTSPEC)) == NULL)
	    hostspec = dictFetchValue(params, PARAM_HOSTNAME);

	if ((timeout = dictFetchValue(params, PARAM_POLLTIME)) != NULL) {
	    seconds = strtod(timeout, &endptr);
	    if (*endptr != '\0') {
		infofmt(*message, "invalid timeout requested in polltime");
		*status = -EINVAL;
		return NULL;
	    }
	    polltime = (unsigned int)(seconds * 1000.0);
	}
    }

    if ((cp = (context_t *)calloc(1, sizeof(context_t))) == NULL) {
	infofmt(*message, "out-of-memory on new web context");
	*status = -ENOMEM;
	return NULL;
    }
    cp->type = PM_CONTEXT_HOST;
    cp->context = -1;
    cp->timeout = polltime;

    handle = (uv_handle_t *)&cp->timer;
    handle->data = (void *)cp;
    uv_timer_init(groups->events, &cp->timer);

    if ((cp->randomid = random()) < 0 ||
	dictFind(groups->contexts, &cp->randomid) != NULL) {
	infofmt(*message, "random number failure on new web context");
	pmwebapi_free_context(cp);
	*status = -ESRCH;
	return NULL;
    }
    cp->origin = sdscatfmt(sdsempty(), "%i", cp->randomid);
    cp->name.sds = sdsdup(hostspec ? hostspec : LOCALHOST);
    cp->realm = sdscatfmt(sdsempty(), "pmapi/%i", cp->randomid);
    if (cp->name.sds == NULL || cp->origin == NULL || cp->realm == NULL) {
	infofmt(*message, "out-of-memory on new web context");
	pmwebapi_free_context(cp);
	*status = -ENOMEM;
	return NULL;
    }

    if (webgroup_access(cp, cp->name.sds, params, status, message, arg) < 0) {
	pmwebapi_free_context(cp);
	return NULL;
    }
    access.password = cp->password;
    access.username = cp->username;
    access.realm = cp->realm;
    if (sp->callbacks.on_check &&
        sp->callbacks.on_check(cp->origin, &access, status, message, arg)) {
  	pmwebapi_free_context(cp);
  	return NULL;
    }

    if ((*message = pmwebapi_new_context(cp)) != NULL) {
	*status = -ENOTCONN;
	pmwebapi_free_context(cp);
	return NULL;
    }
    dictAdd(groups->contexts, &cp->randomid, cp);
    cp->privdata = groups;
    cp->setup = 1;
    return cp;
}

static struct context *
webgroup_lookup_context(pmWebGroupSettings *sp, sds *id, dict *params,
		int *status, sds *message, void *arg)
{
    struct webgroups	*groups = webgroups_lookup(&sp->module);
    struct context	*cp = NULL;
    unsigned int	key;
    pmWebAccess		access;
    char		errbuf[PM_MAXERRMSGLEN], *endptr = NULL;
    int			sts;

    if (*id == NULL) {
	if (!(cp = webgroup_new_context(sp, params, status, message, arg)))
	    return NULL;
    } else {
	key = (unsigned int)strtoul(*id, &endptr, 10);
	if (*endptr != '\0') {
	    infofmt(*message, "invalid context identifier: %s", *id);
	    *status = -EINVAL;
	    return NULL;
	}
	cp = (struct context *)dictFetchValue(groups->contexts, &key);
	if (cp == NULL) {
	    infofmt(*message, "unknown context identifier: %u", key);
	    *status = -ENOTCONN;
	    return NULL;
	}
	if (cp->garbage) {
	    infofmt(*message, "expired context identifier: %u", key);
	    *status = -ENOTCONN;
	    return NULL;
	}
	access.username = cp->username;
	access.password = cp->password;
	access.realm = cp->realm;
	if (sp->callbacks.on_check &&
	    sp->callbacks.on_check(*id, &access, status, message, arg) < 0)
	    return NULL;
    }

    if (pmDebugOptions.http)
	fprintf(stderr, "context %u timer set (%p) to %u msec\n",
			cp->randomid, cp, cp->timeout);
    uv_timer_start(&cp->timer, webgroup_timeout_context, cp->timeout, 0);

    if (cp->setup == 0) {
	if ((sts = pmReconnectContext(cp->context)) < 0) {
	    infofmt(*message, "cannot reconnect context: %s",
			pmErrStr_r(sts, errbuf, sizeof(errbuf)));
	    *status = sts;
	    return NULL;
	}
	cp->setup = 1;
    }

    if ((sts = pmUseContext(cp->context)) < 0) {
	infofmt(*message, "cannot use existing context: %s",
			pmErrStr_r(sts, errbuf, sizeof(errbuf)));
	*status = sts;
	return NULL;
    }

    return cp;
}

int
pmWebGroupContext(pmWebGroupSettings *sp, sds id, dict *params, void *arg)
{
    struct context	*cp;
    pmWebSource		context;
    sds			msg = NULL;
    int			sts = 0;

    if ((cp = webgroup_lookup_context(sp, &id, params, &sts, &msg, arg))) {
	id = cp->origin;
	pmwebapi_context_hash(cp);
	context.source = pmwebapi_hash_sds(NULL, cp->name.hash);
	context.hostspec = cp->host;
	context.labels = cp->labels;
	sp->callbacks.on_context(id, &context, arg);
	sdsfree(context.source);
    } else {
	id = NULL;
    }

    sp->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
    return sts;
}

void
pmWebGroupDestroy(pmWebGroupSettings *settings, sds id, void *arg)
{
    struct context	*cp;
    struct webgroups	*gp;
    int			sts = 0;
    sds			msg = NULL;

    if (id && /* do not create a new context in this case (i.e. no NULL IDs) */
	(cp = webgroup_lookup_context(settings, &id, NULL,
				      &sts, &msg, arg)) != NULL) {
	gp = settings->module.privdata;
	webgroup_destroy_context(cp, gp);
    }
    sdsfree(msg);
}

static int
ismarker(int c)
{
    return (c == '\r' || c == '\n' || c == '&' || c == ';');
}

static int
webgroup_derived_metrics(sds config, sds *errmsg)
{
    unsigned int	line = 0;
    char		*p, *end, *name, *expr, *error;

    /*
     * Pick apart the derived metrics "configuration file" in expr.
     * - skip comments and blank lines
     * - split each line on name '=' expression
     * - call pmRegisterDerivedMetric for each, propogating any errors
     *   (with an additional 'line number' and explanatory notes).
     */
    end = config + sdslen(config);
    for (p = config; p < end; p++) {
	if (isspace(*p)) {
	    if (*p == '\n')
		line++;
	    continue;
	}
	if (*p == '#') {
	    while (!ismarker(*p) && p < end)
	        p++;
	    if (p == end)
		break;
	    line++;
	    continue;
	}

	/* find start and end points of the next metric name */
	name = p;
	while (!isspace(*p) && *p != '=' && p < end)
	    p++;
	if (p == end)
	    break;
	*p++ = '\0';
	while ((isspace(*p) && !ismarker(*p)) || *p == '=')
	    p++;

	/* metric name is prepared - move onto the expression */
	expr = p;
	while (!ismarker(*p) && p < end)
	    p++;
	if (p == end)
	    break;
	*p = '\0';

	/* register the derived metric and reset the parsing */
	if (pmRegisterDerivedMetric(name, expr, &error) < 0) {
	    infofmt(*errmsg, "failed to create derived metric \"%s\""
			" on line %u from: %s\n%s", name, line, expr, error);
	    free(error);
	    return -EINVAL;
	}
	name = expr = NULL;
	line++;
    }

    if (name) {
	/* parsing error - incomplete specification */
	infofmt(*errmsg, "failed to parse derived metric \"%s\""
			" on line %u - incomplete expression\n", name, line);
	return -EINVAL;
    }

    return 0;
}

/*
 * Register a derived metric expression for use with webgroup contexts
 */
extern void
pmWebGroupDerive(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    sds			msg = NULL, expr, metric, message;
    int			sts = 0;

    if (params) {
	metric = dictFetchValue(params, PARAM_MNAME);
	expr = dictFetchValue(params, PARAM_EXPR);
    } else {
	metric = expr = NULL;
    }

    if (expr && !metric) {	/* configuration file mode */
	sts = webgroup_derived_metrics(expr, &msg);
    } else if (expr && metric) {
	if ((sts = pmRegisterDerivedMetric(metric, expr, &message)) < 0) {
	    infofmt(msg, "%s", message);
	    free(message);
	}
    } else {
	infofmt(msg, "invalid derive parameters");
	sts = -EINVAL;
    }

    settings->callbacks.on_done(NULL, sts, msg, arg);
    sdsfree(msg);
}

static int
webgroup_fetch_arrays(pmWebGroupSettings *settings, int numnames,
		struct metric **mplist, pmID **pmidlist, void *arg)
{
    if ((*mplist = calloc(numnames, sizeof(struct metric *))) == NULL)
	return -ENOMEM;
    if ((*pmidlist = calloc(numnames, sizeof(pmID))) == NULL) {
	free(*mplist);
	return -ENOMEM;
    }
    return 0;
}

static sds
webgroup_encode_value(sds value, int type, pmAtomValue *atom)
{
    sdsclear(value);
    switch (type) {
    case PM_TYPE_32:
	return sdscatfmt(value, "%i", atom->l);
    case PM_TYPE_U32:
	return sdscatfmt(value, "%u", atom->ul);
    case PM_TYPE_64:
	return sdscatfmt(value, "%I", atom->ll);
    case PM_TYPE_U64:
	return sdscatfmt(value, "%U", atom->ull);
    case PM_TYPE_STRING:
	return sdscatrepr(value, atom->cp, strlen(atom->cp));
    case PM_TYPE_AGGREGATE:
	return sdscatrepr(value, atom->vbp->vbuf, atom->vbp->vlen);
    case PM_TYPE_FLOAT:
	return sdscatprintf(value, "%.8g", (double)atom->f);
    case PM_TYPE_DOUBLE:
	return sdscatprintf(value, "%.16g", atom->d);

    case PM_TYPE_EVENT:
    case PM_TYPE_HIGHRES_EVENT:
    default:
	break;
    }
    return value;
}

static void
webgroup_fetch(pmWebGroupSettings *settings, context_t *cp,
		int numpmid, struct metric **mplist, pmID *pmidlist,
		void *arg)
{
    struct instance	*instance;
    struct metric	*metric;
    struct indom	*indom;
    struct value	*value;
    pmWebResult		webresult;
    pmWebValueSet	webvalueset;
    pmWebValue		webvalue;
    pmResult		*result;
    char		err[PM_MAXERRMSGLEN];
    sds			v = sdsempty(), series = NULL;
    sds			id = cp->origin, message = NULL;
    int			i, j, k, sts, type, status = 0;

    if ((sts = pmFetch(numpmid, pmidlist, &result)) >= 0) {
	webresult.seconds = result->timestamp.tv_sec;
	webresult.nanoseconds = result->timestamp.tv_usec * 1000;

	settings->callbacks.on_fetch(id, &webresult, arg);

	/* extract all values from the result */
	for (i = 0; i < numpmid; i++)
	    if ((metric = mplist[i]) != NULL)
		pmwebapi_add_valueset(mplist[i], result->vset[i]);

	/* for each metric, send fresh values */
	for (i = 0; i < numpmid; i++) {
	    if ((metric = mplist[i]) == NULL)
		continue;
	    type = metric->desc.type;

	    for (j = 0; j < metric->numnames; j++) {
		series = pmwebapi_hash_sds(series, metric->names[j].hash);
		webvalueset.series = series;
		webvalueset.pmid = metric->desc.pmid;
		webvalueset.name = metric->names[j].sds;
		webvalueset.labels = metric->labels;

		settings->callbacks.on_fetch_values(id, &webvalueset, arg);

		webvalue.pmid = metric->desc.pmid;
		if (metric->desc.indom == PM_INDOM_NULL) {
		    v = webgroup_encode_value(v, type, &metric->u.atom);
		    webvalue.series = series;
		    webvalue.inst = PM_IN_NULL;
		    webvalue.value = v;

		    settings->callbacks.on_fetch_value(id, &webvalue, arg);

		    continue;
		}

		indom = metric->indom;
		pmwebapi_add_indom_labels(indom);
		if (indom->updated == 0) {
		    pmwebapi_add_indom_instances(indom);
		    pmwebapi_add_instances_labels(indom);
		}

		if (metric->u.vlist == NULL)
		    continue;

		for (k = 0; k < metric->u.vlist->listcount; k++) {
		    value = &metric->u.vlist->value[k];
		    instance = dictFetchValue(indom->insts, &value->inst);
		    if (instance == NULL)
			continue;
		    v = webgroup_encode_value(v, type, &value->atom);
		    series = pmwebapi_hash_sds(series, instance->name.hash);
		    webvalue.series = series;
		    webvalue.inst = value->inst;
		    webvalue.value = v;

		    settings->callbacks.on_fetch_value(id, &webvalue, arg);
		}
	    }
	}
	pmFreeResult(result);
    }
    free(pmidlist);

    sdsfree(v);
    sdsfree(series);

    if (sts < 0) {
	message = sdsnew(pmErrStr_r(sts, err, sizeof(err)));
	status = sts;
    }

    settings->callbacks.on_done(id, status, message, arg);
    sdsfree(message);
}

/*
 * Parse possible PMID forms: dotted notation or unsigned integer.
 */
static pmID
webgroup_parse_pmid(const sds name)
{
    unsigned int	cluster, domain, item;
    int			sts;

    sts = sscanf(name, "%u.%u.%u", &domain, &cluster, &item);
    if (sts == 3)
	return pmID_build(domain, cluster, item);
    if (sts == 1)
	return domain;
    return PM_ID_NULL;
}

static struct metric *
webgroup_lookup_pmid(pmWebGroupSettings *settings, context_t *cp, sds name, void *arg)
{
    struct metric	*mp;
    pmID		pmid;
    sds			msg;

    if ((pmid = webgroup_parse_pmid(name)) == PM_ID_NULL) {
	infofmt(msg, "failed to parse PMID %s", name);
	moduleinfo(&settings->module, PMLOG_WARNING, msg, arg);
	return NULL;
    }
    if ((mp = (struct metric *)dictFetchValue(cp->pmids, &pmid)) != NULL)
	return mp;
    return pmwebapi_new_pmid(cp, pmid, settings->module.on_info, arg);
}

static struct metric *
webgroup_lookup_metric(pmWebGroupSettings *settings, context_t *cp, sds name, void *arg)
{
    struct metric	*mp;
    pmID		pmid;
    sds			msg;
    int			sts;

    if ((mp = dictFetchValue(cp->metrics, name)) != NULL)
	return mp;
    if ((sts = pmLookupName(1, &name, &pmid)) < 0) {
	infofmt(msg, "failed to lookup name %s", name);
	moduleinfo(&settings->module, PMLOG_WARNING, msg, arg);
	return NULL;
    }
    return pmwebapi_new_pmid(cp, pmid, settings->module.on_info, arg);
}

static void
webgroup_fetch_names(pmWebGroupSettings *settings, context_t *cp,
	int numnames, sds *names, struct metric **mplist, pmID *pmidlist, void *arg)
{
    struct metric	*metric;
    int			i;

    for (i = 0; i < numnames; i++) {
	metric = mplist[i] = webgroup_lookup_metric(settings, cp, names[i], arg);
	pmidlist[i] = metric ? metric->desc.pmid : PM_ID_NULL;
    }
    webgroup_fetch(settings, cp, numnames, mplist, pmidlist, arg);
}

static void
webgroup_fetch_pmids(pmWebGroupSettings *settings, context_t *cp,
	int numpmids, sds *names, struct metric **mplist, pmID *pmidlist, void *arg)
{
    struct metric	*metric;
    int			i;

    for (i = 0; i < numpmids; i++) {
	metric = mplist[i] = webgroup_lookup_pmid(settings, cp, names[i], arg);
	pmidlist[i] = metric ? metric->desc.pmid : PM_ID_NULL;
    }

    webgroup_fetch(settings, cp, numpmids, mplist, pmidlist, arg);
}

void
pmWebGroupFetch(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct context	*cp;
    struct metric	*mplist;
    size_t		length;
    pmID		*pmidlist;
    sds			msg = NULL, metrics, pmids = NULL, *names = NULL;
    int			sts = 0, numnames = 0;

    if (params) {
	if ((metrics = dictFetchValue(params, PARAM_MNAMES)) == NULL &&
	    (metrics = dictFetchValue(params, PARAM_MNAME)) == NULL &&
	    (pmids = dictFetchValue(params, PARAM_PMIDS)) == NULL)
	    pmids = dictFetchValue(params, PARAM_PMID);
    } else {
	metrics = NULL;
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    /* handle fetch via metric name list */
    if (metrics) {
	length = sdslen(metrics);
	if ((names = sdssplitlen(metrics, length, ",", 1, &numnames)) == NULL ||
	    webgroup_fetch_arrays(settings,
				numnames, &mplist, &pmidlist, arg) < 0) {
	    infofmt(msg, "out-of-memory on allocation");
	    sts = -ENOMEM;
	} else {
	    webgroup_fetch_names(settings, cp,
				numnames, names, &mplist, pmidlist, arg);
	}
    }
    /* handle fetch via numeric/dotted-form PMIDs */
    else if (pmids) {
	length = sdslen(pmids);
	if ((names = sdssplitlen(pmids, length, ",", 1, &numnames)) == NULL ||
	    webgroup_fetch_arrays(settings,
				numnames, &mplist, &pmidlist, arg) < 0) {
	    infofmt(msg, "out-of-memory on allocation");
	    sts = -ENOMEM;
	} else {
	    webgroup_fetch_pmids(settings, cp,
				numnames, names, &mplist, pmidlist, arg);
	}
    }
    else {
	infofmt(msg, "bad parameters passed");
	sts = -EINVAL;
    }
    sdsfreesplitres(names, numnames);

done:
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

/*
 * Parse possible InDom forms: dotted notation or unsigned integer.
 */
static pmInDom
webgroup_parse_indom(const sds name)
{
    unsigned int	domain, serial;
    int			sts;

    sts = sscanf(name, "%u.%u", &domain, &serial);
    if (sts == 2)
	return pmInDom_build(domain, serial);
    if (sts == 1)
	return domain;
    return PM_INDOM_NULL;
}

static struct indom *
webgroup_cache_indom(struct context *cp, pmInDom indom)
{
    struct domain	*dp;
    struct indom	*ip;

    if ((ip = (struct indom *)dictFetchValue(cp->indoms, &indom)) != NULL)
	return ip;
    dp = pmwebapi_add_domain(cp, pmInDom_domain(indom));
    return pmwebapi_new_indom(cp, dp, indom);
}

static struct indom *
webgroup_lookup_indom(pmWebGroupSettings *settings, context_t *cp, sds name, void *arg)
{
    pmInDom		indom;
    sds			msg;

    if ((indom = webgroup_parse_indom(name)) == PM_INDOM_NULL) {
	infofmt(msg, "failed to parse InDom %s", name);
	moduleinfo(&settings->module, PMLOG_WARNING, msg, arg);
	return NULL;
    }
    return webgroup_cache_indom(cp, indom);
}

static int
webgroup_profile(struct indom *ip, enum profile profile, enum matches match,
		sds instnames, sds instids)
{
    struct instance	*instance;
    dictIterator	*iterator;
    dictEntry		*entry;
    pmInDom		indom;
    regex_t		*regex = NULL;
    size_t		length;
    sds			*ids = NULL, *names = NULL;
    int			*insts = NULL;
    int			i, r = 0, sts = 0, count = 0, numids = 0, numnames = 0;

    if (ip == NULL) {
	indom = PM_INDOM_NULL;
	goto profile;
    }
    indom = ip->indom;
    if (instnames) {
	pmwebapi_add_indom_instances(ip);
	length = sdslen(instnames);
	names = sdssplitlen(instnames, length, ",", 1, &numnames);
	insts = calloc(numnames, sizeof(int));
    } else if (instids) {
	pmwebapi_add_indom_instances(ip);
	length = sdslen(instids);
	ids = sdssplitlen(instids, length, ",", 1, &numids);
	insts = calloc(numids, sizeof(int));
    }

    if (match == MATCH_REGEX && numnames > 0) {
	if ((regex = calloc(numnames, sizeof(*regex))) == NULL)
	    return -ENOMEM;
	for (r = 0; r < numnames; r++) {
	    if (regcomp(&regex[r], names[r], REG_EXTENDED|REG_NOSUB) < 0) {
		sts = -EINVAL;
		goto done;
	    }
	}
    }

    iterator = dictGetIterator(ip->insts);
    while ((entry = dictNext(iterator)) != NULL) {
	instance = (instance_t *)dictGetVal(entry);
	if (instance->updated == 0)
	    continue;

	for (i = 0; i < numnames; i++) {
	    if (match == MATCH_EXACT &&
		strcmp(names[i], instance->name.sds) == 0)
		goto inst;
	    if (match == MATCH_REGEX &&
		regexec(&regex[i], instance->name.sds, 0, NULL, 0) == 0)
		goto inst;
	    if (match == MATCH_GLOB &&
		fnmatch(names[i], instance->name.sds, 0) == 0)
		goto inst;
	}
	if (i != numnames)	/* skip this instance based on name */
	    continue;
	for (i = 0; i < numids; i++) {
	    if (instance->inst == atoi(ids[i]))
		goto inst;
	}
	if (i == numids)	/* skip this instance based on ID */
	    continue;
inst:
	/* add instance identifier to list */
	insts[count] = instance->inst;
	count++;
    }
    dictReleaseIterator(iterator);

done:
    if (regex) {
	while (r--)
	    regfree(&regex[r]);
	free(regex);
    }

    sdsfreesplitres(names, numnames);
    sdsfreesplitres(ids, numids);

profile:
    sts = (profile == PROFILE_ADD) ?
	    pmAddProfile(indom, count, insts) :
	    pmDelProfile(indom, count, insts);

    if (count)
	free(insts);
    return sts;
}

/*
 * Manipulate instance domain profile(s) for this web group context
 */
extern void
pmWebGroupProfile(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct context	*cp;
    struct metric	*mp;
    struct indom	*ip;
    enum profile	profile = PROFILE_DEL;
    enum matches	matches = MATCH_EXACT;
    char		err[PM_MAXERRMSGLEN];
    sds			expr, match, metric, indomid, inames, instids;
    sds			msg = NULL;
    int			sts = 0;

    if (params) {
	metric = dictFetchValue(params, PARAM_MNAME);
	indomid = dictFetchValue(params, PARAM_INDOM);
	inames = dictFetchValue(params, PARAM_INAME);
	instids = dictFetchValue(params, PARAM_INSTANCE);
	if ((expr = dictFetchValue(params, PARAM_EXPR)) != NULL) {
	    if (strcmp(expr, "add") == 0)
		profile = PROFILE_ADD;
	    else if (strcmp(expr, "del") != 0) {
		infofmt(msg, "%s - invalid 'expr' parameter value", expr);
		sts = -EINVAL;
		goto done;
	    }
	}
	if ((match = dictFetchValue(params, PARAM_MATCH)) != NULL) {
	    if (strcmp(match, "regex") == 0)
		matches = MATCH_REGEX;
	    else if (strcmp(match, "glob") == 0)
		matches = MATCH_GLOB;
	    else if (strcmp(match, "exact") != 0) {
		infofmt(msg, "%s - invalid 'match' parameter value", match);
		sts = -EINVAL;
		goto done;
	    }
	}
    } else {
	expr = indomid = inames = instids = metric = NULL;
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    if (expr == NULL) {
	infofmt(msg, "invalid profile parameters");
	sts = -EINVAL;
	goto done;
    }
    if (metric) {
	if ((mp = webgroup_lookup_metric(settings, cp, metric, arg)) == NULL) {
	    infofmt(msg, "%s - failed to lookup metric", metric);
	    sts = -EINVAL;
	    goto done;
	}
	if (mp->desc.indom == PM_INDOM_NULL) {
	    infofmt(msg, "%s - metric has null indom", metric);
	    sts = -EINVAL;
	    goto done;
	}
	ip = webgroup_cache_indom(cp, mp->desc.indom);
    }
    else if (indomid == NULL)
	ip = NULL;
    else if ((ip = webgroup_lookup_indom(settings, cp, indomid, arg)) == NULL) {
	infofmt(msg, "invalid profile parameters");
	sts = -EINVAL;
	goto done;
    }

    if ((sts = webgroup_profile(ip, profile, matches, inames, instids)) < 0)
	infofmt(msg, "%s - %s", expr, pmErrStr_r(sts, err, sizeof(err)));

done:
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

void
pmWebGroupChildren(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct context	*cp;
    pmWebChildren	children = {0};
    char		errmsg[PM_MAXERRMSGLEN];
    char		**offspring;
    sds			msg = NULL, prefix = NULL;
    int			i, l, n, sts = 0, *status;

    if (params) {
	if ((prefix = dictFetchValue(params, PARAM_PREFIX)) == NULL)
	     prefix = dictFetchValue(params, PARAM_MNAME);
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    if (prefix == NULL || *prefix == '\0')
	prefix = EMPTYSTRING;

    if ((sts = pmGetChildrenStatus(prefix, &offspring, &status)) < 0) {
	infofmt(msg, "child traversal failed - %s",
		pmErrStr_r(sts, errmsg, sizeof(errmsg)));
    } else {
	children.name = prefix;

	/* zero or more children must be reported from this point onward */
	for (i = 0; i < sts; i++) {
	    if (status[i] == PMNS_NONLEAF_STATUS)
	        children.numnonleaf++;
	    else /* PMNS_LEAF_STATUS */
	        children.numleaf++;
	}
	if (children.numleaf)
	    children.leaf = calloc(children.numleaf, sizeof(sds));
	if (children.numnonleaf)
	    children.nonleaf = calloc(children.numnonleaf, sizeof(sds));
	for (i = l = n = 0; i < sts; i++) {
	    if (status[i] == PMNS_NONLEAF_STATUS)
	        children.nonleaf[n++] = sdsnew(offspring[i]);
	    else /* PMNS_LEAF_STATUS */
	        children.leaf[l++] = sdsnew(offspring[i]);
	}
	if (sts > 0) {
	    free(offspring);
	    sts = 0;
	}

	settings->callbacks.on_children(id, &children, arg);

	/* free up locally allocated array space */
	for (i = 0; i < children.numnonleaf; i++)
	    sdsfree(children.nonleaf[i]);
	if (children.nonleaf)
	    free(children.nonleaf);
	for (i = 0; i < children.numleaf; i++)
	    sdsfree(children.leaf[i]);
	if (children.leaf)
	    free(children.leaf);
    }

done:
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

static int
webgroup_instances(pmWebGroupSettings *settings,
		struct context *cp, struct indom *ip, enum matches match,
		int numnames, sds *instnames, int numids, sds *instids,
		void *arg)
{
    struct instance	*instance;
    pmWebInstance	webinst;
    dictIterator	*iterator;
    dictEntry		*entry;
    regex_t		*regex = NULL;
    int			i, r = 0, sts = 0;

    if (match == MATCH_REGEX && numnames > 0) {
	if ((regex = calloc(numnames, sizeof(*regex))) == NULL)
	    return -ENOMEM;
	for (r = 0; r < numnames; r++) {
	    if (regcomp(&regex[r], instnames[r], REG_EXTENDED|REG_NOSUB) < 0) {
		sts = -EINVAL;
		goto done;
	    }
	}
    }

    iterator = dictGetIterator(ip->insts);
    while ((entry = dictNext(iterator)) != NULL) {
	instance = (instance_t *)dictGetVal(entry);
	if (instance->updated == 0)
	    continue;

	for (i = 0; i < numnames; i++) {
	    if (match == MATCH_EXACT &&
		strcmp(instnames[i], instance->name.sds) == 0)
		break;
	    if (match == MATCH_REGEX &&
		regexec(&regex[i], instance->name.sds, 0, NULL, 0) == 0)
		break;
	    if (match == MATCH_GLOB &&
		fnmatch(instnames[i], instance->name.sds, 0) == 0)
		break;
	}
	if (i != numnames)	/* skip this instance based on name */
	    continue;
	for (i = 0; i < numids; i++) {
	    if (instance->inst != atoi(instids[i]))
		break;
	}
	if (i != numids)	/* skip this instance based on ID */
	    continue;

	webinst.indom = ip->indom;
	webinst.inst = instance->inst;
	webinst.name = instance->name.sds;
	webinst.labels = instance->labels;

	settings->callbacks.on_instance(cp->origin, &webinst, arg);
    }
    dictReleaseIterator(iterator);

done:
    if (regex) {
	while (r--)
	    regfree(&regex[r]);
	free(regex);
    }

    return sts;
}

void
pmWebGroupInDom(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct context	*cp;
    struct domain	*dp;
    struct metric	*mp;
    struct indom	*ip;
    enum matches	matches = MATCH_EXACT;
    pmWebInDom		webindom;
    pmInDom		indom;
    size_t		length;
    sds			msg = NULL, match, metric, indomid, instids, instnames;
    sds			*ids = NULL, *names = NULL;
    int			sts = 0, count = 0, numids = 0, numnames = 0;

    if (params) {
	metric = dictFetchValue(params, PARAM_MNAME);
	indomid = dictFetchValue(params, PARAM_INDOM);
	instids = dictFetchValue(params, PARAM_INSTANCE);
	instnames = dictFetchValue(params, PARAM_INAME);
	if ((match = dictFetchValue(params, PARAM_MATCH)) != NULL) {
	    if (strcmp(match, "regex") == 0)
		matches = MATCH_REGEX;
	    else if (strcmp(match, "glob") == 0)
		matches = MATCH_GLOB;
	    else if (strcmp(match, "exact") != 0) {
		infofmt(msg, "%s - invalid 'match' parameter value", match);
		sts = -EINVAL;
		goto done;
	    }
	    msg = NULL;
	}
    } else {
	metric = indomid = instids = instnames = NULL;
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    if (metric) {
	if ((mp = webgroup_lookup_metric(settings, cp, metric, arg)) == NULL) {
	    infofmt(msg, "%s - failed to lookup metric", metric);
	    sts = -EINVAL;
	    goto done;
	}
	if (mp->desc.indom == PM_INDOM_NULL) {
	    infofmt(msg, "%s - metric has null indom", metric);
	    sts = -EINVAL;
	    goto done;
	}
	indom = mp->desc.indom;
    } else if (indomid) {
	if ((ip = webgroup_lookup_indom(settings, cp, indomid, arg)) == NULL) {
	    infofmt(msg, "failed to lookup indom %s", indomid);
	    sts = -EINVAL;
	    goto done;
	}
	indom = ip->indom;
    } else {
	infofmt(msg, "bad indom parameters");
	sts = -EINVAL;
	goto done;
    }

    dp = pmwebapi_add_domain(cp, pmInDom_domain(indom));
    if ((ip = pmwebapi_add_indom(cp, dp, indom)) == NULL) {
	infofmt(msg, "failed to add indom");
	sts = -EINVAL;
	goto done;
    }
    pmwebapi_add_indom_labels(ip);
    if (ip->updated == 0) {
	count = pmwebapi_add_indom_instances(ip);
	pmwebapi_add_instances_labels(ip);
    }

    if (instnames) {
	length = sdslen(instnames);
	names = sdssplitlen(instnames, length, ",", 1, &numnames);
    } else if (instids) {
	length = sdslen(instids);
	ids = sdssplitlen(instids, length, ",", 1, &numids);
    }

    webindom.indom = ip->indom;
    webindom.labels = ip->labels;
    webindom.oneline = ip->oneline;
    webindom.helptext = ip->helptext;
    webindom.numinsts = count;
    settings->callbacks.on_indom(id, &webindom, arg);

    if ((sts = webgroup_instances(settings, cp, ip, matches,
				numnames, names, numids, ids, arg)) < 0) {
	infofmt(msg, "failed instances lookup");
    }

    sdsfreesplitres(names, numnames);
    sdsfreesplitres(ids, numids);

done:
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

seriesname_t *
webgroup_lookup_series(int numnames, seriesname_t *names, const char *name)
{
    size_t		length = strlen(name);
    int			i;

    /* find the given name in an array of names */
    for (i = 0; i < numnames; i++) {
	if (length != sdslen(names[i].sds))
	    continue;
	if (strncmp(name, names[i].sds, length) == 0)
	    return &names[i];
    }
    return NULL;
}

typedef struct weblookup {
    pmWebGroupSettings	*settings;
    struct context	*context;
    pmWebMetric		metric;	/* keep buffers to reduce memory allocations */
    int			status;
    void		*arg;
} weblookup_t;

/* Metric namespace traversal callback for use with pmTraversePMNS_r(3) */
static void
webmetric_lookup(const char *name, void *arg)
{
    struct weblookup	*lookup = (struct weblookup *)arg;
    pmWebGroupSettings	*settings = lookup->settings;
    pmWebMetric		*metric = &lookup->metric;
    seriesname_t	*snp = NULL;
    context_t		*cp = lookup->context;
    metric_t		*mp;

    /* make sure we use the original caller supplied arg now */
    arg = lookup->arg;

    /* clear buffer contents from any previous call(s) */
    sdsclear(metric->name);
    sdsclear(metric->sem);
    sdsclear(metric->type);
    sdsclear(metric->units);
    sdsclear(metric->labels);
    sdsclear(metric->oneline);
    sdsclear(metric->helptext);

    metric->name = sdscat(metric->name, name);
    mp = webgroup_lookup_metric(settings, cp, metric->name, arg);
    if (mp == NULL)
	return;
    snp = webgroup_lookup_series(mp->numnames, mp->names, name);
    if (snp == NULL)
	return;

    if (mp->cluster) {
	if (mp->cluster->domain)
	    pmwebapi_add_domain_labels(mp->cluster->domain);
	pmwebapi_add_cluster_labels(mp->cluster);
    }
    if (mp->indom)
	pmwebapi_add_indom_labels(mp->indom);
    pmwebapi_add_item_labels(mp);
    pmwebapi_metric_hash(mp);
    pmwebapi_metric_help(mp);

    metric->pmid = mp->desc.pmid;
    metric->indom = mp->desc.indom;
    pmwebapi_hash_str(snp->hash, metric->series, 42);
    sdssetlen(metric->series, 40);
    pmwebapi_semantics_str(mp, metric->sem, 20);
    pmwebapi_units_str(mp, metric->units, 64);
    pmwebapi_type_str(mp, metric->type, 20);
    sdsupdatelen(metric->units);
    sdsupdatelen(metric->type);
    sdsupdatelen(metric->sem);
    if (mp->labels)
	metric->labels = sdscatsds(metric->labels, mp->labels);
    if (mp->oneline)
	metric->oneline = sdscatsds(metric->oneline, mp->oneline);
    if (mp->helptext)
	metric->helptext = sdscatsds(metric->helptext, mp->helptext);

    settings->callbacks.on_metric(cp->origin, metric, arg);
}

void
pmWebGroupMetric(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct weblookup	lookup = {0};
    struct context	*cp;
    pmWebMetric		*metric = &lookup.metric;
    size_t		length;
    char		errmsg[PM_MAXERRMSGLEN];
    sds			msg = NULL, prefix = NULL, *names = NULL;
    int			i, sts = 0, numnames = 0;

    if (params) {
	if ((prefix = dictFetchValue(params, PARAM_PREFIX)) == NULL &&
	    (prefix = dictFetchValue(params, PARAM_MNAMES)) == NULL)
	     prefix = dictFetchValue(params, PARAM_MNAME);
	if (prefix) {
	    length = sdslen(prefix);
	    names = sdssplitlen(prefix, length, ",", 1, &numnames);
	}
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    lookup.settings = settings;
    lookup.context = cp;
    lookup.arg = arg;

    /* allocate uninit'd space for strings */
    metric->series = sdsnewlen(SDS_NOINIT, 42); sdsclear(metric->series);
    metric->name = sdsnewlen(SDS_NOINIT, 16); sdsclear(metric->name);
    metric->sem = sdsnewlen(SDS_NOINIT, 20); sdsclear(metric->sem);
    metric->type = sdsnewlen(SDS_NOINIT, 20); sdsclear(metric->type);
    metric->units = sdsnewlen(SDS_NOINIT, 64); sdsclear(metric->units);
    metric->labels = sdsnewlen(SDS_NOINIT, 128); sdsclear(metric->labels);
    metric->oneline = sdsnewlen(SDS_NOINIT, 128); sdsclear(metric->oneline);
    metric->helptext = sdsnewlen(SDS_NOINIT, 128); sdsclear(metric->helptext);

    if (prefix == NULL || *prefix == '\0') {
	sts = pmTraversePMNS_r("", webmetric_lookup, &lookup);
	if (sts >= 0)
	    sts = (lookup.status < 0) ? lookup.status : 0;
	else
	    infofmt(msg, "namespace traversal failed - %s",
			    pmErrStr_r(sts, errmsg, sizeof(errmsg)));
    }
    for (i = 0; i < numnames; i++) {
	sts = pmTraversePMNS_r(names[i], webmetric_lookup, &lookup);
	if (sts >= 0)
	    sts = (lookup.status < 0) ? lookup.status : 0;
	else
	    infofmt(msg, "%s traversal failed - %s", names[i],
			    pmErrStr_r(sts, errmsg, sizeof(errmsg)));
    }

    sdsfree(metric->name);
    sdsfree(metric->sem);
    sdsfree(metric->type);
    sdsfree(metric->units);
    sdsfree(metric->labels);
    sdsfree(metric->oneline);
    sdsfree(metric->helptext);

done:
    sdsfreesplitres(names, numnames);
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

static void
scrape_metric_labelsets(metric_t *metric, pmWebLabelSet *labels)
{
    cluster_t	*cluster = metric->cluster;
    domain_t	*domain = cluster->domain;
    context_t	*context = domain->context;
    indom_t	*indom = metric->indom;
    int		nsets = 0;

    if (context->labelset)
	labels->sets[nsets++] = context->labelset;
    if (domain->labelset)
	labels->sets[nsets++] = domain->labelset;
    if (indom && indom->labelset)
	labels->sets[nsets++] = indom->labelset;
    if (cluster->labelset)
	labels->sets[nsets++] = cluster->labelset;
    if (metric->labelset)
	labels->sets[nsets++] = metric->labelset;
    labels->nsets = nsets;
    sdsclear(labels->buffer);
}

static void
scrape_instance_labelsets(metric_t *metric, indom_t *indom, instance_t *inst,
		pmWebLabelSet *labels)
{
    domain_t	*domain = indom->domain;
    context_t	*context = domain->context;
    cluster_t	*cluster = metric->cluster;
    int		nsets = 0;

    if (context->labelset)
	labels->sets[nsets++] = context->labelset;
    if (domain->labelset)
	labels->sets[nsets++] = domain->labelset;
    if (indom->labelset)
	labels->sets[nsets++] = indom->labelset;
    if (cluster->labelset)
	labels->sets[nsets++] = cluster->labelset;
    if (metric->labelset)
	labels->sets[nsets++] = metric->labelset;
    if (inst->labelset)
	labels->sets[nsets++] = inst->labelset;
    labels->nsets = nsets;
    sdsclear(labels->buffer);
}

static int
webgroup_scrape(pmWebGroupSettings *settings, context_t *cp,
		int numpmid, struct metric **mplist, pmID *pmidlist,
		void *arg)
{
    struct instance	*instance;
    struct metric	*metric;
    struct indom	*indom;
    struct value	*value;
    pmWebLabelSet	labels;
    pmWebScrape		scrape;
    pmResult		*result;
    sds			sems, types, units;
    sds			v = sdsempty(), series = NULL;
    int			i, j, k, sts, type;

    /* pre-allocate buffers for metric metadata */
    sems = sdsnewlen(SDS_NOINIT, 20); sdsclear(sems);
    types = sdsnewlen(SDS_NOINIT, 20); sdsclear(types);
    units = sdsnewlen(SDS_NOINIT, 64); sdsclear(units);
    labels.buffer = sdsnewlen(SDS_NOINIT, PM_MAXLABELJSONLEN);
    sdsclear(labels.buffer);

    if ((sts = pmFetch(numpmid, pmidlist, &result)) >= 0) {
	scrape.seconds = result->timestamp.tv_sec;
	scrape.nanoseconds = result->timestamp.tv_usec * 1000;

	/* extract all values from the result for later stages */
	for (i = 0; i < numpmid; i++)
	    if ((metric = mplist[i]) != NULL)
		pmwebapi_add_valueset(metric, result->vset[i]);

	/* for each metric, send all metadata and fresh values */
	for (i = 0; i < numpmid; i++) {
	    if ((metric = mplist[i]) == NULL)
		continue;

	    if (metric->updated == 0)
		continue;
	    if (metric->labelset == NULL)
		pmwebapi_add_item_labels(metric);
	    pmwebapi_metric_help(metric);

	    type = metric->desc.type;
	    indom = metric->indom;
	    if (indom && indom->updated == 0 &&
		pmwebapi_add_indom_instances(indom) > 0)
		pmwebapi_add_instances_labels(indom);

	    for (j = 0; j < metric->numnames; j++) {
		series = pmwebapi_hash_sds(series, metric->names[j].hash);
		scrape.metric.series = series;
		scrape.metric.name = metric->names[j].sds;
		scrape.metric.pmid = metric->desc.pmid;
		scrape.metric.indom = metric->desc.indom;

		pmwebapi_semantics_str(metric, sems, 20);
		scrape.metric.sem = sems;
		sdsupdatelen(sems);
		pmwebapi_type_str(metric, types, 20);
		scrape.metric.type = types;
		sdsupdatelen(types);
		pmwebapi_units_str(metric, units, 64);
		scrape.metric.units = units;
		sdsupdatelen(units);
		scrape.metric.labels = NULL;
		scrape.metric.oneline = metric->oneline;
		scrape.metric.helptext = metric->helptext;

		if (metric->desc.indom == PM_INDOM_NULL || metric->u.vlist == NULL) {
		    v = webgroup_encode_value(v, type, &metric->u.atom);
		    scrape.value.series = series;
		    scrape.value.inst = PM_IN_NULL;
		    scrape.value.value = v;
		    memset(&scrape.instance, 0, sizeof(scrape.instance));
		    scrape.instance.inst = PM_IN_NULL;

		    if (metric->labels == NULL)
			pmwebapi_metric_hash(metric);
		    scrape_metric_labelsets(metric, &labels);
		    settings->callbacks.on_scrape_labels(cp->origin, &labels, arg);
		    scrape.metric.labels = labels.buffer;

		    settings->callbacks.on_scrape(cp->origin, &scrape, arg);
		    continue;
		}
		for (k = 0; k < metric->u.vlist->listcount; k++) {
		    value = &metric->u.vlist->value[k];
		    instance = dictFetchValue(indom->insts, &value->inst);
		    if (instance == NULL)
			continue;
		    v = webgroup_encode_value(v, type, &value->atom);
		    series = pmwebapi_hash_sds(series, instance->name.hash);
		    scrape.value.series = series;
		    scrape.value.inst = value->inst;
		    scrape.value.value = v;
		    scrape.instance.inst = instance->inst;
		    scrape.instance.name = instance->name.sds;

		    if (instance->labels == NULL)
			pmwebapi_instance_hash(indom, instance);
		    scrape_instance_labelsets(metric, indom, instance, &labels);
		    settings->callbacks.on_scrape_labels(cp->origin, &labels, arg);
		    scrape.instance.labels = labels.buffer;

		    settings->callbacks.on_scrape(cp->origin, &scrape, arg);
		}
	    }
	}
	pmFreeResult(result);
    }

    sdsfree(v);
    sdsfree(sems);
    sdsfree(types);
    sdsfree(units);
    sdsfree(series);
    sdsfree(labels.buffer);

    return sts < 0 ? sts : 0;
}

static int
webgroup_scrape_names(pmWebGroupSettings *settings, context_t *cp,
	int numnames, sds *names, struct metric **mplist, pmID *pmidlist, void *arg)
{
    struct metric	*metric;
    int			i;

    for (i = 0; i < numnames; i++) {
	metric = mplist[i] = webgroup_lookup_metric(settings, cp, names[i], arg);
	pmidlist[i] = metric ? metric->desc.pmid : PM_ID_NULL;
    }
    return webgroup_scrape(settings, cp, numnames, mplist, pmidlist, arg);
}

typedef struct webscrape {
    pmWebGroupSettings	*settings;
    struct context	*context;
    unsigned int	numnames;	/* current count of metric names */
    sds			*names;		/* metric names for batched up scrape */
    struct metric	**mplist;
    pmID		*pmidlist;
    int			status;
    void		*arg;
} webscrape_t;

/* Metric namespace traversal callback for use with pmTraversePMNS_r(3) */
static void
webgroup_scrape_batch(const char *name, void *arg)
{
    struct webscrape	*scrape = (struct webscrape *)arg;
    int			i, sts;

    if (scrape->names == NULL) {
	scrape->names = calloc(DEFAULT_BATCHSIZE, sizeof(sds));
	scrape->mplist = calloc(DEFAULT_BATCHSIZE, sizeof(metric_t *));
	scrape->pmidlist = calloc(DEFAULT_BATCHSIZE, sizeof(pmID));
	scrape->numnames = 0;
    }

    scrape->names[scrape->numnames] = sdsnew(name);
    scrape->numnames++;

    if (scrape->numnames == DEFAULT_BATCHSIZE) {
	sts = webgroup_scrape_names(scrape->settings, scrape->context,
			scrape->numnames, scrape->names,
			scrape->mplist, scrape->pmidlist, scrape->arg);
	for (i = 0; i < scrape->numnames; i++)
	    sdsfree(scrape->names[i]);
	scrape->numnames = 0;
	if (sts < 0)
	    scrape->status = sts;
    }
}

static int
webgroup_scrape_tree(const char *prefix, struct webscrape *scrape, sds *msg)
{
    int			i, sts;
    char		err[PM_MAXERRMSGLEN];

    sts = pmTraversePMNS_r(prefix, webgroup_scrape_batch, scrape);
    if (sts >= 0 && scrape->status >= 0 && scrape->numnames) {
	/* complete any remaining (sub-batchsize) leftovers */
	sts = webgroup_scrape_names(scrape->settings, scrape->context,
				    scrape->numnames, scrape->names,
				    scrape->mplist, scrape->pmidlist,
				    scrape->arg);
	for (i = 0; i < scrape->numnames; i++)
	    sdsfree(scrape->names[i]);
	scrape->numnames = 0;
    }
    if (sts < 0)
	infofmt(*msg, "'%s' - %s", prefix, pmErrStr_r(sts, err, sizeof(err)));
    else {
	sts = (scrape->status < 0) ? scrape->status : 0;
    }
    return sts;
}

void
pmWebGroupScrape(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct webscrape	scrape = {0};
    struct context	*cp;
    size_t		length;
    int			sts = 0, i, numnames = 0;
    sds			msg = NULL, *names = NULL, metrics;

    if (params) {
	if ((metrics = dictFetchValue(params, PARAM_MNAMES)) == NULL)
	     if ((metrics = dictFetchValue(params, PARAM_MNAME)) == NULL)
		if ((metrics = dictFetchValue(params, PARAM_PREFIX)) == NULL)
		    metrics = dictFetchValue(params, PARAM_TARGET);
    } else {
	metrics = NULL;
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    scrape.settings = settings;
    scrape.context = cp;
    scrape.arg = arg;

    /* handle scrape via metric name list traversal (else entire namespace) */
    if (metrics && sdslen(metrics)) {
	length = sdslen(metrics);
	if ((names = sdssplitlen(metrics, length, ",", 1, &numnames)) == NULL)
	    sts = webgroup_scrape_tree("", &scrape, &msg);
	for (i = 0; i < numnames; i++)
	    sts = webgroup_scrape_tree(names[i], &scrape, &msg);
    } else {
	sts = webgroup_scrape_tree("", &scrape, &msg);
    }

    if (scrape.names) {
	free(scrape.names);
	free(scrape.mplist);
	free(scrape.pmidlist);
    }

done:
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

struct instore {
    pmAtomValue		*atom;
    pmValueSet		*vset;
    sds			*names;
    unsigned int	numnames;
    unsigned int	count;
    unsigned int	maximum;
    unsigned int	type;
    int			status;
};

static void
store_found_insts(void *arg, const struct dictEntry *entry)
{
    struct instore	*store = (struct instore *)arg;
    pmValue		*value;
    int			i;

    if (store->count < store->maximum) {
	i = store->count++;	/* current position */
	value = &store->vset->vlist[i];
	store->status = __pmStuffValue(store->atom, value, store->type);
	value->inst = *(int *)entry->key;
    } else {
	store->status = -E2BIG;
    }
}

static void
store_named_insts(void *arg, const struct dictEntry *entry)
{
    struct instore	*store = (struct instore *)arg;
    sds			name = (sds)dictGetVal(entry);
    int			i;

    for (i = 0; i < store->numnames; i++)
	if (sdscmp(name, store->names[i]) == 0)
	    break;
    if (i != store->numnames)
	store_found_insts(arg, entry);
}

static int
webgroup_store(struct metric *metric, int numnames, sds *names,
		int numids, sds *ids, sds value)
{
    struct instore	store = {0};
    struct indom	*indom;
    pmAtomValue		atom = {0};
    pmValueSet		*valueset = NULL;
    pmResult		*result = NULL;
    size_t		bytes;
    long		cursor = 0;
    int			i, sts, count;

    if ((sts = __pmStringValue(value, &atom, metric->desc.type)) < 0)
	return sts;

    if ((indom = metric->indom) != NULL &&
	(indom->updated == 0))
	pmwebapi_add_indom_instances(indom);

    if (metric->desc.indom == PM_INDOM_NULL)
	count = 1;
    else if (numids > 0)
	count = numids;
    else if (numnames > 0)
	count = numnames;
    else if (indom == NULL || (count = dictSize(indom->insts)) < 1)
	count = 1;

    bytes = sizeof(pmValueSet) + sizeof(pmValue) * (count - 1);
    if ((result = (pmResult *)calloc(1, sizeof(pmResult))) == NULL ||
	(valueset = (pmValueSet *)calloc(1, bytes)) == NULL) {
	if (atom.cp && metric->desc.type == PM_TYPE_STRING)
	    free(atom.cp);
	return -ENOMEM;
    }
    result->vset[0] = valueset;
    result->numpmid = 1;

    if (metric->desc.indom == PM_INDOM_NULL ||
	indom == NULL || dictSize(indom->insts) < 1) {
	valueset->vlist[0].inst = PM_IN_NULL;
        sts = __pmStuffValue(&atom, &valueset->vlist[0], metric->desc.type);
    } else if (numids > 0) {
	for (i = 0; i < numids && sts >= 0; i++) {
	    valueset->vlist[i].inst = atoi(ids[i]);
	    sts = __pmStuffValue(&atom, &valueset->vlist[i], metric->desc.type);
	}
    } else if (numnames > 0) {
	/* walk instances dictionary adding named instances */
	store.atom = &atom;
	store.vset = valueset;
	store.type = metric->desc.type;
	store.names = names;
	store.maximum = count;
	store.numnames = numnames;
	do {
	    cursor = dictScan(indom->insts, cursor,
				store_named_insts, NULL, &store);
	} while (cursor && store.status >= 0);
	sts = store.status;
    } else {
	/* walk instances dictionary adding all instances */
	store.atom = &atom;
	store.vset = valueset;
	store.type = metric->desc.type;
	store.maximum = count;
	do {
	    cursor = dictScan(indom->insts, cursor,
				store_found_insts, NULL, &store);
	} while (cursor && store.status >= 0);
	sts = store.status;
    }
    if (atom.cp && metric->desc.type == PM_TYPE_STRING)
	free(atom.cp);
    if (sts >= 0) {
	valueset->valfmt = sts;
	valueset->numval = count;
	valueset->pmid = metric->desc.pmid;
	sts = pmStore(result);
    }
    pmFreeResult(result);
    return sts;
}

void
pmWebGroupStore(pmWebGroupSettings *settings, sds id, dict *params, void *arg)
{
    struct context	*cp;
    struct metric	*mp;
    size_t		length;
    char		err[PM_MAXERRMSGLEN];
    sds			metric, value, pmid, instids, instnames;
    sds			msg = NULL, *names = NULL, *ids = NULL;
    int			sts = 0, numids = 0, numnames = 0;

    if (params) {
	metric = dictFetchValue(params, PARAM_MNAME);
	value = dictFetchValue(params, PARAM_MVALUE);
	pmid = dictFetchValue(params, PARAM_PMID);
	instids = dictFetchValue(params, PARAM_INSTANCE);
	instnames = dictFetchValue(params, PARAM_INAME);
    } else {
	metric = value = pmid = instids = instnames = NULL;
    }

    if (!(cp = webgroup_lookup_context(settings, &id, params, &sts, &msg, arg)))
	goto done;
    id = cp->origin;

    if (instnames) {
	length = sdslen(instnames);
	names = sdssplitlen(instnames, length, ",", 1, &numnames);
    } else if (instids) {
	length = sdslen(instids);
	ids = sdssplitlen(instids, length, ",", 1, &numids);
    }

    /* handle store via metric name list */
    if (metric) {
	if ((mp = webgroup_lookup_metric(settings, cp, metric, arg)) == NULL) {
	    infofmt(msg, "%s - failed to lookup metric", metric);
	    sts = -EINVAL;
	}
    }
    /* handle store via numeric/dotted-form PMIDs */
    else if (pmid) {
	if ((mp = webgroup_lookup_pmid(settings, cp, pmid, arg)) == NULL) {
	    infofmt(msg, "%s - failed to lookup PMID", pmid);
	    sts = -EINVAL;
	}
    } else {
	infofmt(msg, "bad parameters passed");
	sts = -EINVAL;
    }

    if ((sts >= 0) &&
	(sts = webgroup_store(mp, numnames, names, numids, ids, value)) < 0) {
	infofmt(msg, "failed to store value to metric %s: %s",
		metric ? metric : pmid, pmErrStr_r(sts, err, sizeof(err)));
    }

    sdsfreesplitres(names, numnames);
    sdsfreesplitres(ids, numids);

done:
    settings->callbacks.on_done(id, sts, msg, arg);
    sdsfree(msg);
}

int
pmWebGroupSetup(pmWebGroupModule *module)
{
    struct webgroups	*groups = webgroups_lookup(module);
    struct timeval	tv;
    unsigned int	pid;

    if (groups == NULL)
	return -ENOMEM;

    /* allocate strings for parameter dictionary key lookups */
    PARAM_HOSTNAME = sdsnew("hostname");
    PARAM_HOSTSPEC = sdsnew("hostspec");
    PARAM_CTXNUM = sdsnew("context");
    PARAM_CTXID = sdsnew("contextid");
    PARAM_POLLTIME = sdsnew("polltimeout");
    PARAM_PREFIX = sdsnew("prefix");
    PARAM_MNAMES = sdsnew("names");
    PARAM_PMIDS = sdsnew("pmids");
    PARAM_PMID = sdsnew("pmid");
    PARAM_INDOM = sdsnew("indom");
    PARAM_MNAME = sdsnew("name");
    PARAM_INSTANCE = sdsnew("instance");
    PARAM_INAME = sdsnew("iname");
    PARAM_MVALUE = sdsnew("value");
    PARAM_TARGET = sdsnew("target");
    PARAM_EXPR = sdsnew("expr");
    PARAM_MATCH = sdsnew("match");

    /* generally needed strings, error messages */
    EMPTYSTRING = sdsnew("");
    LOCALHOST = sdsnew("localhost");
    TIMEOUT = sdsnew("pmwebapi.timeout");
    BATCHSIZE = sdsnew("pmwebapi.batchsize");
    AUTH_USERNAME = sdsnew("auth.username");
    AUTH_PASSWORD = sdsnew("auth.password");

    /* setup the random number generator for context IDs */
    gettimeofday(&tv, NULL);
    pid = (unsigned int)getpid();
    srandom(pid ^ (unsigned int)tv.tv_sec ^ (unsigned int)tv.tv_usec);

    /* setup a dictionary mapping context number to data */
    groups->contexts = dictCreate(&intKeyDictCallBacks, NULL);
    return 0;
}

int
pmWebGroupSetEventLoop(pmWebGroupModule *module, void *events)
{
    struct webgroups	*webgroups = webgroups_lookup(module);

    if (webgroups) {
	webgroups->events = (uv_loop_t *)events;
	return 0;
    }
    return -ENOMEM;
}

int
pmWebGroupSetConfiguration(pmWebGroupModule *module, dict *config)
{
    struct webgroups	*webgroups = webgroups_lookup(module);
    char		*endnum;
    sds			value;

    if ((value = dictFetchValue(config, TIMEOUT)) == NULL) {
	default_timeout = DEFAULT_TIMEOUT;
    } else {
	default_timeout = strtoul(value, &endnum, 0);
	if (*endnum != '\0')
	    default_timeout = DEFAULT_TIMEOUT;
    }

    if ((value = dictFetchValue(config, BATCHSIZE)) == NULL) {
	default_batchsize = DEFAULT_BATCHSIZE;
    } else {
	default_batchsize = strtoul(value, &endnum, 0);
	if (*endnum != '\0')
	    default_batchsize = DEFAULT_BATCHSIZE;
    }

    if (webgroups) {
	webgroups->config = config;
	return 0;
    }
    return -ENOMEM;
}

int
pmWebGroupSetMetricRegistry(pmWebGroupModule *module, mmv_registry_t *registry)
{
    struct webgroups	*webgroups = webgroups_lookup(module);

    if (webgroups) {
	webgroups->metrics = registry;
	return 0;
    }
    return -ENOMEM;
}

void
pmWebGroupClose(pmWebGroupModule *module)
{
    struct webgroups	*groups = (struct webgroups *)module->privdata;

    if (groups) {
	dictRelease(groups->contexts);
	memset(groups, 0, sizeof(struct webgroups));
	free(groups);
    }
}