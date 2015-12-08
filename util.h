/**
 * Useful helper functions and macros
 */

#ifndef __UTIL_H__
#define __UTIL_H__

extern char *__progname;

#define DEBUG_INFO 1
#define DEBUG_ERROR 2

#if DEBUG >= DEBUG_INFO
# define prtinfo(fmt, args...) fprintf(stderr, fmt "\n", ##args)
#else
# define prtinfo(fmt, args...)
#endif
#if DEBUG >= DEBUG_ERROR
# define prterr(fmt, args...) fprintf(stderr, "%s:%d:" fmt "\n", __FILE__, __LINE__, ##args)
#else
# define prterr(fmt, args...)
#endif

#endif /* !__UTIL_H__ */

