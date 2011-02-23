#include "libweb.h"
#include "fbbs/fileio.h"
#include "fbbs/web.h"

static int edit_user_file(web_ctx_t *ctx, const char *file, const char *desc, const char *submit)
{
	if (!loginok)
		return BBS_ELGNREQ;
	char buf[HOMELEN];
	sethomefile(buf, currentuser.userid, file);
	parse_post_data(ctx->r);
	const char *text = get_param(ctx->r, "text");
	if (*text != '\0') {
		int fd = open(buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			return BBS_EINTNL;
		fb_flock(fd, LOCK_EX);
		safer_write(fd, text, strlen(text));
		fb_flock(fd, LOCK_UN);
		close(fd);
		xml_header(NULL);
		printf("<bbseufile desc='%s'>", desc);
		print_session(ctx);
		printf("</bbseufile>");
	} else {
		xml_header(NULL);
		printf("<bbseufile desc='%s' submit='%s'><text>", desc, submit);
		xml_printfile(buf, stdout);
		printf("</text>");
		print_session(ctx);
		printf("</bbseufile>");
	}
	return 0;
}

int bbsplan_main(web_ctx_t *ctx)
{
	return edit_user_file(ctx, "plans", "�༭˵����", "plan");
}

int bbssig_main(web_ctx_t *ctx)
{
	return edit_user_file(ctx, "signatures", "�༭ǩ����", "sig");
}

