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
#include "main.h"

#ifdef sched
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
				if (  (HOT[*onGPU] && !HOT[task]) )// && ( x[highest] - E[task][ progress[task] ] >= 0)  )
				{ //hot/cold schedule
					printf("GPU is HOT, schedule COLD task %d insead of %d\n", base_pid + task +1, base_pid+highest+1);
					//printf("GPU is HOT, schedule COLD task %d instead of %d\n", task, highest);
        				setpriority(PRIO_PROCESS,  base_pid+task+1, HIGHEST);
					printf("task %d priority %d\n", base_pid + task +1, getpriority(PRIO_PROCESS, base_pid + task +1)  );
					//printf("x[highest]-e[task] : %d - %d \n", x[highest], E[task][ progress[task]] );
					x[highest]-=  E[task][ progress[task] ];
					break;
				}

				if (  (!HOT[*onGPU] && HOT[task]) )//  && (x[highest] - E[task][ progress[task] ] >= 0)   )
				{
					printf("GPU is COLD, schedule HOT task %d insead of %d\n", base_pid + task +1, base_pid+highest+1);
        				setpriority(PRIO_PROCESS, base_pid+task+1, HIGHEST);
					printf("task %d priority %d\n", base_pid + task +1, getpriority(PRIO_PROCESS, base_pid + task +1)  );
					//printf("x[highest]-e[task] : %d - %d \n", x[highest], E[task][ progress[task]] );
					x[highest]-=  E[task][ progress[task] ];
					break;
				}

			}
		
	}	
}

void cpuResume(int i){
 progress[i]++;
 setpriority(PRIO_PROCESS, getpid(), BASE);
}

void jobRelease(int i){
	x[i] = V[i];
}


int onCPU(void){
	int hotCPU=0;

	for (int j=0; PPriority[hotCPU][j]>=0; j++)
        {
		int task = PPriority[P[hotCPU]][j];
		 if ( progress[task] == 0 || progress[task] == 2 || progress[task] == 4 ){ //running CPU part
		//printf("task %d executes on hot CPU\n", task);
			return task;
		}
	}
	return N; //no task running
}
#endif


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
	int j=0; //take first task

#ifdef sched
	/*GPU scheduling logic*/
	int CPUtask = onCPU();
	//printf("task %d executes on hot CPU\n", CPUtask);
	if( HOT[CPUtask] ) //hot taks on CPU
	{		
	//printf("CPU runs hot task\n");
		for (j=0; q[j]>0; j++){
			int i = q[j]-base_pid-1;
			printf("task %d waits for GPU\n", i);
			if (!HOT[i] || !q[j+1] )
				break;
		}
	}
	else //cold task on CPU
	{
	printf("CPU runs cold task\n");
		for (j=0; ; j++){
			int i = q[j]-base_pid-1;
			printf("task %d waits for GPU\n", i);
			if (HOT[i] || !q[j+1] )
				break;
		}

	}
	
	if (j!=0)
	printf("GPU schedule task %d instead of %d\n", q[j], q[0]);

#endif
//take j-th task
	int tmp =  q[j];
	for (int i=j; q[i]>0; i++){		
	q[i] = q[i+1];
	}

	return tmp;
}


void gpuLock(int i){
 	int ret;
	while( ret = pthread_mutex_trylock(gpu_lock) ){
		//printf("task%d put into wait\n", i);
		printf("trylock return %d\n", ret);


		pthread_mutex_lock(q_lock);
		enqueue(queue, getpid());
		pthread_mutex_unlock(q_lock);

		kill(getpid(), SIGSTOP);
		continue;
	}


#ifdef sched
	*onGPU = i;
#endif
	//printf("task %d executes on GPU\n", i);

	//MPCP priority celing
	setpriority(PRIO_PROCESS, getpid(), HIGHEST);
}

void gpuUnlock(int i)
{
#ifdef sched
	*onGPU = N;
#endif
	setpriority(PRIO_PROCESS, getpid(), BASE); //return to base priority
	pthread_mutex_unlock(gpu_lock);

	pthread_mutex_lock(q_lock);
	kill( dequeue(queue) , SIGCONT);
	pthread_mutex_unlock(q_lock);
}







int main(int argc, char* argv[])
{

	int i, ret;
	int pid[CPU];
	cpu_set_t cpus;

        base_pid = getpid();
        setpriority(PRIO_PROCESS, getpid(), HIGHEST); //parent process
	printf("parent priority %d\n", getpriority(PRIO_PROCESS, getpid())  );

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

    /* set mutex shared between processes */
    /*pthread_mutexattr_t mattr;
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    ret = pthread_mutex_init(gpu_lock, &mattr);
    printf("mutex init error: %d\n", ret);
    pthread_mutex_init(q_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);*/

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


    progress = (int *)mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, progress_id, 0);
    if (progress == MAP_FAILED) {
        perror("ftruncate failed with " MYPROGRESS);
        return -1;
    }


    onGPU = (int*) mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    x = (int*) mmap(NULL, N*sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *onGPU = N;


	for (i = 0; i < N; i++) {
		ret = fork();	
		if (ret != 0) {
		//parent process
		pid[i] = ret;
		continue;
		}
		
	//sleep(1);
        /*base priority*/
        setpriority(PRIO_PROCESS, getpid(), BASE);
	//printf("task %d priority %d\n", i, getpriority(PRIO_PROCESS, getpid())  );

 	/*partition*/
	CPU_ZERO(&cpus);
	CPU_SET(P[i], &cpus);
        sched_setaffinity(getpid(), sizeof(cpus), &cpus);
	//CPU_SET(i, &cpus);



	/*application invocation
	if (i==0)
		 hough_transform(argc, argv, i);*/
		

	struct timespec ts_start, ts_end;
        double elapsedTime;

	/*main loop*/
	while (1){

	jobRelease(i);

	//FIXME /*release*/
	 progress[i]=0;
	 //printf("task %d is on GPU\n", *onGPU);
#ifdef sched	
	cpuSched(i);
#endif
	 



	clock_gettime(CLOCK_MONOTONIC, &ts_start);//release

	/*CPU part*/
	sleep(E[i][ progress[i] ]);

	//FIXME /*CPU completion*/
	cpuResume(i);

#ifdef sched	
	cpuSched(i);
#endif
	/*GPU part*/
	//printf("child process %d lock\n", getpid());
	gpuLock(i);


	/*gpu execution*/
	sleep(E[i][ progress[i] ]);

 	gpuUnlock(i);

	
	//FIXME /*CPU resume*/
	progress[i]=2;
#ifdef sched	
	cpuSched(i);
#endif

	/*CPU part*/
	sleep(E[i][ progress[i] ]);
	//FIXME /*CPU completion*/
	cpuResume(i);


	/*GPU part*/
	//printf("child process %d lock\n", getpid());
	gpuLock(i);

	/*gpu execution*/
	sleep(E[i][ progress[i] ]);

	gpuUnlock(i);




	//FIXME CPU resume
	progress[i]=4;

#ifdef sched	
	cpuSched(i);
#endif
	sleep(E[i][ progress[i] ]);
	

	//FIXME CPU completion
	cpuResume(i);
#ifdef sched	
	cpuSched(i);
#endif
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsedTime = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0;      // sec to ms
        elapsedTime += (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;   // us to ms
        printf("task %d completion time %lf\n", i, elapsedTime);

	/*wait for next release*/
	sleep(T[i]-elapsedTime/1000 );


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
        setpriority(PRIO_PROCESS, getpid(), HIGHEST); //parent process
	//printf("parent priority %d\n", getpriority(PRIO_PROCESS, getpid())  );


	while (1)
	{
	    printf("progress\t");
	    for (int i=0; i<N; i++)
		printf("%d\t", progress[i]);

	    printf("GPU queues\t");
	    for (int i=0; i<N; i++)
		printf("%d\t", queue[i]);
	    printf("\n");
	    usleep(1000000);

	    
	    //if (pid == 0)
	    //	continue;	
	}

	exit(0);
	
	//sleep(180);

}
