#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

enum
{
	PATHSLOP	= 20,
	PATHMSLOP	= 20,
};

static struct Chanalloc
{
	Lock	lk;
	int	fid;
	Chan	*free;
	Chan	*list;
}chanalloc;

typedef struct Elemlist Elemlist;

struct Elemlist
{
	char	*aname;	/* original name */
	char	*name;	/* copy of name, so '/' can be overwritten */
	int	nelems;
	char	**elems;
	int	*off;
	int	mustbedir;
	int	nerror;
	int	prefix;
};

char*
chanpath(Chan *c)
{
	if(c == nil)
		return "<nil chan>";
	if(c->path == nil)
		return "<nil path>";
	if(c->path->s == nil)
		return "<nil path.s>";
	return c->path->s;
}

int
isdotdot(char *p)
{
	return p[0]=='.' && p[1]=='.' && p[2]=='\0';
}

int
incref(Ref *r)
{
	int x;
	lock(&r->lk);
	x = ++r->ref;
	unlock(&r->lk);
	return x;
}

int
decref(Ref *r)
{
	int x;
	lock(&r->lk);
	x = --r->ref;
	unlock(&r->lk);
	if(x < 0)
		panic("decref, pc=0x%p", getcallerpc(&r));
	return x;
}

/*
 * Rather than strncpy, which zeros the rest of the buffer, kstrcpy
 * truncates if necessary, always zero terminates, does not zero fill,
 * and puts ... at the end of the string if it's too long.  Usually used to
 * save a string in up->genbuf;
 */
void
kstrcpy(char *s, char *t, int ns)
{
	int nt;

	nt = strlen(t);
	if(nt < ns){
		memmove(s, t, nt);
		s[nt] = '\0';
		return;
	}
	/* too long, truncate */
	nt = ns-1;
	memmove(s, t, nt);
	s[nt] = '\0';
	/* append ... if there is space */
	ns -= 4;
	if(ns < 0)
		return;
	/* look for first byte of UTF-8 sequence by skipping continuation bytes */
	while(ns>0 && (s[--ns]&0xC0)==0x80)
		;
	strcpy(s+ns, "...");
}

int
emptystr(char *s)
{
	if(s == nil)
		return 1;
	if(s[0] == '\0')
		return 1;
	return 0;
}

/*
 * Atomically replace *p with copy of s
 */
void
kstrdup(char **p, char *s)
{
	int n;
	char *t, *prev;

	n = strlen(s);
	/* if it's a user, we can wait for memory; if not, something's very wrong */
	if(up != nil)
		t = smalloc(n+1);
	else{
		t = malloc(n+1);
		if(t == nil)
			panic("kstrdup: no memory");
	}
	setmalloctag(t, getcallerpc(&p));
	memmove(t, s, n);
	t[n] = '\0';
	prev = *p;
	*p = t;
	free(prev);
}

void
chandevreset(void)
{
	int i;

	for(i=0; devtab[i] != nil; i++)
		devtab[i]->reset();
}

void
chandevinit(void)
{
	int i;

	for(i=0; devtab[i] != nil; i++)
		devtab[i]->init();
}

void
chandevshutdown(void)
{
	int i;
	
	/* shutdown in reverse order */
	for(i=0; devtab[i] != nil; i++)
		;
	for(i--; i >= 0; i--)
		devtab[i]->shutdown();
}

Chan*
newchan(void)
{
	Chan *c;

	lock(&chanalloc.lk);
	c = chanalloc.free;
	if(c != nil){
		chanalloc.free = c->next;
		c->next = nil;
	} else {
		unlock(&chanalloc.lk);
		c = smalloc(sizeof(Chan));
		lock(&chanalloc.lk);
		c->link = chanalloc.list;
		chanalloc.list = c;
	}
	if(c->fid == 0)
		c->fid = ++chanalloc.fid;
	unlock(&chanalloc.lk);

	/* if you get an error before associating with a dev,
	   close calls rootclose, a nop */
	c->type = 0;
	c->flag = 0;
	c->ref.ref = 1;
	c->dev = 0;
	c->offset = 0;
	c->devoffset = 0;
	c->iounit = 0;
	c->umh = nil;
	c->umc = nil;
	c->uri = 0;
	c->dri = 0;
	c->dirrock = nil;
	c->nrock = 0;
	c->mrock = 0;
	c->ismtpt = 0;
	c->mux = nil;
	c->aux = nil;
	c->mchan = nil;
	memset(&c->mqid, 0, sizeof(c->mqid));
	c->path = nil;

	return c;
}

Path*
newpath(char *s)
{
	int i;
	Path *p;

	p = smalloc(sizeof(Path));
	i = strlen(s);
	p->len = i;
	p->alen = i+PATHSLOP;
	p->s = smalloc(p->alen);
	memmove(p->s, s, i+1);
	p->ref.ref = 1;

	/*
	 * Cannot use newpath for arbitrary names because the mtpt 
	 * array will not be populated correctly.  The names #/ and / are
	 * allowed, but other names with / in them draw warnings.
	 */
	if(strchr(s, '/') != nil && strcmp(s, "#/") != 0 && strcmp(s, "/") != 0)
		print("newpath: %s from %#p\n", s, getcallerpc(&s));

	p->mlen = 1;
	p->malen = PATHMSLOP;
	p->mtpt = smalloc(p->malen*sizeof p->mtpt[0]);
	return p;
}

static Path*
copypath(Path *p)
{
	int i;
	Path *pp;
	
	pp = smalloc(sizeof(Path));
	pp->ref.ref = 1;
	
	pp->len = p->len;
	pp->alen = p->alen;
	pp->s = smalloc(p->alen);
	memmove(pp->s, p->s, p->len+1);
	
	pp->mlen = p->mlen;
	pp->malen = p->malen;
	pp->mtpt = smalloc(p->malen*sizeof pp->mtpt[0]);
	for(i=0; i<pp->mlen; i++){
		pp->mtpt[i] = p->mtpt[i];
		if(pp->mtpt[i] != nil)
			incref(&pp->mtpt[i]->ref);
	}

	return pp;
}

void
pathclose(Path *p)
{
	int i;

	if(p == nil || decref(&p->ref))
		return;
	for(i=0; i<p->mlen; i++)
		if(p->mtpt[i] != nil)
			cclose(p->mtpt[i]);
	free(p->mtpt);
	free(p->s);
	free(p);
}

/*
 * In place, rewrite name to compress multiple /, eliminate ., and process ..
 * (Really only called to remove a trailing .. that has been added.
 * Otherwise would need to update n->mtpt as well.)
 */
static void
fixdotdotname(Path *p)
{
	char *r;

	if(p->s[0] == '#'){
		r = strchr(p->s, '/');
		if(r == nil)
			return;
		cleanname(r);

		/*
		 * The correct name is #i rather than #i/,
		 * but the correct name of #/ is #/.
		 */
		if(strcmp(r, "/")==0 && p->s[1] != '/')
			*r = '\0';
	}else
		cleanname(p->s);
	p->len = strlen(p->s);
}

static Path*
uniquepath(Path *p)
{
	Path *new;
	
	if(p->ref.ref > 1){
		/* copy on write */
		new = copypath(p);
		pathclose(p);
		p = new;
	}
	return p;
}

static Path*
addelem(Path *p, char *s, Chan *from)
{
	char *t;
	int a, i;
	Chan *c, **tt;

	if(s[0]=='.' && s[1]=='\0')
		return p;

	p = uniquepath(p);

	i = strlen(s);
	a = p->len+1+i+1;
	if(a > p->alen){
		a += PATHSLOP;
		t = smalloc(a);
		memmove(t, p->s, p->len+1);
		free(p->s);
		p->s = t;
		p->alen = a;
	}
	/* don't insert extra slash if one is present */
	if(p->len>0 && p->s[p->len-1]!='/' && s[0]!='/')
		p->s[p->len++] = '/';
	memmove(p->s+p->len, s, i+1);
	p->len += i;
	if(isdotdot(s)){
		fixdotdotname(p);
		if(p->mlen > 1 && (c = p->mtpt[--p->mlen]) != nil){
			p->mtpt[p->mlen] = nil;
			cclose(c);
		}
	}else{
		if(p->mlen >= p->malen){
			p->malen = p->mlen+1+PATHMSLOP;
			tt = smalloc(p->malen*sizeof tt[0]);
			memmove(tt, p->mtpt, p->mlen*sizeof tt[0]);
			free(p->mtpt);
			p->mtpt = tt;
		}
		p->mtpt[p->mlen++] = from;
		if(from != nil)
			incref(&from->ref);
	}
	return p;
}

void
chanfree(Chan *c)
{
	c->flag = CFREE;

	if(c->dirrock != nil){
		free(c->dirrock);
		c->dirrock = nil;
		c->nrock = 0;
		c->mrock = 0;
	}
	if(c->umh != nil){
		putmhead(c->umh);
		c->umh = nil;
	}
	if(c->umc != nil){
		cclose(c->umc);
		c->umc = nil;
	}
	if(c->mux != nil){
		muxclose(c->mux);
		c->mux = nil;
	}
	if(c->mchan != nil){
		cclose(c->mchan);
		c->mchan = nil;
	}

	pathclose(c->path);
	c->path = nil;

	lock(&chanalloc.lk);
	c->next = chanalloc.free;
	chanalloc.free = c;
	unlock(&chanalloc.lk);
}

void
cclose(Chan *c)
{
	if(c == nil || c->ref.ref < 1 || c->flag&CFREE)
		panic("cclose %#p", getcallerpc(&c));

	if(decref(&c->ref))
		return;

	if(!waserror()){
		devtab[c->type]->close(c);
		poperror();
	}
	chanfree(c);
}

/*
 * Make sure we have the only copy of c.  (Copy on write.)
 */
Chan*
cunique(Chan *c)
{
	Chan *nc;

	if(c->ref.ref != 1){
		nc = cclone(c);
		cclose(c);
		c = nc;
	}

	if(c->umh != nil){	//BUG
		print("cunique umh != nil from %#p\n", getcallerpc(&c));
		putmhead(c->umh);
		c->umh = nil;
	}

	return c;
}

int
eqqid(Qid a, Qid b)
{
	return a.path==b.path && a.vers==b.vers;
}

int
eqchan(Chan *a, Chan *b, int skipvers)
{
	if(a->qid.path != b->qid.path)
		return 0;
	if(!skipvers && a->qid.vers!=b->qid.vers)
		return 0;
	if(a->type != b->type)
		return 0;
	if(a->dev != b->dev)
		return 0;
	return 1;
}

int
eqchantdqid(Chan *a, int type, int dev, Qid qid, int skipvers)
{
	if(a->qid.path != qid.path)
		return 0;
	if(!skipvers && a->qid.vers!=qid.vers)
		return 0;
	if(a->type != type)
		return 0;
	if(a->dev != (ulong)dev)
		return 0;
	return 1;
}

Mhead*
newmhead(Chan *from)
{
	Mhead *mh;

	mh = smalloc(sizeof(Mhead));
	mh->ref.ref = 1;
	mh->from = from;
	incref(&from->ref);
	return mh;
}

/*
 * This is necessary because there are many
 * pointers to the top of a given mount list:
 *
 *	- the mhead in the namespace hash table
 *	- the mhead in chans returned from findmount:
 *	  used in namec and then by unionread.
 *	- the mhead in chans returned from createdir:
 *	  used in the open/create race protect, which is gone.
 *
 * The RWlock in the Mhead protects the mount list it contains.
 * The mount list is deleted in cunmount() and closepgrp().
 * The RWlock ensures that nothing is using the mount list at that time.
 *
 * It is okay to replace c->mh with whatever you want as 
 * long as you are sure you have a unique reference to it.
 *
 * This comment might belong somewhere else.
 */
void
putmhead(Mhead *m)
{
	if(m == nil)
		return;
	if(decref(&m->ref))
		return;
	assert(m->mount == nil);
	cclose(m->from);
	free(m);
}

int
cmount(Chan *new, Chan *old, int flag, char *spec)
{
	int order;
	Mhead *m, **l, *mh;
	Mount *nm, *f, *um;
	Pgrp *pg;

	if(old->umh != nil)
		print("cmount: unexpected umh, caller %#p\n", getcallerpc(&new));

	if(QTDIR & (old->qid.type^new->qid.type))
		error(Emount);

	order = flag&MORDER;

	if((old->qid.type&QTDIR) == 0 && order != MREPL)
		error(Emount);

	nm = newmount(new, flag, spec);
	mh = new->umh;
	if(mh != nil) {
		rlock(&mh->lock);
		if(waserror()) {
			runlock(&mh->lock);
			mountfree(nm);
			nexterror();
		}
		um = mh->mount;
		if(um != nil){
			/*
			 * Not allowed to bind when the old directory is itself a union. 
			 * (Maybe it should be allowed, but I don't see what the semantics
			 * would be.)
			 *
			 * We need to check mh->mount->next to tell unions apart from
			 * simple mount points, so that things like
			 *	mount -c fd /root
			 *	bind -c /root /
			 * work.  
			 * 
			 * The check of mount->mflag allows things like
			 *	mount fd /root
			 *	bind -c /root /
			 * 
			 * This is far more complicated than it should be, but I don't
			 * see an easier way at the moment.
			 */
			if((flag&MCREATE) != 0 && (um->next != nil || (um->mflag&MCREATE) == 0))
				error(Emount);

			/*
			 *  copy a union when binding it onto a directory
			 */
			f = nm;
			for(um = um->next; um != nil; um = um->next){
				f->next = newmount(um->to, order==MREPL? MAFTER: order, um->spec);
				f = f->next;
			}
		}
		runlock(&mh->lock);
		poperror();
	}

	pg = up->pgrp;
	wlock(&pg->ns);
	l = &MOUNTH(pg, old->qid);
	for(m = *l; m != nil; m = m->hash){
		if(eqchan(m->from, old, 1))
			break;
		l = &m->hash;
	}
	if(m == nil){
		/*
		 *  nothing mounted here yet.  create a mount
		 *  head and add to the hash table.
		 */
		m = newmhead(old);
		/*
		 *  if this is a union mount, add the old
		 *  node to the mount chain.
		 */
		if(order != MREPL)
			m->mount = newmount(old, 0, nil);
		*l = m;
	}
	wlock(&m->lock);
	um = m->mount;
	if(um != nil && order == MAFTER){
		for(f = um; f->next != nil; f = f->next)
			;
		f->next = nm;
		um = nil;
	} else {
		if(order != MREPL){
			for(f = nm; f->next != nil; f = f->next)
				;
			f->next = um;
			um = nil;
		}
		m->mount = nm;
	}
	order = nm->mountid;
	wunlock(&m->lock);
	wunlock(&pg->ns);

	mountfree(um);

	return order;
}

void
cunmount(Chan *mnt, Chan *mounted)
{
	Pgrp *pg;
	Mhead *m, **l;
	Mount *f, **p;

	if(mnt->umh != nil)	/* should not happen */
		print("cunmount newp extra umh %p has %p\n", mnt, mnt->umh);

	/*
	 * It _can_ happen that mounted->umh is non-nil, 
	 * because mounted is the result of namec(Aopen)
	 * (see sysfile.c:/^sysunmount).
	 * If we open a union directory, it will have a umh.
	 * Although surprising, this is okay, since the
	 * cclose will take care of freeing the umh.
	 */

	pg = up->pgrp;
	wlock(&pg->ns);

	l = &MOUNTH(pg, mnt->qid);
	for(m = *l; m != nil; m = m->hash){
		if(eqchan(m->from, mnt, 1))
			break;
		l = &m->hash;
	}

	if(m == nil){
		wunlock(&pg->ns);
		error(Eunmount);
	}

	wlock(&m->lock);
	f = m->mount;
	if(mounted == nil){
		*l = m->hash;
		m->mount = nil;
		wunlock(&m->lock);
		wunlock(&pg->ns);
		mountfree(f);
		putmhead(m);
		return;
	}
	for(p = &m->mount; f != nil; f = f->next){
		if(eqchan(f->to, mounted, 1) ||
		  (f->to->mchan != nil && eqchan(f->to->mchan, mounted, 1))){
			*p = f->next;
			f->next = nil;
			if(m->mount == nil){
				*l = m->hash;
				wunlock(&m->lock);
				wunlock(&pg->ns);
				mountfree(f);
				putmhead(m);
				return;
			}
			wunlock(&m->lock);
			wunlock(&pg->ns);
			mountfree(f);
			return;
		}
		p = &f->next;
	}
	wunlock(&m->lock);
	wunlock(&pg->ns);
	error(Eunion);
}

Chan*
cclone(Chan *c)
{
	Chan *nc;
	Walkqid *wq;

	if(c == nil || c->ref.ref < 1 || c->flag&CFREE)
		panic("cclone: %#p", getcallerpc(&c));
	wq = devtab[c->type]->walk(c, nil, nil, 0);
	if(wq == nil)
		error("clone failed");
	nc = wq->clone;
	free(wq);
	if((nc->path = c->path) != nil)
		incref(&c->path->ref);
	return nc;
}

/* also used by sysfile.c:/^mountfix */
int
findmount(Chan *volatile *cp, Mhead **mp, int type, int dev, Qid qid)
{
	Chan *to;
	Pgrp *pg;
	Mhead *m;

	pg = up->pgrp;
	rlock(&pg->ns);
	for(m = MOUNTH(pg, qid); m != nil; m = m->hash){
		if(eqchantdqid(m->from, type, dev, qid, 1)){
			if(mp != nil)
				incref(&m->ref);
			rlock(&m->lock);
			to = m->mount->to;
			incref(&to->ref);
			runlock(&m->lock);
			runlock(&pg->ns);
			if(mp != nil){
				putmhead(*mp);
				*mp = m;
			}
			if(*cp != nil)
				cclose(*cp);
			*cp = to;
			return 1;
		}
	}
	runlock(&pg->ns);
	return 0;
}

/*
 * Calls findmount but also updates path.
 */
static int
domount(Chan **cp, Mhead **mp, Path *volatile *path)
{
	Chan **lc, *from;
	Path *p;

	if(findmount(cp, mp, (*cp)->type, (*cp)->dev, (*cp)->qid) == 0)
		return 0;

	if(path != nil){
		p = *path;
		p = uniquepath(p);
		if(p->mlen <= 0)
			print("domount: path %s has mlen==%d\n", p->s, p->mlen);
		else{
			from = (*mp)->from;
			incref(&from->ref);
			lc = &p->mtpt[p->mlen-1];
			if(*lc != nil)
				cclose(*lc);
			*lc = from;
		}
		*path = p;
	}
	return 1;
}

/*
 * If c is the right-hand-side of a mount point, returns the left hand side.
 * Changes name to reflect the fact that we've uncrossed the mountpoint,
 * so name had better be ours to change!
 */
static Chan*
undomount(Chan *c, Path *path)
{
	Chan *nc;

	if(path->ref.ref != 1 || path->mlen == 0)
		print("undomount: path %s ref %ld mlen %d caller %#p\n",
			path->s, path->ref, path->mlen, getcallerpc(&c));

	if(path->mlen > 0 && (nc = path->mtpt[path->mlen-1]) != nil){
		cclose(c);
		path->mtpt[path->mlen-1] = nil;
		c = nc;
	}
	return c;
}

/*
 * Call dev walk but catch errors.
 */
static Walkqid*
ewalk(Chan *c, Chan *nc, char **name, int nname)
{
	Walkqid *wq;

	if(waserror())
		return nil;
	wq = devtab[c->type]->walk(c, nc, name, nname);
	poperror();
	return wq;
}

/*
 * Either walks all the way or not at all.  No partial results in *cp.
 * *nerror is the number of names to display in an error message.
 */
static char Edoesnotexist[] = "does not exist";
int
walk(Chan **cp, char **names, int nnames, int nomount, int *nerror)
{
	int dev, didmount, dotdot, i, n, nhave, ntry, type;
	Chan *c, *nc, *mtpt;
	Path *path;
	Mhead *mh, *nmh;
	Mount *f;
	Walkqid *wq;

	c = *cp;
	incref(&c->ref);
	path = c->path;
	incref(&path->ref);
	mh = nil;

	/*
	 * While we haven't gotten all the way down the path:
	 *    1. step through a mount point, if any
	 *    2. send a walk request for initial dotdot or initial prefix without dotdot
	 *    3. move to the first mountpoint along the way.
	 *    4. repeat.
	 *
	 * Each time through the loop:
	 *
	 *	If didmount==0, c is on the undomount side of the mount point.
	 *	If didmount==1, c is on the domount side of the mount point.
	 * 	Either way, c's full path is path.
	 */
	didmount = 0;
	for(nhave=0; nhave<nnames; nhave+=n){
		if((c->qid.type&QTDIR) == 0){
			if(nerror)
				*nerror = nhave;
			pathclose(path);
			cclose(c);
			kstrcpy(up->errstr, Enotdir, ERRMAX);
			putmhead(mh);
			return -1;
		}
		ntry = nnames - nhave;
		if(ntry > MAXWELEM)
			ntry = MAXWELEM;
		dotdot = 0;
		for(i=0; i<ntry; i++){
			if(isdotdot(names[nhave+i])){
				if(i==0){
					dotdot = 1;
					ntry = 1;
				}else
					ntry = i;
				break;
			}
		}

		if(!dotdot && !nomount && !didmount)
			domount(&c, &mh, &path);
		
		type = c->type;
		dev = c->dev;

		if((wq = ewalk(c, nil, names+nhave, ntry)) == nil){
			/* try a union mount, if any */
			if(mh != nil && !nomount){
				/*
				 * mh->mount->to == c, so start at mh->mount->next
				 */
				rlock(&mh->lock);
				if((f = mh->mount) != nil)
					f = f->next;
				for(; f != nil; f = f->next)
					if((wq = ewalk(f->to, nil, names+nhave, ntry)) != nil){
						type = f->to->type;
						dev = f->to->dev;
						break;
					}
				runlock(&mh->lock);
			}
			if(wq == nil){
				cclose(c);
				pathclose(path);
				if(nerror)
					*nerror = nhave+1;
				putmhead(mh);
				return -1;
			}
		}

		didmount = 0;
		if(dotdot){
			assert(wq->nqid == 1);
			assert(wq->clone != nil);

			path = addelem(path, "..", nil);
			nc = undomount(wq->clone, path);
			nmh = nil;
			n = 1;
		}else{
			nc = nil;
			nmh = nil;
			if(!nomount){
				for(i=0; i<wq->nqid && i<ntry-1; i++){
					if(findmount(&nc, &nmh, type, dev, wq->qid[i])){
						didmount = 1;
						break;
					}
				}
			}
			if(nc == nil){	/* no mount points along path */
				if(wq->clone == nil){
					cclose(c);
					pathclose(path);
					if(wq->nqid == 0 || (wq->qid[wq->nqid-1].type&QTDIR) != 0){
						if(nerror)
							*nerror = nhave+wq->nqid+1;
						kstrcpy(up->errstr, Edoesnotexist, ERRMAX);
					}else{
						if(nerror)
							*nerror = nhave+wq->nqid;
						kstrcpy(up->errstr, Enotdir, ERRMAX);
					}
					free(wq);
					putmhead(mh);
					return -1;
				}
				n = wq->nqid;
				nc = wq->clone;
			}else{		/* stopped early, at a mount point */
				assert(didmount);
				if(wq->clone != nil){
					cclose(wq->clone);
					wq->clone = nil;
				}
				n = i+1;
			}
			for(i=0; i<n; i++){
				mtpt = nil;
				if(i==n-1 && nmh!=nil)
					mtpt = nmh->from;
				path = addelem(path, names[nhave+i], mtpt);
			}
		}
		cclose(c);
		c = nc;
		putmhead(mh);
		mh = nmh;
		free(wq);
	}
	putmhead(mh);
	c = cunique(c);

	pathclose(c->path);
	c->path = path;

	cclose(*cp);
	*cp = c;
	if(nerror)
		*nerror = nhave;
	return 0;
}

/*
 * c is a mounted non-creatable directory.  find a creatable one.
 */
Chan*
createdir(Chan *c, Mhead *m)
{
	Chan *nc;
	Mount *f;

	rlock(&m->lock);
	if(waserror()){
		runlock(&m->lock);
		nexterror();
	}
	for(f = m->mount; f != nil; f = f->next){
		if((f->mflag&MCREATE) != 0){
			nc = cclone(f->to);
			runlock(&m->lock);
			poperror();
			cclose(c);
			return nc;
		}
	}
	error(Enocreate);
	return 0;
}

void
saveregisters(void)
{
}

static void
growparse(Elemlist *e)
{
	char **new;
	int *inew;
	enum { Delta = 8 };

	if((e->nelems % Delta) == 0){
		new = smalloc((e->nelems+Delta) * sizeof(char*));
		memmove(new, e->elems, e->nelems*sizeof(char*));
		free(e->elems);
		e->elems = new;
		inew = smalloc((e->nelems+Delta+1) * sizeof(int));
		memmove(inew, e->off, (e->nelems+1)*sizeof(int));
		free(e->off);
		e->off = inew;
	}
}

/*
 * The name is known to be valid.
 * Copy the name so slashes can be overwritten.
 * An empty string will set nelem=0.
 * A path ending in / or /. or /.//./ etc. will have
 * e.mustbedir = 1, so that we correctly
 * reject, e.g., "/adm/users/." when /adm/users is a file
 * rather than a directory.
 */
static void
parsename(char *aname, Elemlist *e)
{
	char *name, *slash;

	kstrdup(&e->name, aname);
	name = e->name;
	e->nelems = 0;
	e->elems = nil;
	e->off = smalloc(sizeof(int));
	e->off[0] = skipslash(name) - name;
	for(;;){
		name = skipslash(name);
		if(*name == '\0'){
			e->off[e->nelems] = name+strlen(name) - e->name;
			e->mustbedir = 1;
			break;
		}
		growparse(e);
		e->elems[e->nelems++] = name;
		slash = utfrune(name, '/');
		if(slash == nil){
			e->off[e->nelems] = name+strlen(name) - e->name;
			e->mustbedir = 0;
			break;
		}
		e->off[e->nelems] = slash - e->name;
		*slash++ = '\0';
		name = slash;
	}
}

static void
namelenerror(char *aname, int len, char *err)
{
	char *ename, *name, *next;
	int i, errlen;

	/*
	 * If the name is short enough, just use the whole thing.
	 */
	errlen = strlen(err);
	if(len < ERRMAX/3 || len+errlen < 2*ERRMAX/3)
		snprint(up->genbuf, sizeof up->genbuf, "%.*s", 
			utfnlen(aname, len), aname);
	else{
		/*
		 * Print a suffix of the name, but try to get a little info.
		 */
		ename = aname+len;
		next = ename;
		do{
			name = next;
			if(next == aname)
				break;
			while(next > aname)
				if(*--next == '/')
					break;
			len = ename-next;
		}while(len < ERRMAX/3 || len + errlen < 2*ERRMAX/3);

		/*
		 * If the name is ridiculously long, chop it.
		 */
		if(name == ename){
			name = ename-ERRMAX/4;
			if(name <= aname)
				panic("bad math in namelenerror");
			/* walk out of current UTF sequence */
			for(i=0; (*name&0xC0)==0x80 && i<UTFmax; i++)
				name++;
		}
		snprint(up->genbuf, sizeof up->genbuf, "...%.*s",
			utfnlen(name, ename-name), name);
	}				
	snprint(up->errstr, ERRMAX, "%#q %s", up->genbuf, err);
	nexterror();
}

void
nameerror(char *name, char *err)
{
	namelenerror(name, strlen(name), err);
}

/*
 * Turn a name into a channel.
 * &name[0] is known to be a valid address.  It may be a kernel address.
 *
 * Opening with amode Aopen, Acreate, Aremove, or Aaccess guarantees
 * that the result will be the only reference to that particular fid.
 * This is necessary since we might pass the result to
 * devtab[]->remove().
 *
 * Opening Atodir or Amount does not guarantee this.
 *
 * Under certain circumstances, opening Aaccess will cause
 * an unnecessary clone in order to get a cunique Chan so it
 * can attach the correct name.  Sysstat and sys_stat need the
 * correct name so they can rewrite the stat info.
 */
Chan*
namec(char *aname, int amode, int omode, ulong perm)
{
	int len, n, t, nomount;
	Chan *c;
	Chan *volatile cnew;
	Path *volatile path;
	Elemlist e;
	Rune r;
	Mhead *m;
	char *createerr, tmperrbuf[ERRMAX];
	char *name;

	if(aname[0] == '\0')
		error("empty file name");
	aname = validnamedup(aname, 1);
	if(waserror()){
		free(aname);
		nexterror();
	}
	name = aname;

	/*
	 * Find the starting off point (the current slash, the root of
	 * a device tree, or the current dot) as well as the name to
	 * evaluate starting there.
	 */
	nomount = 0;
	switch(name[0]){
	case '/':
		c = up->slash;
		incref(&c->ref);
		break;
	
	case '#':
		nomount = 1;
		up->genbuf[0] = '\0';
		n = 0;
		while(*name != '\0' && (*name != '/' || n < 2)){
			if((unsigned long)n >= sizeof(up->genbuf)-1)
				error(Efilename);
			up->genbuf[n++] = *name++;
		}
		up->genbuf[n] = '\0';
		/*
		 *  noattach is sandboxing.
		 *
		 *  the OK exceptions are:
		 *	|  it only gives access to pipes you create
		 *	d  this process's file descriptors
		 *	e  this process's environment
		 *  the iffy exceptions are:
		 *	c  time and pid, but also cons and consctl
		 *	p  control of your own processes (and unfortunately
		 *	   any others left unprotected)
		 */
		n = chartorune(&r, up->genbuf+1)+1;
		if(up->pgrp->noattach && utfrune("|decp", r)==nil)
			error(Enoattach);
		t = devno(r, 1);
		if(t == -1)
			error(Ebadsharp);
		c = devtab[t]->attach(up->genbuf+n);
		break;

	default:
		c = up->dot;
		incref(&c->ref);
		break;
	}

	e.aname = aname;
	e.prefix = name - aname;
	e.name = nil;
	e.elems = nil;
	e.off = nil;
	e.nelems = 0;
	e.nerror = 0;
	if(waserror()){
		cclose(c);
		free(e.name);
		free(e.elems);
		/*
		 * Prepare nice error, showing first e.nerror elements of name.
		 */
		if(e.nerror == 0)
			nexterror();
		strcpy(tmperrbuf, up->errstr);
		if(e.off[e.nerror]==0)
			print("nerror=%d but off=%d\n",
				e.nerror, e.off[e.nerror]);
		len = e.prefix+e.off[e.nerror];
		free(e.off);
		namelenerror(aname, len, tmperrbuf);
	}

	/*
	 * Build a list of elements in the name.
	 */
	parsename(name, &e);

	/*
	 * On create, ....
	 */
	if(amode == Acreate){
		/* perm must have DMDIR if last element is / or /. */
		if(e.mustbedir && !(perm&DMDIR)){
			e.nerror = e.nelems;
			error("create without DMDIR");
		}

		/* don't try to walk the last path element just yet. */
		if(e.nelems == 0)
			error(Eexist);
		e.nelems--;
	}

	if(walk(&c, e.elems, e.nelems, nomount, &e.nerror) < 0){
		if(e.nerror < 0 || e.nerror > e.nelems){
			print("namec %s walk error nerror=%d\n", aname, e.nerror);
			e.nerror = 0;
		}
		nexterror();
	}

	if(e.mustbedir && (c->qid.type&QTDIR) == 0)
		error("not a directory");

	if(amode == Aopen && (omode&3) == OEXEC && (c->qid.type&QTDIR) != 0)
		error("cannot exec directory");

	switch(amode){
	case Abind:
		/* no need to maintain path - cannot dotdot an Abind */
		m = nil;
		if(!nomount)
			domount(&c, &m, nil);
		if(waserror()){
			putmhead(m);
			nexterror();
		}
		c = cunique(c);
		c->umh = m;
		poperror();
		break;

	case Aaccess:
	case Aremove:
	case Aopen:
	Open:
		/* save&update the name; domount might change c */
		path = c->path;
		incref(&path->ref);
		if(waserror()){
			pathclose(path);
			nexterror();
		}
		m = nil;
		if(!nomount)
			domount(&c, &m, &path);
		if(waserror()){
			putmhead(m);
			nexterror();
		}
		/* our own copy to open or remove */
		c = cunique(c);
		poperror();

		/* now it's our copy anyway, we can put the name back */
		pathclose(c->path);
		c->path = path;
		poperror();

		/* record whether c is on a mount point */
		c->ismtpt = m!=nil;

		switch(amode){
		case Aaccess:
		case Aremove:
			putmhead(m);
			break;

		case Aopen:
		case Acreate:
			/* only save the mount head if it's a multiple element union */
			if(m != nil) {
				rlock(&m->lock);
				if(m->mount != nil && m->mount->next != nil) {
					c->umh = m;
					runlock(&m->lock);
				} else {
					runlock(&m->lock);
					putmhead(m);
				}
			}

			/* save registers else error() in open has wrong value of c saved */
			saveregisters();

			c = devtab[c->type]->open(c, omode&~OCEXEC);

			if(omode & OCEXEC)
				c->flag |= CCEXEC;
			if(omode & ORCLOSE)
				c->flag |= CRCLOSE;
			break;
		}
		break;

	case Atodir:
		/*
		 * Directories (e.g. for cd) are left before the mount point,
		 * so one may mount on / or . and see the effect.
		 */
		if((c->qid.type&QTDIR) == 0)
			error(Enotdir);
		break;

	case Amount:
		/*
		 * When mounting on an already mounted upon directory,
		 * one wants subsequent mounts to be attached to the
		 * original directory, not the replacement.  Don't domount.
		 */
		break;

	case Acreate:
		/*
		 * We've already walked all but the last element.
		 * If the last exists, try to open it OTRUNC.
		 * If omode&OEXCL is set, just give up.
		 */
		e.nelems++;
		e.nerror++;
		if(walk(&c, e.elems+e.nelems-1, 1, nomount, nil) == 0){
			if(omode&OEXCL)
				error(Eexist);
			omode |= OTRUNC;
			goto Open;
		}

		/*
		 * The semantics of the create(2) system call are that if the
		 * file exists and can be written, it is to be opened with truncation.
		 * On the other hand, the create(5) message fails if the file exists.
		 * If we get two create(2) calls happening simultaneously, 
		 * they might both get here and send create(5) messages, but only 
		 * one of the messages will succeed.  To provide the expected create(2)
		 * semantics, the call with the failed message needs to try the above
		 * walk again, opening for truncation.  This correctly solves the 
		 * create/create race, in the sense that any observable outcome can
		 * be explained as one happening before the other.
		 * The create/create race is quite common.  For example, it happens
		 * when two rc subshells simultaneously update the same
		 * environment variable.
		 *
		 * The implementation still admits a create/create/remove race:
		 * (A) walk to file, fails
		 * (B) walk to file, fails
		 * (A) create file, succeeds, returns 
		 * (B) create file, fails
		 * (A) remove file, succeeds, returns
		 * (B) walk to file, return failure.
		 *
		 * This is hardly as common as the create/create race, and is really
		 * not too much worse than what might happen if (B) got a hold of a
		 * file descriptor and then the file was removed -- either way (B) can't do
		 * anything with the result of the create call.  So we don't care about this race.
		 *
		 * Applications that care about more fine-grained decision of the races
		 * can use the OEXCL flag to get at the underlying create(5) semantics;
		 * by default we provide the common case.
		 *
		 * We need to stay behind the mount point in case we
		 * need to do the first walk again (should the create fail).
		 *
		 * We also need to cross the mount point and find the directory
		 * in the union in which we should be creating.
		 *
		 * The channel staying behind is c, the one moving forward is cnew.
		 */
		m = nil;
		cnew = nil;	/* is this assignment necessary? */
		if(!waserror()){	/* try create */
			if(!nomount && findmount(&cnew, &m, c->type, c->dev, c->qid))
				cnew = createdir(cnew, m);
			else{
				cnew = c;
				incref(&cnew->ref);
			}

			/*
			 * We need our own copy of the Chan because we're
			 * about to send a create, which will move it.  Once we have
			 * our own copy, we can fix the name, which might be wrong
			 * if findmount gave us a new Chan.
			 */
			cnew = cunique(cnew);
			pathclose(cnew->path);
			cnew->path = c->path;
			incref(&cnew->path->ref);

			cnew = devtab[cnew->type]->create(cnew, e.elems[e.nelems-1], omode&~(OEXCL|OCEXEC), perm);
			poperror();
			if(omode & OCEXEC)
				cnew->flag |= CCEXEC;
			if(omode & ORCLOSE)
				cnew->flag |= CRCLOSE;
			putmhead(m);
			cclose(c);
			c = cnew;
			c->path = addelem(c->path, e.elems[e.nelems-1], nil);
			break;
		}

		/* create failed */
		cclose(cnew);
		putmhead(m);
		if(omode & OEXCL)
			nexterror();
		/* save error */
		createerr = up->errstr;
		up->errstr = tmperrbuf;
		/* note: we depend that walk does not error */
		if(walk(&c, e.elems+e.nelems-1, 1, nomount, nil) < 0){
			up->errstr = createerr;
			error(createerr);	/* report true error */
		}
		up->errstr = createerr;
		omode |= OTRUNC;
		goto Open;

	default:
		panic("unknown namec access %d", amode);
	}

	/* place final element in genbuf for e.g. exec */
	if(e.nelems > 0)
		kstrcpy(up->genbuf, e.elems[e.nelems-1], sizeof up->genbuf);
	else
		kstrcpy(up->genbuf, ".", sizeof up->genbuf);
	free(e.name);
	free(e.elems);
	free(e.off);
	poperror();	/* e c */
	free(aname);
	poperror();	/* aname */

	return c;
}

/*
 * name is valid. skip leading / and ./ as much as possible
 */
char*
skipslash(char *name)
{
	while(name[0]=='/' || (name[0]=='.' && (name[1]==0 || name[1]=='/')))
		name++;
	return name;
}

char isfrog[256]={
	/*NUL*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*BKS*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*DLE*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*CAN*/	1, 1, 1, 1, 1, 1, 1, 1,
	['/'] = 1,
	[0x7f]= 1,
};

/*
 * Check that the name
 *  a) is in valid memory.
 *  b) is shorter than 2^16 bytes, so it can fit in a 9P string field.
 *  c) contains no frogs.
 * The first byte is known to be addressible by the requester, so the
 * routine works for kernel and user memory both.
 * The parameter slashok flags whether a slash character is an error
 * or a valid character.
 *
 * The parameter dup flags whether the string should be copied
 * out of user space before being scanned the second time.
 * (Otherwise a malicious thread could remove the NUL, causing us
 * to access unchecked addresses.) 
 */
static char*
validname0(char *aname, int slashok, int dup, uintptr pc)
{
	char *ename, *name, *s;
	int c, n;
	Rune r;

	name = aname;
	ename = memchr(name, 0, (1<<16));

	if(ename==nil || ename-name>=(1<<16))
		error(Etoolong);

	s = nil;
	if(dup){
		n = ename-name;
		s = smalloc(n+1);
		memmove(s, name, n);
		s[n] = 0;
		aname = s;
		name = s;
		setmalloctag(s, pc);
	}
	
	while(*name){
		/* all characters above '~' are ok */
		c = *(uchar*)name;
		if(c >= Runeself)
			name += chartorune(&r, name);
		else{
			if(isfrog[c])
			if(!slashok || c!='/'){
				snprint(up->genbuf, sizeof(up->genbuf), "%s: %q", Ebadchar, aname);
				free(s);
				error(up->genbuf);
			}
			name++;
		}
	}
	return s;
}

void
validname(char *aname, int slashok)
{
	validname0(aname, slashok, 0, getcallerpc(&aname));
}

char*
validnamedup(char *aname, int slashok)
{
	return validname0(aname, slashok, 1, getcallerpc(&aname));
}

void
isdir(Chan *c)
{
	if(c->qid.type & QTDIR)
		return;
	error(Enotdir);
}


enum
{
	DIRSIZE	= STATFIXLEN + 16 * 4		/* enough for encoded stat buf + some reasonable strings */
};

Dir*
dirchanstat(Chan *c)
{
	Dir *d;
	uchar *buf;
	int n, nd, i;

	nd = DIRSIZE;
	for(i=0; i<2; i++){	/* should work by the second try */
		d = smalloc(sizeof(Dir) + BIT16SZ + nd);
		if(waserror()){
			free(d);
			nexterror();
		}
		buf = (uchar*)&d[1];
		n = devtab[c->type]->stat(c, buf, BIT16SZ+nd);
		if(n < BIT16SZ)
			error(Eshortstat);
		nd = GBIT16(buf);	/* upper bound on size of Dir + strings */
		if(nd <= n){
			if(convM2D(buf, n, d, (char*)&d[1]) == 0)
				error(Eshortstat);
			poperror();
			return d;
		}
		/* else sizeof(Dir)+BIT16SZ+nd is plenty */
		free(d);
		poperror();
	}
	error(Eshortstat);
	return nil;
}
