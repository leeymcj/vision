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

#define U 0.3 //total utilization

/*nice priority */
#define HIGHEST 0
#define BASE N-i
#define sched

int *queue;

pthread_mutex_t *gpu_lock;
pthread_mutex_t *q_lock;

int base_pid;
//int C[N][K] = {0};
//int G[N][K] = {0};

int E[N][K*2+1] = {0};

int T[N] = {0};

/*partition*/
int P[N] = { 0, 0, 0, 0};
int *progress;
int *x;
int *onGPU;

#ifdef sched
int V[N] = { 1000, 1000, 1000, 1000};
/*shared variable*/

int HOT[N+1] = {1, 0, 0, 1, 0};
int PPriority[CPU][N] = {
                        {3, 2, 1, 0}, //in the ordre of priority
                        {-1, -1, -1, -1},
                        {-1, -1, -1, -1},
                        {-1, -1, -1, -1}
                        };

void cpuSched(int i);
void cpuResume(int i);
void jobRelease(int i);
int onCPU(void);
#endif


void gpuLock(int i);
void gpuUnlock(int i);

int hough_transform(int argc, char* argv[]);
int feature_tracker(int argc, char* argv[]);
int motion_estimation(int argc, char* argv[]);
int video_stabilizer(int argc, char* argv[]);


