#include <arpa/inet.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fbbs/dbi.h"
#include "fbbs/pool.h"
#include "fbbs/string.h"
#include "fbbs/util.h"

/** Postgresql epoch (Jan 1, 2000) in unix time. */
#define POSTGRES_EPOCH_TIME INT64_C(946684800)

/**
 * Default postgresql installation used to use double to represent timestamp.
 * It was changed to 64-bit integer since 8.4 (Jul 2009).
 */
#define HAVE_INT64_TIMESTAMP

fb_time_t ts_to_time(db_timestamp ts)
{
	return ts / INT64_C(1000000) + POSTGRES_EPOCH_TIME;
}

db_timestamp time_to_ts(fb_time_t t)
{
	return (t - POSTGRES_EPOCH_TIME) * INT64_C(1000000);
}

void db_clear(db_res_t *res)
{
	if (res)
		PQclear(res);
}

#define _is_binary_field(res, col)  PQfformat(res, col)

int16_t db_get_smallint(const db_res_t *res, int row, int col)
{
	const char *r = PQgetvalue(res, row, col);
	if (_is_binary_field(res, col)) {
		return (int16_t)ntohs(*((uint16_t *)r));
	} else {
		return (int16_t)strtol(r, NULL, 10);
	}
}

int32_t db_get_integer(const db_res_t *res, int row, int col)
{
	const char *r = PQgetvalue(res, row, col);
	if (_is_binary_field(res, col)) {
		return (int32_t)ntohl(*((uint32_t *)r));
	} else {
		return (int32_t)strtol(r, NULL, 10);
	}
}

int64_t db_get_bigint(const db_res_t *res, int row, int col)
{
	const char *r = PQgetvalue(res, row, col);
	if (_is_binary_field(res, col)) {
		return (int64_t)be64toh(*((uint64_t *)r));
	} else {
		return (int64_t)strtoll(r, NULL, 10);
	}
}

bool db_get_bool(const db_res_t *res, int row, int col)
{
	const char *r = PQgetvalue(res, row, col);
	if (_is_binary_field(res, col)) {
		return *r;
	} else {
		switch (*r) {
			case 'T':
			case 't':
			case 'Y':
			case 'y':
			case '1':
				return true;
			default:
				return false;
		}
	}
}

fb_time_t db_get_time(const db_res_t *res, int row, int col)
{
	const char *r = PQgetvalue(res, row, col);
	if (_is_binary_field(res, col)) {
#ifdef HAVE_INT64_TIMESTAMP
		db_timestamp ts = (int64_t)be64toh(*(uint64_t *)r);
#else
		uint64_t t = be64toh(*(uint64_t *)r);
		db_timestamp ts = *(double *)&t * 1000000;
#endif
		return ts_to_time(ts);
	}
	return 0;
}

#define is_supported_format(c) (c == 'd' || c == 'l' || c == 's' || c == 't')

static int get_num_args(const char *cmd)
{
	int argc = 0;
	while (*cmd != '\0') {
		if (*cmd == '%') {
			++cmd;
			if (is_supported_format(*cmd)) {
				++argc;
			}
		}
		++cmd;
	}
	return argc;
}

typedef struct query_t {
	pstring_t *s;
	pool_t *p;
	const char **vals;
	int *lens;
	int *fmts;
} query_t;

static query_t *query_new(const char *cmd, int argc, va_list ap)
{
	pool_t *p = pool_create(DEFAULT_POOL_SIZE);
	query_t *q = pool_alloc(p, sizeof(*q));
	q->p = p;
	q->vals = pool_alloc(p, sizeof(*q->vals) * argc);
	q->lens = pool_alloc(p, sizeof(*q->lens) * argc);
	q->fmts = pool_alloc(p, sizeof(*q->fmts) * argc);
	q->s = pstring_sized_new(p, strlen(cmd));

	int n = 0;
	while (*cmd != '\0') {
		if (*cmd == '%') {
			int d;
			int64_t l;
			const char *s;
			fb_time_t t;

			++cmd;
			switch (*cmd) {
				case 'd':
					d = va_arg(ap, int);
					int *dp = pool_alloc(p, sizeof(*dp));
					*dp = htonl(d);
					q->vals[n] = (const char *) dp;
					q->lens[n] = sizeof(int);
					q->fmts[n] = 1;
					break;
				case 'l':
					l = va_arg(ap, int64_t);
					int64_t *lp = pool_alloc(p, sizeof(*lp));
					*lp = htobe64(l);
					q->vals[n] = (const char *) lp;
					q->lens[n] = sizeof(int64_t);
					q->fmts[n] = 1;
					break;
				case 's':
					s = va_arg(ap, const char *);
					q->vals[n] = s;
					q->lens[n] = 0;
					q->fmts[n] = 0;
					break;
				case 't':
					t = va_arg(ap, fb_time_t);
					db_timestamp ts = time_to_ts(t);
					db_timestamp *tsp = pool_alloc(p, sizeof(*tsp));
					*tsp = htobe64(ts);
					q->vals[n] = (const char *) tsp;
					q->lens[n] = sizeof(int64_t);
					q->fmts[n] = 1;
					break;
				default:
					pstring_append_c(p, q->s, *cmd);
					break;
			}
			if (is_supported_format(*cmd)) {
				pstring_append_printf(p, q->s, "$%d", ++n);
			}
		} else {
			pstring_append_c(p, q->s, *cmd);
		}
		++cmd;
	}
	return q;
}

static void query_free(query_t *q)
{
	pool_destroy(q->p);
}

static db_res_t *_db_exec_cmd(db_conn_t *conn, const char *cmd, bool binary,
		int expected, va_list ap)
{
	db_res_t *res;
	int argc = get_num_args(cmd);
	if (argc > 0) {
		query_t *q = query_new(cmd, argc, ap);
		res = PQexecParams(conn, q->s->str, argc, NULL, q->vals, q->lens,
				q->fmts, binary);
		query_free(q);
	} else {
		res = PQexecParams(conn, cmd, 0, NULL, NULL, NULL, NULL, binary);
	}

	if (db_res_status(res) != expected) {
		db_clear(res);
		return NULL;
	}

	return res;
}

db_res_t *db_exec_cmd(db_conn_t *conn, const char *cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	db_res_t *res = _db_exec_cmd(conn, cmd, true, DBRES_COMMAND_OK, ap);
	va_end(ap);
	return res;
}

db_res_t *db_exec_query(db_conn_t *conn, bool binary, const char *cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	db_res_t *res = _db_exec_cmd(conn, cmd, binary, DBRES_TUPLES_OK, ap);
	va_end(ap);
	return res;
}

int db_begin_trans(db_conn_t *conn)
{
	db_res_t *res = PQexec(conn, "BEGIN");
	int r = (PQresultStatus(res) == PGRES_COMMAND_OK ? 0 : -1);
	PQclear(res);
	return r;
}

int db_end_trans(db_conn_t *conn)
{
	db_res_t *res = PQexec(conn, "END");
	int r = (PQresultStatus(res) == PGRES_COMMAND_OK ? 0 : -1);
	PQclear(res);
	return r;
}
