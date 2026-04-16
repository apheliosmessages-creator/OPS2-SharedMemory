#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>

#define KEYBOARD_CAP 10
#define SHARED_MEM_NAME "/memory"
#define MIN_STUDENTS KEYBOARD_CAP
#define MAX_STUDENTS 20
#define MIN_KEYBOARDS 1
#define MAX_KEYBOARDS 5
#define MIN_KEYS 5
#define MAX_KEYS KEYBOARD_CAP

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)

void usage(char* program_name)
{
    fprintf(stderr, "Usage: \n");
    fprintf(stderr, "\t%s n m k\n", program_name);
    fprintf(stderr, "\t  n - number of students, %d <= n <= %d\n", MIN_STUDENTS, MAX_STUDENTS);
    fprintf(stderr, "\t  m - number of keyboards, %d <= m <= %d\n", MIN_KEYBOARDS, MAX_KEYBOARDS);
    fprintf(stderr, "\t  k - number of keys in a keyboard, %d <= k <= %d\n", MIN_KEYS, MAX_KEYS);
    exit(EXIT_FAILURE);
}

typedef struct shared{
    pthread_barrier_t barrier;
    pthread_mutex_t keyboards[MAX_KEYBOARDS*MAX_KEYS];
    int find_dead;
    pthread_mutex_t find_dead_mutex;
}shared_t;

void ms_sleep(unsigned int milli)
{
    time_t sec = (int)(milli / 1000);
    milli = milli - (sec * 1000);
    struct timespec ts = {0};
    ts.tv_sec = sec;
    ts.tv_nsec = milli * 1000000L;
    if (nanosleep(&ts, &ts))
        ERR("nanosleep");
}

void print_keyboards_state(double* keyboards, int m, int k)
{
    for (int i=0;i<m;++i)
    {
        printf("Klawiatura nr %d:\n", i + 1);
        for (int j=0;j<k;++j)
            printf("  %e", keyboards[i * k + j]);
        printf("\n\n");
    }
}

void child_work(int m, shared_t* shared, int k){

    pthread_barrier_wait(&shared->barrier);


    int fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
    if(fd == -1){
        ERR("shm_open");
    }
    double* shared_mem = mmap(NULL, m*k*sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(shared_mem == MAP_FAILED){
        ERR("mmap");
    }


    char buff[20];
    srand(getpid());
    sem_t *sem_arr[MAX_KEYBOARDS];
    for(int i = 0; i < m; i++){
        snprintf(buff, sizeof(buff), "/sop-sem-%d", i);
        sem_t* sem = sem_open(buff, O_CREAT, 0666, KEYBOARD_CAP);
        if(sem == SEM_FAILED){
            ERR("sem_open");
        }
        sem_arr[i] = sem;
    }


    int ran;
    for(;1;){
        pthread_mutex_lock(&shared->find_dead_mutex);
        if(shared->find_dead == 1){
            pthread_mutex_unlock(&shared->find_dead_mutex);
            break;
        }
        pthread_mutex_unlock(&shared->find_dead_mutex);
        ran = rand()%(m*k);
        sem_wait(sem_arr[ran%m]);
        ms_sleep(300);
        if(pthread_mutex_lock(&shared->keyboards[ran]) == EOWNERDEAD){
            pthread_mutex_lock(&shared->find_dead_mutex);
            shared->find_dead = 1;
            printf("Student <%d>: someone is lying here, help!!!\n", getpid());
            pthread_mutex_unlock(&shared->find_dead_mutex);
            pthread_mutex_consistent(&shared->keyboards[ran]);
        }
        if(rand()%100 == 69){
            printf("Student <%d>: I have no more strength!\n", getpid());
            sem_post(sem_arr[ran%m]);
            abort();
        }
        shared_mem[ran] /= 3;
        pthread_mutex_unlock(&shared->keyboards[ran]);
        sem_post(sem_arr[ran%m]);
        printf("Student <%d>: cleaning keyboard <%d>, key <%d>\n", getpid(), ran%m, ran/m);
        }


    for(int i = 0; i < m; i++){
        if(sem_close(sem_arr[i]) == -1){
            ERR("sem_close");
        }
    }
    munmap(shared, sizeof(shared_t));
    munmap(shared_mem, sizeof(double)*m*k);
    close(fd);
}

int main(int argc, char** argv) { 

    if(argc != 4){
        usage(argv[0]);
    }
    int n = atoi(argv[1]);
    int m = atoi(argv[2]);
    int k = atoi(argv[3]);

    if(n < KEYBOARD_CAP || n > 20 || m < 1 || m > 5 || k < 5 || k > KEYBOARD_CAP){
        usage(argv[0]);
    }

    char buff[20];


    for(int i = 0; i < m; i++){
        snprintf(buff, sizeof(buff), "/sop-sem-%d", i);
        sem_unlink(buff);
    }

    shared_t *shared = mmap(NULL, sizeof(shared_t), PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(shared == MAP_FAILED){
        ERR("mmap");
    }
    pthread_barrierattr_t attr;
    pthread_barrierattr_init(&attr);
    pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_barrier_init(&shared->barrier, &attr, n+1);
    pthread_barrierattr_destroy(&attr);

    pthread_mutexattr_t attr_mutex;
    pthread_mutexattr_init(&attr_mutex);
    pthread_mutexattr_setpshared(&attr_mutex, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr_mutex, PTHREAD_MUTEX_ROBUST);
    for(int i = 0; i < m*k; i++){
        pthread_mutex_init(&shared->keyboards[i], &attr_mutex);
    }
    pthread_mutex_init(&shared->find_dead_mutex, &attr_mutex);
    pthread_mutexattr_destroy(&attr_mutex);
    shared->find_dead = 0;
    

    for(int i = 0; i < n; i++){
        pid_t pid = fork();
        if(pid == -1){
            ERR("fork");
        }
        if(pid == 0){
            child_work(m, shared, k);
            exit(EXIT_SUCCESS);
        }
    }

    int fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if(fd == -1){
        ERR("shm_open");
    }
    if(ftruncate(fd, m*k*sizeof(double)) == -1){
        ERR("ftruncate");
    }
    double* shared_mem = mmap(NULL, m*k*sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(shared_mem == MAP_FAILED){
        ERR("mmap");
    }
    for(int i = 0; i < k*m; i++){
        shared_mem[i] = 1.0;
    }
    ms_sleep(500);
    pthread_barrier_wait(&shared->barrier);
    
    

    while(wait(NULL) > 0){}
    for(int i = 0; i < m; i++){
        snprintf(buff, sizeof(buff), "/sop-sem-%d", i);
        sem_unlink(buff);
    }

    print_keyboards_state(shared_mem, m, k);

    close(fd);
    pthread_barrier_destroy(&shared->barrier);
        for(int i = 0; i < m*k; i++){
        pthread_mutex_destroy(&shared->keyboards[i]);
    }
    munmap(shared, sizeof(shared_t));
    munmap(shared_mem, sizeof(double)*m*k);
    printf("Cleaning finished!\n");
}