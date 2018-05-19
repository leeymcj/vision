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
#define MYQUEUE  "/mycond"

#define N 4


/*#include <NVX/nvx.h>
#include <NVX/nvx_timer.hpp>

#include <NVXIO/Application.hpp>
#include <NVXIO/FrameSource.hpp>
#include <NVXIO/Render.hpp>
#include <NVXIO/SyncTimer.hpp>
#include <NVXIO/Utility.hpp>*/

int hough_transform(int argc, char* argv[]);
int feature_tracker(int argc, char* argv[]);
int motion_estimation(int argc, char* argv[]);
int video_stabilizer(int argc, char* argv[]);

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

int dequeue(int* q)
{
	/* sort */
	bubbleSort(q, N);	

	int tmp =  q[0];
	for (int i=0; q[i]>0; i++){		
	q[i] = q[i+1];
	}

	return tmp;
}


int main(int argc, char* argv[])
{

	int i, ret;
	int pid[CPU];
	cpu_set_t cpus;

 

    /*inter-process mutex*/
    //int *cond;
    int *queue;
	

    pthread_mutex_t *gpu_lock;
    pthread_mutex_t *q_lock;
    
    int queue_id, mutex_id;
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
    /* cond */
    queue_id = shm_open(MYQUEUE, O_CREAT | O_RDWR | O_TRUNC, mode);
    if (queue_id < 0) {
        perror("shm_open failed with " MYQUEUE);
        return -1;
    }
    if (ftruncate(queue_id, sizeof(int)) == -1) {
        perror("ftruncate failed with " MYQUEUE);
        return -1;
    }
    q_lock = (pthread_mutex_t *)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, mutex_id, 0);
    if (q_lock == MAP_FAILED) {
        perror("mmap failed with " MYQUEUE);
        return -1;
	}
    queue = (int *)mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, queue_id, 0);
    if (queue == MAP_FAILED) {
        perror("ftruncate failed with " MYQUEUE);
        return -1;
    }
    /* set mutex shared between processes */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(gpu_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_mutexattr_t attr;
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(q_lock, &attr);
    pthread_mutexattr_destroy(&attr);

	int C[N];
	int G[N];
	int T[N];
	struct timespec ts_start, ts_end;
    	double elapsedTime;
    //initialize
     for (i = 0; i < N; i++){
	queue[i] = 0;
	C[i] = (N-i);
	G[i] = (N-i);
	T[i] = (C[i] + G[i])*3;
     }


    /*lock-free queue*/
	for (i = 0; i < N; i++) {
		ret = fork();	
		if (ret != 0) {
		//parent process
		pid[i] = ret;
		continue;
		}
		

        /*base priority*/
        ret = setpriority(PRIO_PROCESS, getpid(), -10-i);

 	/*partition*/
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
        sched_setaffinity(getpid(), sizeof(cpus), &cpus);
	//CPU_SET(i, &cpus);



	while (1){
	clock_gettime(CLOCK_MONOTONIC, &ts_start);//release

	/*CPU part*/
	sleep(C[i]);



	
	/*GPU part*/
	//printf("child process %d lock\n", getpid());
	while( pthread_mutex_trylock(gpu_lock) ){
		printf("task%d put into wait\n", i);

		//pthread_mutex_lock(q_lock);
		enqueue(queue, getpid());
		//pthread_mutex_unlock(q_lock);

		kill(getpid(), SIGSTOP);
	//MPCP priority celing
	ret = setpriority(PRIO_PROCESS, getpid(), -20);
	
		continue;
	}	

	/*gpu execution*/
	printf("task %d executes\n", i);
	sleep(G[i]); //FIXME GPU time

	//GPU completion CPU resume
		

	
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsedTime = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0;      // sec to ms
        elapsedTime += (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;   // us to ms
        printf("task %d completion time %lf\n", i, elapsedTime);

	ret = setpriority(PRIO_PROCESS, getpid(), -10-i); //return to base priority
	pthread_mutex_unlock(gpu_lock);

	//pthread_mutex_lock(q_lock);
	kill( dequeue(queue) , SIGCONT);
	//pthread_mutex_unlock(q_lock);


	
	/*wait for next release*/
	sleep(T[i]-elapsedTime/1000 );

	

	//exit(0);
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
	//    for (int i=0; i<N; i++)
	//	printf("%d\t", queue[i]);
	//    printf("\n");
	    sleep(1);

	    
	    //if (pid == 0)
	    //	continue;	
	}

	exit(0);
	
	//sleep(180);

}
