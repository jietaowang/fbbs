#ifndef _LIBWEB_H_

#define _LIBWEB_H_

#define USEFCGI

#include "../include/bbs.h"
#include "sys/ipc.h"
#include "sys/shm.h"
#include "stdarg.h"
#ifdef USEFCGI
#include <fcgi_stdio.h>  //Should be included last.
#endif
#include <crypt.h>

#define CGIPATH "/fcgi/"
#define FIRST_PAGE	"/bbsmain.html"
#define CSS_FILE 	"/css/bbs%d.css"
#define CHARSET		"gb2312"

#define TLINES 18
#define SQUID

#define LINKLEN   80

#ifdef FDQUAN
	#define MAX_LARGEMAIL_UPLIMIT   300
	#define MAX_BOARDSMAIL_UPLIMIT	120
	#define MAX_MAIL_UPLIMIT	60
#else
	#define MAX_LARGEMAIL_UPLIMIT   20000
	#define MAX_BOARDSMAIL_UPLIMIT  20000
	#define MAX_MAIL_UPLIMIT        20000
#endif

#ifdef FDQUAN
	#define SECNUM 9
#else
	#define SECNUM 12
#endif

enum HTTP_STATUS {
	HTTP_STATUS_OK = 200,
	HTTP_STATUS_BADREQUEST = 400,
	HTTP_STATUS_FORBIDDEN = 403,
	HTTP_STATUS_NOTFOUND = 404,
	HTTP_STATUS_INTERNAL_ERROR = 500,
	HTTP_STATUS_SERVICE_UNAVAILABLE = 503
};

enum {
	UPLOAD_MAX = 1 * 1024 * 1024,
	UPLOAD_OVERHEAD = 1024
};

#define HTTP_END (printf("\n</html>\n"));

#define file_size(x) f_stat(x)->st_size
#define file_time(x) f_stat(x)->st_mtime
#define file_rtime(x) f_stat(x)->st_atime
#define file_isdir(x) ((f_stat(x)->st_mode & S_IFDIR)!=0)
#define file_isfile(x) ((f_stat(x)->st_mode & S_IFREG)!=0)

void setcookie(const char *a, const char *b);
void refreshto(int second, const char *url);

#define FLOCK(x,y) flock(x,y)

extern char seccode[][6];
extern char secname[][2][20];

struct post_log {
	char	author[IDLEN+1]; // TODO: is this too short for "id."?
	char	board[18];
	char	title[66];
	time_t	date;
	int	number;
};

extern int loginok;

extern struct userec currentuser;
extern struct user_info *u_info;

extern char fromhost[];

char *anno_path_of(char *board);

int mailnum_under_limit(char *userid);
int mailsize_under_limit(char *userid);

char * entity_char(char *s);
int file_has_word(char *file, char *word);
struct stat *f_stat(char *file);
int del_record(char *file, int size, int num);

char *getsenv(const char *s);
void http_quit(void);
void http_fatal(const char *prompt);
void http_fatal2(enum HTTP_STATUS status, const char *prompt);
int hsprintf(char *s, char *fmt, ...);
int hprintf(char *fmt, ...);
int hhprintf(char *fmt, ...);
void xml_fputs(const char *s, FILE *stream);
void xml_fputs2(const char *s, size_t size, FILE *stream);

extern char parm_name[][80];
extern char *parm_val[];
extern int parm_num;

int user_init(struct userec *x, struct user_info **y);
void xml_header(const char *xslfile);
void http_header(void);

int post_mail(char *userid, char *title, char *file, char *id, char *nickname, char *ip, int sig);
int post_article(const struct userec *user, const struct boardheader *bp, 
		const char *title, const char *content, const char *ip, 
		const struct fileheader *o_fp);

void check_title(char *title);
char* anno_path_of(char *board);

int count_mails(char *id, int *total, int *unread);

int send_msg(char *myuserid, int mypid, char *touserid, int topid, char *msg);

int iconexp(int exp, int *repeat);

int save_user_data(struct userec *x);
int user_perm(struct userec *x, int level);

extern struct override fff[];
extern int friendnum;

int loadfriend(char *id);
int isfriend(char *id);
void sort_friend(int left, int right);

int init_all();
char *sec(char c);
char *flag_str(int access);
char *flag_str2(int access, int has_read);
char *userid_str(char *s);

struct fileheader *get_file_ent(char *board, char *file);
char *getbfroma(const char *path);
int set_my_cookie(void);

void trace(const char* content);

void printpretable(void);
void printposttable(void);
void printpretable_lite(void);
void printposttable_lite(void);

int showcontent(char *filename);

void printpremarquee(char *width, char *height);
void printpostmarquee(void);

void showheadline(char *board);
void showrecommend(char *board, int showall, int showborder);
void showrawcontent(char *filename);

bool bbscon_search(const struct boardheader *bp, unsigned int fid, 
		int action, struct fileheader *fp);
int maxlen(const char *board);

// bbs.c
int has_BM_perm(struct userec *user, char *board);
int has_read_perm(struct userec *user, char *board);
void do_report(const char* fname, const char* content);
// libBBS/string.c
char *cn_Ctime(time_t t);
char *Ctime(time_t t);
char *nohtml(char *s);
char *getparm(const char *name);
void http_parm_init(void);
void parse_post_data(void);

int fcgi_init_all(void);
int fcgi_init_loop(void);

int bbsleft_main(void);
int bbssec_main(void);
int bbsfoot_main(void);
int bbsgetmsg_main(void);
int bbsall_main(void);
int bbsboa_main(void);
int bbslogin_main(void);
int bbslogout_main(void);
int bbsdoc_main(void);
int bbscon_main(void);
int bbspst_main(void);
int bbssnd_main(void);
int bbsqry_main(void);
int bbsclear_main(void);
int bbsupload_main(void);
int bbspreupload_main(void);
int bbs0an_main(void);
int bbsanc_main(void);

#endif
