#include "strptime.h"
#include <time.h>

#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif

VALUE rb_cStrptime;

#define GetStrptimeval(obj, tobj) ((tobj) = get_strptimeval(obj))
#define GetNewStrptimeval(obj, tobj) ((tobj) = get_new_strptimeval(obj))
#define STRPTIME_INIT_P(tobj) ((tobj)->isns)

#define LIKELY(x) (__builtin_expect((x), 1))
#define UNLIKELY(x) (__builtin_expect((x), 0))

#define REG_PC (pc)
#define GET_PC() REG_PC
#define SET_PC(x) (REG_PC = (x))
#define GET_CURRENT_INSN() (*GET_PC())
#define GET_OPERAND(n) (GET_PC()[(n)])
#define ADD_PC(n) (SET_PC(REG_PC + (n)))

#define JUMP(dst) (REG_PC += (dst))

#define LABEL(x) INSN_LABEL_##x
#define ELABEL(x) INSN_ELABEL_##x
#define LABEL_PTR(x) &&LABEL(x)

#define INSN_ENTRY(insn) LABEL(insn) :

#define TC_DISPATCH(insn)                                                      \
    goto *(void const *)GET_CURRENT_INSN();                                    \
    ;
#define END_INSN(insn) TC_DISPATCH(insn);

#define INSN_DISPATCH()                                                        \
    TC_DISPATCH(__START__)                                                     \
    {

#define END_INSNS_DISPATCH()                                                   \
    rb_bug("unknown insn: %p", GET_CURRENT_INSN());                            \
    } /* end of while loop */

#define NEXT_INSN() TC_DISPATCH(__NEXT_INSN__)

static const char *month_names[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
    "Jan",     "Feb",      "Mar",       "Apr",     "May",      "Jun",
    "Jul",     "Aug",      "Sep",       "Oct",     "Nov",      "Dec"};

#define sizeof_array(o) (sizeof o / sizeof o[0])

#define issign(c) ((c) == '-' || (c) == '+')
#undef isdigit
#define isdigit(c) ((unsigned char)((c) - '0') <= 9u)
#undef isspace
#define isspace(c)                                                             \
    ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\v' || (c) == '\f' || \
     (c) == '\r')

/* imply NUL-terminated string */
static size_t
read_digits(const char *s, int *n, size_t width)
{
    const char *s0 = s;
    const char *se = s + width;
    int r = 0;

    for (; s < se && isdigit(*s); s++) {
	r *= 10;
	r += (unsigned char)((*s) - '0');
    }
    *n = r;
    return (size_t)(s - s0);
}

#define fail()                                                                 \
    {                                                                          \
	return -1;                                                             \
    }

#define READ_DIGITS(n, w)                                                      \
    {                                                                          \
	size_t l;                                                              \
	l = read_digits(&str[si], &n, w);                                      \
	if (l == 0) fail();                                                    \
	si += l;                                                               \
    }

#define READ_DIGITS_MAX(n) READ_DIGITS(n, LONG_MAX)

#ifdef HAVE_GMTIME_R
#define rb_gmtime_r(t, tm) gmtime_r((t), (tm))
#define rb_localtime_r(t, tm) localtime_r((t), (tm))
#else
static inline struct tm *
rb_gmtime_r(const time_t *tp, struct tm *result)
{
    struct tm *t = gmtime(tp);
    if (t) *result = *t;
    return t;
}

static inline struct tm *
rb_localtime_r(const time_t *tp, struct tm *result)
{
    struct tm *t = localtime(tp);
    if (t) *result = *t;
    return t;
}
#endif

static struct tm *
rb_localtime_r2(const time_t *t, struct tm *result)
{
#if defined __APPLE__ && defined __LP64__
    if (*t != (time_t)(int)*t) return NULL;
#endif
    result = rb_localtime_r(t, result);
#if defined(HAVE_MKTIME) && defined(LOCALTIME_OVERFLOW_PROBLEM)
    if (result) {
	long gmtoff1 = 0;
	long gmtoff2 = 0;
	struct tm tmp = *result;
	time_t t2;
#if defined(HAVE_STRUCT_TM_TM_GMTOFF)
	gmtoff1 = result->tm_gmtoff;
#endif
	t2 = mktime(&tmp);
#if defined(HAVE_STRUCT_TM_TM_GMTOFF)
	gmtoff2 = tmp.tm_gmtoff;
#endif
	if (*t + gmtoff1 != t2 + gmtoff2) result = NULL;
    }
#endif
    return result;
}
#define LOCALTIME(tm, result) (tzset(), rb_localtime_r2((tm), &(result)))

#if !defined(HAVE_STRUCT_TM_TM_GMTOFF)
static struct tm *
rb_gmtime_r2(const time_t *t, struct tm *result)
{
    result = rb_gmtime_r(t, result);
#if defined(HAVE_TIMEGM) && defined(LOCALTIME_OVERFLOW_PROBLEM)
    if (result) {
	struct tm tmp = *result;
	time_t t2 = timegm(&tmp);
	if (*t != t2) result = NULL;
    }
#endif
    return result;
}
#define GMTIME(tm, result) rb_gmtime_r2((tm), &(result))
#endif

static int
valid_range_p(int v, int a, int b)
{
    return !(v < a || v > b);
}

static int
leap_year_p(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static const int common_year_days_in_month[] = {31, 28, 31, 30, 31, 30,
						31, 31, 30, 31, 30, 31};
static const int leap_year_days_in_month[] = {31, 29, 31, 30, 31, 30,
					      31, 31, 30, 31, 30, 31};

static void
timespec_now(struct timespec *ts)
{
#ifdef HAVE_CLOCK_GETTIME
    if (clock_gettime(CLOCK_REALTIME, ts) == -1) {
	rb_sys_fail("clock_gettime");
    }
#else
    {
	struct timeval tv;
	if (gettimeofday(&tv, 0) < 0) {
	    rb_sys_fail("gettimeofday");
	}
	ts->tv_sec = tv.tv_sec;
	ts->tv_nsec = tv.tv_usec * 1000;
    }
#endif
}

static struct tm *
localtime_with_gmtoff_zone(const time_t *t, struct tm *result, long *gmtoff,
			   const char **zone)
{
    struct tm tm;

    if (LOCALTIME(t, tm)) {
#if defined(HAVE_STRUCT_TM_TM_GMTOFF)
	*gmtoff = tm.tm_gmtoff;
#else
	struct tm *u, *l;
	long off;
	struct tm tmbuf;
	l = &tm;
	u = GMTIME(t, tmbuf);
	if (!u) return NULL;
	if (l->tm_year != u->tm_year)
	    off = l->tm_year < u->tm_year ? -1 : 1;
	else if (l->tm_mon != u->tm_mon)
	    off = l->tm_mon < u->tm_mon ? -1 : 1;
	else if (l->tm_mday != u->tm_mday)
	    off = l->tm_mday < u->tm_mday ? -1 : 1;
	else
	    off = 0;
	off = off * 24 + l->tm_hour - u->tm_hour;
	off = off * 60 + l->tm_min - u->tm_min;
	off = off * 60 + l->tm_sec - u->tm_sec;
	*gmtoff = off;
#endif

	*result = tm;
	return result;
    }
    return NULL;
}

static void
tm_add_offset(struct tm *tm, long diff)
{
    int sign, tsec, tmin, thour, tday;

    if (diff < 0) {
	sign = -1;
	diff = -diff;
    }
    else {
	sign = 1;
    }
    tsec = diff % 60;
    diff = diff / 60;
    tmin = diff % 60;
    diff = diff / 60;
    thour = diff % 24;
    diff = diff / 24;

    if (sign < 0) {
	tsec = -tsec;
	tmin = -tmin;
	thour = -thour;
    }

    tday = 0;

    if (tsec) {
	tsec += tm->tm_sec;
	if (tsec < 0) {
	    tsec += 60;
	    tmin -= 1;
	}
	if (60 <= tsec) {
	    tsec -= 60;
	    tmin += 1;
	}
	tm->tm_sec = tsec;
    }

    if (tmin) {
	tmin += tm->tm_min;
	if (tmin < 0) {
	    tmin += 60;
	    thour -= 1;
	}
	if (60 <= tmin) {
	    tmin -= 60;
	    thour += 1;
	}
	tm->tm_min = tmin;
    }

    if (thour) {
	thour += tm->tm_hour;
	if (thour < 0) {
	    thour += 24;
	    tday = -1;
	}
	if (24 <= thour) {
	    thour -= 24;
	    tday = 1;
	}
	tm->tm_hour = thour;
    }

    if (tday) {
	if (tday < 0) {
	    if (tm->tm_mon == 1 && tm->tm_mday == 1) {
		tm->tm_mday = 31;
		tm->tm_mon = 12; /* December */
		tm->tm_year = tm->tm_year - 1;
	    }
	    else if (tm->tm_mday == 1) {
		const int *days_in_month = leap_year_p(tm->tm_year)
					       ? leap_year_days_in_month
					       : common_year_days_in_month;
		tm->tm_mon--;
		tm->tm_mday = days_in_month[tm->tm_mon - 1];
	    }
	    else {
		tm->tm_mday--;
	    }
	}
	else {
	    int leap = leap_year_p(tm->tm_year);
	    if (tm->tm_mon == 12 && tm->tm_mday == 31) {
		tm->tm_year = tm->tm_year + 1;
		tm->tm_mon = 1; /* January */
		tm->tm_mday = 1;
	    }
	    else if (tm->tm_mday ==
		     (leap ? leap_year_days_in_month
			   : common_year_days_in_month)[tm->tm_mon - 1]) {
		tm->tm_mon++;
		tm->tm_mday = 1;
	    }
	    else {
		tm->tm_mday++;
	    }
	}
    }
}

static int
strptime_exec0(void **pc, const char *fmt, const char *str, size_t slen,
	       struct timespec *tsp, int *gmtoffp)
{
    size_t si = 0;
    int year = INT_MAX, mon = -1, mday = -1, hour = -1, min = -1, sec = -1,
	nsec = 0, gmtoff = INT_MAX;
    if (UNLIKELY(tsp == NULL)) {
	static const void *const insns_address_table[] = {
	    LABEL_PTR(A),   LABEL_PTR(B), LABEL_PTR(C),   LABEL_PTR(D),
	    LABEL_PTR(E),   LABEL_PTR(F), LABEL_PTR(G),   LABEL_PTR(H),
	    LABEL_PTR(I),   NULL,	 NULL,		  LABEL_PTR(L),
	    LABEL_PTR(M),   LABEL_PTR(N), LABEL_PTR(O),   LABEL_PTR(P),
	    LABEL_PTR(Q),   LABEL_PTR(R), LABEL_PTR(S),   LABEL_PTR(T),
	    LABEL_PTR(U),   LABEL_PTR(V), LABEL_PTR(W),   LABEL_PTR(X),
	    LABEL_PTR(Y),   LABEL_PTR(Z), LABEL_PTR(_25), LABEL_PTR(_2b),
	    LABEL_PTR(_3a), NULL,	 LABEL_PTR(_5f), LABEL_PTR(_60),
	    LABEL_PTR(a),   LABEL_PTR(B), LABEL_PTR(c),   LABEL_PTR(d),
	    LABEL_PTR(d),   NULL,	 LABEL_PTR(g),   LABEL_PTR(B),
	    NULL,	   LABEL_PTR(j), NULL,		  LABEL_PTR(l),
	    LABEL_PTR(m),   LABEL_PTR(n), NULL,		  LABEL_PTR(p),
	    NULL,	   LABEL_PTR(r), LABEL_PTR(s),   LABEL_PTR(t),
	    LABEL_PTR(u),   LABEL_PTR(v), LABEL_PTR(w),   LABEL_PTR(x),
	    LABEL_PTR(y),   LABEL_PTR(z),
	};
	*pc = (void *)insns_address_table;
	return 0;
    }

first:
    INSN_DISPATCH();
    INSN_ENTRY(A)
    {
	ADD_PC(1);
	END_INSN(A)
    }
    INSN_ENTRY(B)
    {
	int i;
	for (i = 0; i < (int)sizeof_array(month_names); i++) {
	    size_t l = strlen(month_names[i]);
	    if (strncasecmp(month_names[i], &str[si], l) == 0) {
		si += l;
		mon = (i % 12) + 1;
		ADD_PC(1);
		END_INSN(B)
	    }
	}
	fail();
    }
    INSN_ENTRY(C)
    {
	ADD_PC(1);
	END_INSN(C)
    }
    INSN_ENTRY(D)
    {
	ADD_PC(1);
	END_INSN(D)
    }
    INSN_ENTRY(E)
    {
	ADD_PC(1);
	END_INSN(E)
    }
    INSN_ENTRY(F)
    {
	ADD_PC(1);
	END_INSN(F)
    }
    INSN_ENTRY(G)
    {
	ADD_PC(1);
	END_INSN(G)
    }
    INSN_ENTRY(H)
    {
	READ_DIGITS(hour, 2);
	if (!valid_range_p(hour, 0, 23)) fail();
	ADD_PC(1);
	END_INSN(H)
    }
    INSN_ENTRY(I)
    {
	ADD_PC(1);
	END_INSN(I)
    }
    INSN_ENTRY(L)
    {
	ADD_PC(1);
	END_INSN(L)
    }
    INSN_ENTRY(M)
    {
	READ_DIGITS(min, 2);
	if (!valid_range_p(min, 0, 59)) fail();
	ADD_PC(1);
	END_INSN(M)
    }
    INSN_ENTRY(N)
    {
	size_t l;
	l = read_digits(&str[si], &nsec, 9);
	if (!l) fail();
	si += l;
	for (; l < 9; l++) {
	    nsec *= 10;
	}
	ADD_PC(1);
	END_INSN(N)
    }
    INSN_ENTRY(O)
    {
	ADD_PC(1);
	END_INSN(O)
    }
    INSN_ENTRY(P)
    {
	ADD_PC(1);
	END_INSN(P)
    }
    INSN_ENTRY(Q)
    {
	ADD_PC(1);
	END_INSN(Q)
    }
    INSN_ENTRY(R)
    {
	ADD_PC(1);
	END_INSN(R)
    }
    INSN_ENTRY(S)
    {
	READ_DIGITS(sec, 2);
	if (!valid_range_p(sec, 0, 60)) fail();
	ADD_PC(1);
	END_INSN(S)
    }
    INSN_ENTRY(T)
    {
	ADD_PC(1);
	END_INSN(T)
    }
    INSN_ENTRY(U)
    {
	ADD_PC(1);
	END_INSN(U)
    }
    INSN_ENTRY(V)
    {
	ADD_PC(1);
	END_INSN(V)
    }
    INSN_ENTRY(W)
    {
	ADD_PC(1);
	END_INSN(W)
    }
    INSN_ENTRY(X)
    {
	ADD_PC(1);
	END_INSN(X)
    }
    INSN_ENTRY(Y)
    {
	char c = str[si];
	if (issign(c)) si++;
	READ_DIGITS(year, 4);
	if (c == '-') year *= -1;
	ADD_PC(1);
	END_INSN(Y)
    }
    INSN_ENTRY(Z)
    {
	ADD_PC(1);
	END_INSN(Z)
    }
    INSN_ENTRY(a)
    {
	ADD_PC(1);
	END_INSN(a)
    }
    INSN_ENTRY(c)
    {
	ADD_PC(1);
	END_INSN(c)
    }
    INSN_ENTRY(d)
    {
	READ_DIGITS(mday, 2);
	if (!valid_range_p(mday, 1, 31)) fail();
	ADD_PC(1);
	END_INSN(d)
    }
    INSN_ENTRY(g)
    {
	ADD_PC(1);
	END_INSN(g)
    }
    INSN_ENTRY(j)
    {
	ADD_PC(1);
	END_INSN(j)
    }
    INSN_ENTRY(l)
    {
	ADD_PC(1);
	END_INSN(l)
    }
    INSN_ENTRY(m)
    {
	READ_DIGITS(mon, 2);
	if (!valid_range_p(mon, 1, 12)) fail();
	ADD_PC(1);
	END_INSN(m)
    }
    INSN_ENTRY(n)
    {
	for (; si < slen && isspace(str[si]); si++) {
	}
	ADD_PC(1);
	END_INSN(n)
    }
    INSN_ENTRY(p)
    {
	ADD_PC(1);
	END_INSN(p)
    }
    INSN_ENTRY(r)
    {
	ADD_PC(1);
	END_INSN(r)
    }
    INSN_ENTRY(s)
    {
	ADD_PC(1);
	END_INSN(s)
    }
    INSN_ENTRY(t)
    {
	ADD_PC(1);
	END_INSN(t)
    }
    INSN_ENTRY(u)
    {
	ADD_PC(1);
	END_INSN(u)
    }
    INSN_ENTRY(v)
    {
	ADD_PC(1);
	END_INSN(v)
    }
    INSN_ENTRY(w)
    {
	ADD_PC(1);
	END_INSN(w)
    }
    INSN_ENTRY(x)
    {
	ADD_PC(1);
	END_INSN(x)
    }
    INSN_ENTRY(y)
    {
	ADD_PC(1);
	END_INSN(y)
    }
    INSN_ENTRY(z)
    {
	const char *p0 = str + si;
	int r;
	size_t len;
	if (*p0 == 'z') {
	    gmtoff = 0;
	    ADD_PC(1);
	    END_INSN(z)
	}
	if (issign(*p0)) si++;
	READ_DIGITS(r, 2);
	gmtoff = r * 60;
	if (str[si] == ':') si++;
	len = read_digits(&str[si], &r, 2);
	if (len) {
	    si += len;
	    gmtoff += r;
	}
	gmtoff *= 60;
	if (*p0 == '-') gmtoff = -gmtoff;
	ADD_PC(1);
	END_INSN(z)
    }
    INSN_ENTRY(_25)
    {
	ADD_PC(1);
	END_INSN(_25)
    }
    INSN_ENTRY(_2b)
    {
	ADD_PC(1);
	END_INSN(_2b)
    }
    INSN_ENTRY(_3a)
    {
	ADD_PC(1);
	END_INSN(_3a)
    }
    INSN_ENTRY(_60)
    {
	size_t v = (size_t)GET_OPERAND(1);
	size_t fi = v & 0xFFFF;
	size_t cnt = v >> 16;
	if (memcmp(str + si, fmt + fi, cnt)) return Qnil;
	pc += 2;
	si += cnt;
	END_INSN(_60)
    }
    INSN_ENTRY(_5f)
    {
	struct timespec ts;
	struct tm tm;
	time_t t;

	/* get current time with timezone */
	timespec_now(&ts);
	{
	    static time_t ct;
	    static struct tm ctm;
	    static long cgmtoff;
	    static long cloff;
	    long off;
	    if (ct == ts.tv_sec) {
		off = cgmtoff;
	    }
	    else {
		ct = ts.tv_sec;
		localtime_with_gmtoff_zone(&ct, &ctm, &off, NULL);
		cloff = off;
		cgmtoff = INT_MAX;
	    }
	    if (gmtoff == INT_MAX) {
		gmtoff = cloff;
	    }
	    if (gmtoff != off) {
		tm_add_offset(&ctm, gmtoff - off);
	    }
	    memcpy(&tm, &ctm, sizeof(struct tm));
	    cgmtoff = gmtoff;
	}

	/* overwrite time */
	if (year != INT_MAX) {
	    tm.tm_year = year - 1900;
	    if (mon == -1) mon = 1;
	setmonth:
	    tm.tm_mon = mon - 1;
	    if (mday == -1) mday = 1;
	setmday:
	    tm.tm_mday = mday;
	    if (hour == -1) hour = 0;
	sethour:
	    tm.tm_hour = hour;
	    if (min == -1) min = 0;
	setmin:
	    tm.tm_min = min;
	    if (sec == -1) sec = 0;
	    tm.tm_sec = sec;
	}
	else {
	    if (mon != -1) goto setmonth;
	    if (mday != -1) goto setmday;
	    if (hour != -1) goto sethour;
	    if (min != -1) goto setmin;
	    if (sec != -1) tm.tm_sec = sec;
	}

	{
	    static time_t ct;
	    static struct tm cache;
	    /* struct tm to time_t */
	    if (ct && cache.tm_year == tm.tm_year &&
		cache.tm_mon == tm.tm_mon && cache.tm_mday == tm.tm_mday) {
		t = ct + (tm.tm_hour - cache.tm_hour) * 3600 +
		    (tm.tm_min - cache.tm_min) * 60 +
		    (tm.tm_sec - cache.tm_sec);
	    }
	    else {
		ct = t = timegm(&tm);
		memcpy((void *)&cache, &tm, sizeof(struct tm));
	    }
	    t -= gmtoff;
	}
	tsp->tv_sec = t;
	tsp->tv_nsec = nsec;
	*gmtoffp = gmtoff;
	return 0;
	END_INSN(_5f)
    }
    END_INSNS_DISPATCH();

    /* unreachable */
    rb_bug("vm_eval: unreachable");
    goto first;
}

void **
strptime_compile(const char *fmt, size_t flen)
{
    size_t fi = 0;
    char c;
    void **isns0 = ALLOC_N(void *, flen + 2);
    void **isns = isns0;
    void **insns_address_table;
    void *tmp;
    strptime_exec0((void **)&insns_address_table, NULL, NULL, 0, NULL, NULL);

    while (fi < flen) {
	switch (fmt[fi]) {
	case '%':
	    fi++;
	    c = fmt[fi];
	    switch (c) {
	    case 'B':
	    case 'H':
	    case 'M':
	    case 'N':
	    case 'S':
	    case 'Y':
	    case 'b':
	    case 'd':
	    case 'e':
	    case 'h':
	    case 'm':
	    case 'n':
	    case 'z':
		tmp = insns_address_table[c - 'A'];
		if (tmp) {
		    *isns++ = tmp;
		    fi++;
		    continue;
		}
	    default: rb_raise(rb_eArgError, "invalid format"); break;
	    }
	case ' ':
	case '\t':
	case '\n':
	case '\v':
	case '\f':
	case '\r':
	    *isns++ = insns_address_table['n' - 'A'];
	    fi++;
	    break;
	default: {
	    const char *p0 = fmt + fi, *p = p0, *pe = fmt + flen;
	    size_t v = fi;
	    while (p < pe && *p != '%' && !ISSPACE(*p))
		p++;
	    v += (p - p0) << 16;
	    fi += p - p0;
	    *isns++ = insns_address_table['`' - 'A'];
	    *isns++ = (void *)v;
	} break;
	}
    }
    *isns++ = insns_address_table['_' - 'A'];
    REALLOC_N(isns0, void *, isns - isns0);
    return isns0;
}

struct strptime_object {
    void **isns;
    VALUE fmt;
};

static void
strptime_mark(void *ptr)
{
    struct strptime_object *tobj = ptr;
    rb_gc_mark(tobj->fmt);
}

static size_t
strptime_memsize(const void *tobj)
{
    return sizeof(struct strptime_object);
}

static const rb_data_type_t strptime_data_type = {
    "strptime",
    {
	strptime_mark, RUBY_TYPED_DEFAULT_FREE, strptime_memsize,
    },
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
    0,
    0,
    RUBY_TYPED_FREE_IMMEDIATELY
#endif
};

static VALUE
strptime_s_alloc(VALUE klass)
{
    VALUE obj;
    struct strptime_object *tobj;

    obj = TypedData_Make_Struct(klass, struct strptime_object,
				&strptime_data_type, tobj);

    return obj;
}

static struct strptime_object *
get_strptimeval(VALUE obj)
{
    struct strptime_object *tobj;
    TypedData_Get_Struct(obj, struct strptime_object, &strptime_data_type,
			 tobj);
    if (!STRPTIME_INIT_P(tobj)) {
	rb_raise(rb_eTypeError, "uninitialized %" PRIsVALUE, rb_obj_class(obj));
    }
    return tobj;
}

static struct strptime_object *
get_new_strptimeval(VALUE obj)
{
    struct strptime_object *tobj;
    TypedData_Get_Struct(obj, struct strptime_object, &strptime_data_type,
			 tobj);
    if (STRPTIME_INIT_P(tobj)) {
	rb_raise(rb_eTypeError, "already initialized %" PRIsVALUE,
		 rb_obj_class(obj));
    }
    return tobj;
}

/*
 *  call-seq:
 *     Strptime.new(format) -> object
 *
 * returns parser object
 */
static VALUE
strptime_init(VALUE self, VALUE fmt)
{
    struct strptime_object *tobj;
    void **isns;
    StringValue(fmt);
    TypedData_Get_Struct(self, struct strptime_object, &strptime_data_type,
			 tobj);
    isns = strptime_compile(RSTRING_PTR(fmt), RSTRING_LEN(fmt));
    tobj->isns = isns;
    tobj->fmt = rb_str_new_frozen(fmt);
    return self;
}

/* :nodoc: */
static VALUE
strptime_init_copy(VALUE copy, VALUE self)
{
    struct strptime_object *tobj, *tcopy;

    if (!OBJ_INIT_COPY(copy, self)) return copy;
    GetStrptimeval(self, tobj);
    GetNewStrptimeval(copy, tcopy);
    MEMCPY(tcopy, tobj, struct strptime_object, 1);

    return copy;
}

typedef uint64_t WIDEVALUE;
typedef WIDEVALUE wideval_t;
#ifndef PACKED_STRUCT_UNALIGNED
#define PACKED_STRUCT_UNALIGNED(x) x
#endif
PACKED_STRUCT_UNALIGNED(struct vtm {
    VALUE year;	/* 2000 for example.  Integer. */
    VALUE subsecx;     /* 0 <= subsecx < TIME_SCALE.  possibly Rational. */
    VALUE utc_offset;  /* -3600 as -01:00 for example.  possibly Rational. */
    const char *zone;  /* "JST", "EST", "EDT", etc. */
    uint16_t yday : 9; /* 1..366 */
    uint8_t mon : 4;   /* 1..12 */
    uint8_t mday : 5;  /* 1..31 */
    uint8_t hour : 5;  /* 0..23 */
    uint8_t min : 6;   /* 0..59 */
    uint8_t sec : 6;   /* 0..60 */
    uint8_t wday : 3;  /* 0:Sunday, 1:Monday, ..., 6:Saturday 7:init */
    uint8_t isdst : 2; /* 0:StandardTime 1:DayLightSavingTime 3:init */
});
PACKED_STRUCT_UNALIGNED(struct time_object {
    wideval_t timew; /* time_t value * TIME_SCALE.  possibly Rational. */
    struct vtm vtm;
    uint8_t gmt : 3; /* 0:utc 1:localtime 2:fixoff 3:init */
    uint8_t tm_got : 1;
});

/*
 *  call-seq:
 *     Strptime#exec(str) -> time
 *
 *
 */
static VALUE
strptime_exec(VALUE self, VALUE str)
{
    struct strptime_object *tobj;
    int r, gmtoff = 0;
    StringValue(str);
    GetStrptimeval(self, tobj);
    struct timespec ts;

    r = strptime_exec0(tobj->isns, RSTRING_PTR(tobj->fmt), RSTRING_PTR(str),
		       RSTRING_LEN(str), &ts, &gmtoff);
    if (r) rb_raise(rb_eArgError, "string doesn't match");
    return timespec_new(&ts, gmtoff);
}

/*
 *  call-seq:
 *     Strptime#execi(str) -> integer
 *
 * returns epoch as integer
 */
static VALUE
strptime_execi(VALUE self, VALUE str)
{
    struct strptime_object *tobj;
    struct timespec ts;
    int r, gmtoff = 0;
    StringValue(str);
    GetStrptimeval(self, tobj);

    r = strptime_exec0(tobj->isns, RSTRING_PTR(tobj->fmt), RSTRING_PTR(str),
		       RSTRING_LEN(str), &ts, &gmtoff);
    if (r) rb_raise(rb_eArgError, "string doesn't match");
    return TIMET2NUM(ts.tv_sec);
}

/*
 *  call-seq:
 *     Strptime#source -> string
 *
 * returns source format string
 */
static VALUE
strptime_source(VALUE self)
{
    struct strptime_object *tobj;
    GetStrptimeval(self, tobj);

    return tobj->fmt;
}

/*
 *  Strptime is a faster way to parse time strings.
 *
 *    parser = Strptime.new('%Y-%m-%dT%H:%M:%S%z')
 *    parser.exec('2015-12-25T12:34:56+09') #=> 2015-12-25 12:34:56 +09:00
 *    parser.execi('2015-12-25T12:34:56+09') #=> 1451014496
 */
void
Init_strptime(void)
{
    rb_cStrptime = rb_define_class("Strptime", rb_cObject);
    rb_define_alloc_func(rb_cStrptime, strptime_s_alloc);
    rb_define_method(rb_cStrptime, "initialize", strptime_init, 1);
    rb_define_method(rb_cStrptime, "initialize_copy", strptime_init_copy, 1);
    rb_define_method(rb_cStrptime, "exec", strptime_exec, 1);
    rb_define_method(rb_cStrptime, "execi", strptime_execi, 1);
    rb_define_method(rb_cStrptime, "source", strptime_source, 0);
}
