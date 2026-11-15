#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

#define MAX_WORKERS 16
#define CAPACITY 131072
#define SAMPLE_NS 500000000LL
#define PERIOD_NS 10000000LL
#define SERVICE_NS 2000000LL

typedef struct { double response_ms, wake_ms; } Measurement;
typedef struct { _Atomic unsigned count; Measurement samples[CAPACITY]; } Shared;
typedef struct { unsigned n; double mean, p95, maximum; } Stats;
typedef struct { unsigned long long ticks, wait_ns; char state; int nice; bool valid; } Proc;
static volatile sig_atomic_t interrupted;
static pid_t children[MAX_WORKERS+1];
static unsigned child_count;
static Shared *shared;
static FILE *csv;
static void on_signal(int sig) { (void)sig; interrupted=1; }
static int64_t now_ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock,&t)) { perror("clock_gettime"); exit(1); }
    return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;
}
static void sleep_until(int64_t when) {
    struct timespec t={when/1000000000LL,when%1000000000LL};
    int rc;
    while ((rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL))==EINTR && !interrupted) {}
    if (rc && rc!=EINTR) { errno=rc; perror("clock_nanosleep"); exit(1); }
}
static void cleanup(void) {
    for(unsigned i=0;i<child_count;i++) if(children[i]>0) kill(children[i],SIGTERM);
    for(unsigned i=0;i<child_count;i++) while(waitpid(children[i],NULL,0)<0 && errno==EINTR) {}
    child_count=0;
}
static void pin(int cpu) {
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu,&mask);
    if(sched_setaffinity(0,sizeof(mask),&mask)) { perror("sched_setaffinity"); _exit(1); }
}
static pid_t spawn(int cpu, bool probe) {
    pid_t parent=getpid(), pid=fork();
    if(pid<0) { perror("fork"); exit(1); }
    if(pid) { children[child_count++]=pid; return pid; }
    signal(SIGINT,SIG_DFL); signal(SIGTERM,SIG_DFL);
    if(prctl(PR_SET_PDEATHSIG,SIGTERM)<0 || getppid()!=parent) _exit(1);
    pin(cpu);
    prctl(PR_SET_NAME,probe?"fd-probe":"fd-cpu-worker",0,0,0);
    volatile uint64_t value=1;
    if(!probe) for(;;) value=value*6364136223846793005ULL+1;
    int64_t release=now_ns(CLOCK_MONOTONIC)+PERIOD_NS;
    for(;;) {
        sleep_until(release);
        int64_t wake=now_ns(CLOCK_MONOTONIC), start=now_ns(CLOCK_PROCESS_CPUTIME_ID);
        do { for(int j=0;j<1000;j++) value=value*6364136223846793005ULL+1; }
        while(now_ns(CLOCK_PROCESS_CPUTIME_ID)-start<SERVICE_NS);
        int64_t finish=now_ns(CLOCK_MONOTONIC);
        unsigned n=atomic_load_explicit(&shared->count,memory_order_relaxed);
        if(n<CAPACITY) {
            shared->samples[n]=(Measurement){(finish-release)/1e6,(wake-release)/1e6};
            atomic_store_explicit(&shared->count,n+1,memory_order_release);
        }
        release+=PERIOD_NS;
        /* Skip missed releases: do not manufacture an ever-growing backlog. */
        if(release<finish) release=finish+PERIOD_NS;
    }
}
static Proc observe(pid_t pid) {
    Proc p={0}; char path[80],buf[4096];
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    FILE *f=fopen(path,"r"); if(!f) return p;
    if(!fgets(buf,sizeof(buf),f)) { fclose(f); return p; } fclose(f);
    char *tail=strrchr(buf,')'); if(!tail) return p;
    char *save=NULL,*token=strtok_r(tail+2," ",&save); int field=3;
    unsigned long long utime=0,stime=0;
    while(token) {
        if(field==3) p.state=token[0];
        if(field==14) utime=strtoull(token,NULL,10);
        if(field==15) stime=strtoull(token,NULL,10);
        if(field==19) p.nice=atoi(token);
        token=strtok_r(NULL," ",&save); field++;
    }
    p.ticks=utime+stime; p.valid=field>19;
    snprintf(path,sizeof(path),"/proc/%d/schedstat",pid);
    f=fopen(path,"r"); if(f) {
        unsigned long long runtime=0;
        if(fscanf(f,"%llu %llu",&runtime,&p.wait_ns)!=2) p.wait_ns=0;
        fclose(f);
    }
    return p;
}
static int cmp_double(const void *a,const void *b) { double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static Stats stats(unsigned begin,unsigned end) {
    Stats s={0}; if(end<=begin) return s;
    s.n=end-begin; double *v=malloc(s.n*sizeof(*v)); if(!v) { perror("malloc"); exit(1); }
    for(unsigned i=0;i<s.n;i++) { v[i]=shared->samples[begin+i].response_ms; s.mean+=v[i]; }
    s.mean/=s.n; qsort(v,s.n,sizeof(*v),cmp_double);
    s.maximum=v[s.n-1]; s.p95=v[(unsigned)ceil(.95*s.n)-1]; free(v); return s;
}
typedef struct { unsigned begin,end; Stats stats; } Phase;
static int streak,max_streak;
static bool detected;
static pid_t contributor;
static double contributor_cpu;
static Phase phase(const char *name,double seconds,pid_t *workers,int count,double threshold,bool analyze) {
    Phase result={0}; result.begin=atomic_load_explicit(&shared->count,memory_order_acquire);
    unsigned previous=result.begin; Proc prior[MAX_WORKERS];
    for(int i=0;i<count;i++) prior[i]=observe(workers[i]);
    int64_t start=now_ns(CLOCK_MONOTONIC),last=start,end=start+(int64_t)(seconds*1e9),next=start;
    printf("\n[%s] %.1f seconds\n",name,seconds);
    while(!interrupted && next<end) {
        next+=SAMPLE_NS; if(next>end) next=end; sleep_until(next); if(interrupted) break;
        int64_t current=now_ns(CLOCK_MONOTONIC);
        unsigned n=atomic_load_explicit(&shared->count,memory_order_acquire);
        Stats window=stats(previous,n); double total=0,best=0; pid_t best_pid=0;
        for(int i=0;i<count;i++) {
            Proc p=observe(workers[i]);
            if(!p.valid || p.state=='Z') { fprintf(stderr,"Worker %d exited unexpectedly\n",workers[i]); exit(1); }
            double cpu=(p.ticks-prior[i].ticks)*100.0/sysconf(_SC_CLK_TCK)/((current-last)/1e9);
            total+=cpu; if(cpu>best) {best=cpu;best_pid=workers[i];} prior[i]=p;
        }
        bool abnormal=window.n && window.mean>threshold && total>=30;
        if(analyze) {
            streak=abnormal?streak+1:0; if(streak>max_streak) max_streak=streak;
            if(streak>=3) { detected=true; contributor=best_pid; contributor_cpu=best; }
        }
        printf("  samples=%-3u response=%7.3f ms  p95=%7.3f ms  workload CPU=%5.1f%% %s\n",window.n,window.mean,window.p95,total,analyze&&abnormal?"DELAY + CONTENTION":"");
        for(unsigned i=previous;i<n;i++) fprintf(csv,"%s,%u,%.6f,%.6f,%.3f\n",name,i,shared->samples[i].response_ms,shared->samples[i].wake_ms,total);
        previous=n;last=current;
    }
    result.end=previous;
    result.stats=stats(result.begin,result.end); return result;
}
static void json_stats(FILE *f,const char *key,Stats s,bool comma) {
    fprintf(f,"  \"%s\": {\"samples\": %u, \"mean_ms\": %.6f, \"p95_ms\": %.6f, \"max_ms\": %.6f}%s\n",key,s.n,s.mean,s.p95,s.maximum,comma?",":"");
}
static void usage(const char *exe) {
    printf("Usage: %s [--seconds 2..60] [--workers 1..16] [--cpu N] [--report FILE] [--csv FILE] [--no-recovery]\nDefault: 4 seconds per phase, 4 workers, first allowed CPU, report.json, samples.csv.\nNo root required. Only child workloads are reniced.\n",exe);
}
int main(int argc,char **argv) {
    double seconds=4; int count=4,cpu=-1; bool recover=true;
    const char *report="report.json",*csv_path="samples.csv";
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--help")) {usage(argv[0]);return 0;}
        if(!strcmp(argv[i],"--no-recovery")) {recover=false;continue;}
        if(i+1>=argc) {usage(argv[0]);return 2;}
        const char *key=argv[i],*value=argv[++i]; char *tail;
        if(!strcmp(key,"--report")) report=value;
        else if(!strcmp(key,"--csv")) csv_path=value;
        else if(!strcmp(key,"--seconds")) {seconds=strtod(value,&tail);if(*tail||!isfinite(seconds)||seconds<2||seconds>60) return 2;}
        else if(!strcmp(key,"--workers")) {long v=strtol(value,&tail,10);if(*tail||v<1||v>MAX_WORKERS) return 2;count=(int)v;}
        else if(!strcmp(key,"--cpu")) {long v=strtol(value,&tail,10);if(*tail||v<0||v>=CPU_SETSIZE) return 2;cpu=(int)v;}
        else {usage(argv[0]);return 2;}
    }
    if(!strcmp(report,csv_path)) {fprintf(stderr,"Report and CSV paths must differ\n");return 2;}
    cpu_set_t allowed; if(sched_getaffinity(0,sizeof(allowed),&allowed)) {perror("sched_getaffinity");return 1;}
    if(cpu<0) for(int i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,&allowed)) {cpu=i;break;}
    if(cpu<0||!CPU_ISSET(cpu,&allowed)) {fprintf(stderr,"CPU is outside permitted affinity\n");return 2;}
    csv=fopen(csv_path,"w");if(!csv) {perror(csv_path);return 1;}
    FILE *out=fopen(report,"w");if(!out) {perror(report);fclose(csv);return 1;}
    shared=mmap(NULL,sizeof(*shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(shared==MAP_FAILED) {perror("mmap");return 1;}
    atomic_init(&shared->count,0);
    if(!atomic_is_lock_free(&shared->count)) {fprintf(stderr,"Lock-free shared atomics required\n");return 1;}
    atexit(cleanup); struct sigaction sa={0}; sa.sa_handler=on_signal;sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL);sigaction(SIGTERM,&sa,NULL);
    setvbuf(stdout,NULL,_IOLBF,0);
    fprintf(csv,"phase,sample,response_ms,wake_delay_ms,workload_cpu_percent\n");
    printf("FreezeDoctor | real Linux scheduling experiment\nCPU %d | %d workers | probe: 2 ms CPU service every 10 ms\n",cpu,count);
    pid_t probe=spawn(cpu,true),workers[MAX_WORKERS];
    Phase baseline=phase("baseline",seconds,NULL,0,0,false);
    double threshold=fmax(baseline.stats.mean*1.5,baseline.stats.mean+1.0);
    if(interrupted) {fclose(csv);fclose(out);return 130;}
    for(int i=0;i<count;i++) workers[i]=spawn(cpu,false);
    Phase before=phase("contention",seconds,workers,count,threshold,true);
    bool applied=false; int original_nice[MAX_WORKERS]={0},adjusted=0;
    Proc worker_before[MAX_WORKERS],worker_after[MAX_WORKERS];
    for(int i=0;i<count;i++) worker_before[i]=observe(workers[i]);
    int64_t intervention_ns=0;
    if(!interrupted&&detected&&recover) {
        intervention_ns=now_ns(CLOCK_MONOTONIC);
        printf("\nDETECTED: %d consecutive abnormal windows; contributor PID %d, %.1f%% CPU\n",max_streak,contributor,contributor_cpu);
        /* Every worker is an owned contributor in this controlled experiment. */
        for(int i=0;i<count;i++) {
            errno=0; original_nice[i]=getpriority(PRIO_PROCESS,workers[i]);
            if(errno) {perror("getpriority");continue;}
            if(setpriority(PRIO_PROCESS,workers[i],19)) {perror("setpriority");continue;}
            errno=0; int actual=getpriority(PRIO_PROCESS,workers[i]);
            if(!errno&&actual==19&&original_nice[i]<19) {adjusted++;printf("Recovery: PID %d nice %d -> %d\n",workers[i],original_nice[i],actual);}
        }
        applied=adjusted==count;
    }
    Phase after={0};
    if(!interrupted) after=phase("verification",seconds,workers,count,threshold,false);
    bool verified=!interrupted&&baseline.stats.n>=10&&detected&&applied&&before.stats.n>=10&&after.stats.n>=10&&after.stats.mean<before.stats.mean*.9&&after.stats.p95<before.stats.p95;
    double improvement=before.stats.mean>0?(before.stats.mean-after.stats.mean)*100/before.stats.mean:0;
    Proc probe_info=observe(probe);
    for(int i=0;i<count;i++) worker_after[i]=observe(workers[i]);
    fprintf(out,"{\n  \"schema_version\": 1,\n  \"cpu\": %d,\n  \"workers\": %d,\n  \"probe_pid\": %d,\n  \"contributor_pid\": %d,\n  \"contributor_cpu_percent\": %.3f,\n  \"threshold_ms\": %.6f,\n  \"persistent_windows\": %d,\n  \"contention_detected\": %s,\n  \"priority_adjustment_applied\": %s,\n  \"adjusted_workers\": %d,\n  \"recovery_verified\": %s,\n  \"interrupted\": %s,\n  \"improvement_percent\": %.3f,\n  \"probe_scheduler_wait_ns\": %llu,\n",cpu,count,probe,contributor,contributor_cpu,threshold,max_streak,detected?"true":"false",applied?"true":"false",adjusted,verified?"true":"false",interrupted?"true":"false",improvement,probe_info.wait_ns);
    fprintf(out,"  \"intervention_monotonic_ns\": %lld,\n  \"workload_evidence\": [\n",(long long)intervention_ns);
    for(int i=0;i<count;i++) fprintf(out,"    {\"pid\": %d, \"state_before\": \"%c\", \"nice_before\": %d, \"nice_after\": %d, \"cpu_ticks_before\": %llu, \"cpu_ticks_after\": %llu}%s\n",workers[i],worker_before[i].state?worker_before[i].state:'?',worker_before[i].nice,worker_after[i].nice,worker_before[i].ticks,worker_after[i].ticks,i+1<count?",":"");
    fprintf(out,"  ],\n");
    json_stats(out,"baseline",baseline.stats,true);json_stats(out,"before",before.stats,true);json_stats(out,"after",after.stats,false);fprintf(out,"}\n");
    bool io_error=ferror(csv)||ferror(out);if(fclose(csv))io_error=true;if(fclose(out))io_error=true;
    cleanup();munmap(shared,sizeof(*shared));
    printf("\n%s\nBefore %.3f ms -> after %.3f ms (%.1f%% mean improvement)\nReports: %s, %s\n",verified?"RECOVERY VERIFIED":interrupted?"INTERRUPTED":"RECOVERY NOT VERIFIED",before.stats.mean,after.stats.mean,improvement,report,csv_path);
    if(io_error) {fprintf(stderr,"Failed writing report\n");return 1;}
    return interrupted?130:0;
}
