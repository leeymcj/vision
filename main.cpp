/*
# Copyright (c) 2014-2016, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <queue>
#include <memory>
#include <ctime>


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>

#define CPU 4
#define MYMUTEX "/mymutex"
#define MYQLOCK "/myqlock"
#define MYQUEUE  "/myqueue"
#define MYPROGRESS  "/myprogress"

#define N 4 //number of tasks

#define K 2 //maximum number of CPUs GPU sections

#define U 0.3

//#define sched

int *queue;

pthread_mutex_t *gpu_lock;
pthread_mutex_t *q_lock;

int base_pid;
//int C[N][K] = {0};
//int G[N][K] = {0};

int E[N][K*2+1] = {0};

int T[N] = {0};

	
int V[N] = { 80, 37, 20, 20};
int x[N];
int *progress;
int *onGPU;
int HOT[N+1] = {1, 1, 0, 0, 0};
int P[N] = { 1, 1, 3, 3};
int PPriority[CPU][N] = {
			{-1, -1, -1, -1}, //in the ordre of priority
			{1, 0, -1, -1},
			{-1, -1, -1, -1},
			{3, 2, -1, -1}
			};



/*#include <NVX/nvx.h>
#include <NVX/nvx_timer.hpp>

#include <NVXIO/Application.hpp>
#include <NVXIO/FrameSource.hpp>
#include <NVXIO/Render.hpp>
#include <NVXIO/SyncTimer.hpp>
#include <NVXIO/Utility.hpp>*/

int hough_transform(int argc, char* argv[], int i);
int feature_tracker(int argc, char* argv[], int i);
int motion_estimation(int argc, char* argv[], int i);
int video_stabilizer(int argc, char* argv[], int i);

//
// main - Application entry point
//






void delay(int sec)
{
	for (long long int i=0; i<sec*50000000; i++)
	asm("NOP");
}


void swap(int *xp, int *yp)
{
    int temp = *xp;
    *xp = *yp;
    *yp = temp;
}

void bubbleSort(int arr[], int n)
{
   int i, j;
   for (i = 0; i < n-1; i++)      
 
       // Last i elements are already in place   
       for (j = 0; j < n-i-1; j++) 
           if (arr[j] < arr[j+1])
              swap(&arr[j], &arr[j+1]);
}



void enqueue(int* q, int val)
{
  for (int i=0; i<N; i++){
	if (q[i] == 0){
	q[i] = val;
	break;
	}
  }  
}

int onCPU(void){
	int hotCPU=2;

	for (int j=0; PPriority[hotCPU][j]>=0; j++)
        {
		int task = PPriority[P[hotCPU]][j];
		 if ( progress[task] == 0 || progress[task] == 2 || progress[task] == 4 ){ //running CPU part
		printf("task %d executes on hot CPU\n", task);
			return task;
		}
	}
	return N; //no task running
}


int dequeue(int* q)
{
	/* sort */
	bubbleSort(q, N);	
	int j=0; //take first task

#ifdef sched
	/*GPU scheduling logic*/
	int CPUtask = onCPU();
	if( HOT[CPUtask] ) //hot taks on CPU
	{		
		for (int j=0; q[j]>0; j++){
			int i = q[j]-base_pid-1;
			if (!HOT[i])
				break;
		}
	}
	else //cold task on CPU
	{
		for (int j=0; q[j]>0; j++){
			int i = q[j]-base_pid-1;
			if (HOT[i])
				break;
		}

	}
	

#endif
//take j-th task
	int tmp =  q[j];
	for (int i=j; q[i]>0; i++){		
	q[i] = q[i+1];
	}

	return tmp;
}


void gpuLock(int i){

	while( pthread_mutex_trylock(gpu_lock) ){
		//printf("task%d put into wait\n", i);

		pthread_mutex_lock(q_lock);
		enqueue(queue, getpid());
		pthread_mutex_unlock(q_lock);

		kill(getpid(), SIGSTOP);
		continue;
	}
	*onGPU = i;
	//printf("task %d executes on GPU\n", i);

	//MPCP priority celing
	setpriority(PRIO_PROCESS, getpid(), -20);
}

void gpuUnlock(int i)
{
	*onGPU = N;
	setpriority(PRIO_PROCESS, getpid(), -10-i); //return to base priority
	pthread_mutex_unlock(gpu_lock);

	//pthread_mutex_lock(q_lock);
	pthread_mutex_lock(q_lock);
	kill( dequeue(queue) , SIGCONT);
	pthread_mutex_unlock(q_lock);
}

void cpuSched(int i){

	//next running task from the highest priority
	int j, highest;
	for (j=0, highest=-1; PPriority[P[i]][j]>=0; j++)
	{
			int task = PPriority[P[i]][j];
			if ( progress[task] == 0 || progress[task] == 2 || progress[task] == 4 ) //running CPU part
			{
				//printf("task%d is running on CPU part on the same core with %d\n", PPriority[P[i]][j], i );	
				//highest
				if (highest<0){
				highest=task;

				if (  (HOT[*onGPU] && !HOT[highest]) || (!HOT[*onGPU] && HOT[highest]) )
				break;
				else
				continue;
				}
	
				//priority inversion
				if (  (HOT[*onGPU] && !HOT[task]) && ( x[highest] - E[task][ progress[task] ] >= 0)  )
				{ //hot/cold schedule
        				setpriority(PRIO_PROCESS,  base_pid+i+1, -20);
					x[highest]-=  E[task][ progress[task] ];
					printf("GPU is HOT, schedule COLD task %d instead of %d\n", task, highest);
					break;
				}

				if (  (!HOT[*onGPU] && HOT[task])  && (x[highest] - E[task][ progress[task] ] >= 0)   )
				{
        				setpriority(PRIO_PROCESS, base_pid+1, -20);
					x[highest]-=  E[task][ progress[task] ];
					printf("GPU is COLD, schedule HOT task %d insead of %d\n", task, highest);
					break;
				}

			}
		
	}	
}

int main(int argc, char* argv[])
{

	int i, ret;
	int pid[CPU];
	cpu_set_t cpus;

    base_pid = getpid();

    /*inter-process mutex*/
    //int *cond;
    
    int mutex_id, qlock_id, queue_id, progress_id;
    int mode = S_IRWXU | S_IRWXG;
    /* mutex */
    mutex_id = shm_open(MYMUTEX, O_CREAT | O_RDWR | O_TRUNC, mode);
    if (mutex_id < 0) {
        perror("shm_open failed with " MYMUTEX);
        return -1;
    }
    if (ftruncate(mutex_id, sizeof(pthread_mutex_t)) == -1) {
        perror("ftruncate failed with " MYMUTEX);
        return -1;
    }
    gpu_lock = (pthread_mutex_t *)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, mutex_id, 0);
    if (gpu_lock == MAP_FAILED) {
        perror("mmap failed with " MYMUTEX);
        return -1;
    }


    qlock_id = shm_open(MYQLOCK, O_CREAT | O_RDWR | O_TRUNC, mode);
    if (qlock_id < 0) {
        perror("shm_open failed with " MYQLOCK);
        return -1;
    }
    if (ftruncate(qlock_id, sizeof(pthread_mutex_t)) == -1) {
        perror("ftruncate failed with " MYQLOCK);
        return -1;
    }
    q_lock = (pthread_mutex_t *)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, qlock_id, 0);
    if (q_lock == MAP_FAILED) {
        perror("mmap failed with " MYQLOCK);
        return -1;
	}



    queue_id = shm_open(MYQUEUE, O_CREAT | O_RDWR | O_TRUNC, mode);
    if (queue_id < 0) {
        perror("shm_open failed with " MYQUEUE);
        return -1;
    }
    if (ftruncate(queue_id, sizeof(int)) == -1) {
        perror("ftruncate failed with " MYQUEUE);
        return -1;
    }
    queue = (int *)mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, queue_id, 0);
    if (queue == MAP_FAILED) {
        perror("ftruncate failed with " MYQUEUE);
        return -1;
    }


    progress_id = shm_open(MYPROGRESS, O_CREAT | O_RDWR | O_TRUNC, mode);
    if (queue_id < 0) {
        perror("shm_open failed with " MYPROGRESS);
        return -1;
    }
    if (ftruncate(progress_id, sizeof(int)) == -1) {
        perror("ftruncate failed with " MYPROGRESS);
        return -1;
    }
     progress = (int *)mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, progress_id, 0);
    if (progress == MAP_FAILED) {
        perror("ftruncate failed with " MYPROGRESS);
        return -1;
    }


    onGPU = (int*) mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);


    /* set mutex shared between processes */
/*    pthread_mutexattr_t mattr;
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(gpu_lock, &mattr);

    pthread_mutex_init(q_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);*/

	struct timespec ts_start, ts_end;
    	double elapsedTime;
    //initialize
     for (i = 0; i < N; i++){
	queue[i] = 0;

	int E_SUM=0;

	for (int k=0; k< (2*K+1) ; k++)
	{
	E[i][k] = (N-i);
	//G[i][k] = (N-i);
	E_SUM+= E[i][k];
	}
	
	T[i] = E_SUM / U ;
     }
     *onGPU = N;
	
	


	for (i = 0; i < N; i++) {
		ret = fork();	
		if (ret != 0) {
		//parent process
		pid[i] = ret;
		continue;
		}
		

        /*base priority*/
        setpriority(PRIO_PROCESS, getpid(), -10-i);

 	/*partition*/
	CPU_ZERO(&cpus);
	CPU_SET(P[i], &cpus);
        sched_setaffinity(getpid(), sizeof(cpus), &cpus);
	//CPU_SET(i, &cpus);




	/*main loop*/
	while (1){
#ifdef sched
	x[i] = V[i];
#endif

	//FIXME /*release*/
	progress[i]=0;
	//printf("task %d is on GPU\n", *onGPU);
#ifdef sched
	cpuSched(i);
#endif
	 



	clock_gettime(CLOCK_MONOTONIC, &ts_start);//release

	switch(i)
	{
	case 0 : hough_transform(argc, argv, i);
		 break;
	case 1 : feature_tracker(argc, argv, i);
		 break;
	case 2 : motion_estimation(argc, argv, i);
		 break;
	case 3 : video_stabilizer(argc, argv, i);
		 break;
	default: ;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsedTime = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0;      // sec to ms
        elapsedTime += (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;   // us to ms
        printf("task %d completion time %lf\n", i, elapsedTime);

	/*wait for next release*/
	//sleep(T[i]-elapsedTime/1000 );


}	



/*	switch(i)
	{
	case 0 : CPU_SET(3, &cpus);
		 sched_setaffinity(getpid(), sizeof(cpus), &cpus);
		 hough_transform(argc, argv);
		 break;
	case 1 : CPU_SET(3, &cpus);
		 sched_setaffinity(getpid(), sizeof(cpus), &cpus);
		 feature_tracker(argc, argv);
		 break;
	case 2 : CPU_SET(1, &cpus);
		 sched_setaffinity(getpid(), sizeof(cpus), &cpus);
		 motion_estimation(argc, argv);
		 break;
	case 3 : CPU_SET(1, &cpus);
		 sched_setaffinity(getpid(), sizeof(cpus), &cpus);
		 video_stabilizer(argc, argv);
		 break;
	default: ;
	}*/

	}
	/*parent*/
	printf("parent process\n");

	while (1)
	{
	    for (int i=0; i<N; i++)
		printf("%d\t", progress[i]);
	    printf("\n");
	    sleep(1);

	    
	    //if (pid == 0)
	    //	continue;	
	}

	exit(0);
	
	//sleep(180);

}
