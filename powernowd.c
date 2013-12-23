/*
 * powernowd.c: (c) 2003-2008 John Clemens <clemej@alum.rpi.edu>
 *
 * Released under the GNU General Public License. See the LICENSE file
 * included with this file.
 *
 * * Changelog:
 *   v0.75, initial release.
 *   v0.80, add syslog support, -s,-p,-u,-l options, clean up error reporting
 *          some (still some more work needed), made packaging better, removed
 *          find_mount() code.. /sys is the blessed place, so be it.. no sense
 *          having unnecessary code and the maintenance thereof.  fixed bug 
 *          where mode was actually defaulting to SINE, not AGGRESSIVE. 
 *   v0.85, Minor memory init fixes, add clearer error messages, check for 
 *   	    root, added -b and -n options, add pause support (SIGUSR1/2, -b 
 *   	    option), added #ifdef'd-out code to handle buggy athlons which
 *   	    might not be necessary anymore, fixed some help text bugs, added
 *   	    a few more comments.
 *   v0.90, Support drivers that report speed in Mhz instead of Khz. Removed
 *   	    buggy athlon workaround, as it's not needed. Removed pause/unpause
 *   	    code. Added sample powernowd init script to show how to emulate
 *   	    old pause/unpause behavior a cleaner way. Added LEAPS mode and 
 *   	    verbosity patches by Hans Ulrich Niedermann. Cleaned up verbosity
 *   	    handling more.
 *   v0.95, Added proper HT support, and SMT/CMP infrastructure. Hope it 
 *   	    works. Added scalaing_available_frequencies support and made 
 *   	    default.  
 *   v0.96, Fix HT detection code to default to 1 if cpuid gives strange
 *          result (fixes centrinos). Cleaned up error reporting in 
 *          read_file() (fixes non-SAF drivers. i.e. pmac).
 *   v0.97, Better handling of multi-core/HT cpus, and fixed a lot of SMP 
 *          bugs in general.  Now uses affected_cpus if it's there.  Fixed 
 *          braindead think-o in the multi-cpu speed change logic.  Random
 *          other small cleanups/suggestions from debian bugtrack.  Fix
 *          Segfault on exit on SMP systems.
 *   v1.00, Small fixes and cleanups for a 1.00 release.  s/strtol/strtoll to
 *          handle large uptimes (from Debian/Ubuntu package).  Fix harmless 
 *          buffer overrun (non-exploitable).  valgrind clean.
 *
 *   
 * * Contributions from:
 * 	Warren Togami <warren@togami.com>
 * 	Michael Schreiter <Michael_Schreiter@gmx.de>
 * 	Wolfgang Tremmel <tremmel@garf.de>
 * 	Hans Ulrich Niedermann <kdev@n-dimensional.de>
 * 	Bdale Garbee, Vince Weaver
 * 	-- And many others who have sent me patches over the years,
 * 	   some of whom I've never even acknowledged.  Just know that your 
 * 	   support is appreciated, even if I didn't merge in your ideas.  
 * 	   Thank you.
 */


#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#define pprintf(level, ...) do { \
	if (level <= verbosity) { \
		if (daemonize) \
			syslog(LOG_INFO, __VA_ARGS__); \
		else \
			printf(__VA_ARGS__); \
	} \
} while(0)

typedef struct cpustats {
	unsigned long long user;
	unsigned long long mynice;
	unsigned long long system;
	unsigned long long idle;
	unsigned long long iowait;
	unsigned long long irq;
	unsigned long long softirq;
} cpustats_t;

typedef struct cpuinfo {
	unsigned int cpuid;
	unsigned int nspeeds;
	unsigned int max_speed;
	unsigned int min_speed;
	unsigned int current_speed;
	unsigned int speed_index;
	int fd;
	char *sysfs_dir;
	cpustats_t *last_reading;
	cpustats_t *reading;
	int in_mhz; /* 0 = speed in kHz, 1 = speed in mHz */
	unsigned long *freq_table;
	int table_size;
	int threads_per_core;
	int scalable_unit;
} cpuinfo_t;

/* 
 * Global array that contains a pointer to all CPU info. 
 */

cpuinfo_t **all_cpus;

/* idea stolen from procps */
static char buf[2048];

enum function {
	SINE,
	AGGRESSIVE,
	PASSIVE,
	LEAPS
} func = AGGRESSIVE; 

enum modes {
	LOWER,
	SAME,
	RAISE
};

/* for a daemon as simple as this, global data is ok. */
/* settings */
int daemonize = 1;
int ignore_nice = 1;
int verbosity = 0;
unsigned int step = 100000;  /* in kHz */
unsigned int poll = 1000; /* in msecs */
unsigned int highwater = 80;
unsigned int lowwater = 20;
unsigned int max_limit = 0;
unsigned int min_limit = 0;
unsigned int step_specified = 0;
unsigned int t_per_core = 1;
unsigned int cores_specified = 0;

/* statistics */
unsigned int change_speed_count = 0;
time_t start_time = 0;

#define SYSFS_TREE "/sys/devices/system/cpu/"
#define SYSFS_SETSPEED "scaling_setspeed"

#define VERSION	"1.00"

void help(void)
{
	
	printf("PowerNow Daemon v%s, (c) 2003-2008 John Clemens\n", VERSION);
	printf("Daemon to control the speed and voltage of cpus.\n\n");
	printf("This is a simple client to the CPUFreq driver, and uses\n");
	printf("linux kernel v2.5+ sysfs interface.  You need a supported\n");
	printf("cpu, and a kernel that supports sysfs to run this daemon.\n");
	printf("\nAvailable Options:\n");
	printf(" 	-h	Print this help message\n");
	printf("	-d	Don't detach from terminal (default is to\n");
	printf("		detach and run in the background)\n");
	printf("	-v	Increase output verbosity, can be used more than once.\n");
	printf("	-q	Quiet mode, only emergency output.\n");
	printf("	-n	Include 'nice'd processes in calculations\n");
	printf("	-m #	Modes of operation, can be 0, 1, 2, or 3:\n");
	printf("		0 = SINE, 1 = AGGRESSIVE (default),\n");
	printf("		2 = PASSIVE, 3 = LEAPS\n");
	printf("	-s #	Frequency step in kHz (default = 100000)\n");
	printf("	-p #	Polling frequency in msecs (default = 1000)\n");
	printf("	-c #	Specify number of threads per power-managed core\n");
	printf("	-u #	CPU usage upper limit percentage [0 .. 100, default 80]\n");
	printf("	-l #    CPU usage lower limit percentage [0 .. 100, default 20]\n");

	printf("\n");
	return;
}

/* 
 * Open a file and copy it's first 1024 bytes into the global "buf".
 * Zero terminate the buffer. 
 */
int read_file(const char *file, int fd, int new)
{
	int n, err;
	
	if (new) {
		if ((fd = open(file, O_RDONLY)) == -1) {
			err = errno;
			perror(file);
			return err;
		}
	}
	
	lseek(fd, 0, SEEK_SET);
	if ((n = read(fd, buf, sizeof(buf)-1)) < 0) {
		err = errno;
		perror(file);
		close(fd);
		return err;
	}
	buf[n] = '\0';

	if (new)
		close(fd);
	
	return 0;
}

/*
 * Reads /proc/stat into buf, and parses the output.
 *
 * Format of line:
 * ...
 * cpu<id> <user> <nice> <system> <idle> <iowait> <irq> <softirq>
 */
int get_stat(cpuinfo_t *cpu)
{
	char *p1, *p2, searchfor[10];
	int err;
	
	if ((err = read_file("/proc/stat", cpu->fd, 0)) != 0) {
		return err;
	}

	sprintf(searchfor, "cpu%d ", cpu->cpuid);

	p1 = strstr(buf, searchfor);
	if (p1 == NULL) {
		perror("Error parsing /proc/stat");
		return ENOENT;
	}
	
	p2 = p1+strlen(searchfor);

	memcpy(cpu->last_reading, cpu->reading, sizeof(cpustats_t));
	
	cpu->reading->user = strtoll(p2, &p2, 10);
	cpu->reading->mynice = strtoll(p2, &p2, 10);
	cpu->reading->system = strtoll(p2, &p2, 10);
	cpu->reading->idle = strtoll(p2, &p2, 10);
	cpu->reading->iowait = strtoll(p2, &p2, 10);
	cpu->reading->irq = strtoll(p2, &p2, 10);
	cpu->reading->softirq = strtoll(p2, &p2, 10);

	return 0;
}

/*
 * Once a decision is made, change the speed.
 */

int change_speed(cpuinfo_t *cpu, enum modes mode)
{
	int fd, len, err, i;
	cpuinfo_t *save;
	char writestr[100];

	if (cpu->cpuid != cpu->scalable_unit) 
		return 0;
	
	if (mode == RAISE) {
		if ((func == AGGRESSIVE) || (func == LEAPS)) {
			cpu->speed_index = 0;
		} else {
			if (cpu->speed_index != 0) cpu->speed_index--;
		} 
	} else {
		if ((func == PASSIVE) || (func == LEAPS)) {
			cpu->speed_index = (cpu->table_size-1);
		} else {
			if (cpu->speed_index != (cpu->table_size-1))
				cpu->speed_index++;
		}
	}

	/* 
	 * We need to set the current speed on all virtual CPUs that fall
	 * into this CPU's scalable unit.
	 */
	save = cpu;
	for (i = save->cpuid; i < (save->cpuid + save->threads_per_core); i++) {
		cpu = all_cpus[i];
		cpu->current_speed = save->freq_table[save->speed_index];
	}
	cpu = save;

	pprintf(3,"Setting speed to %d\n", cpu->current_speed);

	change_speed_count++;
	
	strncpy(writestr, cpu->sysfs_dir, 50);
	strncat(writestr, SYSFS_SETSPEED, 20);
	
	if ((fd = open(writestr, O_WRONLY)) < 0) {
		err = errno;
		perror("Can't open scaling_setspeed");
		return err;
	}

	lseek(fd, 0, SEEK_CUR);
	
	sprintf(writestr, "%d\n", (cpu->in_mhz) ?
			(cpu->current_speed / 1000) : cpu->current_speed); 

	pprintf(4,"mode=%d, str=%s", mode, writestr);
	
	if ((len = write(fd, writestr, strlen(writestr))) < 0) {
		err = errno;
		perror("Couldn't write to scaling_setspeed\n");
		return err;
	}

	if (len != strlen(writestr)) {
		printf("Could not write scaling_setspeed\n");
		close(fd);
		return EPIPE;
	}
	close(fd);

	return 0;
}

/*
 * The heart of the program... decide to raise or lower the speed.
 */
enum modes inline decide_speed(cpuinfo_t *cpu)
{
	int err;
	float pct;
	unsigned long long usage, total;
	
	if ((err = get_stat(cpu)) < 0) {
		perror("Can't get stats");
		return err;
	}

	total = (cpu->reading->user - cpu->last_reading->user) +
		(cpu->reading->system - cpu->last_reading->system) +
		(cpu->reading->mynice - cpu->last_reading->mynice) +
		(cpu->reading->idle - cpu->last_reading->idle) +
		(cpu->reading->iowait - cpu->last_reading->iowait) +
		(cpu->reading->irq - cpu->last_reading->irq) +
		(cpu->reading->softirq - cpu->last_reading->softirq);

	if (ignore_nice) { 
		usage = (cpu->reading->user - cpu->last_reading->user) +
			(cpu->reading->system - cpu->last_reading->system) +
			(cpu->reading->irq - cpu->last_reading->irq) +
			(cpu->reading->softirq - cpu->last_reading->softirq);
	} else {
		usage = (cpu->reading->user - cpu->last_reading->user) +
			(cpu->reading->mynice - cpu->last_reading->mynice) +
			(cpu->reading->system - cpu->last_reading->system) +
			(cpu->reading->irq - cpu->last_reading->irq) +
			(cpu->reading->softirq - cpu->last_reading->softirq);
	}
	
	pct = ((float)usage)/((float)total);
	
	pprintf(4,"PCT = %f\n", pct);
	
	if ((pct >= ((float)highwater/100.0)) && 
			(cpu->current_speed != cpu->max_speed)) {
		/* raise speed to next level */
		pprintf(6, "got here RAISE\n"); 
		return RAISE;
	} else if ((pct <= ((float)lowwater/100.0)) && 
			(cpu->current_speed != cpu->min_speed)) {
		/* lower speed */
		pprintf(6, "got here LOWER\n"); 
		return LOWER;
	}
	
	return SAME;
}

/* 
 * Abuse glibc's qsort.  Compare function to sort list of frequencies in 
 * ascending order.
 */
int faked_compare(const void *a, const void *b)
{
	unsigned long *a1 = (unsigned long *)a;
	unsigned long *b1 = (unsigned long *)b;

	if (*a1 < *b1) return 1;
	if (*a1 > *b1) return -1;

	return 0;
}

/*
 * Allocates and initialises the per-cpu data structures.
 */
int get_per_cpu_info(cpuinfo_t *cpu, int cpuid)
{
	char cpustr[100], scratch[100], tmp[11], *p1;
	int fd, err;
	unsigned long temp;
	
	cpu->cpuid = cpuid;
	cpu->sysfs_dir = (char *)malloc(50*sizeof(char));
	if (cpu->sysfs_dir == NULL) {
		perror("Couldn't allocate per-cpu sysfs_dir");
		return ENOMEM;
	}
	memset(cpu->sysfs_dir, 0, (50*sizeof(char)));

	strncpy(cpu->sysfs_dir, SYSFS_TREE, 30);
	sprintf(cpustr, "cpu%d/cpufreq/", cpuid);
	strncat(cpu->sysfs_dir, cpustr, 20);
	
	strncpy(scratch, cpu->sysfs_dir, 50);
	strncat(scratch, "cpuinfo_max_freq", 18);
	if ((err = read_file(scratch, 0, 1)) != 0) {
		return err;
	}
	
	cpu->max_speed = strtol(buf, NULL, 10);
	
	strncpy(scratch, cpu->sysfs_dir, 50);
	strncat(scratch, "cpuinfo_min_freq", 18);

	if ((err = read_file(scratch, 0, 1)) != 0) {
		return err;
	}

	cpu->min_speed = strtol(buf, NULL, 10);

	/* 
	 * More error handling, make sure step is not larger than the 
	 * difference between max and min speeds. If so, truncate it.
	 */
	if (step > (cpu->max_speed - cpu->min_speed)) {
		step = cpu->max_speed - cpu->min_speed;
	}
	
	/* XXXjc read the real current speed */
	cpu->current_speed = cpu->max_speed;
	cpu->speed_index = 0;

	strncpy(scratch, cpu->sysfs_dir, 50);
	strncat(scratch, "scaling_available_frequencies", 50);

	if (((err = read_file(scratch, 0, 1)) != 0) || (step_specified)) {
		/* 
		 * We don't have scaling_available_frequencies. build the
		 * table from the min, max, and step values.  the driver
		 * could ignore these, but we'll represent it this way since
		 * we don't have any other info.
		 */
		cpu->table_size = ((cpu->max_speed-cpu->min_speed)/step) + 1;
		cpu->table_size += ((cpu->max_speed-cpu->min_speed)%step)?1:0;
		
		cpu->freq_table = (unsigned long *)
			malloc(cpu->table_size*sizeof(unsigned long));

		if (cpu->freq_table == (unsigned long *)NULL) {
			perror("couldn't allocate cpu->freq_table");
			return ENOMEM;
		}

		/* populate the table.  Start at the top, and subtract step */
		for (temp = 0; temp < cpu->table_size; temp++) {
			cpu->freq_table[temp] = 
			((cpu->min_speed<(cpu->max_speed-(temp*step))) ? 
			 (cpu->max_speed-(temp*step)) :
			 (cpu->min_speed) );
		}	
	} else {
		/* 
		 * We do have the file, parse it and build the table from
		 * there.
		 */ 
		/* The format of scaling_available_frequencies (SAF) is:
		 * "number<space>number2<space>...numberN<space>\n", but this
		 * can change. So we're relying on the fact that strtoul will 
		 * return 0 if it can't find anything, and that 0 will never 
		 * be a real value for the available frequency. 
		 */
		p1 = buf;
		
		temp = strtoul(p1, &p1, 10);
		while((temp > 0) && (cpu->table_size < 100)) {
			cpu->table_size++;
			temp = strtoul(p1, &p1, 10);
		}
	
		cpu->freq_table = (unsigned long *)
			malloc(cpu->table_size*sizeof(unsigned long));
		if (cpu->freq_table == (unsigned long *)NULL) {
			perror("Couldn't allocate cpu->freq_table\n");
			return ENOMEM;
		}
	
		p1 = buf;
		for (temp = 0; temp < cpu->table_size; temp++) {
			cpu->freq_table[temp] = strtoul(p1, &p1, 10);
		}
	}

	/* now lets sort the table just to be sure */
	qsort(cpu->freq_table, cpu->table_size, sizeof(unsigned long), 
			&faked_compare);
	
	strncpy(scratch, cpu->sysfs_dir, 50);
	strncat(scratch, "scaling_governor", 20);

	if ((err = read_file(scratch, 0, 1)) != 0) {
		perror("couldn't open scaling_governors file");
		return err;
	}

	if (strncmp(buf, "userspace", 9) != 0) {
		if ((fd = open(scratch, O_RDWR)) < 0) {
			err = errno;
			perror("couldn't open govn's file for writing");
			return err;
		}
		strncpy(tmp, "userspace\n", 11);
		if (write(fd, tmp, 11*sizeof(char)) < 0) {
			err = errno;
			perror("Error writing file governor");
			close(fd);
			return err;
		}
		if ((err = read_file(scratch, fd, 0)) != 0) {
			perror("Error reading back governor file");
			close(fd);
			return err;
		}
		close(fd);
		if (strncmp(buf, "userspace", 9) != 0) {
			perror("Can't set to userspace governor, exiting");
			return EPIPE;
		}
	}
	
	cpu->last_reading = (cpustats_t *)malloc(sizeof(cpustats_t));
	cpu->reading = (cpustats_t *)malloc(sizeof(cpustats_t));
	memset(cpu->last_reading, 0, sizeof(cpustats_t));
	memset(cpu->reading, 0, sizeof(cpustats_t));
	
	/*
	 * Some cpufreq drivers (longhaul) report speeds in MHz instead
	 * of KHz.  Assume for now that any currently supported cpufreq 
	 * processor will a) not be faster then 10GHz, and b) not be slower
	 * then 10MHz. Therefore, is the number for max_speed is less than
	 * 10000, assume the driver is reporting speeds in MHz, not KHz,
	 * and adjust accordingly.
	 *
	 * XXXjc the longhaul driver has been fixed (2.6.5ish timeframe)
	 * so this should't be needed anymore.  Remove for 1.0?
	 */
	cpu->in_mhz = 0;
	if (cpu->max_speed <= 10000) {
		cpu->in_mhz = 1;
		cpu->max_speed *= 1000;
		cpu->min_speed *= 1000;
		cpu->current_speed *= 1000;
	}
	
	if ((cpu->fd = open("/proc/stat", O_RDONLY)) < 0) {
		err = errno;
		perror("can't open /proc/stat");
		return err;
	}
	
	if ((err = get_stat(cpu)) < 0) {
		perror("can't read /proc/stat");
		return err;
	}
	
	return 0;
}

/*
 * Signal handler for SIGTERM/SIGINT... clean up after ourselves
 */
void terminate(int signum)
{
	int ncpus, i;
	cpuinfo_t *cpu;
	
	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpus < 1) ncpus = 1;
	
	/* 
	 * for each cpu, force it back to full speed.
	 * don't mix this with the below statement.
	 * 
	 * 5 minutes ago I convinced myself you couldn't 
	 * mix these two, now I can't remember why.  
	 */
	for(i = 0; i < ncpus; i++) {
		cpu = all_cpus[i];
		func = LEAPS;
		change_speed(cpu, RAISE);
	}

	for(i = 0; i < ncpus; i++) {
		cpu = all_cpus[i];
		/* close the /proc/stat fd */
		close(cpu->fd);
		/* deallocate everything */
		free(cpu->sysfs_dir);
		free(cpu->last_reading);
		free(cpu->reading);
		free(cpu->freq_table);
		free(cpu);
	}
	free(all_cpus);
	time_t duration = time(NULL) - start_time;
	pprintf(1,"Statistics:\n");
	pprintf(1,"  %d speed changes in %d seconds\n",
			change_speed_count, (unsigned int) duration);
	pprintf(0,"PowerNow Daemon Exiting.\n");

	closelog();

	exit(0);
}

const char *str_func(void)
{
	switch (func) {
		case SINE: return "SINE";
		case AGGRESSIVE: return "AGGRESSIVE";
		case PASSIVE: return "PASSIVE";
		case LEAPS: return "LEAPS";
		default: return "UNKNOWN";
	}
}

/* Generic x86 cpuid function lifted from kernel sources */
/*
 * Generic CPUID function
 */
static inline void cpuid(int op, int *eax, int *ebx, int *ecx, int *edx)
{
        __asm__("cpuid"
                : "=a" (*eax),
                  "=b" (*ebx),
                  "=c" (*ecx),
                  "=d" (*edx)
                : "0" (op));
}

/* 
 * A little bit of black magic to try and detect the number of cores per
 * processor.  This will have to be added on to for every architecture, 
 * as we learn how to detect them from userspace.  Note: This method
 * assumes uniform processor/thread ID's.  First look for affected_cpus, 
 * if not, then fall back to the cpuid check, or just default to 1. 
 *
 * By 'thread' in this case, I mean 'entity that is part of one scalable
 * instance'.  For example, a P4 with hyperthreading has 2 threads in 
 * one scalable instace.  So does an Athlon X2 dual core, because each
 * core has to have the same speed.  The new Yonah, on the other hand, may
 * have two scalable elements, as rumors say you can control both cores' 
 * speed individually.  Lets hope the speedstep driver populates affected_cpus
 * correctly...
 *
 * You can always override this by using the -c command line option to 
 * specify the number of threads per core.  If you do so, it will do a static
 * mapping, uniform for all real processors in the system.  Actually, so 
 * will this one, because there's no way for me to bind to a processor.
 * (yet. :)
 */
int determine_threads_per_core(int ncpus)
{
	char filename[100], *p1;
	int err, count;

	/* if ncpus is one, we don't care */
	if (ncpus == 1) return 1;
	
	/* 
	 * First look for the affected_cpus file, and count the 
	 * number of cpus that supports.  Assume this is true for all
	 * cpus on the system.
	 */
	strncpy(filename, SYSFS_TREE, 30);
	strncat(filename, "cpu0/cpufreq/affected_cpus", 99-strlen(filename));
	
	/* 
	 * OK, the funkiest system I can think of right now is
	 * Sun's Niagara processor, which I think would have 32
	 * "cpus" in one scalable element.  So make this robust 
	 * enough to at least handle more than 32 affected cpus 
	 * at once.
	 *
	 * NOTE: I don't even know if Niagara supports scaling, 
	 * I'm dealing with hypotheticals. 
	 */
	
	count = 1;
	
	if ((err = read_file(filename, 0, 1)) == 0) {
		p1 = buf;
		err = strtoul(p1, &p1, 5);
		/* 
		 * The first cpu should always be 0, so err should be 0
		 * after the first read.  If its anything else, default to
		 * one, print a message, and move on. 
		 */
		if (err != 0) {
			pprintf(0, "WARN: cpu0 scaling doesn't affect cpu 0?"
				       " Assuming 1 thread per core.\n");
			return 1;
		}
		while ((err = strtol(p1, &p1, 5)) != 0)
			count ++;
		pprintf(1, "about to return count = %d\n", count);
		return count;	
	}
	pprintf(0,"err=%d", err);
	
#ifdef __i386__ 
	/* Only get here if there's no affected_cpus file. */
	/* 
	 * XXXjc fix eventually to run on each processor so you
	 * can support mixed multi and single-core cpus. Need to know
	 * how to force ourselves to run on one particular processor.
	 */
	int eax,ebx,ecx,edx, num=1;

	cpuid(1,&eax,&ebx,&ecx,&edx);

	/* 
	 * Do we support hyperthreading?
	 * AMD's dual-core will masquerade as HT, so this should work 
	 * for them too. (update: it does but this doesn't emulate
	 * the extra ebx parameter, appearently).
	 */
	if(edx & 0x08000000) { 
		/* 
		 * if so, is it enabled? If so, how many threads 
		 * are enabled?  Thank you, sandpile.org and the LKML. 
		 */
		num = (ebx & 0x00FF0000) >> 16;
	}
	/* 
	 * if num = 0, default to 1.  Other non-multiples of cpus will
	 * be taken care of later
	 * XXXjc, rewrite to use sysfs (affected_cpus).
	 */
	return ((num)?num:1);
#endif
	/* always default to one thread per core */
	return 1;
}


/* 
 * Main program loop.. parse arguments, sanity chacks, setup signal handlers
 * and then enter main loop
 */
int main(int argc, char **argv)
{
	cpuinfo_t *cpu;
	int ncpus, i, j, err, num_real_cpus, threads_per_core, cpubase;
	enum modes change, change2;

	/* Parse command line args */
	while(1) {
		int c;

		c = getopt(argc, argv, "dnvqm:s:p:c:u:l:U:L:h");
		if (c == -1)
			break;

		switch(c) {
			case 'd':
				daemonize = 0;
				break;
			case 'n':
				ignore_nice = 0;
				break;
 			case 'v':
 				verbosity++;
				if (verbosity > 10) verbosity = 10;
 				break;
 			case 'q':
 				verbosity = -1;
 				break;
			case 'c':
				t_per_core = strtol(optarg, NULL, 10);
				if (t_per_core < 1) {
					printf("invalid number of cores/proc");
					help();
					exit(ENOTSUP);
				}
				cores_specified = 1;
				break;
			case 'm':
				func = strtol(optarg, NULL, 10);
				if ((func < 0) || (func > 3)) {
					printf("Invalid mode specified");
					help();
					exit(ENOTSUP);
				}
				pprintf(2,"Using %s mode.\n", str_func());
				break;
			case 's':
				step = strtol(optarg, NULL, 10);
				if (step < 0) {
					printf("step must be non-negative");
					help();
					exit(ENOTSUP);
				}
				step_specified = 1;
				pprintf(2,"Using %dHz step.\n", step);
				break;
			case 'p':
				poll = strtol(optarg, NULL, 10);
				if (poll < 0) {
					printf("poll must be non-negative");
					help();
					exit(ENOTSUP);
				}
				pprintf(2,"Polling every %d msecs\n", poll);
				break;
			case 'u':
				highwater = strtol(optarg, NULL, 10);
				if ((highwater < 0) || (highwater > 100)) {
					printf("upper limit must be between 0 and 100\n");
					help();
					exit(ENOTSUP);
				}
				pprintf(2,"Using upper pct of %d%%\n",highwater);
				break;
			case 'l':
				lowwater = strtol(optarg, NULL, 10);
				if ((lowwater < 0) || (lowwater > 100)) {
					printf("lower limit must be between 0 and 100\n");
					help();
					exit(ENOTSUP);
				}
				pprintf(2,"Using lower pct of %d%%\n",lowwater);
				break;
			case 'h':
			default:
				help();
				return 0;
		}
	}

	/* last things to check... */
	if (lowwater > highwater) {
		printf("Invalid: lower pct higher than upper pct!\n");
		help();
		exit(ENOTSUP);
	}
	
	/* so we don't interfere with anything, including ourself */
	nice(5);
	
	if (daemonize)
		openlog("powernowd", LOG_AUTHPRIV|LOG_PERROR, LOG_DAEMON);
	
	/* My ego's pretty big... */
	pprintf(0,"PowerNow Daemon v%s, (c) 2003-2008 John Clemens\n", 
			VERSION);

	/* are we root?? */
	if (getuid() != 0) {
		printf("Go away, you are not root. Only root can run me.\n");
		exit(EPERM);
	}

	pprintf(1,"Settings:\n");
	pprintf(1,"  verbosity:     %4d\n", verbosity);
	pprintf(1,"  mode:          %4d     (%s)\n", func, str_func());
	pprintf(1,"  step:          %4d MHz (%d kHz)\n", step/1000, step);
	pprintf(1,"  lowwater:      %4d %%\n", lowwater);
	pprintf(1,"  highwater:     %4d %%\n", highwater);
	pprintf(1,"  poll interval: %4d ms\n", poll);

	/* 
	 * This should tell us the number of CPUs that Linux thinks we have,
	 * or, at least GLIBC
	 */	
	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpus < 0) {
		perror("sysconf could not determine number of cpus, assuming 1\n");
		ncpus = 1;
	}
	
	if (cores_specified) {
		if (ncpus < t_per_core) {
			printf("\nWARNING: bogus # of thread per core, assuming 1\n");
			threads_per_core = 1;
		} else {
			threads_per_core = t_per_core;
		}
	} else { 
		threads_per_core = determine_threads_per_core(ncpus);
		if (threads_per_core < 0) 
			threads_per_core = 1;
	}
	
	/* We don't support mixed configs yet */
	if (!ncpus || !threads_per_core || ncpus % threads_per_core) {	
		printf("WARN: ncpus(%d) is not a multiple of threads_per_core(%d)!\n",
			ncpus, threads_per_core);
		printf("WARN: Assuming 1.\n");
		threads_per_core = 1;
		/*help();
		exit(ENOTSUP);*/
	}
	
	num_real_cpus = ncpus/threads_per_core;

	/* Malloc, initialise data structs */
	all_cpus = (cpuinfo_t **) malloc(sizeof(cpuinfo_t *)*ncpus);
	if (all_cpus == (cpuinfo_t **)NULL) {
		perror("Couldn't malloc all_cpus");
		return ENOMEM;
	}
	
	for (i=0; i<ncpus; i++) {
		all_cpus[i] = (cpuinfo_t *)malloc(sizeof(cpuinfo_t));
		if (all_cpus[i] == (cpuinfo_t *)NULL) {
			perror("Couldn't malloc all_cpus");
			return ENOMEM;
		}
		memset(all_cpus[i],0,sizeof(cpuinfo_t));
	}
	
	for (i=0;i<ncpus;i++) {
		all_cpus[i]->threads_per_core = threads_per_core;
		all_cpus[i]->scalable_unit = (i/threads_per_core)*threads_per_core;
	}
	
	pprintf(0,"Found %d scalable unit%s:  -- %d 'CPU%s' per scalable unit\n",
			num_real_cpus,
			(num_real_cpus>1)?"s":"",
			threads_per_core,
			(threads_per_core>1)?"s":"");
	
	for (i=0;i<ncpus;i++) {
		cpu = all_cpus[i];
		if ((err = get_per_cpu_info(cpu, i)) != 0) {
			printf("\n");
			goto out;
		}
		pprintf(0,"  cpu%d: %dMhz - %dMhz (%d steps)\n", 
				cpu->cpuid,
				cpu->min_speed / 1000, 
				cpu->max_speed / 1000, 
				cpu->table_size);
		for(j=0;j<cpu->table_size; j++) {
			pprintf(1, "     step%d : %ldMhz\n", j+1, 
					cpu->freq_table[j] / 1000);
		}
	}
	
	/* now that everything's all set up, lets set up a exit handler */
	signal(SIGTERM, terminate);
	signal(SIGINT, terminate);
	
	if (daemonize)
		daemon(0, 0);

	start_time = time(NULL);

	/* Now the main program loop */
	while(1) {
		usleep(poll*1000);
		for(i=0; i<num_real_cpus; i++) {
			change = LOWER;
			cpubase = i*threads_per_core;
			pprintf(6, "i = %d, cpubase = %d, ",i,cpubase);
			/* handle SMT/CMP here */
			for (j=0; j<all_cpus[cpubase]->threads_per_core; j++) {
				change2 = decide_speed(all_cpus[cpubase+j]);
				pprintf(6, "change = %d, change2 = %d\n",change,change2);
				if (change2 > change)
					change = change2;
			}
			if (change != SAME) change_speed(all_cpus[cpubase], change);
		}
	}

out:
	printf("PowerNowd encountered and error and could not start.\n");
	printf("Please make sure that:\n");
	printf(" - You are running a v2.6.7 kernel or later\n");
	printf(" - That you have sysfs mounted /sys\n");
	printf(" - That you have the core cpufreq and cpufreq-userspace\n");
	printf("   modules loaded into your kernel\n");
	printf(" - That you have the cpufreq driver for your cpu loaded,\n");
	printf("   (for example: powernow-k7), and that it works. Check\n"); 
	printf("   'dmesg' for errors.\n");
	printf("If all of the above are true, and you still have problems,\n");
	printf("please email the author: clemej@alum.rpi.edu\n");
	
	/* should free more here.. will get to that later.... */
	/* or we can just be lazy and let the OS do it for us... */
	free(cpu);
	return err;
}

