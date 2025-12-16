/* 
 *	https://www.usb.org/sites/default/files/pos1_03.pdf
 */


#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "/sys/src/cmd/nusb/lib/usb.h"



typedef struct Devfile Devfile;


static	void	rstart(Srv *);
static	void	rend(Srv *);
static	void	ropen(Req *r);
static	void	rread(Req *r);
static	void	rwrite(Req *r);
static	char*	rdscale(Req *r);
static	char*	rdctl(Req *r);
static	char*	wrctl(Req *r);

struct Devfile {
	char	*name;
	char*	(*doread)(Req*);
	char*	(*dowrite)(Req*);
	int	mode;
	int this;
};


Devfile files[] = {
	{ "ctl", rdctl, wrctl, DMEXCL|0666 },
	{ "scale", rdscale, nil, 0444 },
};


Srv s = {
	.start = rstart,
	.open = ropen,
	.read = rread,
	.write = rwrite,
	.end = rend,
};


const char* unittab[13] = {
    "units",        // unknown unit
    "mg",           // milligram
    "g",            // gram
    "kg",           // kilogram
    "cd",           // carat
    "taels",        // lian
    "gr",           // grain
    "dwt",          // pennyweight
    "tonnes",       // metric tons
    "tons",         // avoir ton
    "ozt",          // troy ounce
    "oz",           // ounce
    "lbs"           // pound
};


char *devno;
int scalefd;
int tarefd;
Dev *d;		/* control */
Dev *dii;	/* intr in */
Dev *dio;	/* intr out */

int debug;


static void
rstart(Srv*)
{
	File *root;
	File *devdir;
	char* user;
	int i;

	user = getuser();
	s.tree = alloctree(user, user, 0555, nil);
	if(s.tree == nil)
		sysfatal("initfs: alloctree: %r");
	root = s.tree->root;
	if((devdir = createfile(root, "scalefs", user, DMDIR|0555, nil)) == nil)
		sysfatal("initfs: createfile: scalefs: %r");
	for(i = 0; i < nelem(files); i++)
		if(createfile(devdir, files[i].name, user, files[i].mode, files + i) == nil)
			sysfatal("initfs: createfile: %s: %r", files[i].name);
}


static void
rread(Req *r)
{
	Devfile *f;

	r->ofcall.count = 0;
	f = r->fid->file->aux;
	respond(r, f->doread(r));
}


static void
rwrite(Req *r)
{
	Devfile *f;

	if(r->ifcall.count == 0){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}

	f = r->fid->file->aux;
	respond(r, f->dowrite(r));
}


static void
ropen(Req *r)
{
	respond(r, nil);
}


static void
rend(Srv*)
{
	close(scalefd);
	close(tarefd);
	closedev(dii);
	closedev(dio);
	closedev(d);

	postnote(PNGROUP, getpid(), "shutdown");
	threadexitsall(nil);
}


static char*
rdscale(Req *r)
{
	char out[32];
	uchar in[6], page, status, unit, scaling, lsb, msb;
	int n;
	u16int raw;
	double weight;


	/* do 1 read to clear any past value */
	n = read (scalefd, in, sizeof(in));

	memset(in, 0, sizeof(in));
	sleep(10);

	n = read (scalefd, in, sizeof(in));

	if(n != 6)
		return("bad read");

	page = in[0];
	status = in[1];
	unit = in[2];
	scaling = in[3];
	lsb = in[4];
	msb = in[5];

	switch(status){
	case 1:
		return("scale fault");
		break;
	case 2:
		return("scale zero'd");
		break;
	case 3:
		return("in motion");
		break;
	case 4:
		raw = (u16int)lsb | ((u16int)msb << 8);
		weight = (double)raw;
		weight = weight / 10;
		sprint(out, "%.1f\n", weight);
		break;
	case 5: /* negative weight */
		raw = (u16int)lsb | ((u16int)msb << 8);
		weight = (double)raw;
		weight = weight / 10;
		weight *= -1;
		sprint(out, "%.1f\n", weight);
		break;
	case 6:
		return("over weight");
		break;
	default:
		return("other error");
		break;
	}

	readstr(r, out);
	return nil;
}


static char*
rdctl(Req *r)
{
	uchar raw1[3], page1, class1, unit1;
	uchar raw5[5], page5, unit5, scaling5, lsb5, msb5;
	int n;
	u16int temp;
	double weight;
	char out[256], *s, *e;

	/* 0x01 = get_report, 0x03.. = feature, 0x..01 = report ID 1 */
	n = usbcmd(d, Rd2h|Rclass|Riface, 0x01, 0x0301, 0, raw1, 3);

	if(n < 0)
		return("read fail report 1");

	/* 0x01 = get_report, 0x03.. = feature, 0x..05 = report ID 5 */
	n = usbcmd(d, Rd2h|Rclass|Riface, 0x01, 0x0305, 0, raw5, 5);

	if(n < 0)
		return("read fail report 5");

	page1 = raw1[0];
	class1 = raw1[1];
	unit1 = raw1[2];

	page5 = raw5[0];
	unit5 = raw5[1];
	scaling5 = raw5[2];
	lsb5 = raw5[3];
	msb5 = raw5[4];

	temp = (u16int)lsb5 | ((u16int)msb5 << 8);
	weight = (double)temp;
	weight /= 10;

	s = out;
	e = out + sizeof(out);
	s = seprint(s, e, "Page %uX\n", page1);
	s = seprint(s, e, "Class %uX\n", class1);
	s = seprint(s, e, "Units %s\n", unittab[unit1]);
	s = seprint(s, e, "Page %uX\n", page5);
	s = seprint(s, e, "Units %s\n", unittab[unit5]);
	s = seprint(s, e, "Scaling %uX\n", scaling5);
	s = seprint(s, e, "Max weight %.1f\n", weight);

	readstr(r, out);
	return(nil);	
}


static char*
wrctl(Req *r)
{
	char buf[32];
	char *cmd[2];
	int n, t;

	/* works on stamps.com scale */
	uchar tarecmd[2] = { 0x02, 0x01 };

	/* what USB docs say should work */
	/* uchar tarecmd[2] = { 0x02, 0x02 } */


	memmove(buf, r->ifcall.data, sizeof(buf));

	t = tokenize(buf, cmd, 2);

	if(!t)
		return("no tokens");

	if((strcmp(cmd[0], "tare")) != 0)
		return("bad command");

	n = write(tarefd, tarecmd, sizeof(tarecmd));

	if(n < 0)
		return("tare write fail");

	r->ofcall.count = r->ifcall.count;
	return nil;
}


static void
scalesetup(void)
{
	Usbdev *ud;
	Ep *epi, *epo;
	int i;

	if((d = getdev(devno)) == nil)
		sysfatal("getdev failed %s: %r", devno);

	ud = d->usb;

	/* setup interrupt in endpoint */
	for(i = 0; i < nelem(ud->ep); i++){
		if((epi = ud->ep[i]) == nil)
			continue;
		if(epi->type == Eintr && epi->dir == Ein)
			break;
	}

	dii = openep(d, epi);

	if(dii == nil)
		sysfatal("openep failed: %r");

	if(opendevdata(dii, OREAD) < 0)
		sysfatal("opendevdata: %r");

	/* for reading scale */
	scalefd = dii->dfd;

	/* setup interupt out endpoint */
	for(i = 0; i < nelem(ud->ep); i++){
		if((epo = ud->ep[i]) == nil)
			continue;
		if(epo->type == Eintr && epo->dir == Eout)
			break;
	}

	dio = openep(d, epo);

	if(dio == nil)
		sysfatal("openep failed: %r");

	if(opendevdata(dio, OWRITE) < 0)
		sysfatal("opendevdata: %r");

	/* for sending tare command */
	tarefd = dio->dfd;
}


void
usage(void)
{
	fprint(2, "usage: %s [-u devno] [-m mtpt] [-s service]\n", argv0);
	exits("usage");
}


void
threadmain(int argc, char *argv[])
{
	char *srvname, *mtpt, *addr;

	mtpt = "/mnt";
	srvname = "scalefs";

	ARGBEGIN {
	case 'u':
		devno = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'd':
		debug++;
		break;
	default:
		usage();
	} ARGEND;

	if(devno == nil)
		usage();

	scalesetup();

	threadpostmountsrv(&s, srvname, mtpt, MBEFORE);
	threadexits(nil);
}