#define _GNU_SOURCE
#include "bbs.h"
#include "fbbs/brc.h"
#include "fbbs/fileio.h"
#include "fbbs/friend.h"
#include "fbbs/helper.h"
#include "fbbs/list.h"
#include "fbbs/mail.h"
#include "fbbs/post.h"
#include "fbbs/session.h"
#include "fbbs/string.h"
#include "fbbs/terminal.h"
#include "fbbs/tui_list.h"

static bool is_asc(slide_list_base_e base)
{
	return (base == SLIDE_LIST_TOPDOWN || base == SLIDE_LIST_NEXT);
}

static void clear_filter(post_filter_t *filter)
{
	filter->min = filter->max = filter->tid = 0;
	if (filter->type != POST_LIST_AUTHOR)
		filter->uid = 0;
	if (filter->type == POST_LIST_NORMAL)
		filter->flag = 0;
	if (filter->type != POST_LIST_KEYWORD)
		filter->utf8_keyword[0] = '\0';
}

/** @defgroup plist_cache Post List Cache */
/** @{ */

typedef struct {
	post_info_t *posts;  ///< Array to hold posts in.
	int begin;  ///< Index of first visible post.
	int end;  ///< Off-the-end index of last visible post.
	int count;  ///< Number of posts in the cache.
	int scount;  ///< Number of sticky posts in the cache.
	int capacity;  ///< Maximum number of posts.
	int scapacity;  ///< Maximum number of sticky posts.
	int page;  ///< Count of posts in a page.
	post_id_t top;  ///< The oldest post id.
	post_id_t bottom;  ///< The newest post id.
	bool sticky;  ///< True if sticky posts support is on.
} plist_cache_t;

static void plist_cache_clear(plist_cache_t *c)
{
	c->begin = c->end = c->count = 0;
	c->top = c->bottom = 0;
}

static void plist_cache_init(plist_cache_t *c, int page, int scapacity)
{
	if (!c->posts) {
		c->scount = -1;
		c->page = page;
		c->capacity = page * 3;
		c->scapacity = scapacity;
		c->posts = malloc(sizeof(*c->posts) * (c->capacity + c->scapacity));
		plist_cache_clear(c);
	}
}

static post_info_t *plist_cache_get(const plist_cache_t *c, int pos)
{
	if (pos >= 0) {
		if (c->begin + pos < c->end)
			return c->posts + c->begin + pos;
		pos -= c->end - c->begin;
		if (c->sticky && pos < c->scount)
			return c->posts + c->capacity + pos;
	}
	return NULL;
}

static post_info_t *plist_cache_get_non_sticky(const plist_cache_t *c,
		int pos)
{
	while (1) {
		post_info_t *p = plist_cache_get(c, pos);
		if (!p || !(p->flag & POST_FLAG_STICKY))
			return p;
		if (--pos < 0)
			break;
	}
	return NULL;
}

static bool plist_cache_is_top(const plist_cache_t *c, int pos)
{
	if (!c->top)
		return false;
	if (pos < 0)
		pos = 0;
	post_info_t *p = plist_cache_get(c, pos);
	return p && p->id <= c->top;
}

static bool plist_cache_is_bottom(const plist_cache_t *c, int pos)
{
	if (!c->bottom)
		return false;
	post_info_t *p = NULL;
	if (pos < 0)
		pos = c->page - 1;
	p = plist_cache_get_non_sticky(c, pos);
	return p && p->id >= c->bottom;
}

static int plist_cache_max_visible(const plist_cache_t *c)
{
	int max = c->end - c->begin;
	if (c->sticky && plist_cache_is_bottom(c, -1) && c->end == c->count)
		max += c->scount;
	return max > c->page ? c->page : max;
}

static void adjust_window(plist_cache_t *c, int delta)
{
	c->begin += delta;
	c->end += delta;
	if (c->begin < 0)
		c->begin = 0;
	if (c->end > c->count)
		c->end = c->count;
	if (c->end - c->begin < c->page) {
		if (delta > 0) {
			if (c->sticky && c->bottom
					&& c->posts[c->end - 1].id >= c->bottom) {
				if (c->end - c->begin < c->page - c->scount
						|| c->begin >= c->end)
					c->begin = c->end + c->scount - c->page;
			} else {
				c->begin = c->end - c->page;
			}
			if (c->begin < 0)
				c->begin = 0;
		} else {
			c->end = c->begin + c->page;
			if (c->end > c->count)
				c->end = c->count;
		}
	}
}

static bool cached_top(const plist_cache_t *c)
{
	return c->top && c->posts[0].id <= c->top;
}

static bool cached_bottom(const plist_cache_t *c)
{
	return c->bottom && c->posts[c->count - 1].id >= c->bottom;
}

static bool already_cached(plist_cache_t *c, slide_list_base_e base)
{
	if (base == SLIDE_LIST_TOPDOWN && cached_top(c))
		adjust_window(c, -c->capacity);
	else if (base == SLIDE_LIST_BOTTOMUP && cached_bottom(c))
		adjust_window(c, c->capacity);
	else if (base == SLIDE_LIST_PREV && (c->begin >= c->page || cached_top(c)))
		adjust_window(c, -c->page);
	else if (base == SLIDE_LIST_NEXT &&
			(c->count - c->end >= c->page || cached_bottom(c)))
		adjust_window(c, c->page);
	else
		return false;
	return true;
}

static void set_window(plist_cache_t *c, slide_list_base_e base)
{
	if (base == SLIDE_LIST_TOPDOWN) {
		adjust_window(c, -c->capacity);
	} else if (base == SLIDE_LIST_BOTTOMUP) {
		adjust_window(c, c->capacity + c->scapacity);
	} else {
		adjust_window(c, is_asc(base) ? c->page : -c->page);
	}
}

static int load_posts_from_db(plist_cache_t *c, post_filter_t *filter,
		slide_list_base_e base, int limit)
{
	bool asc = is_asc(base);

	query_t *q = build_post_query(filter, asc, limit);
	db_res_t *res = query_exec(q);

	int rows = db_res_rows(res);
	int extra = rows + c->count - c->capacity;
	if (extra < 0)
		extra = 0;
	if (asc) {
		memmove(c->posts, c->posts + extra,
				sizeof(*c->posts) * (c->count - extra));
		c->begin -= extra;
		c->end -= extra;
		for (int i = 0; i < rows; ++i) {
			res_to_post_info(res, i, filter->archive,
					c->posts + c->count - extra + i);
		}
	} else {
		memmove(c->posts + extra, c->posts,
				sizeof(*c->posts) * (c->count - extra));
		c->begin += extra;
		c->end += extra;
		for (int i = 0; i < rows; ++i) {
			res_to_post_info(res, i, filter->archive, c->posts + rows - i - 1);
		}
	}
	c->count += rows - extra;
	if (base == SLIDE_LIST_TOPDOWN || base == SLIDE_LIST_BOTTOMUP
			|| (rows < limit && c->count >= 0)) {
		if (base == SLIDE_LIST_NEXT || base == SLIDE_LIST_BOTTOMUP)
			c->bottom = c->posts[c->count - 1].id;
		else
			c->top = c->posts[0].id;
	}

	set_window(c, base);
	return rows;
}

static void plist_cache_set_sticky(plist_cache_t *c, const post_filter_t *fp)
{
	c->sticky = (fp->type == POST_LIST_NORMAL && !fp->archive);
}

static int plist_cache_load(plist_cache_t *c, post_filter_t *filter,
		slide_list_base_e base, bool force)
{
	if (!force && already_cached(c, base))
		return 0;
	if (force)
		plist_cache_clear(c);

	int limit = 0;
	if (base == SLIDE_LIST_TOPDOWN || base == SLIDE_LIST_BOTTOMUP) {
		limit = c->capacity;
		plist_cache_clear(c);
	} else if (base == SLIDE_LIST_NEXT) {
		limit = c->capacity - (c->count - c->begin);
		if (c->count > 0) {
			filter->min = c->posts[c->count - 1].id + 1;
			if (filter->type == POST_LIST_THREAD)
				filter->tid = c->posts[c->count - 1].tid;
		}
	} else if (base == SLIDE_LIST_PREV) {
		limit = c->capacity - c->end;
		if (c->count > 0) {
			filter->max = c->posts[0].id - 1;
			if (filter->type == POST_LIST_THREAD)
				filter->tid = c->posts[0].tid;
		}
	}

	int rows = load_posts_from_db(c, filter, base, limit);
	if (rows < c->page && base == SLIDE_LIST_NEXT) {
		plist_cache_clear(c);
		clear_filter(filter);
		rows = load_posts_from_db(c, filter, SLIDE_LIST_BOTTOMUP, limit);
	}
	return rows;
}

static void plist_cache_load_sticky(plist_cache_t *c, post_filter_t *filter,
		bool force)
{
	post_info_t *p = c->posts + c->capacity;
	if (force || (c->sticky && c->scount < 0))
		c->scount = load_sticky_posts(filter->bid, &p);
}

static bool match_filter(post_filter_t *filter, post_info_t *p)
{
	bool match = true;
	if (filter->uid)
		match &= p->uid == filter->uid;
	if (filter->min)
		match &= p->id >= filter->min;
	if (filter->max)
		match &= p->id <= filter->max;
	if (filter->tid)
		match &= p->tid == filter->tid;
	if (*filter->utf8_keyword)
		match &= (bool)strcasestr(p->utf8_title, filter->utf8_keyword);
	return match;
}

static int relocate_cursor(plist_cache_t *c, int cursor)
{
	if (cursor >= 0 && cursor < c->count)
	while (cursor < c->begin)
		adjust_window(c, -c->page);
	while (cursor >= c->end)
		adjust_window(c, c->page);
	return cursor - c->begin;
}

static int plist_cache_relocate(plist_cache_t *c, int current,
		post_filter_t *filter, bool upward)
{
	int found = -1;

	int delta = upward ? -1 : 1;
	for (int i = c->begin + current + delta;
			i >= 0 && i < c->count;
			i += delta) {
		if (match_filter(filter, c->posts + i)) {
			found = i;
			break;
		}
	}

	if (found >= 0)
		return relocate_cursor(c, found);
	return found;
}

static void plist_cache_free(plist_cache_t *c)
{
	free(c->posts);
}

/** @} */

typedef struct post_list_position_t {
	post_list_type_e type;
	user_id_t uid;
	post_id_t min_pid;
	post_id_t min_tid;
	post_id_t cur_pid;
	post_id_t cur_tid;
	UTF8_BUFFER(keyword, POST_LIST_KEYWORD_LEN);
	SLIST_FIELD(post_list_position_t) next;
} post_list_position_t;

SLIST_HEAD(post_list_position_list_t, post_list_position_t);

typedef struct {
	post_filter_t filter;
	post_list_position_t *pos;
	archive_list_t *archives;
	slide_list_base_e abase;

	bool reload;
	bool sreload;
	post_id_t relocate;
	post_id_t current_tid;
	post_id_t mark_pid;

	plist_cache_t cache;
} post_list_t;

static bool match(const post_list_position_t *p, const post_filter_t *fp)
{
	return !(p->type != fp->type
			|| (p->type == POST_LIST_AUTHOR && p->uid != fp->uid)
			|| (p->type == POST_LIST_KEYWORD
				&& streq(p->utf8_keyword, fp->utf8_keyword)));
}

static void filter_to_position_record(const post_filter_t *fp,
		post_list_position_t *p)
{
	p->type = fp->type;
	p->uid = fp->uid;
	p->min_pid = p->min_tid = p->cur_pid = p->cur_tid = 0;
	memcpy(p->utf8_keyword, fp->utf8_keyword, sizeof(p->utf8_keyword));
}

static post_list_position_t *get_post_list_position(const post_filter_t *fp)
{
	static struct post_list_position_list_t *list = NULL;
	if (!list) {
		list = malloc(sizeof(*list));
		SLIST_INIT_HEAD(list);
	}

	if (fp->archive)
		return NULL;

	SLIST_FOREACH(post_list_position_t, p, list, next) {
		if (match(p, fp))
			return p;
	}

	post_list_position_t *p = malloc(sizeof(*p));
	filter_to_position_record(fp, p);
	SLIST_INSERT_HEAD(list, p, next);
	return p;
}

static void save_post_list_position(const slide_list_t *p)
{
	post_list_t *l = p->data;
	if (!l->pos)
		return;

	post_info_t *min = plist_cache_get(&l->cache, 0);
	post_info_t *cur = plist_cache_get_non_sticky(&l->cache, p->cur);
	if (!cur)
		cur = min;

	l->pos->min_tid = min ? min->tid : 0;
	l->pos->cur_tid = cur ? cur->tid : 0;
	l->pos->min_pid = min ? min->id : 0;
	l->pos->cur_pid = cur ? cur->id : 0;
}

static post_info_t *get_post_info(slide_list_t *p)
{
	post_list_t *l = p->data;
	return plist_cache_get(&l->cache, p->cur);
}

static slide_list_loader_t post_list_loader(slide_list_t *p)
{
	post_list_t *l = p->data;

	int page = t_lines - 4;
	plist_cache_init(&l->cache, page, MAX_NOTICE);

	if (l->reload) {
		plist_cache_clear(&l->cache);

		if (l->abase != SLIDE_LIST_CURRENT) {
			p->base = l->abase;
			l->filter.min = 0;
			l->abase = SLIDE_LIST_CURRENT;
		} else {
			p->base = SLIDE_LIST_NEXT;
			if (l->pos->min_pid)
				l->filter.min = l->pos->min_pid;
			else
				l->filter.min = brc_last_read() + 1;
		}
		l->filter.max = 0;
	}

	if (p->base == SLIDE_LIST_CURRENT) {
		save_post_list_position(p);
		return 0;
	}

	plist_cache_set_sticky(&l->cache, &l->filter);
	plist_cache_load_sticky(&l->cache, &l->filter, l->sreload);
	plist_cache_load(&l->cache, &l->filter, p->base, l->reload || l->relocate);

	if (l->relocate || l->reload) {
		post_filter_t filter = {
			.min = l->relocate ? l->relocate : l->pos->cur_pid
		};
		int pos = plist_cache_relocate(&l->cache, -1, &filter, false);
		if (pos >= 0)
			p->cur = pos;
		l->relocate = 0;
	}
	p->max = plist_cache_max_visible(&l->cache);
	if (p->update != FULLUPDATE)
		p->update = PARTUPDATE;

	save_post_list_position(p);
	clear_filter(&l->filter);
	l->sreload = l->reload = false;
	return 0;
}

static basic_session_info_t *get_sessions_by_name(const char *uname)
{
	return db_query("SELECT "BASIC_SESSION_INFO_FIELDS
			" FROM sessions s JOIN alive_users u ON s.user_id = u.id"
			" WHERE s.active AND lower(u.name) = lower(%s)", uname);
}

static int bm_color(const char *uname)
{
	basic_session_info_t *s = get_sessions_by_name(uname);
	int visible = 0, color = 33;

	if (s) {
		for (int i = 0; i < basic_session_info_count(s); ++i) {
			if (basic_session_info_visible(s, i))
				++visible;
		}

		if (visible)
			color = 32;
		else if (HAS_PERM(PERM_SEECLOAK) && basic_session_info_count(s) > 0)
			color = 36;
	}
	basic_session_info_clear(s);
	return color;
}

enum {
	BM_NAME_LIST_LEN = 56,
};

static int show_board_managers(const board_t *bp)
{
	prints("\033[33m");

	if (!bp->bms[0]) {
		prints("诚征版主中");
		return 10;
	}

	char bms[sizeof(bp->bms)];
	strlcpy(bms, bp->bms, sizeof(bms));

	prints("版主:");
	int width = 5;
	for (const char *s = strtok(bms, " "); s; s = strtok(NULL, " ")) {
		++width;
		prints(" ");

		int len = strlen(s);
		if (width + len > BM_NAME_LIST_LEN) {
			prints("...");
			return width + 3;
		}

		width += len;
		int color = bm_color(s);
		prints("\033[%dm%s", color, s);
	}

	prints("\033[36m");
	return width;
}

static void repeat(int c, int repeat)
{
	for (int i = 0; i < repeat; ++i)
		outc(c);
}

static void align_center(const char *s, int remain)
{
	int len = strlen(s);
	if (len > remain) {
		prints("%s", s);
	} else {
		int spaces = (remain - len) / 2;
		repeat(' ', spaces);
		prints("%s", s);
		spaces = remain - len - spaces;
		repeat(' ', spaces);
	}
}

static void show_prompt(const board_t *bp, const char *prompt, int remain)
{
	GBK_BUFFER(descr, BOARD_DESCR_CCHARS);
	convert_u2g(bp->descr, gbk_descr);
	int blen = strlen(bp->name) + 2;
	int plen = prompt ? strlen(prompt) : strlen(gbk_descr);

	if (blen + plen > remain) {
		if (prompt) {
			align_center(prompt, remain);
		} else {
			repeat(' ', remain - blen);
			prints("[%s]", bp->name);
		}
		return;
	} else {
		align_center(prompt ? prompt : gbk_descr, remain - blen);
		prints("\033[33m[%s]", bp->name);
	}
}

static const char *mode_description(post_list_type_e type)
{
	const char *s;
	switch (type) {
		case POST_LIST_NORMAL:
			if (DEFINE(DEF_THESIS))
				s = "主题模式";
			else
				s =  "一般模式";
			break;
		case POST_LIST_THREAD:
			s = "同主题";
			break;
		case POST_LIST_TOPIC:
			s = "原作";
			break;
		case POST_LIST_MARKED:
			s = "保留";
			break;
		case POST_LIST_DIGEST:
			s = "文摘";
			break;
		case POST_LIST_AUTHOR:
			s = "同作者";
			break;
		case POST_LIST_KEYWORD:
			s = "标题关键字";
			break;
		case POST_LIST_TRASH:
			s = "垃圾箱";
			break;
		case POST_LIST_JUNK:
			s = "站务垃圾箱";
			break;
//		case ATTACH_MODE:
//			readmode = "附件区";
//			break;
		default:
			s = "未定义";
	}
	return s;
}

static void _post_list_title(int archive_list)
{
	prints("\033[1;44m");
	int width = show_board_managers(currbp);

	const char *prompt = NULL;
	if (chkmail())
		prompt = "[您有信件，按 M 看新信]";
	else if ((currbp->flag & BOARD_VOTE_FLAG))
		prompt = "※投票中,按 v 进入投票※";
	show_prompt(currbp, prompt, 80 - width);

	move(1, 0);
	prints("\033[m 离开[\033[1;32m←\033[m,\033[1;32me\033[m] "
		"选择[\033[1;32m↑\033[m,\033[1;32m↓\033[m] "
		"阅读[\033[1;32m→\033[m,\033[1;32mRtn\033[m]");
	if (!archive_list) {
		prints(" 发文章[\033[1;32mCtrl-P\033[m] 砍信[\033[1;32md\033[m] "
				"备忘录[\033[1;32mTAB\033[m]");
	}
	prints(" 求助[\033[1;32mh\033[m]\n");
}

static slide_list_title_t post_list_title(slide_list_t *p)
{
	_post_list_title(false);

	post_list_t *l = p->data;
	const char *mode = mode_description(l->filter.type);

	prints("\033[1;37;44m编号 %-12s %6s %-25s     在线:%-4d",
			"刊 登 者", "日  期", " 标  题", count_onboard(currbp->id));
	if (l->filter.archive) {
		int year = l->filter.archive / 10000;
		int month = (l->filter.archive % 10000) / 100;
		prints("[%d年%d月存档]", year, month);
	} else {
		prints("    [%s]", mode);
	}
	prints("\033[K\033[m\n");
	clrtobot();
}

char *getshortdate(time_t time) {
	static char str[10];
	struct tm *tm;

	tm = localtime(&time);
	sprintf(str, "%02d.%02d.%02d", tm->tm_year - 100, tm->tm_mon + 1,
			tm->tm_mday);
	return str;
}

#ifdef ENABLE_FDQUAN
#define ENABLE_COLOR_ONLINE_STATUS
#endif

#ifdef ENABLE_COLOR_ONLINE_STATUS
const char *get_board_online_color(const char *uname, int bid)
{
	if (streq(uname, currentuser.userid))
		return session.visible ? "1;37" : "1;36";

	const char *color = "";
	bool online = false;
	int invisible = 0;

	basic_session_info_t *s = get_sessions_by_name(uname);
	if (s) {
		for (int i = 0; i < basic_session_info_count(s); ++i) {
			if (!basic_session_info_visible(s, i)) {
				if (HAS_PERM(PERM_SEECLOAK))
					++invisible;
				else
					continue;
			}

			online = true;
			if (get_current_board(basic_session_info_sid(s, i)) == bid) {
				if (basic_session_info_visible(s, i))
					color = "1;37";
				else
					color = "1;36";
				basic_session_info_clear(s);
				return color;
			}
		}
	}

	if (!online)
		color = "1;30";
	else if (invisible > 0 && invisible == basic_session_info_count(s))
		color = "36";

	basic_session_info_clear(s);
	return color;
}
#else
#define get_board_online_color(n, i)  ""
#endif

static const char *get_post_date(fb_time_t stamp)
{
#ifdef FDQUAN
	if (time(NULL) - stamp < 24 * 60 * 60)
		return fb_ctime(&stamp) + 10;
#endif
	return fb_ctime(&stamp) + 4;
}

static void post_list_display_entry(const post_list_t *l, const post_info_t *p,
		bool last)
{
	post_list_type_e type = l->filter.type;

	char mark_buf[16];
	if (p->flag & POST_FLAG_STICKY) {
		strlcpy(mark_buf, "\033[1;31m∞\033[m", sizeof(mark_buf));
	} else {
		mark_buf[0] = ' ';
		mark_buf[1] = (p->id == l->mark_pid) ? '@' : get_post_mark(p);
		mark_buf[2] = '\0';
	}

	const char *mark_prefix = "", *mark_suffix = "";
	if ((p->flag & POST_FLAG_IMPORT) && am_curr_bm()) {
		mark_prefix = (type == ' ') ? "\033[42m" : "\033[32m";
		mark_suffix = "\033[m";
	}
#if 0
	if (digestmode == ATTACH_MODE) {
		filetime = ent->timeDeleted;
	} else {
		filetime = atoi(ent->filename + 2);
	}
#endif

	const char *date = get_post_date(p->stamp);

	const char *idcolor = get_board_online_color(p->owner, currbp->id);

	char color[10] = "";
#ifdef COLOR_POST_DATE
	struct tm *mytm = fb_localtime(&p->stamp);
	snprintf(color, sizeof(color), "\033[1;%dm", 30 + mytm->tm_wday + 1);
#endif

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	if (strneq(p->utf8_title, "Re: ", 4)) {
		if (type == POST_LIST_THREAD) {
			GBK_BUFFER(title2, POST_TITLE_CCHARS);
			convert_u2g(p->utf8_title, gbk_title2);
			snprintf(gbk_title, sizeof(gbk_title), "%s %s",
					last ? "└" : "├", gbk_title2 + 4);
		} else {
			convert_u2g(p->utf8_title, gbk_title);
		}
	} else {
		GBK_BUFFER(title2, POST_TITLE_CCHARS);
		convert_u2g(p->utf8_title, gbk_title2);
		snprintf(gbk_title, sizeof(gbk_title), "◆ %s", gbk_title2);
	}

	const int title_width = 49;
	if (is_deleted(type)) {
		char buf[80], date[12];
		ellipsis(gbk_title, title_width - sizeof(date) - strlen(p->ename) + 1);
		struct tm *t = fb_localtime(&p->estamp);
		strftime(date, sizeof(date), "%m-%d %H:%S", t);
		snprintf(buf, sizeof(buf), "%s\033[1;%dm[%s %s]\033[m", gbk_title,
				(p->flag & POST_FLAG_JUNK) ? 31 : 32, p->ename, date);
		strlcpy(gbk_title, buf, sizeof(gbk_title));
	} else {
		ellipsis(gbk_title, title_width);
	}

//	if (digestmode == ATTACH_MODE) {
//		sprintf(buf, " %5d %c %-12.12s %s%6.6s\033[m %s", num, type,
//				ent->owner, color, date, title);

	const char *thread_color = "0;37";
	if (p->tid == l->current_tid) {
		if (p->id == p->tid)
			thread_color = "1;32";
		else
			thread_color = "1;36";
	}

	char buf[128];
	snprintf(buf, sizeof(buf),
			"  %s%s%s \033[%sm%-12.12s %s%6.6s %s\033[%sm%s\033[m\n",
			mark_prefix, mark_buf, mark_suffix,
			idcolor, p->owner, color, date,
			(p->flag & POST_FLAG_LOCKED) ? "\033[1;33mx" : " ",
			thread_color, gbk_title);
	outs(buf);
}

static slide_list_display_t post_list_display(slide_list_t *p)
{
	post_list_t *l = p->data;
	bool empty = false;
	for (int i = 0; i < l->cache.page; ++i) {
		post_info_t *ip = plist_cache_get(&l->cache, i);
		if (!ip) {
			if (i == 0)
				empty = true;
			continue;
		}
		post_info_t *next = plist_cache_get(&l->cache, i + 1);
		bool last = false;
		if (l->filter.type == POST_LIST_THREAD) {
			last = next && ip->tid != next->tid;
		}
		post_list_display_entry(l, ip, last);
	}

	if (empty)
		prints("     (无内容)\n");
	return 0;
}

static int toggle_post_lock(post_info_t *ip)
{
	if (ip) {
		bool locked = ip->flag & POST_FLAG_LOCKED;
		if (am_curr_bm() || (session.id == ip->uid && !locked)) {
			post_filter_t filter = {
				.bid = ip->bid, .min = ip->id, .max = ip->id
			};
			if (set_post_flag(&filter, "locked", !locked, false)) {
				set_post_flag_local(ip, POST_FLAG_LOCKED, !locked);
				return PARTUPDATE;
			}
		}
	}
	return DONOTHING;
}

static int toggle_post_stickiness(post_info_t *ip, post_list_t *l)
{
	if (!ip || is_deleted(l->filter.type))
		return DONOTHING;

	bool sticky = ip->flag & POST_FLAG_STICKY;
	if (am_curr_bm() && sticky_post_unchecked(ip->bid, ip->id, !sticky)) {
		l->sreload = l->reload = true;
		return PARTUPDATE;
	}
	return DONOTHING;
}

static int toggle_post_flag(post_info_t *ip, post_flag_e flag,
		const char *field)
{
	if (ip) {
		bool set = ip->flag & flag;
		if (am_curr_bm()) {
			post_filter_t filter = {
				.bid = ip->bid, .min = ip->id, .max = ip->id
			};
			if (set_post_flag(&filter, field, !set, false)) {
				set_post_flag_local(ip, flag, !set);
				return PARTUPDATE;
			}
		}
	}
	return DONOTHING;
}

static int post_list_with_filter(const post_filter_t *filter);

static int post_list_deleted(post_list_t *l)
{
	if (l->filter.type != POST_LIST_NORMAL || !am_curr_bm())
		return DONOTHING;

	post_filter_t filter = { .type = POST_LIST_TRASH, .bid = l->filter.bid };
	post_list_with_filter(&filter);
	l->reload = l->sreload = true;
	return FULLUPDATE;
}

static int post_list_admin_deleted(post_list_t *l)
{
	if (l->filter.type != POST_LIST_NORMAL || !HAS_PERM(PERM_OBOARDS))
		return DONOTHING;

	post_filter_t filter = { .type = POST_LIST_JUNK, .bid = l->filter.bid };
	post_list_with_filter(&filter);
	l->reload = l->sreload = true;
	return FULLUPDATE;
}

static int tui_post_list_selected(slide_list_t *p, post_info_t *ip)
{
	post_list_t *l = p->data;
	if (!ip || l->filter.type != POST_LIST_NORMAL)
		return DONOTHING;

	char ans[3];
	getdata(t_lines - 1, 0, "切换模式到: 1)文摘 2)同主题 3)被 m 文章 4)原作"
			" 5)同作者 6)标题关键字 [1]: ", ans, sizeof(ans), DOECHO, YEA);

	int c = ans[0];
	if (!c)
		c = '1';
	c -= '1';

	const post_list_type_e types[] = {
		POST_LIST_DIGEST, POST_LIST_THREAD, POST_LIST_MARKED,
		POST_LIST_TOPIC, POST_LIST_AUTHOR, POST_LIST_KEYWORD,
	};
	if (c < 0 || c >= ARRAY_SIZE(types))
		return MINIUPDATE;

	post_filter_t filter = { .bid = l->filter.bid, .type = types[c] };
	switch (filter.type) {
		case POST_LIST_DIGEST:
			filter.flag |= POST_FLAG_DIGEST;
			break;
		case POST_LIST_THREAD:
			filter.type = POST_LIST_THREAD;
			break;
		case POST_LIST_MARKED:
			filter.flag |= POST_FLAG_MARKED;
			break;
		case POST_LIST_TOPIC:
			filter.type = POST_LIST_TOPIC;
			break;
		case POST_LIST_AUTHOR: {
				char uname[IDLEN + 1];
				strlcpy(uname, ip->owner, sizeof(uname));
				getdata(t_lines - 1, 0, "您想查找哪位网友的文章? ", uname,
						sizeof(uname), DOECHO, false);
				user_id_t uid = get_user_id(uname);
				if (!uid)
					return MINIUPDATE;
				filter.uid = uid;
			}
			break;
		case POST_LIST_KEYWORD: {
				GBK_BUFFER(keyword, POST_LIST_KEYWORD_LEN);
				getdata(t_lines - 1, 0, "您想查找的文章标题关键字: ",
						gbk_keyword, sizeof(gbk_keyword), DOECHO, YEA);
				convert_g2u(gbk_keyword, filter.utf8_keyword);
				if (!filter.utf8_keyword[0])
					return MINIUPDATE;
			}
			break;
		default:
			break;
	}

	save_post_list_position(p);
	post_list_with_filter(&filter);
	l->reload = true;
	return FULLUPDATE;
}

static int relocate_to_filter(slide_list_t *p, post_filter_t *filter,
		bool upward)
{
	post_list_t *l = p->data;

	int found = plist_cache_relocate(&l->cache, p->cur, filter, upward);
	if (found < 0) {
		query_t *q = build_post_query(filter, !upward, 1);
		db_res_t *res = query_exec(q);
		if (res && db_res_rows(res) == 1) {
			post_info_t info;
			res_to_post_info(res, 0, filter->archive, &info);
			if (upward) {
				l->filter.min = 0;
				l->filter.max = info.id;
			} else {
				l->filter.min = info.id;
				l->filter.max = 0;
			}
			if (l->filter.type == POST_LIST_THREAD)
				l->filter.tid = info.tid;
			p->base = upward ? SLIDE_LIST_PREV : SLIDE_LIST_NEXT;
			l->relocate = info.id;
		}
		db_clear(res);
	} else {
		p->cur = found;
	}
	return PARTUPDATE;
}

static void set_filter_base(post_filter_t *filter, const post_info_t *ip,
		bool upward)
{
	if (upward) {
		filter->min = 0;
		filter->max = ip->id - 1;
	} else {
		filter->max = 0;
		filter->min = ip->id + 1;
	}
	filter->tid = filter->type == POST_LIST_THREAD ? ip->tid : 0;
}

static int tui_search_author(slide_list_t *p, bool upward)
{
	post_info_t *ip = get_post_info(p);
	if (!ip)
		return DONOTHING;

	char prompt[80];
	snprintf(prompt, sizeof(prompt), "向%s搜索作者 [%s]: ",
			upward ? "上" : "下", ip->owner);
	char ans[IDLEN + 1];
	getdata(t_lines - 1, 0, prompt, ans, sizeof(ans), DOECHO, YEA);

	user_id_t uid = ip->uid;
	if (*ans && !streq(ans, ip->owner))
		uid = get_user_id(ans);

	post_list_t *l = p->data;
	post_filter_t filter = l->filter;
	filter.uid = uid;
	set_filter_base(&filter, ip, upward);
	return relocate_to_filter(p, &filter, upward);
}

static int tui_search_title(slide_list_t *p, bool upward)
{
	post_list_t *l = p->data;

	char prompt[80];
	static GBK_BUFFER(title, POST_TITLE_CCHARS) = "";
	snprintf(prompt, sizeof(prompt), "向%s搜索标题[%s]: ",
			upward ? "上" : "下", gbk_title);

	GBK_BUFFER(ans, POST_TITLE_CCHARS);
	getdata(t_lines - 1, 0, prompt, gbk_ans, sizeof(gbk_ans), DOECHO, YEA);

	if (*gbk_ans != '\0')
		strlcpy(gbk_title, gbk_ans, sizeof(gbk_title));

	if (!*gbk_title != '\0')
		return MINIUPDATE;

	post_filter_t filter = l->filter;
	convert_g2u(gbk_title, filter.utf8_keyword);

	post_info_t *ip = get_post_info(p);
	set_filter_base(&filter, ip, upward);
	return relocate_to_filter(p, &filter, upward);
}

static int jump_to_thread_first(slide_list_t *p)
{
	post_info_t *ip = get_post_info(p);
	if (ip && ip->id != ip->tid) {
		post_list_t *l = p->data;
		l->current_tid = ip->tid;
		post_filter_t filter = l->filter;
		filter.tid = ip->tid;
		filter.min = 0;
		filter.max = ip->tid;
		return relocate_to_filter(p, &filter, true);
	}
	return DONOTHING;
}

static int jump_to_thread_prev(slide_list_t *p)
{
	post_info_t *ip = get_post_info(p);
	if (!ip || ip->id == ip->tid)
		return DONOTHING;

	post_list_t *l = p->data;
	l->current_tid = ip->tid;
	post_filter_t filter = l->filter;
	filter.tid = ip->tid;
	filter.min = 0;
	filter.max = ip->id - 1;
	return relocate_to_filter(p, &filter, true);
}

static int jump_to_thread_next(slide_list_t *p)
{
	post_info_t *ip = get_post_info(p);
	if (ip) {
		post_list_t *l = p->data;
		l->current_tid = ip->tid;
		post_filter_t filter = l->filter;
		filter.tid = ip->tid;
		filter.min = ip->id + 1;
		filter.max = 0;
		return relocate_to_filter(p, &filter, false);
	}
	return DONOTHING;
}

static int read_posts(slide_list_t *p, post_info_t *ip, bool thread, bool user);

static int jump_to_thread_first_unread(slide_list_t *p)
{
	post_list_t *l = p->data;
	post_info_t *ip = get_post_info(p);

	if (!ip || l->filter.type != POST_LIST_NORMAL)
		return DONOTHING;
	l->current_tid = ip->tid;

	post_filter_t filter = {
		.bid = l->filter.bid, .tid = ip->tid, .min = brc_first_unread(),
		.archive = l->filter.archive,
	};

	const int limit = 40;
	bool end = false;
	while (!end) {
		query_t *q = build_post_query(&filter, true, limit);
		db_res_t *res = query_exec(q);

		int rows = db_res_rows(res);
		for (int i = 0; i < rows; ++i) {
			post_id_t id = db_get_post_id(res, i, 0);
			if (brc_unread(id)) {
				post_info_t info;
				res_to_post_info(res, i, l->filter.archive, &info);
				read_posts(p, &info, true, false);
				db_clear(res);
				return FULLUPDATE;
			}
		}
		if (rows < limit)
			end = true;
		if (rows > 0)
			filter.min = db_get_post_id(res, rows - 1, 0) + 1;
		db_clear(res);
	}
	return PARTUPDATE;
}

static int jump_to_thread_last(slide_list_t *p)
{
	post_list_t *l = p->data;
	post_info_t *ip = get_post_info(p);

	if (ip) {
		l->current_tid = ip->tid;
		post_filter_t filter = {
			.bid = l->filter.bid, .tid = ip->tid, .min = ip->id + 1,
			.archive = l->filter.archive,
		};

		query_t *q = build_post_query(&filter, false, 1);
		db_res_t *res = query_exec(q);

		post_info_t info = { .id = ip->id };
		if (res && db_res_rows(res) > 0) {
			res_to_post_info(res, 0, l->filter.archive, &info);
		}
		db_clear(res);
		if (info.id == ip->id)
			return DONOTHING;
		filter.min = info.id;
		return relocate_to_filter(p, &filter, false);
	}
	return DONOTHING;
}

static int skip_post(slide_list_t *p)
{
	post_info_t *ip = get_post_info(p);
	if (ip) {
		brc_mark_as_read(ip->id);
		if (++p->cur >= t_lines - 4) {
			p->base = SLIDE_LIST_NEXT;
			p->cur = 0;
		}
	}
	return DONOTHING;
}

static int tui_delete_single_post(post_list_t *p, post_info_t *ip)
{
	if (ip && (ip->uid == session.uid || am_curr_bm())) {
		move(t_lines - 1, 0);
		if (askyn("确定删除", NA, NA)) {
			post_filter_t f = {
				.bid = p->filter.bid, .min = ip->id, .max = ip->id,
			};
			if (delete_posts(&f, true, false, true)) {
				p->reload = true;
				return PARTUPDATE;
			}
		} else {
			return MINIUPDATE;
		}
	}
	return DONOTHING;
}

static int tui_undelete_single_post(post_list_t *p, post_info_t *ip)
{
	if (ip && is_deleted(p->filter.type)) {
		post_filter_t f = {
			.type = p->filter.type,
			.bid = p->filter.bid, .min = ip->id, .max = ip->id,
		};
		if (undelete_posts(&f)) {
			p->reload = true;
			return PARTUPDATE;
		}
	}
	return DONOTHING;
}

static int show_post_info(const post_info_t *ip)
{
	if (!ip)
		return DONOTHING;

	clear();
	move(0, 0);
	prints("%s的详细信息:\n\n", "版面文章");

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	convert_u2g(ip->utf8_title, gbk_title);
	prints("标题: %s\n", gbk_title);
	prints("作者: %s\n", ip->owner);
	prints("时间: %s\n", getdatestring(ip->stamp, DATE_ZH));
	//	prints("大    小:     %d 字节\n", filestat.st_size);

	char buf[PID_BUF_LEN];
	prints("id:   %s (%"PRIdPID")\n",
			pid_to_base32(ip->id, buf, sizeof(buf)), ip->id);
	prints("tid:  %s (%"PRIdPID")\n",
			pid_to_base32(ip->tid, buf, sizeof(buf)), ip->tid);
	prints("reid: %s (%"PRIdPID")\n",
			pid_to_base32(ip->reid, buf, sizeof(buf)), ip->reid);

	char link[STRLEN];
	snprintf(link, sizeof(link),
			"http://%s/bbs/con?new=1&bid=%d&f=%"PRIdPID"%s\n",
			BBSHOST, currbp->id, ip->id,
			(ip->flag & POST_FLAG_STICKY) ? "&s=1" : "");
	prints("\n%s", link);

	pressanykey();
	return FULLUPDATE;
}

static bool dump_content(const post_info_t *ip, char *file, size_t size)
{
	post_filter_t filter = {
		.min = ip->id, .max = ip->id, .bid = ip->bid,
		.type = post_list_type(ip), .archive = ip->archive,
	};
	db_res_t *res = query_post_by_pid(&filter, "content");
	if (!res || db_res_rows(res) < 1)
		return false;

	int ret = dump_content_to_gbk_file(db_get_value(res, 0, 0),
			db_get_length(res, 0, 0), file, size);

	db_clear(res);
	return ret == 0;
}

extern int tui_cross_post_legacy(const char *file, const char *title);

static int tui_cross_post(const post_info_t *ip)
{
	char file[HOMELEN];
	if (!ip || !dump_content(ip, file, sizeof(file)))
		return DONOTHING;

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	convert_u2g(ip->utf8_title, gbk_title);

	tui_cross_post_legacy(file, gbk_title);

	unlink(file);
	return FULLUPDATE;
}

static int forward_post(const post_info_t *ip, bool uuencode)
{
	char file[HOMELEN];
	if (!ip || !dump_content(ip, file, sizeof(file)))
		return DONOTHING;

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	convert_u2g(ip->utf8_title, gbk_title);

	tui_forward(file, gbk_title, uuencode);

	unlink(file);
	return FULLUPDATE;
}

static int reply_with_mail(const post_info_t *ip)
{
	char file[HOMELEN];
	if (!ip || dump_content(ip, file, sizeof(file)))
		return DONOTHING;

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	convert_u2g(ip->utf8_title, gbk_title);

	post_reply(ip->owner, gbk_title, file);

	unlink(file);
	return FULLUPDATE;
}

static int tui_edit_post_title(post_info_t *ip)
{
	if (!ip || (ip->uid != session.uid && !am_curr_bm()))
		return DONOTHING;

	GBK_UTF8_BUFFER(title, POST_TITLE_CCHARS);

	ansi_filter(utf8_title, ip->utf8_title);
	convert_u2g(utf8_title, gbk_title);

	getdata(t_lines - 1, 0, "新文章标题: ", gbk_title, sizeof(gbk_title),
			DOECHO, NA);

	check_title(gbk_title, sizeof(gbk_title));
	convert_g2u(gbk_title, utf8_title);

	if (!*utf8_title || streq(utf8_title, ip->utf8_title))
		return MINIUPDATE;

	if (alter_title(ip, utf8_title)) {
		strlcpy(ip->utf8_title, utf8_title, sizeof(ip->utf8_title));
		return PARTUPDATE;
	}
	return MINIUPDATE;
}

static int tui_edit_post_content(post_info_t *ip)
{
	if (!ip || (ip->uid != session.uid && !am_curr_bm()))
		return DONOTHING;

	char file[HOMELEN];
	if (!dump_content(ip, file, sizeof(file)))
		return DONOTHING;

	int status = session.status;
	set_user_status(ST_EDIT);

	clear();
	if (vedit(file, NA, NA, NULL) != -1) {
		char *content = convert_file_to_utf8_content(file);
		if (content) {
			if (alter_content(ip, content)) {
				char buf[STRLEN];
				snprintf(buf, sizeof(buf), "edited post #%"PRIdPID, ip->id);
				report(buf, currentuser.userid);
			}
			free(content);
		}
	}

	unlink(file);
	set_user_status(status);
	return FULLUPDATE;
}

extern int show_board_notes(const char *bname, int command);
extern int noreply;

static int tui_new_post(int bid, post_info_t *ip)
{
	time_t now = time(NULL);
	if (now - get_last_post_time() < 3) {
		move(t_lines - 1, 0);
		clrtoeol();
		prints("您太辛苦了，先喝杯咖啡歇会儿，3 秒钟后再发表文章。\n");
		pressreturn();
		return MINIUPDATE;
	}

	board_t board;
	if (!get_board_by_bid(bid, &board) ||
			!has_post_perm(&currentuser, &board)) {
		move(t_lines - 1, 0);
		clrtoeol();
		prints("此讨论区是唯读的, 或是您尚无权限在此发表文章。");
		pressreturn();
		return FULLUPDATE;
	}

	int status = session.status;
	set_user_status(ST_POSTING);

	clear();
	show_board_notes(board.name, 1);

	struct postheader header = { .mail_owner = false, };
	GBK_BUFFER(title, POST_TITLE_CCHARS);
	if (ip) {
		header.reply = true;
		if (strncaseeq(ip->utf8_title, "Re: ", 4)) {
			convert_u2g(ip->utf8_title, header.title);
		} else {
			convert_u2g(ip->utf8_title, gbk_title);
			snprintf(header.title, sizeof(header.title), "Re: %s", gbk_title);
		}
	}

	strlcpy(header.ds, board.name, sizeof(header.ds));
	header.postboard = true;
	header.prefix[0] = '\0';

	if (post_header(&header) == YEA) {
		if (!header.reply && header.prefix[0]) {
#ifdef FDQUAN
			if (board.flag & BOARD_PREFIX_FLAG) {
				snprintf(gbk_title, sizeof(gbk_title),
						"\033[1;33m[%s]\033[m%s", header.prefix, header.title);
			} else {
				snprintf(gbk_title, sizeof(gbk_title),
						"\033[1m[%s]\033[m%s", header.prefix, header.title);
			}
#else
			snprintf(gbk_title, sizeof(gbk_title),
					"[%s]%s", header.prefix, header.title);
#endif
		} else {
			ansi_filter(gbk_title, header.title);
		}
	} else {
		set_user_status(status);
		return FULLUPDATE;
	}

	now = time(NULL);
	set_last_post_time(now);

	in_mail = NA;

	char file[HOMELEN];
	snprintf(file, sizeof(file), "tmp/editbuf.%d", getpid());
	if (ip) {
		char orig[HOMELEN];
		dump_content(ip, orig, sizeof(orig));
		do_quote(orig, file, header.include_mode, header.anonymous);
		unlink(orig);
	} else {
		do_quote("", file, header.include_mode, header.anonymous);
	}

	if (vedit(file, true, true, &header) == -1) {
		unlink(file);
		clear();
		set_user_status(status);
		return FULLUPDATE;
	}

	valid_title(header.title);

	if (header.mail_owner && header.reply) {
		if (header.anonymous) {
			prints("对不起，您不能在匿名版使用寄信给原作者功能。");
		} else {
			if (!is_blocked(ip->owner)
					&& !mail_file(file, ip->owner, gbk_title)) {
				prints("信件已成功地寄给原作者 %s", ip->owner);
			}
			else {
				prints("信件邮寄失败，%s 无法收信。", ip->owner);
			}
		}
		pressanykey();
	}

	post_request_t req = {
		.autopost = false,
		.crosspost = false,
		.uname = currentuser.userid,
		.nick = currentuser.username,
		.user = &currentuser,
		.board = &board,
		.title = gbk_title,
		.content = NULL,
		.gbk_file = file,
		.sig = 0,
		.ip = NULL,
		.reid = ip ? ip->id : 0,
		.tid = ip ? ip->tid : 0,
		.locked = header.locked,
		.marked = false,
		.anony = header.anonymous,
		.cp = NULL,
	};

	post_id_t pid = publish_post(&req);
	unlink(file);
	if (pid) {
		brc_mark_as_read(pid);

		char buf[STRLEN];
		snprintf(buf, sizeof(buf), "posted '%s' on %s",
				gbk_title, board.name);
		report(buf, currentuser.userid);
	}

	if (!is_junk_board(&board) && !header.anonymous) {
		set_safe_record();
		currentuser.numposts++;
		substitut_record(PASSFILE, &currentuser, sizeof (currentuser),
				usernum);
	}
	bm_log(currentuser.userid, currboard, BMLOG_POST, 1);
	set_user_status(status);
	return FULLUPDATE;
}

static int tui_save_post(const post_info_t *ip)
{
	if (!ip || !am_curr_bm())
		return DONOTHING;

	char file[HOMELEN];
	if (!dump_content(ip, file, sizeof(file)))
		return DONOTHING;

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	convert_u2g(ip->utf8_title, gbk_title);

	a_Save(gbk_title, file, false, true);

	return MINIUPDATE;
}

static int tui_import_post(const post_info_t *ip)
{
	if (!ip || !HAS_PERM(PERM_BOARDS))
		return DONOTHING;

	if (DEFINE(DEF_MULTANNPATH)
			&& set_ann_path(NULL, NULL, ANNPATH_GETMODE) == 0)
		return FULLUPDATE;

	char file[HOMELEN];
	if (dump_content(ip, file, sizeof(file))) {
		GBK_BUFFER(title, POST_TITLE_CCHARS);
		convert_u2g(ip->utf8_title, gbk_title);

		a_Import(gbk_title, file, NA);
	}

	if (DEFINE(DEF_MULTANNPATH))
		return FULLUPDATE;
	return DONOTHING;
}

static int tui_mark_range(slide_list_t *p, post_id_t *min, post_id_t *max)
{
	*min = *max = 0;

	if (!am_curr_bm())
		return DONOTHING;

	post_info_t *ip = get_post_info(p);
	if (ip->flag & POST_FLAG_STICKY)
		return DONOTHING;

	post_list_t *l = p->data;
	if (!l->mark_pid) {
		l->mark_pid = ip->id;
		return PARTUPDATE;
	}

	if (ip->id >= l->mark_pid) {
		*min = l->mark_pid;
		*max = ip->id;
	} else {
		*min = ip->id;
		*max = l->mark_pid;
	}
	l->mark_pid = 0;
	return DONOTHING;
}

static int tui_delete_posts_in_range(slide_list_t *p)
{
	post_list_t *l = p->data;
	if (l->filter.type != POST_LIST_NORMAL)
		return DONOTHING;

	post_id_t min, max;
	int ret = tui_mark_range(p, &min, &max);
	if (!max)
		return ret;

	move(t_lines - 1, 0);
	clrtoeol();
	if (askyn("确定删除", NA, NA)) {
		post_filter_t filter = {
			.bid = l->filter.bid, .min = min, .max = max
		};
		if (delete_posts(&filter, false, !HAS_PERM(PERM_OBOARDS), false)) {
			bm_log(currentuser.userid, currboard, BMLOG_DELETE, 1);
			l->reload = true;
		}
		return PARTUPDATE;
	}
	move(t_lines - 1, 50);
	clrtoeol();
	prints("放弃删除...");
	egetch();
	return MINIUPDATE;
}

static bool count_posts_in_range(post_id_t min, post_id_t max, bool asc,
		int sort, int least, char *file, size_t size)
{
	query_t *q = query_new(0);
	query_select(q, "uname, COUNT(*) AS a, SUM(marked::integer) AS m"
			", SUM(digest::integer) AS g, SUM(water::integer) AS w, "
			"SUM((NOT marked AND NOT digest AND NOT water)::integer) AS n");
	query_from(q, "posts_recent");
	query_where(q, "id >= %"DBIdPID, min);
	query_and(q, "id <= %"DBIdPID, max);
	query_groupby(q, "uname");

	const char *field[] = { "a", "m", "g", "w", "n" };
	query_orderby(q, field[sort], asc);

	db_res_t *res = query_exec(q);

	if (res) {
		snprintf(file, sizeof(file), "tmp/count.%d", getpid());
		FILE *fp = fopen(file, "w");
		if (fp) {
			fprintf(fp, "版    面: \033[1;33m%s\033[m\n", currboard);

			int count = 0;
			for (int i = db_res_rows(res) - 1; i >=0; --i) {
				count += db_get_bigint(res, i, 1);
			}
			char min_str[PID_BUF_LEN], max_str[PID_BUF_LEN];
			fprintf(fp, "有效篇数: \033[1;33m%d\033[m 篇"
					" [\033[1;33m%s-%s\033[m]\n", count,
					pid_to_base32(min, min_str, sizeof(min_str)),
					pid_to_base32(max, max_str, sizeof(max_str)));
			
			const char *descr[] = {
				"总数", "被m的", "被g的", "被w的", "无标记"
			};
			fprintf(fp, "排序方式: \033[1;33m按%s%s\033[m\n", descr[sort],
					asc ? "升序" : "降序");
			fprintf(fp, "文章数下限: \033[1;33m%d\033[m\n\n", least);
			fprintf(fp, "\033[1;44;37m 使用者代号  │总  数│ 被M的│ 被G的"
					"│ 被w的│无标记 \033[m\n");

			for (int i = 0; i < db_res_rows(res); ++i) {
				int all = db_get_bigint(res, i, 1);
				if (all > least) {
					int m = db_get_bigint(res, i, 2);
					int g = db_get_bigint(res, i, 3);
					int w = db_get_bigint(res, i, 4);
					int n = db_get_bigint(res, i, 5);
					fprintf(fp, " %-12.12s  %6d  %6d  %6d  %6d  %6d \n",
							db_get_value(res, i, 0), all, m, g, w, n);
				}
			}
			fclose(fp);
			db_clear(res);
			return true;
		}
		db_clear(res);
	}
	return false;
}

static int tui_count_posts_in_range(slide_list_t *p)
{
	post_list_t *l = p->data;
	if (l->filter.type != POST_LIST_NORMAL)
		return DONOTHING;

	post_id_t min, max;
	int ret = tui_mark_range(p, &min, &max);
	if (!max)
		return ret;

	char num[8];
	getdata(t_lines - 1, 0, "排序方式 (0)降序 (1)升序 ? : ", num, 2, DOECHO, YEA);
	bool asc = (num[0] == '1');

	getdata(t_lines - 1, 0, "排序选项 (0)总数 (1)被m (2)被g (3)被w (4)无标记 ? : ",
			num, 2, DOECHO, YEA);
	int sort = strtol(num, NULL, 10);
	if (sort < 0 || sort > 4)
		sort = 0;

	getdata(t_lines - 1, 0, "文章数下限(默认0): ", num, 6, DOECHO, YEA);
	int least = strtol(num, NULL, 10);

	char file[HOMELEN];
	if (!count_posts_in_range(min, max, asc, sort, least, file, sizeof(file)))
		return MINIUPDATE;

	GBK_BUFFER(title, POST_TITLE_CCHARS);
	char min_str[PID_BUF_LEN], max_str[PID_BUF_LEN];
	snprintf(gbk_title, sizeof(gbk_title), "[%s]统计文章数(%s-%s)",
			currboard, pid_to_base32(min, min_str, sizeof(min_str)),
			pid_to_base32(max, max_str, sizeof(max_str)));
	mail_file(file, currentuser.userid, gbk_title);
	unlink(file);

	return MINIUPDATE;
}

#if 0
		struct stat filestat;
		int i, len;
		char tmp[1024];

		sprintf(genbuf, "upload/%s/%s", currboard, fileinfo->filename);
		if (stat(genbuf, &filestat) < 0) {
			clear();
			move(10, 30);
			prints("对不起，%s 不存在！\n", genbuf);
			pressanykey();
			clear();
			return FULLUPDATE;
		}

		clear();
		prints("文件详细信息\n\n");
		prints("版    名:     %s\n", currboard);
		prints("序    号:     第 %d 篇\n", ent);
		prints("文 件 名:     %s\n", fileinfo->filename);
		prints("上 传 者:     %s\n", fileinfo->owner);
		prints("上传日期:     %s\n", getdatestring(fileinfo->timeDeleted, DATE_ZH));
		prints("文件大小:     %d 字节\n", filestat.st_size);
		prints("文件说明:     %s\n", fileinfo->title);
		prints("URL 地址:\n");
		sprintf(tmp, "http://%s/upload/%s/%s", BBSHOST, currboard,
				fileinfo->filename);
		strtourl(genbuf, tmp);
		len = strlen(genbuf);
		clrtoeol();
		for (i = 0; i < len; i += 78) {
			strlcpy(tmp, genbuf + i, 78);
			tmp[78] = '\n';
			tmp[79] = '\0';
			outs(tmp);
		}
		if (!(ch == KEY_UP || ch == KEY_PGUP))
			ch = egetch();
		switch (ch) {
			case 'N':
			case 'Q':
			case 'n':
			case 'q':
			case KEY_LEFT:
				break;
			case ' ':
			case 'j':
			case KEY_RIGHT:
				if (DEFINE(DEF_THESIS)) {
					sread(0, 0, fileinfo);
					break;
				} else
					return READ_NEXT;
			case KEY_DOWN:
			case KEY_PGDN:
				return READ_NEXT;
			case KEY_UP:
			case KEY_PGUP:
				return READ_PREV;
			default:
				break;
		}
		return FULLUPDATE;
#endif

enum {
	POST_LIST_THREAD_CACHE_SIZE = 15,
};

typedef struct {
	post_info_full_t posts[POST_LIST_THREAD_CACHE_SIZE];
	int size;
	bool inited;
} thread_post_cache_t;

static int load_full_posts(const post_filter_t *fp, post_info_full_t *ip,
		bool upward, int limit)
{
	query_t *q = query_new(0);
	query_select(q, POST_LIST_FIELDS_FULL);
	query_from(q, post_table_name(fp));
	build_post_filter(q, fp, NULL);
	query_orderby(q, post_table_index(fp), !upward);
	query_limit(q, limit);

	db_res_t *res = query_exec(q);
	int rows = db_res_rows(res);
	if (rows > 0) {
		for (int i = 0; i < rows; ++i) {
			res_to_post_info_full(res, upward ? rows - 1 - i : i,
					fp->archive, ip + i);
		}
	} else {
		db_clear(res);
	}
	return rows;
}

static void thread_post_cache_free(thread_post_cache_t *cache)
{
	if (cache->inited && cache->size > 0) {
		free_post_info_full(cache->posts);
		cache->size = 0;
	}
}

static post_info_full_t *thread_post_cache_load(thread_post_cache_t *cache,
		int archive, post_id_t tid, post_id_t id, bool upward)
{
	post_filter_t filter = { .archive = archive, .tid = tid, .min = id };
	if (id <= tid && upward) {
		thread_post_cache_free(cache);
	} else {
		cache->size = load_full_posts(&filter, cache->posts, upward,
				ARRAY_SIZE(cache->posts));
	}
	cache->inited = true;
	if (cache->size)
		return cache->posts + (upward ? cache->size - 1 : 0);
	return NULL;
}

static post_info_full_t *thread_post_cache_lookup(thread_post_cache_t *cache,
		int archive, post_info_full_t *ip, bool upward)
{
	post_info_full_t *next = upward ? ip - 1 : ip + 1;
	if (next >= cache->posts && next < cache->posts + cache->size)
		return next;

	if (!upward && cache->size < ARRAY_SIZE(cache->posts))
		return NULL;

	post_id_t id = ip->p.id, tid = ip->p.tid;
	thread_post_cache_free(cache);
	return thread_post_cache_load(cache, archive, tid,
			upward ? id - 1 : id + 1, upward);
}

static void back_to_post_list(slide_list_t *p, post_id_t id, post_id_t tid)
{
	post_list_t *l = p->data;
	post_filter_t filter = l->filter;
	filter.min = id;
	filter.max = 0;
	if (filter.type == POST_LIST_THREAD)
		filter.tid = tid;

	int found = plist_cache_relocate(&l->cache, -1, &filter, false);
	if (found >= 0) {
		p->cur = found;
	} else {
		relocate_to_filter(p, &filter, false);
	}
}

static int read_posts(slide_list_t *p, post_info_t *ip, bool thread, bool user)
{
	post_list_t *l = p->data;
	post_info_full_t info = { .p = *ip };
	post_info_full_t *fip = &info;
	post_filter_t filter = l->filter;
	thread_post_cache_t cache = { .inited = false };

	char file[HOMELEN];
	if (!ip || !dump_content(ip, file, sizeof(file)))
		return DONOTHING;

	bool end = false, upward = false;
	post_id_t thread_entry = 0, last_id = 0;
	while (!end) {
		brc_mark_as_read(fip->p.id);
		last_id = fip->p.id;
		l->current_tid = fip->p.tid;

		int ch = ansimore(file, false);

		move(t_lines - 1, 0);
		clrtoeol();
		prints("\033[0;1;44;31m[阅读文章]  \033[33m回信 R │ 结束 Q,← │上一封 ↑"
				"│下一封 <Space>,↓│主题阅读 ^s或p \033[m");
		refresh();

		if (!(ch == KEY_UP || ch == KEY_PGUP))
			ch = egetch();
		switch (ch) {
			case 'N': case 'Q': case 'n': case 'q': case KEY_LEFT:
				end = true;
				break;
			case 'Y': case 'R': case 'y': case 'r':
				// TODO
				tui_new_post(fip->p.bid, &fip->p);
				break;
			case '\n': case 'j': case KEY_DOWN: case KEY_PGDN:
				upward = false;
				thread_entry = 0;
				break;
			case ' ': case 'p': case KEY_RIGHT: case Ctrl('S'):
				upward = false;
				if (!filter.uid && !filter.tid) {
					thread_entry = fip->p.id;
					filter.tid = fip->p.tid;
				}
				break;
			case KEY_UP: case KEY_PGUP: case 'u': case 'U':
				upward = true;
				break;
			case Ctrl('A'):
				t_query(fip->p.owner);
				break;
			case Ctrl('R'):
				reply_with_mail(&fip->p);
				break;
			case 'g':
				toggle_post_flag(&fip->p, POST_FLAG_DIGEST, "digest");
				break;
			case '*':
				show_post_info(&fip->p);
				break;
			case Ctrl('U'):
				if (!filter.uid && !filter.tid)
					filter.uid = fip->p.uid;
				break;
			default:
				break;
		}

		unlink(file);

		if (!end) {
			if (!cache.inited) {
				if (filter.tid) {
					fip = thread_post_cache_load(&cache, filter.archive,
							filter.tid, fip->p.id, upward);
				}
			}

			if (cache.inited) {
				fip = thread_post_cache_lookup(&cache, filter.archive, fip,
						upward);
				if (fip) {
					dump_content_to_gbk_file(fip->content, fip->length,
							file, sizeof(file));
				} else {
					break;
				}
			} else {
				if (upward) {
					filter.min = 0;
					filter.max = fip->p.id - 1;
				} else {
					filter.min = fip->p.id + 1;
					filter.max = 0;
				}
				if (load_full_posts(&filter, fip, upward, 1)) {
					dump_content_to_gbk_file(fip->content, fip->length,
							file, sizeof(file));
				} else {
					break;
				}
			}
		}
	}
	thread_post_cache_free(&cache);
	free_post_info_full(&info);

	back_to_post_list(p, thread_entry ? thread_entry : last_id,
			l->current_tid);
	return FULLUPDATE;
}

static void construct_prompt(char *s, size_t size, const char **options,
		size_t len)
{
	char *p = s;
	strappend(&p, &size, "区段:");
	for (int i = 0; i < len; ++i) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d)%s", i + 1, options[i]);
		strappend(&p, &size, buf);
	}
	strappend(&p, &size, "[0]:");
}

#if 0
		clear();
		prints("\n\n您将进行区段转载。转载范围是：[%d -- %d]\n", num1, num2);
		prints("当前版面是：[ %s ] \n", currboard);
		board_complete(6, "请输入要转贴的讨论区名称: ", bname, sizeof(bname),
				AC_LIST_BOARDS_ONLY);
		if (!*bname)
			return FULLUPDATE;

		if (!strcmp(bname, currboard)&&session.status != ST_RMAIL) {
			prints("\n\n对不起，本文就在您要转载的版面上，所以无需转载。\n");
			pressreturn();
			clear();
			return FULLUPDATE;
		}
		if (askyn("确定要转载吗", NA, NA)==NA)
			return FULLUPDATE;
			case 4:
				break;
			case 5:
				break;
			default:
				break;
		}

	return DIRCHANGED;
}
#endif

extern int import_file(const char *title, const char *file, const char *path);

static int import_posts(post_filter_t *filter, const char *path)
{
	query_t *q = query_new(0);
	query_update(q, post_table_name(filter));
	query_set(q, "imported = TRUE");
	build_post_filter(q, filter, NULL);
	query_returning(q, "title, content");

	db_res_t *res = query_exec(q);
	if (res) {
		int rows = db_res_rows(res);
		for (int i = 0; i < rows; ++i) {
			GBK_BUFFER(title, POST_TITLE_CCHARS);
			convert_u2g(db_get_value(res, i, 0), gbk_title);

			char file[HOMELEN];
			dump_content_to_gbk_file(db_get_value(res, i, 1),
					db_get_length(res, i, 1), file, sizeof(file));

			import_file(gbk_title, file, path);
		}
		db_clear(res);
		return rows;
	}
	return 0;
}

static int tui_import_posts(post_filter_t *filter)
{
	if (DEFINE(DEF_MULTANNPATH) && !!set_ann_path(NULL, NULL, ANNPATH_GETMODE))
		return FULLUPDATE;

	char annpath[256];
	sethomefile(annpath, currentuser.userid, ".announcepath");

	FILE *fp = fopen(annpath, "r");
	if (!fp) {
		presskeyfor("对不起, 您没有设定丝路. 请先用 f 设定丝路.",
				t_lines - 1);
		return MINIUPDATE;
	}

	fscanf(fp, "%s", annpath);
	fclose(fp);

	if (!dashd(annpath)) {
		presskeyfor("您设定的丝路已丢失, 请重新用 f 设定.", t_lines - 1);
		return MINIUPDATE;
	}

	import_posts(filter, annpath);
	return PARTUPDATE;
}

static int operate_posts_in_range(int choice, post_list_t *l, post_id_t min,
		post_id_t max)
{
	bool deleted = is_deleted(l->filter.type);
	post_filter_t filter = { .bid = l->filter.bid, .min = min, .max = max };
	int ret = PARTUPDATE;
	switch (choice) {
		case 0:
			set_post_flag(&filter, "marked", true, true);
			break;
		case 1:
			set_post_flag(&filter, "digest", true, true);
			break;
		case 2:
			set_post_flag(&filter, "locked", true, true);
			break;
		case 3:
			delete_posts(&filter, true, !HAS_PERM(PERM_OBOARDS), false);
			break;
		case 4:
			ret = tui_import_posts(&filter);
			break;
		case 5:
			set_post_flag(&filter, "water", true, true);
			break;
		case 6:
			break;
		default:
			if (deleted) {
				filter.type = l->filter.type;
				undelete_posts(&filter);
			} else {
				filter.flag |= POST_FLAG_WATER;
				delete_posts(&filter, true, !HAS_PERM(PERM_OBOARDS), false);
			}
			break;
	}

	switch (choice) {
		case 3:
			bm_log(currentuser.userid, currboard, BMLOG_RANGEDEL, 1);
			break;
		case 4:
			bm_log(currentuser.userid, currboard, BMLOG_RANGEANN, 1);
			break;
		default:
			bm_log(currentuser.userid, currboard, BMLOG_RANGEOTHER, 1);
			break;
	}
	l->reload = true;
	return ret;
}

static int tui_operate_posts_in_range(slide_list_t *p)
{
	post_list_t *l = p->data;

	post_id_t min, max;
	int ret = tui_mark_range(p, &min, &max);
	if (!max)
		return ret;

	const char *option8 = is_deleted(l->filter.type) ? "恢复" : "删水文";
	const char *options[] = {
		"保留",  "文摘", "不可RE", "删除",
		"精华区", "水文", "转载", option8,
	};

	char prompt[120], ans[8];
	construct_prompt(prompt, sizeof(prompt), options, ARRAY_SIZE(options));
	getdata(t_lines - 1, 0, prompt, ans, sizeof(ans), DOECHO, YEA);

	int choice = *ans - '1';
	if (choice < 0 || choice >= ARRAY_SIZE(options))
		return MINIUPDATE;

	snprintf(prompt, sizeof(prompt), "区段[%s]操作，确定吗", options[choice]);
	if (!askyn(prompt, NA, YEA))
		return MINIUPDATE;

	return operate_posts_in_range(choice, l, min, max);
}

static void operate_posts_in_batch(post_filter_t *fp, post_info_t *ip, int mode,
		int choice, bool first, post_id_t pid, bool quote, bool junk,
		const char *annpath, const char *utf8_keyword, const char *gbk_keyword)
{
	post_filter_t filter = { .type = fp->type, .bid = fp->bid };
	switch (mode) {
		case 1:
			filter.uid = ip->uid;
			break;
		case 2:
			strlcpy(filter.utf8_keyword, utf8_keyword,
					sizeof(filter.utf8_keyword));
			break;
		default:
			filter.tid = ip->tid;
			break;
	}
	if (!first)
		filter.min = ip->id;

	switch (choice) {
		case 0:
			delete_posts(&filter, junk, !HAS_PERM(PERM_OBOARDS), false);
			break;
		case 1:
			set_post_flag(&filter, "marked", false, true);
			break;
		case 2:
			set_post_flag(&filter, "digest", false, true);
			break;
		case 3:
			import_posts(&filter, annpath);
			break;
		case 4:
			set_post_flag(&filter, "water", false, true);
			break;
		case 5:
			set_post_flag(&filter, "locked", false, true);
			break;
		case 6:
			// pack thread
			break;
		default:
			if (is_deleted(fp->type)) {
				filter.type = fp->type;
				undelete_posts(&filter);
			} else {
				// merge thread
			}
			break;
	}

	switch (choice) {
		case 0:
			bm_log(currentuser.userid, currboard, BMLOG_RANGEDEL, 1);
			break;
		case 3:
			bm_log(currentuser.userid, currboard, BMLOG_RANGEANN, 1);
			break;
		case 6:
			bm_log(currentuser.userid, currboard, BMLOG_COMBINE, 1);
			break;
		default:
			bm_log(currentuser.userid, currboard, BMLOG_RANGEOTHER, 1);
			break;
	}
}

static int tui_operate_posts_in_batch(slide_list_t *p)
{
	if (!am_curr_bm() || !get_post_info(p))
		return DONOTHING;

	post_list_t *l = p->data;
	bool deleted = is_deleted(l->filter.type);

	const char *batch_modes[] = { "相同主题", "相同作者", "相关主题" };
	const char *option8 = deleted ? "恢复" : "合并";
	const char *options[] = {
		"删除", "保留", "文摘", "精华区", "水文", "不可RE", "合集", option8
	};

	char ans[16];
	move(t_lines - 1, 0);
	clrtoeol();
	ans[0] = '\0';
	getdata(t_lines - 1, 0, "执行: 1) 相同主题  2) 相同作者 3) 相关主题"
			" 0) 取消 [0]: ", ans, sizeof(ans), DOECHO, YEA);
	int mode = strtol(ans, NULL, 10) - 1;
	if (mode < 0 || mode >= ARRAY_SIZE(batch_modes))
		return MINIUPDATE;

	char prompt[120];
	construct_prompt(prompt, sizeof(prompt), options, ARRAY_SIZE(options));
	getdata(t_lines - 1, 0, prompt, ans, sizeof(ans), DOECHO, YEA);
	int choice = strtol(ans, NULL, 10) - 1;
	if (choice < 0 || choice >= ARRAY_SIZE(options))
		return MINIUPDATE;

	char buf[STRLEN];
	move(t_lines - 1, 0);
	snprintf(buf, sizeof(buf), "确定要执行%s[%s]吗", batch_modes[mode],
			options[choice]);
	if (!askyn(buf, NA, NA))
		return MINIUPDATE;

	post_id_t pid = 0;
	bool quote = true;
	if (choice == 6) {
		move(t_lines-1, 0);
		quote = askyn("制作的合集需要引言吗？", YEA, YEA);
	} else if (choice == 7) {
		if (!deleted) {
			getdata(t_lines - 1, 0, "本主题加至版面第几篇后？", ans,
					sizeof(ans), DOECHO, YEA);
			pid = base32_to_pid(ans);
		}
	}

	GBK_UTF8_BUFFER(keyword, POST_TITLE_CCHARS);
	if (mode == 2) {
		getdata(t_lines - 1, 0, "请输入主题关键字: ", gbk_keyword,
				sizeof(gbk_keyword), DOECHO, YEA);
		if (gbk_keyword[0] == '\0')
			return MINIUPDATE;
		convert_g2u(gbk_keyword, utf8_keyword);
	}

	bool junk = true;
	if (choice == 0) {
		junk = askyn("是否小d", YEA, YEA);
	}

	bool first = false;
	move(t_lines - 1, 0);
	snprintf(buf, sizeof(buf), "是否从%s第一篇开始%s (Y)第一篇 (N)目前这一篇",
			(mode == 1) ? "该作者" : "此主题", options[choice]);
	first = askyn(buf, YEA, NA);

	char annpath[512];
	if (choice == 3) {
		if (DEFINE(DEF_MULTANNPATH) &&
				!set_ann_path(NULL, NULL, ANNPATH_GETMODE))
			return FULLUPDATE;

		sethomefile(annpath, currentuser.userid, ".announcepath");
		FILE *fp = fopen(annpath, "r");
		if (!fp) {
			presskeyfor("对不起, 您没有设定丝路. 请先用 f 设定丝路.", t_lines - 1);
			return MINIUPDATE;
		}
		fscanf(fp, "%s", annpath);
		fclose(fp);
		if (!dashd(annpath)) {
			presskeyfor("您设定的丝路已丢失, 请重新用 f 设定.",t_lines - 1);
			return MINIUPDATE;
		}
	}

	operate_posts_in_batch(&l->filter, get_post_info(p), mode, choice, first,
			pid, quote, junk, annpath, utf8_keyword, gbk_keyword);
#if 0
	if (BMch == 7) {
		if (strneq(keyword, "Re: ", 4) || strneq(keyword, "RE: ", 4))
			snprintf(buf, sizeof(buf), "[合集]%s", keyword + 4);
		else
			snprintf(buf, sizeof(buf), "[合集]%s", keyword);

		ansi_filter(keyword, buf);

		sprintf(buf, "tmp/%s.combine", currentuser.userid);

		Postfile(buf, currboard, keyword, 2);
		unlink(buf);
	}
#endif
	l->reload = true;
	return PARTUPDATE;
}

extern int tui_select_board(int);

static int switch_board(post_list_t *l)
{
	if (l->filter.type != POST_LIST_NORMAL || !l->filter.bid)
		return DONOTHING;

	int bid = tui_select_board(l->filter.bid);
	if (bid) {
		l->filter.bid = bid;
		l->reload = l->sreload = true;
		l->pos = get_post_list_position(&l->filter);
	}
	return FULLUPDATE;
}

static tui_list_loader_t archive_list_loader(tui_list_t *p)
{
	post_list_t *l = p->data;
	p->all = archive_list_count(l->archives);
	p->cur = p->all - 1;
	return 0;
}

static tui_list_title_t archive_list_title(tui_list_t *p)
{
	post_list_t *l = p->data;
	_post_list_title(true);
	move(2, 0);
	prints("\033[1;44m 选择一个存档 (共 %d 个)\033[K\033[m",
			archive_list_count(l->archives));
	return;
}

static tui_list_display_t archive_list_display(tui_list_t *p, int row)
{
	post_list_t *l = p->data;
	int rows = archive_list_count(l->archives);
	if (row < rows) {
		int number = archive_list_number(l->archives, row);
		int year = number / 10000;
		int month = (number % 10000) / 100;
		prints("   %d年%d月\n", year, month);
	}
	return 0;
}

static tui_list_handler_t archive_list_handler(tui_list_t *p, int ch)
{
	post_list_t *l = p->data;
	switch (ch) {
		case '\r': case '\n': case KEY_RIGHT:
			l->filter.archive = archive_list_number(l->archives, p->cur);
			l->abase = SLIDE_LIST_TOPDOWN;
			l->reload = true;
			return -1;
		default:
			return 0;
	}
}

static int archive_list(post_list_t *l)
{
	if (!l->archives)
		l->archives = archive_list_load(l->filter.bid);
	if (!l->archives)
		return DONOTHING;

	tui_list_t t = {
		.data = l,
		.loader = archive_list_loader,
		.title = archive_list_title,
		.display = archive_list_display,
		.handler = archive_list_handler,
		.query = NULL,
	};
	tui_list(&t);
	return FULLUPDATE;
}

static int find_successor(post_list_t *l, bool upward)
{
	int current = l->filter.archive, count = archive_list_count(l->archives);
	if (!current) {
		return count - 1;
	} else {
		for (int i = count - 1; i >= 0; --i) {
			if (archive_list_number(l->archives, i) == current) {
				int j = i + (upward ? -1 : 1);
				if (j >= 0 && j < count)
					return j;
				return -1;
			}
		}
		return -1;
	}
}

static int switch_archive(post_list_t *l, bool upward)
{
	if (!l->filter.bid || l->filter.type != POST_LIST_NORMAL)
		return READ_AGAIN;

	if (!l->archives)
		l->archives = archive_list_load(l->filter.bid);
	if (!l->archives)
		return READ_AGAIN;

	int successor = find_successor(l, upward);

	char buf[128], defa[STRLEN] = "", default_choice = 'C';
	if (successor >= 0) {
		snprintf(defa, sizeof(defa), "(%c)查看%s存档 ", upward ? 'P' : 'N',
				upward ? (l->filter.archive ? "上一" : "最新" ) : "下一");
		default_choice = upward ? 'P' : 'N';
	}
	snprintf(buf, sizeof(buf), "已到达%s一篇文章。%s(A)所有存档列表 "
			"(C)取消？[%c]", upward ? "第" : "最后", defa, default_choice);
	char ans[2];
	getdata(t_lines - 1, 0, buf, ans, sizeof(ans), true, true);

	char c = toupper(ans[0]);
	if (!c)
		c = default_choice;
	if (c == 'P' || c == 'N') {
		if (successor >= 0) {
			l->filter.archive = archive_list_number(l->archives, successor);
			l->abase = upward ? SLIDE_LIST_BOTTOMUP : SLIDE_LIST_TOPDOWN;
			l->reload = true;
			return FULLUPDATE;
		}
		return MINIUPDATE;
	}
	if (c == 'A')
		return archive_list(l);
	return MINIUPDATE;
}

static int tui_jump(slide_list_t *p)
{
	char buf[16];
	getdata(t_lines - 1, 0, "跳转到 (P)文章 (A)存档 (C)取消？[C]",
			buf, sizeof(buf), true, true);
	char c = tolower(buf[0]);
	if (c == 'p') {
		;
	} else if (c == 'a') {
		return archive_list(p->data);
	}
	return MINIUPDATE;
}

extern int show_online(void);
extern int thesis_mode(void);
extern int deny_user(void);
extern int club_user(void);
extern int tui_send_msg(const char *);
extern int x_lockscreen(void);
extern int vote_results(const char *bname);
extern int b_vote(void);
extern int vote_maintain(const char *bname);
extern int b_notes_edit(void);
extern int b_notes_passwd(void);
extern int mainreadhelp(void);
extern int show_b_note(void);
extern int show_b_secnote(void);
extern int into_announce(void);
extern int show_hotspot(void);
extern int tui_follow_uname(const char *uname);

static slide_list_handler_t post_list_handler(slide_list_t *p, int ch)
{
	post_list_t *l = p->data;
	post_info_t *ip = get_post_info(p);

	switch (ch) {
		case Ctrl('P'):
			return tui_new_post(l->filter.bid, NULL);
		case '@':
			return show_online();
		case '.':
			return post_list_deleted(l);
		case 'J':
			return post_list_admin_deleted(l);
		case 't':
			return thesis_mode();
		case '!':
			return Q_Goodbye();
		case 'S':
			s_msg();
			return FULLUPDATE;
		case 'o':
			show_online_followings();
			return FULLUPDATE;
		case 'u':
			t_query(NULL);
			return FULLUPDATE;
		case '|':
			return x_lockscreen();
		case 'R':
			return vote_results(currboard);
		case 'v':
			return b_vote();
		case 'V':
			return vote_maintain(currboard);
		case KEY_TAB:
			return show_b_note();
		case 'z':
			return show_b_secnote();
		case 'W':
			return b_notes_edit();
		case Ctrl('W'):
			return b_notes_passwd();
		case 'h':
			return mainreadhelp();
		case Ctrl('D'):
			return deny_user();
		case Ctrl('K'):
			return club_user();
		case 'x':
			if (into_announce() != DONOTHING)
				return FULLUPDATE;
		case 's':
			return switch_board(l);
		case '%':
			return tui_jump(p);
		case 'q': case 'e': case KEY_LEFT: case EOF:
			if (p->in_query) {
				p->in_query = false;
				return FULLUPDATE;
			}
			if (!l->filter.archive)
				return -1;
			l->filter.archive = 0;
			l->reload = true;
			return FULLUPDATE;
		case Ctrl('L'):
			redoscr();
			return DONOTHING;
		case 'M':
			m_new();
			return FULLUPDATE;
		case 'H':
			return show_hotspot();
		case 'l':
			msg_more();
			return FULLUPDATE;
		default:
			if (!ip)
				return DONOTHING;
	}

	switch (ch) {
		case '\n': case '\r': case KEY_RIGHT: case 'r':
		case Ctrl('S'): case 'p':
			return read_posts(p, ip, false, false);
		case Ctrl('U'):
			return read_posts(p, ip, false, true);
		case '_':
			return toggle_post_lock(ip);
		case '#':
			return toggle_post_stickiness(ip, l);
		case 'm':
			return toggle_post_flag(ip, POST_FLAG_MARKED, "marked");
		case 'g':
			return toggle_post_flag(ip, POST_FLAG_DIGEST, "digest");
		case 'w':
			return toggle_post_flag(ip, POST_FLAG_WATER, "water");
		case 'T':
			return tui_edit_post_title(ip);
		case 'E':
			return tui_edit_post_content(ip);
		case 'i':
			return tui_save_post(ip);
		case 'I':
			return tui_import_post(ip);
		case 'D':
			return tui_delete_posts_in_range(p);
		case 'L':
			return tui_operate_posts_in_range(p);
		case 'b':
			return tui_operate_posts_in_batch(p);
		case 'C':
			return tui_count_posts_in_range(p);
		case Ctrl('G'): case Ctrl('T'): case '`':
			return tui_post_list_selected(p, ip);
		case 'a':
			return tui_search_author(p, false);
		case 'A':
			return tui_search_author(p, true);
		case '/':
			return tui_search_title(p, false);
		case '?':
			return tui_search_title(p, true);
		case '=':
			return jump_to_thread_first(p);
		case '[':
			return jump_to_thread_prev(p);
		case ']':
			return jump_to_thread_next(p);
		case 'n': case Ctrl('N'):
			return jump_to_thread_first_unread(p);
		case '\\':
			return jump_to_thread_last(p);
		case 'K':
			return skip_post(p);
		case 'c':
			brc_clear(ip->id);
			return PARTUPDATE;
		case 'f':
			brc_clear_all();
			return PARTUPDATE;
		case 'd':
			return tui_delete_single_post(l, ip);
		case 'Y':
			return tui_undelete_single_post(l, ip);
		case '*':
			return show_post_info(ip);
		case Ctrl('C'):
			return tui_cross_post(ip);
		case 'F':
			return forward_post(ip, false);
		case 'U':
			return forward_post(ip, true);
		case Ctrl('R'):
			return reply_with_mail(ip);
		case 'Z':
			tui_send_msg(ip->owner);
			return FULLUPDATE;
		case Ctrl('A'):
			return ip ? t_query(ip->owner) : DONOTHING;
		case 'P': case Ctrl('B'): case KEY_PGUP:
			if (plist_cache_is_top(&l->cache, 0))
				return switch_archive(l, true);
			return READ_AGAIN;
		case 'j': case KEY_UP:
			if (plist_cache_is_top(&l->cache, p->cur))
				return switch_archive(l, true);
			return READ_AGAIN;
		case 'N': case Ctrl('F'): case KEY_PGDN:
			if (plist_cache_is_bottom(&l->cache, -1)) {
				if (l->filter.archive) {
					return switch_archive(l, false);
				} else {
					p->cur = p->max - 1;
					return DONOTHING;
				}
			}
			return READ_AGAIN;
		case 'k': case KEY_DOWN:
			if (plist_cache_is_bottom(&l->cache, p->cur)) {
				if (l->filter.archive)
					return switch_archive(l, false);
				if ((ip->flag & POST_FLAG_STICKY) && p->cur == p->max - 1)
					return DONOTHING;
			}
			return READ_AGAIN;
		case '$': case KEY_END:
			if (ip->flag & POST_FLAG_STICKY) {
				for (int i = p->cur - 1; i >= 0; --i) {
					post_info_t *_ip = plist_cache_get(&l->cache, i);
					if (_ip && !(_ip->flag & POST_FLAG_STICKY)) {
						p->cur = i;
						break;
					}
				}
				return DONOTHING;
			}
			if (plist_cache_is_bottom(&l->cache, -1)) {
				p->cur = plist_cache_max_visible(&l->cache) - 1;
				return DONOTHING;
			}
			return READ_AGAIN;
		case 'O':
			return ip->uid ? tui_follow_uname(ip->owner) : DONOTHING;
		default:
			return READ_AGAIN;
	}
}

static int post_list_with_filter(const post_filter_t *filter)
{
	post_list_t p = {
		.relocate = 0,
		.filter = *filter,
		.reload = true,
		.pos = get_post_list_position(filter),
		.sreload = filter->type == POST_LIST_NORMAL,
		.cache = { .posts = NULL },
	};

	int status = session.status;
	set_user_status(ST_READING);

	slide_list_t s = {
		.base = SLIDE_LIST_CURRENT,
		.data = &p,
		.update = FULLUPDATE,
		.loader = post_list_loader,
		.title = post_list_title,
		.display = post_list_display,
		.handler = post_list_handler,
	};

	slide_list(&s);

	save_post_list_position(&s);

	plist_cache_free(&p.cache);
	archive_list_free(p.archives);

	set_user_status(status);
	return 0;
}

int post_list_normal(int bid)
{
	post_filter_t filter = { .type = POST_LIST_NORMAL, .bid = bid };
	return post_list_with_filter(&filter);
}
