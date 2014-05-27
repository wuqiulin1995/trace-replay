#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <pthread.h>
#include <libaio.h>
#include <errno.h>
#include <signal.h>

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>


#include "flist.h"
#include "trace_replay.h"

FILE *result_fp;
struct thread_info_t th_info[MAX_THREADS];
struct trace_info_t traces[MAX_THREADS];
int cnt=0;
int cnt2=0;
int nr_thread;
int nr_trace;
pthread_spinlock_t spinlock;
struct timeval tv_start, tv_end, tv_result;
double execution_time = 0.0;
unsigned long genrand();
#define RND(x) ((x>0)?(genrand() % (x)):0)


int timeval_subtract (result, x, y)
 struct timeval *result, *x, *y;
{
  /* Perform the carry for the later subtraction by
  * updating y. */
  if (x->tv_usec < y->tv_usec) {
   int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
   y->tv_usec -= 1000000 * nsec;
   y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
  	    int nsec = (y->tv_usec - x->tv_usec) / 1000000;
	    y->tv_usec += 1000000 * nsec;
	    y->tv_sec -= nsec;
 }
							  	
  /* Compute the time remaining to wait.
  * 	     tv_usec  is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;
						  	
  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}

void sum_time(struct timeval *t1,struct timeval *t2)
{
	if((t1->tv_usec + t2->tv_usec ) < 1000000){
		t1->tv_usec += t2->tv_usec;
		t1->tv_sec += t2->tv_sec;
	}else{
		int nsec = (t1->tv_usec + t2->tv_usec) / 1000000;
		t1->tv_usec += t2->tv_usec;
		t1->tv_usec -= 1000000*nsec;
		t1->tv_sec += t2->tv_sec;
		t1->tv_sec += nsec;
	}
}

static double time_since(struct timeval *start_tv, struct timeval *stop_tv)
{
    double sec, usec;
    double ret;
    sec = stop_tv->tv_sec - start_tv->tv_sec;
    usec = stop_tv->tv_usec - start_tv->tv_usec;
    if (sec > 0 && usec < 0) {
        sec--;
	usec += 1000000;
    } 
    ret = sec + usec / (double)1000000;
    if (ret < 0)
        ret = 0;
    return ret;
}

/*
 * return seconds between start_tv and now in double precision
 */
static double time_since_now(struct timeval *start_tv)
{
    struct timeval stop_time;
    gettimeofday(&stop_time, NULL);
    return time_since(start_tv, &stop_time);
}

/* allocate a alignment-bytes aligned buffer */
void *allocate_aligned_buffer(size_t size)
{
	void *p;

#if 0 
	posix_memalign(&p, getpagesize(), size);
#else
	p=(void *)memalign(getpagesize(), size);
	//p = malloc(size);

#endif

	if(!p) {
		perror("memalign");
		exit (0);
		return NULL;
	}

	return p;
}


float tv_to_sec(struct timeval *tv){
	return (float)tv->tv_sec + (float)tv->tv_usec/1000000;
}

/* Fatal error handler */
static void io_error(const char *func, int rc)
{
	if (rc == -ENOSYS)
		fprintf(stderr, "AIO not in this kernel");
	else if (rc < 0)
		fprintf(stderr, "%s: %s", func, strerror(rc));
	else
		fprintf(stderr, "%s: error %d", func, rc);

}



void align_sector(struct thread_info_t *t_info, int *blkno, int *bcount){
	struct trace_info_t *trace = t_info->trace;
	int pageno = *blkno / SPP;
	int pcount;
	
	pageno %= trace->total_pages;

	if(*bcount % SPP){
		pcount = *bcount/SPP + 1;
		*bcount = pcount*SPP;
	}
	if(*bcount % SPP){
		printf(" bcount error %d \n", *bcount % SPP);
	}
	pcount = *bcount/SPP;

	if(pageno+pcount >= trace->total_pages){
		pageno-=pcount;
	}

	*blkno = pageno * SPP;
#if 0
	*bcount=SPP;
	*blkno = 0;
#endif 
#if 0
	*bcount=SPP;
	*blkno = RND(t_info->total_pages)*SPP;
#endif 
}

void update_iostat(struct thread_info_t *t_info, struct io_job *job){
	struct io_stat_t *io_stat = &t_info->io_stat;

	gettimeofday(&job->stop_time, NULL);

	io_stat->latency_sum += time_since(&job->start_time, &job->stop_time);
	io_stat->latency_count ++;

	io_stat->total_bytes += job->bytes;
	if(job->rw)
		io_stat->total_rbytes += job->bytes;
	else
		io_stat->total_wbytes += job->bytes;
}

struct simple_bio {
	int devno;
	int blkno;
	int bcount;
	int flags;
};

void trace_reset(struct trace_info_t *trace){
	pthread_spin_lock(&trace->trace_lock);
	trace->trace_io_cur = 0;
	pthread_spin_unlock(&trace->trace_lock);
}

int trace_eof(struct trace_info_t *trace){
	int res = 0;

	pthread_spin_lock(&trace->trace_lock);
	if(trace->trace_io_cur>=trace->trace_io_cnt){
		res = 1;
		goto exit;
	}
	pthread_spin_unlock(&trace->trace_lock);

exit:
	return res;
}

int trace_io_get(double* arrival_time, int* devno, int* blkno, int*bcount, int* flags, struct trace_info_t *trace){
	struct trace_io_req* io;
	int res = 0;

	pthread_spin_lock(&trace->trace_lock);
	if(trace->trace_io_cur>=trace->trace_io_cnt){
		res = -1;
		goto exit;
	}
	
	io = &(trace->trace_buf[trace->trace_io_cur]);
	*arrival_time = io->arrival_time;
	*devno = io->devno;
	*bcount = io->bcount;
	*blkno = io->blkno;
	*flags = io->flags;

	trace->trace_io_cur++;
	pthread_spin_unlock(&trace->trace_lock);

exit:
	return res;
}

int make_jobs(struct thread_info_t *t_info){
	struct io_job *job;
	char line[201];
	double arrival_time;
	int devno;
	int blkno;
	int bcount;
	int flags;
	int i,j;
	struct io_stat_t *io_stat = &t_info->io_stat;
	struct simple_bio bio[128];
	int sb_cnt = 0;
	int temp,temp2;
//	while(!feof(t_info->trace_fp)){
	for(i = 0;i < t_info->queue_depth;i++){
		unsigned long page;
		struct trace_info_t *trace = t_info->trace;
		//job = (struct io_job *)malloc(sizeof(struct io_job));
		job = t_info->th_jobs[i];
		
		if(trace_io_get(&arrival_time, &devno, &blkno, &bcount, &flags, t_info->trace)){
			return -1;
		}
		/*
		if (fgets(line, 200, t_info->trace_fp) == NULL) {
			//printf(" fges error \n");
			return -1;
		}
		
		if (sscanf(line, "%lf %d %d %d %x\n", &arrival_time, &devno, &blkno, &bcount, &flags) != 5) {
			fprintf(stderr, "Wrong number of arguments for I/O trace event type\n");
			fprintf(stderr, "line: %s", line);
			return -1;
		}
		*/
		cnt++;
		if(cnt % 100 == 0 && io_stat->latency_count>0 && io_stat->execution_time>0)
		{

			printf("time: %lf bandwidth = %f Latency = %f\r",arrival_time, (double)io_stat->total_bytes/MB/io_stat->execution_time, (double)io_stat->latency_sum/io_stat->latency_count);
		}
	
		/*
		bcount=8;
		if(cnt % 10 == 0){
			if(cnt2 == 0)
			{
				cnt2 = 1;
				cnt -= 10;
			}

			else if(cnt2 ==1)
			{
				cnt -= 10;
				cnt2++;
			}
			else
				cnt2=0;
		}
		blkno = cnt*1000;
		*/


#if 1
		align_sector(t_info, &blkno, &bcount);
//		printf( "%lf %d %d %d %x\n", arrival_time, devno, blkno, bcount, flags);
		job->offset = (long long)blkno * SECTOR_SIZE;
		job->bytes = (size_t)bcount * SECTOR_SIZE;
		if(job->bytes>(size_t)MAX_BYTES)
			job->bytes = MAX_BYTES;

		if(flags)
			job->rw = 1;
		else
			job->rw = 0;
#else
		page = RND(t_info->total_pages);
		job->offset = (long long)page * PAGE_SIZE;
		job->bytes = PAGE_SIZE;
		job->rw = 0;
#endif
		bio[i].bcount = bcount;
		bio[i].blkno = blkno;
		bio[i].devno = devno;
		bio[i].flags = flags;
		
#if 0 
		for( j=0; j<i; j++ ){
			if( bio[i].devno == bio[j].devno ) {
	
				if(bio[i].flags == bio[j].flags
				&& (bio[i].blkno == ( bio[j].blkno + bio[j].bcount - 8))) {
					blkno += 8;
					bcount -= 8;
			
					if(bcount <= 0)
					{
						trace->trace_io_cur--;
						return 0;
					}
					job->offset = (long long)blkno * SECTOR_SIZE;
					job->bytes = (size_t)bcount * SECTOR_SIZE;
					goto out;
					
				}
#if 0 
				temp =  bio[i].blkno - ( bio[j].blkno + bio[j].bcount -1);
				temp2 =   ((bio[i].blkno + bio[i].bcount - 1) - bio[j].blkno) ;
				
				temp = (temp > 0) ? 1 : -1;
				temp2 = (temp2 > 0) ? 1 : -1;

				if( temp * temp2 < 0) {
					trace->trace_io_cur--;
					return 0;
				}
#endif 
			}
			
		}
#endif 
out:
		//printf( "%d: %lf %d %d %d %x\n", i, arrival_time, devno, blkno, bcount, flags);
		//job->buf = allocate_aligned_buffer(job->bytes);
		job->buf = t_info->th_buf[i];

		gettimeofday(&job->start_time, NULL);
		flist_add_tail(&job->list, &t_info->queue);
		t_info->queue_count++;

	}
	return 0;
}

int  remove_ioq(struct thread_info_t *t_info, struct iocb **ioq){
	struct flist_head *ptr, *tmp;
	struct io_job *job;
	int cnt = 0;

	flist_for_each_safe(ptr, tmp, &t_info->queue){
		job = flist_entry(ptr, struct io_job, list);
		flist_del(&job->list);
		ioq[cnt] = &job->iocb;

		//if(job->rw)
		//	io_prep_pread(&job->iocb, t_info->fd, job->buf, job->bytes, job->offset);
		//else
		//	io_prep_pwrite(&job->iocb, t_info->fd, job->buf, job->bytes, job->offset);

		//cnt++;
		t_info->queue_count--;

		//free(job->buf);
		//free(job);
	}

	return cnt;
}

int  make_ioq(struct thread_info_t *t_info, struct iocb **ioq){
	struct flist_head *ptr, *tmp;
	struct io_job *job;
	struct trace_info_t *trace = t_info->trace;
	int cnt = 0;

	flist_for_each_safe(ptr, tmp, &t_info->queue){
		job = flist_entry(ptr, struct io_job, list);
		flist_del(&job->list);
		ioq[cnt] = &job->iocb;

		if(job->rw)
			io_prep_pread(&job->iocb, trace->fd, job->buf, job->bytes, job->offset);
		else
			io_prep_pwrite(&job->iocb, trace->fd, job->buf, job->bytes, job->offset);

		cnt++;
		t_info->queue_count--;
	}

	return cnt;
}

void wait_completion(struct thread_info_t *t_info, int cnt){
	struct io_job *job;
	int complete_count = 0;
	int i;

	while(cnt){
		complete_count = io_getevents(t_info->io_ctx, 1, cnt, t_info->events, NULL);
		if(complete_count < 0){
			printf(" io error \n");
		}
		for(i = 0;i < complete_count;i++){
			job = (struct io_job *)((unsigned long)t_info->events[i].obj);
			//printf(" tid = %d,no = %d blkno = %d, bytes = %d, lat = %f\n",
			//		(int) t_info->tid, i, (int)job->offset, (int)job->bytes,
			//	time_since(&job->start_time, &job->stop_time));

			update_iostat(t_info, job);

		//	free(job->buf);
		//	free(job);
		}
		cnt-=complete_count;
	}

}
#if USE_MAINWORKER == 0
void *sub_worker(void *threadid)
{
	long tid = (long)threadid;
	struct thread_info_t *t_info = &th_info[tid];
	struct trace_info_t *trace = t_info->trace;
	struct io_stat_t *io_stat = &t_info->io_stat;
	struct iocb *ioq[MAX_QDEPTH];
	int rc;
	int iter = 0;
	
	int cnt = 0;

	//printf (" pthread start id = %d \n", (int)tid);
	printf(" Starting thread %d ... %s\n", (int)tid, trace->tracename);

	gettimeofday(&io_stat->start_time, NULL);

	while(1){
		//printf(" tid %d qcount %d \n", (int)tid, th_info[tid].queue_count);
		rc = make_jobs(t_info);
		//if(rc<0)
		//	goto check_timeout;
#if 1
		cnt = make_ioq(t_info, ioq);
		if(!cnt){
			if(trace_eof(trace))
				goto check_timeout;
		}
		
		rc = io_submit(t_info->io_ctx, cnt, ioq);
		if (rc < 0)
			io_error("io_submit", rc);

		wait_completion(t_info, cnt);
#else

		remove_ioq(t_info, ioq);
#endif 

		iter++;
		if(iter>100){
			gettimeofday(&io_stat->end_time, NULL);
			io_stat->execution_time = time_since(&io_stat->start_time, &io_stat->end_time);
			if(io_stat->execution_time >trace->timeout && trace->timeout>0.0){
				goto Timeout;
			}
		}

check_timeout:

		//if(feof(t_info->trace_fp)){
		if(trace_eof(trace)){
			if(trace->timeout && io_stat->execution_time < trace->timeout){
				trace_reset(trace);
				io_stat->trace_repeat_count++;
				printf(" repeat trace file thread %d ... %s\n", (int)tid, trace->tracename);
			}else{
				goto Timeout;
			}
		}
	}

Timeout:

	gettimeofday(&io_stat->end_time, NULL);
	io_stat->execution_time = time_since(&io_stat->start_time, &io_stat->end_time);
	printf(" Finalizing thread %d ... %s\n", (int)tid, trace->tracename);

	//printf (" pthread end id = %d \n", (int)tid);


	return NULL;
}
#endif 

int print_result(int nr_trace, int nr_thread, FILE *fp){
	unsigned long long total_bytes = 0;
	unsigned long long total_rbytes = 0;
	unsigned long long total_wbytes = 0;
	unsigned long long total_ios = 0;
	int per_thread = nr_thread / nr_trace;
	int i, j;
	
	for(i = 0;i < nr_trace;i++){
		struct io_stat_t io_stat_dst;
		unsigned long long bytes = 0;
		unsigned long long rbytes = 0;
		unsigned long long wbytes = 0;
		unsigned long long ios = 0;
		memset(&io_stat_dst, 0x00, sizeof(struct io_stat_t));

		for(j = 0;j < per_thread;j++){
			int th_num = i * per_thread + j;
			struct io_stat_t *io_stat_src = &th_info[th_num].io_stat;

			io_stat_dst.latency_sum+= io_stat_src->latency_sum;
			io_stat_dst.latency_count += io_stat_src->latency_count;
			io_stat_dst.total_operations += io_stat_src->total_operations;
			io_stat_dst.total_bytes+= io_stat_src->total_bytes;
			io_stat_dst.total_rbytes+= io_stat_src->total_rbytes;
			io_stat_dst.total_wbytes+= io_stat_src->total_wbytes;
			io_stat_dst.total_error_bytes+= io_stat_src->total_error_bytes;
			io_stat_dst.execution_time+= io_stat_src->execution_time;
			io_stat_dst.trace_repeat_count+=io_stat_src->trace_repeat_count;
		}

		fprintf(fp, "\n");

		fprintf(fp, " Per Trace %d I/O statistics \n", i);
		fprintf(fp, " Trace name = %s \n", traces[i].tracename);
		io_stat_dst.execution_time = io_stat_dst.execution_time/per_thread;
		fprintf(fp, " Execution time = %f sec\n", io_stat_dst.execution_time);
		fprintf(fp, " Avg latency = %f sec\n", (double)io_stat_dst.latency_sum/io_stat_dst.latency_count);
		fprintf(fp, " IOPS = %f\n", io_stat_dst.latency_count/io_stat_dst.execution_time);
		fprintf(fp, " Bandwidth (total) = %f MB/s\n", (double)io_stat_dst.total_bytes/MB/io_stat_dst.execution_time);
		fprintf(fp, " Bandwidth (read) = %f MB/s\n", (double)io_stat_dst.total_rbytes/MB/io_stat_dst.execution_time);
		fprintf(fp, " Bandwidth (write) = %f MB/s\n", (double)io_stat_dst.total_wbytes/MB/io_stat_dst.execution_time);
		fprintf(fp, " Total traffic = %f MB\n", (double)io_stat_dst.total_bytes/MB);
		fprintf(fp, " Read traffic = %f MB\n", (double)io_stat_dst.total_rbytes/MB);
		fprintf(fp, " Write traffic = %f MB\n", (double)io_stat_dst.total_wbytes/MB);
		fprintf(fp, " Trace reset count = %d\n", io_stat_dst.trace_repeat_count);

		total_bytes += io_stat_dst.total_bytes;
		total_rbytes += io_stat_dst.total_rbytes;
		total_wbytes += io_stat_dst.total_wbytes;
		total_ios += io_stat_dst.latency_count;
	}

	fprintf(fp, "\n Aggregrated Result \n");
	fprintf(fp, " Agg Execution time: %.6f sec\n", execution_time);
	fprintf(fp, " Agg IOPS = %f \n", (double)total_ios/execution_time);
	fprintf(fp, " Agg Total bandwidth = %f MB/s \n", (double)total_bytes/MB/execution_time);
	fprintf(fp, " Agg Read bandwidth = %f MB/s \n", (double)total_rbytes/MB/execution_time);
	fprintf(fp, " Agg Write bandwidth = %f MB/s \n", (double)total_wbytes/MB/execution_time);
	fprintf(fp, " Agg Total traffic = %f MB\n", (double)total_bytes/MB);
	fprintf(fp, " Agg Read traffic = %f MB\n", (double)total_rbytes/MB);
	fprintf(fp, " Agg Write traffic = %f MB\n", (double)total_wbytes/MB);
	fflush(fp);
}

#define ARG_QDEPTH 1
#define ARG_THREAD 2
#define ARG_OUTPUT 3
#define ARG_TIMEOUT 4
#define ARG_DEV 5
#define ARG_TRACE 6

void usage_help(){
	printf("\n Invalid command!!\n");
	printf(" Usage:\n");
	printf(" #./trace_replay qdepth per_thread output timeout devicefile tracefile1 tracefile2\n");
	printf(" #./trace_replay 32 2 result.txt 60 /dev/sdb1 trace.dat trace.dat\n\n");
}

void finalize(){
	gettimeofday(&tv_end, NULL);
	timeval_subtract(&tv_result, &tv_end, &tv_start);
	execution_time = time_since(&tv_start, &tv_end);

	//print_result(nr_thread, stdout);
	print_result(nr_trace, nr_thread, result_fp);
	fclose(result_fp);
	fprintf(stdout, " Finalizing Trace Replayer \n");
}

void sig_handler(int signum)
{
	printf("Received signal %d\n", signum);

	finalize();

	signal( SIGINT, SIG_DFL);
	exit(0);
}



int trace_io_put(char* line, struct trace_info_t* trace){
	struct trace_io_req* io;	
	if(trace->trace_buf_size <= trace->trace_io_cnt){
		trace->trace_buf_size *=2;
		trace->trace_buf = realloc(trace->trace_buf, sizeof(struct trace_io_req) * trace->trace_buf_size);
	}
	io = &(trace->trace_buf[trace->trace_io_cnt]);

	if (sscanf(line, "%lf %d %d %d %x\n", &(io->arrival_time), &(io->devno), &(io->blkno), &(io->bcount), &(io->flags)) != 5) {
		fprintf(stderr, "Wrong number of arguments for I/O trace event type\n");
		fprintf(stderr, "line: %s", line);
		return -1;
	}
	trace->trace_io_cnt++;
	return 0;
}

int main(int argc, char **argv){
	pthread_t threads[MAX_THREADS];
	pthread_attr_t attr;
	int rc;
	int i;
	long t;
	int open_flags;
	int argc_offset = ARG_TRACE;
	int qdepth ;
	int per_thread;
	double timeout;
	char line[201];

	nr_trace = argc - argc_offset;

	if(nr_trace<1){
		usage_help();
		return 0; 
	}


	per_thread = atoi(argv[ARG_THREAD]);
	if(per_thread<1 || per_thread*nr_trace>MAX_THREADS){
		printf(" invalid per thread num = %d \n", per_thread);
		return -1;
	}
	nr_thread= nr_trace * per_thread;

	if(nr_thread<1 || nr_thread>MAX_THREADS){
		printf(" invalid thread num = %d \n", nr_thread);
		return -1;
	}

	qdepth = atoi(argv[ARG_QDEPTH]);
	if(qdepth > MAX_QDEPTH)
		qdepth = MAX_QDEPTH;
	if(qdepth==0)
		qdepth = 1;

	timeout = atof(argv[ARG_TIMEOUT]);

	result_fp = fopen(argv[ARG_OUTPUT],"w");
	if(result_fp==NULL){
		printf(" open file %s error \n", argv[ARG_OUTPUT]);
		return -1;
	}

	fprintf(stdout, " Starting Trace Replayer \n");
	fprintf(result_fp, " Q depth = %d \n", qdepth);
	fprintf(result_fp, " Timeout = %.2f seconds \n", timeout); 
	fprintf(result_fp, " No of traces = %d \n", nr_trace);
	fprintf(result_fp, " No of threads = %d \n", nr_thread);
	fprintf(result_fp, " per thread = %d \n", per_thread);
	fprintf(result_fp, " Result file = %s \n", argv[ARG_OUTPUT]);

	for(i=0;i<nr_trace;i++){
		struct thread_info_t *t_info = &th_info[t];
		struct trace_info_t *trace = &traces[i];
		int fd;

	//	t_info->trace = trace;

		trace->trace_buf_size = 1024;		
		trace->trace_buf = malloc(sizeof(struct trace_io_req) * 1024);
		trace->trace_io_cnt = 0;
		trace->trace_io_cur = 0;
		trace->trace_fp = fopen(argv[argc_offset+i], "r");
		if(trace->trace_fp == NULL){
			printf("file open error %s\n", argv[argc_offset+i]);
			return -1;
		}
		while(1){
			if (fgets(line, 200, trace->trace_fp) == NULL) {
				break;
			}
			if(trace_io_put(line, trace))
				continue;
		}

		strcpy(trace->tracename, argv[argc_offset+i]);
		strcpy(trace->filename, argv[ARG_DEV]);
		fprintf(result_fp, " %d thread using %s trace \n", (int)i, trace->filename);

		open_flags = O_RDWR|O_DIRECT|O_SYNC;
		//open_flags = O_RDWR|O_SYNC;
		trace->fd =disk_open(trace->filename, open_flags); 
		if(trace->fd < 0)
			return -1;

		pthread_spin_init(&trace->trace_lock, 0);
		ioctl(trace->fd, BLKGETSIZE64, &trace->total_capacity);
		trace->total_pages = trace->total_capacity/PAGE_SIZE; 
		trace->total_pages = trace->total_pages/nr_thread;
		trace->total_sectors = trace->total_pages * SPP;
		trace->total_capacity = trace->total_pages * PAGE_SIZE;
		trace->start_partition = trace->total_capacity * t;
		trace->start_page = trace->start_partition/PAGE_SIZE;
		trace->timeout = timeout;


		fprintf(result_fp, " %d thread start part = %fGB size = %fGB (%llu, %llu pages)\n", (int)t,
				(double)trace->start_partition/1024/1024/1024, (double)trace->total_capacity/1024/1024/1024
				, trace->start_page, trace->start_page + trace->total_pages);

	}

	for(t=0;t<nr_thread;t++){
		struct thread_info_t *t_info = &th_info[t];
		struct trace_info_t *trace = &traces[t/per_thread];
		t_info->trace = trace;

		printf(" thread %d uses %s trace \n", (int)t, trace->tracename);

		INIT_FLIST_HEAD(&t_info->queue);
		pthread_mutex_init(&t_info->mutex, NULL);
		pthread_cond_init(&t_info->cond_sub, NULL);
		pthread_cond_init(&t_info->cond_main, NULL);

		memset(&t_info->io_ctx, 0, sizeof(io_context_t));

		t_info->tid = (int)t;
		t_info->queue_depth = qdepth;
		t_info->queue_count = 0;
		t_info->active_count = 0;

		for(i=0;i<qdepth;i++){
			t_info->th_buf[i] = allocate_aligned_buffer(MAX_BYTES);
			t_info->th_jobs[i] = malloc(sizeof(struct io_job));
		}

		memset(&t_info->io_stat, 0x00, sizeof(struct io_stat_t));

		io_queue_init(t_info->queue_depth, &t_info->io_ctx);

		//printf(" %d %f GB, %d sectors\n", fd, (double)t_info[t].total_capacity/1024/1024/1024,
		//		(int)t_info[t].total_sectors);
	}

	for(t=0;t<nr_thread;t++){
	//	printf("In main: creating thread %ld\n", t);
		rc = pthread_create(&threads[t], NULL, sub_worker, (void *)t);
		if (rc){
			printf("ERROR; return code from pthread_create( is %d\n", rc);
			exit(-1);
		}
	}

	pthread_spin_init(&spinlock, 0);
	gettimeofday(&tv_start, NULL);

#if USE_MAINWORKER == 1
	printf(" use main worker ... \n");
	main_worker();
#endif 
	signal(SIGINT, sig_handler);

//	sleep(10);

	for(t=0;t<nr_thread;t++){
		struct trace_info_t *trace = th_info[t].trace;
	//	printf("In main: creating thread %ld\n", t);
	//	pthread_cancel(threads[t]);
	//	pthread_cond_signal(&th_info[t].cond_sub);
		rc = pthread_join(threads[t], NULL);
		if (rc){
			//printf("ERROR; return code from pthread_create( is %d\n", rc);
			//exit(-1);
		}
		pthread_mutex_destroy(&th_info[t].mutex);
		pthread_cond_destroy(&th_info[t].cond_sub);
		pthread_cond_destroy(&th_info[t].cond_main);
		io_queue_release(th_info[t].io_ctx);
//		printf(" %d thread latency = %f \n", (int)t, (double)th_info[t].io_stat.latency_sum/th_info[t].io_stat.latency_count);


		for(i=0;i<qdepth;i++){
			free(th_info[t].th_buf[i]); 
			free(th_info[t].th_jobs[i]); 
		}
	}

	for(t=0;t<nr_trace;t++){
		fclose(traces[t].trace_fp);
		disk_close(traces[t].fd);
	}

	finalize();

	return 0;
}

#define N 624
#define M 397
#define MATRIX_A 0x9908b0df   /* constant vector a */
#define UPPER_MASK 0x80000000 /* most significant w-r bits */
#define LOWER_MASK 0x7fffffff /* least significant r bits */

/* Tempering parameters */   
#define TEMPERING_MASK_B 0x9d2c5680
#define TEMPERING_MASK_C 0xefc60000
#define TEMPERING_SHIFT_U(y)  (y >> 11)
#define TEMPERING_SHIFT_S(y)  (y << 7)
#define TEMPERING_SHIFT_T(y)  (y << 15)
#define TEMPERING_SHIFT_L(y)  (y >> 18)

static unsigned long mt[N]; /* the array for the state vector  */
static int mti=N+1; /* mti==N+1 means mt[N] is not initialized */

/* Initializing the array with a seed */
void
sgenrand(seed)
unsigned long seed;	
{
	int i;

	for (i=0;i<N;i++) {
		mt[i] = seed & 0xffff0000;
		seed = 69069 * seed + 1;
		mt[i] |= (seed & 0xffff0000) >> 16;
		seed = 69069 * seed + 1;
	}
	mti = N;
}

/* Initialization by "sgenrand()" is an example. Theoretically,      */
/* there are 2^19937-1 possible states as an intial state.           */
/* This function allows to choose any of 2^19937-1 ones.             */
/* Essential bits in "seed_array[]" is following 19937 bits:         */
/*  (seed_array[0]&UPPER_MASK), seed_array[1], ..., seed_array[N-1]. */
/* (seed_array[0]&LOWER_MASK) is discarded.                          */ 
/* Theoretically,                                                    */
/*  (seed_array[0]&UPPER_MASK), seed_array[1], ..., seed_array[N-1]  */
/* can take any values except all zeros.                             */
void
lsgenrand(seed_array)
unsigned long seed_array[]; 
/* the length of seed_array[] must be at least N */
{
	int i;

	for (i=0;i<N;i++) 
		mt[i] = seed_array[i];
	mti=N;
}

unsigned long 
genrand()
{
	unsigned long y;
	static unsigned long mag01[2]={0x0, MATRIX_A};
	/* mag01[x] = x * MATRIX_A  for x=0,1 */

	if (mti >= N) { /* generate N words at one time */
		int kk;

		if (mti == N+1)   /* if sgenrand() has not been called, */
			sgenrand(4357); /* a default initial seed is used   */

		for (kk=0;kk<N-M;kk++) {
			y = (mt[kk]&UPPER_MASK)|(mt[kk+1]&LOWER_MASK);
			mt[kk] = mt[kk+M] ^ (y >> 1) ^ mag01[y & 0x1];
		}
		for (;kk<N-1;kk++) {
			y = (mt[kk]&UPPER_MASK)|(mt[kk+1]&LOWER_MASK);
			mt[kk] = mt[kk+(M-N)] ^ (y >> 1) ^ mag01[y & 0x1];
		}
		y = (mt[N-1]&UPPER_MASK)|(mt[0]&LOWER_MASK);
		mt[N-1] = mt[M-1] ^ (y >> 1) ^ mag01[y & 0x1];

		mti = 0;
	}

	y = mt[mti++];
	y ^= TEMPERING_SHIFT_U(y);
	y ^= TEMPERING_SHIFT_S(y) & TEMPERING_MASK_B;
	y ^= TEMPERING_SHIFT_T(y) & TEMPERING_MASK_C;
	y ^= TEMPERING_SHIFT_L(y);

	return y; 
}


#if USE_MAINWORKER == 1
void *sub_worker(void *threadid)
{
	long tid =  (long)threadid;
	struct io_job *job, *test_job;
	struct io_job *jobq[MAX_QDEPTH];
	struct iocb *ioq[MAX_QDEPTH];
	int rc;

	//printf (" pthread start id = %d \n", (int)tid);

	while(1){
		struct flist_head *ptr, *tmp;
		int cnt = 0;
		int complete_count = 0;
		int i;

		pthread_mutex_lock(&th_info[tid].mutex);

		while(!th_info[tid].queue_count)
			pthread_cond_wait(&th_info[tid].cond_sub, &th_info[tid].mutex);

		//printf(" tid %d qcount %d \n", (int)tid, th_info[tid].queue_count);

		flist_for_each_safe(ptr, tmp, &th_info[tid].queue){
			job = flist_entry(ptr, struct io_job, list);
			gettimeofday(&job->start_time, NULL);
			flist_del(&job->list);
			th_info[tid].queue_count--;
			jobq[cnt++] = job;
		}
		pthread_mutex_unlock(&th_info[tid].mutex);
		pthread_cond_signal(&th_info[tid].cond_main);

		if(!cnt)
			continue;

		
#if 1
		for(i = 0;i < cnt;i++){
			job = jobq[i]; 
			ioq[i] = &job->iocb;
			if(job->rw)
				io_prep_pread(&job->iocb, th_info[tid].fd, job->buf, job->bytes, job->offset);
			else
				io_prep_pwrite(&job->iocb, th_info[tid].fd, job->buf, job->bytes, job->offset);
		}

		rc = io_submit(th_info[tid].io_ctx, cnt, ioq);
		if (rc < 0)
			io_error("io_submit", rc);

		while(cnt){

			//printf(" io getevents \n");
			complete_count = io_getevents(th_info[tid].io_ctx, cnt, cnt, th_info[tid].events, NULL);
			//printf(" complete count = %d cnt = %d \n", complete_count, cnt);

			for(i = 0;i < complete_count;i++){
				test_job = (struct io_job *)((unsigned long)th_info[tid].events[i].obj);
				gettimeofday(&test_job->stop_time, NULL);
				//printf(" tid = %d,no = %d blkno = %d, bytes = %d, lat = %f\n",
				//		(int) tid, i, (int)test_job->offset, (int)test_job->bytes,
				//		time_since(&test_job->start_time, &test_job->stop_time));

				pthread_mutex_lock(&th_info[tid].mutex);
			//	th_info[tid].active_count--;
				th_info[tid].io_stat.latency_sum += time_since(&test_job->start_time, &test_job->stop_time);
				th_info[tid].io_stat.latency_count ++;
				//if(th_info[i].queue_count)
				//	printf(" wait ... qcount = %d\n", th_info[i].queue_count);
				pthread_mutex_unlock(&th_info[tid].mutex);
				cnt--;

				free(test_job->buf);
				free(test_job);
			}
		}
#else
		// calculating IPC of pthreads 
		for(i = 0;i < cnt;i++){
			job = jobq[i]; 
			pthread_mutex_lock(&th_info[tid].mutex);
			//	th_info[tid].active_count--;
			th_info[tid].latency_sum += time_since(&job->start_time, &job->stop_time);
			th_info[tid].latency_count ++;
			//if(th_info[i].queue_count)
			//	printf(" wait ... qcount = %d\n", th_info[i].queue_count);
			pthread_mutex_unlock(&th_info[tid].mutex);
			cnt--;

			free(job->buf);
			free(job);
		}
#endif 
	}

	printf (" pthread end id = %d \n", (int)tid);


	return NULL;
}
void main_worker(){
	struct io_job *job;
	int i, j, k;

	for(j = 0;j < 4*256*64;j++){
		for (i=0; i<nr_thread; i++) {
			unsigned long page;

			pthread_mutex_lock(&th_info[i].mutex);

			while(th_info[i].queue_count>=MAX_QDEPTH){
			//	printf(" wait ... qcount = %d\n", th_info[i].queue_count);
				pthread_cond_wait(&th_info[i].cond_main, &th_info[i].mutex);
			}

			for(k = 0;k < 1;k++){
				job = (struct io_job *)malloc(sizeof(struct io_job));

				page = RND(th_info[i].total_pages);
				job->offset = (long long)page * PAGE_SIZE;
				job->bytes = PAGE_SIZE;
				job->rw = 0;
				job->buf = allocate_aligned_buffer(job->bytes);

				//gettimeofday(&job->start_time, NULL);
				flist_add_tail(&job->list, &th_info[i].queue);
				th_info[i].queue_count++;

				pthread_spin_lock(&spinlock);
				total_bytes += job->bytes;
				pthread_spin_unlock(&spinlock);
			}


		//	if(th_info[i].queue_count>=MAX_QDEPTH)
			//printf(" qcount = %d\n", th_info[i].queue_count);


			pthread_mutex_unlock(&th_info[i].mutex);
			pthread_cond_signal(&th_info[i].cond_sub);

		}
	}
	printf(" finish main worker .. \n");
}

#endif 

#if 0 
int issue_req(int is_rw, int is_rand, int req_size, int part_size, 
						struct timeval *tv_result, char *filename){
	struct timeval tv_start, tv_end;
	FILE *micro_fp = NULL;
	char *rand_map = NULL;
	char *alignedbuff = NULL;
	int blk_count = 0;
	int i = 0;

	micro_fp = fopen(filename, "w");

	if(micro_fp == NULL){
		printf("cannot open !!!\n");
		return -1;
	}

	blk_count = (int)((__s64)part_size * MB / req_size);

	alignedbuff = allocate_aligned_buffer(req_size);
	if(alignedbuff == NULL){
		printf("alloc aligned buffer error\n");
		return -1;
	}
	memset(alignedbuff, 0xff, req_size);

	rand_map = (char *)malloc(blk_count);
	if(rand_map == NULL){
		printf(" malloc error \n");
		return -1;
	}
	memset(rand_map, 0x0, blk_count);

	gettimeofday(&tv_start, NULL);

	for(i = 0;i < blk_count;i++){
		struct timeval micro_result;
		__u32 offset = RND(blk_count);

		if(!is_rand)
			offset = i;

		if(rand_map[offset] == 1){
			i--;
			continue;
		}else{
			rand_map[offset] = 1;
		}


		if(is_rw){
			if(io_read(alignedbuff, req_size, offset, &micro_result) < 0 )
				return -1;
		}else{
			if(io_write(alignedbuff, req_size, offset, &micro_result) < 0 )
				return -1;
		}
		
		fprintf(micro_fp, "%d\t%.6f\n", i, tv_to_sec(&micro_result));
	}

	gettimeofday(&tv_end, NULL);
	timeval_subtract(tv_result, &tv_end, &tv_start);

	free(alignedbuff);
	free(rand_map);

	fclose(micro_fp);

	return 0;
}

int start_io_test(int is_rw, int is_rand, int part_size, int max_req_size){
	FILE *result_fp;
	unsigned int req_size;	
	char result_str[STR_SIZE];
	char rw_str[STR_SIZE];
	char rand_str[STR_SIZE];
	int i = 1;

	max_req_size *= MB;

	if(is_rw){
		sprintf(rw_str,"read");
	}else{
		sprintf(rw_str,"write");
	}

	if(is_rand){
		sprintf(rand_str,"rand");
	}else{
		sprintf(rand_str,"seq");
	}

	sprintf(result_str, "result/bandwidth_%s_%s_result.txt", rw_str, rand_str);
	result_fp = fopen(result_str,"w");
	if(result_fp == NULL){
		printf("file open error %s\n",result_str);
		return -1;
	}

	fprintf(result_fp, "#Index\tReq Size\tBandwidth(MB/s)\n");

	for(req_size = PAGE_SIZE; req_size <= max_req_size; req_size *= 2, i++){
		struct timeval tv_result;
		char filename[STR_SIZE];
		int res;

		sprintf(filename,"result/response_%s_%s_%d.txt", rw_str, rand_str, req_size);

		printf("  >>%s %s : req size %d\n",rw_str, rand_str, req_size);	
		fflush ( stdout );

		if((res = issue_req(is_rw, is_rand, req_size, part_size, &tv_result, filename)) < 0){
			return res;
		}

		if(tv_result.tv_sec || tv_result.tv_usec){
			fprintf(stdout, " %d\t%d\t%f\n\n", i, 
					req_size, (float)part_size/(float)tv_to_sec(&tv_result));
			fprintf(result_fp, "%d\t%d\t%f\n", i, 
					req_size, (float)part_size/(float)tv_to_sec(&tv_result));
			fflush(result_fp);
		}

		sync();
		flush_buffer_cache(fd);
	}

	fclose(result_fp);
}
#endif 
#if 0
	while (!feof(trace_fp)) {
		int r;
		struct timeval micro_result;

		pthread_spin_lock(&spinlock);
		if(done){
			pthread_spin_unlock(&spinlock);
			break;
		}
		p = fgets(str, 4096, trace_fp);
		if(p==NULL){
		//	printf(" trace null \n");
			done = 1;
			pthread_spin_unlock(&spinlock);
			break;
		}
		pthread_spin_unlock(&spinlock);


		r = sscanf(p, "%256s %256s %llu %u", fname, act, &offset, &bytes);

		if(r!=4)
			continue;

		printf(" %s %s %llu %u\n", fname, act, offset, bytes);

	/*	if(offset % getpagesize()){
			printf(" rem = %llu \n", offset % getpagesize());
			printf(" rem = %llu \n", (unsigned long long)bytes % getpagesize());
		} */

		if(offset % getpagesize()){
			offset = offset - (offset % getpagesize());
		}
		if(bytes%getpagesize()){
			bytes = bytes + getpagesize() - (bytes % getpagesize());
		}

		if(offset > (unsigned long long)400*1024*1024*1024){
			offset = (offset+bytes)%((unsigned long long)400*1024*1024*1024);
		}
#if 0 
		if(offset % getpagesize() == 0){
			//printf (" corrected .. \n");
		}
#endif 

		if (!strcmp(act, "read")){
			if(io_read(alignedbuff, bytes, offset, &micro_result) < 0 ){
				error_bytes += bytes;
				continue;
			}
		}else if (!strcmp(act, "write")){
			if(io_write(alignedbuff, bytes, offset, &micro_result) < 0 ){
				error_bytes += bytes;
				continue;
			}
		}

		if(error_bytes>1*1024*1024*1024){
			printf(" Error exceeds %llu\n", (unsigned long long)error_bytes/1024/1024);
		}
		operations++;
		sum_bytes+=bytes;
	}
#endif 
