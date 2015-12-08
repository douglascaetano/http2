/**
 * Useful helper functions and macros
 */

#ifndef __UTIL_H__
#define __UTIL_H__

extern char *__progname;

#define DEBUG_INFO 2
#define DEBUG_ERROR 1

#if DEBUG >= DEBUG_INFO
# define prtinfo(fmt, args...) fprintf(stderr, fmt "\n", ##args)
#else
# define prtinfo(fmt, args...)
#endif

#if DEBUG >= DEBUG_ERROR
# define prterr(fmt, args...) fprintf(stderr, "%s:%d:" fmt "\n", __FILE__, __LINE__, ##args)
# define prterrno(func) fprintf(stderr, func ": %s", strerror(errno))
#else
# define prterr(fmt, args...)
# define prterrno(func)
#endif

#endif /* !__UTIL_H__ */

