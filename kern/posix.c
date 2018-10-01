/*
 * Posix generic OS implementation for drawterm.
 */

#include "u.h"

#ifndef _XOPEN_SOURCE	/* for Apple and OpenBSD; not sure if needed */
#define _XOPEN_SOURCE 500
#endif

#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <termios.h>

#include "lib.h"
#include "dat.h"
#include "fns.h"

typedef struct Oproc Oproc;
struct Oproc
{
	int nsleep;
	int nwakeup;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

static pthread_key_t prdakey;

Proc*
_getproc(void)
{
	void *v;

	if((v = pthread_getspecific(prdakey)) == nil)
		panic("cannot getspecific");
	return v;
}

void
_setproc(Proc *p)
{
	if(pthread_setspecific(prdakey, p) != 0)
		panic("cannot setspecific");
}

void
osinit(void)
{
	if(pthread_key_create(&prdakey, 0))
		panic("cannot pthread_key_create");
}

#undef pipe
void
osnewproc(Proc *p)
{
	Oproc *op;
	pthread_mutexattr_t attr;

	op = (Oproc*)p->oproc;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
	pthread_mutex_init(&op->mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	pthread_cond_init(&op->cond, 0);
}

void
osmsleep(int ms)
{
	struct timeval tv;

	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000; /* micro */
	if(select(0, NULL, NULL, NULL, &tv) < 0)
		panic("select");
}

void
osyield(void)
{
	sched_yield();
}

void
oserrstr(void)
{
	char *p;

	if((p = strerror(errno)) != nil)
		strecpy(up->errstr, up->errstr+ERRMAX, p);
	else
		snprint(up->errstr, ERRMAX, "unix error %d", errno);
}

void
oserror(void)
{
	oserrstr();
	nexterror();
}

static void* tramp(void*);

void
osproc(Proc *p)
{
	pthread_t pid;

	if(pthread_create(&pid, nil, tramp, p)){
		oserrstr();
		panic("osproc: %r");
	}
	sched_yield();
}

static void*
tramp(void *vp)
{
	Proc *p;

	p = vp;
	if(pthread_setspecific(prdakey, p))
		panic("cannot setspecific");
	(*p->fn)(p->arg);
	/* BUG: leaks Proc */
	pthread_setspecific(prdakey, 0);
	pthread_exit(0);
	return 0;
}

void
procsleep(void)
{
	Proc *p;
	Oproc *op;

	p = up;
	op = (Oproc*)p->oproc;
	pthread_mutex_lock(&op->mutex);
	op->nsleep++;
	while(op->nsleep > op->nwakeup)
		pthread_cond_wait(&op->cond, &op->mutex);
	pthread_mutex_unlock(&op->mutex);
}

void
procwakeup(Proc *p)
{
	Oproc *op;

	op = (Oproc*)p->oproc;
	pthread_mutex_lock(&op->mutex);
	op->nwakeup++;
	if(op->nwakeup == op->nsleep)
		pthread_cond_signal(&op->cond);
	pthread_mutex_unlock(&op->mutex);
}

static int randfd;
#undef open
void
randominit(void)
{
	if((randfd = open("/dev/urandom", OREAD)) < 0)
	if((randfd = open("/dev/random", OREAD)) < 0)
		panic("open /dev/random: %r");
}

#undef read
ulong
randomread(void *v, ulong n)
{
	int m;

	if((m = read(randfd, v, n)) != n)
		panic("short read from /dev/random: %d but %d", n, m);
	return m;
}

#undef time
long
seconds(void)
{
	return time(0);
}

ulong
ticks(void)
{
	static long sec0 = 0, usec0;
	struct timeval t;

	if(gettimeofday(&t, nil) < 0)
		return 0;
	if(sec0 == 0){
		sec0 = t.tv_sec;
		usec0 = t.tv_usec;
	}
	return (t.tv_sec-sec0)*1000+(t.tv_usec-usec0+500)/1000;
}

long
showfilewrite(char *a, int n)
{
	error("not implemented");
	return -1;
}

void
setterm(int raw)
{
	struct termios t;

	if(tcgetattr(0, &t) < 0)
		return;
	if(raw)
		t.c_lflag &= ~(ECHO|ICANON);
	else
		t.c_lflag |= (ECHO|ICANON);
	tcsetattr(0, TCSAFLUSH, &t);
}
