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

#include "lfq.h"


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
	

    pthread_mutex_t *mutex;
    
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
    mutex = (pthread_mutex_t *)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, mutex_id, 0);
    if (mutex == MAP_FAILED) {
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
    queue = (int *)mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, queue_id, 0);
    if (queue == MAP_FAILED) {
        perror("ftruncate failed with " MYQUEUE);
        return -1;
    }
    /* set mutex shared between processes */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    //initialize
     for (i = 0; i < N; i++)
	queue[i] = 0;


    /*lock-free queue*/
	for (i = 0; i < N; i++) {
		ret = fork();	
		if (ret != 0) {
		//parent process
		pid[i] = ret;
		continue;
		}
		

	CPU_ZERO(&cpus);
	//CPU_SET(i, &cpus);

	while (1){

	//printf("child process %d lock\n", getpid());
	if( pthread_mutex_trylock(mutex) ){
		printf("child process %d put into wait\n", getpid());
		enqueue(queue, getpid());
		kill(getpid(), SIGSTOP);
		printf("child process %d is waken up\n", getpid());
		continue;		
	}
	else{	
	/*gpu execution*/
	printf("child process %d executes\n", getpid());
	sleep(5);
	}

	int pid = dequeue(queue);
	pthread_mutex_unlock(mutex);
	kill( pid, SIGCONT);
	printf("wake child process %d\n", pid);
	sleep(1);

	}
	

	//exit(0);
	



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
		printf("%d\t", queue[i]);
	    printf("\n");
	    sleep(1);

	    
	    //if (pid == 0)
	    //	continue;	
	}

	exit(0);
	
	//sleep(180);

}
