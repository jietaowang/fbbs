#include "libweb.h"
#include "mmap.h"
#include "fbbs/web.h"

enum {
	POSTS_PER_PAGE = 20,
};

extern const struct fileheader *dir_bsearch(const struct fileheader *begin, 
		const struct fileheader *end, unsigned int fid);

// If 'action' == 'n', return at most 'count' posts
// after 'fid' in thread 'gid', otherwise, posts before 'fid'.
// Return NULL if not found, otherwise, memory should be freed by caller.
static struct fileheader *bbstcon_search(const struct boardheader *bp,
		unsigned int gid, unsigned int fid, char action, int *count)
{
	if (bp == NULL || count == NULL || *count < 1)
		return NULL;
	struct fileheader *fh = malloc(sizeof(struct fileheader) * (*count));
	if (fh == NULL)
		return NULL;

	// Open index file.
	char dir[HOMELEN];
	setbfile(dir, bp->filename, DOT_DIR);
	mmap_t m;
	m.oflag = O_RDONLY;
	if (mmap_open(dir, &m) < 0) {
		free(fh);
		return NULL;
	}
	struct fileheader *begin = m.ptr, *end;
	end = begin + (m.size / sizeof(*begin));

	// Search 'fid'.
	const struct fileheader *f = dir_bsearch(begin, end, fid);
	int c = 0;
	if (action == 'n') {  // forward
		if (f != end && f->id == fid)
			++f;	// skip current post.
		for (; f < end; ++f) {
			if (f->gid == gid) {
				fh[c++] = *f;
				if (c == *count)
					break;
			}
		}
		*count = c;
	} else { // backward
		c = *count;
		for (--f; f >= begin && f->id >= gid; --f) {
			if (f->gid == gid) {
				fh[--c] = *f;
				if (c == 0)
					break;
			}
		}
		*count -= c;
	}
	mmap_close(&m);
	return fh;
}

// TODO: brc
int bbstcon_main(web_ctx_t *ctx)
{
	int bid = strtol(get_param(ctx->r, "bid"), NULL, 10);
	struct boardheader *bp;
	if (bid <= 0) {
		bp = getbcache(get_param(ctx->r, "board"));
		bid = getbnum2(bp);
	} else {
		bp = getbcache2(bid);
	}
	if (bp == NULL || !hasreadperm(&currentuser, bp))
		return BBS_ENOBRD;
	if (bp->flag & BOARD_DIR_FLAG)
		return BBS_EINVAL;
	unsigned int gid = strtoul(get_param(ctx->r, "g"), NULL, 10);
	unsigned int fid = strtoul(get_param(ctx->r, "f"), NULL, 10);
	char action = *(get_param(ctx->r, "a"));
	if (gid == 0) {
		gid = fid;
		fid--;
		action = 'n';
	}

	int count = POSTS_PER_PAGE;
	int c = count;
	struct fileheader *fh = bbstcon_search(bp, gid, fid, action, &c);
	if (fh == NULL)
		return BBS_ENOFILE;
	struct fileheader *begin, *end;
	xml_header(NULL);
	printf("<bbstcon bid='%d' gid='%u' page='%d'>", bid, gid, count);
	print_session(ctx);
	if (action == 'n') {
		begin = fh;
		end = fh + c;
	} else {
		begin = fh + count - c;
		end = fh + count;
	}
	char file[HOMELEN];
	brc_fcgi_init(currentuser.userid, bp->filename);
	for (; begin != end; ++begin) {
		printf("<po fid='%u' owner='%s'>", begin->id, begin->owner);
		setbfile(file, bp->filename, begin->filename);
		xml_print_file(ctx->r, file);
		puts("</po>");
		brc_addlist(begin->filename);
	}
	free(fh);
	puts("</bbstcon>");
	brc_update(currentuser.userid, bp->filename);
	return 0;
}
