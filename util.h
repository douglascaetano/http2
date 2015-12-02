/**
 * Useful helper functions and macros
 */

#ifndef __UTIL_H__
#define __UTIL_H__

extern char *__progname;

#define prterr(fmt, args...) fprintf(stderr, "%s: " fmt "\n", __progname, ##args)

#endif /* !__UTIL_H__ */

