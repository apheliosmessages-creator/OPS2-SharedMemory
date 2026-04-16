#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <time.h>

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

void ms_sleep(unsigned int milli)
{
    struct timespec ts;
    ts.tv_sec = milli / 1000;
    ts.tv_nsec = (milli % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
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

// --- STAGE 2: Block of anonymous shared memory ---
typedef struct {
    // --- STAGE 2: Barrier shared between processes ---
    pthread_barrier_t barrier;
    
    // --- STAGE 4: Shared flag to announce panic, protected by a mutex ---
    int panic_flag;
    pthread_mutex_t flag_mutex;
    
    // --- STAGE 3: Separate mutex for each key to prevent simultaneous cleaning ---
    pthread_mutex_t key_mutexes[MAX_KEYBOARDS * MAX_KEYS];
} shared_data_t;

void student_work(int m, int k, shared_data_t *shared)
{
    // --- STAGE 2: Students, before creating semaphores, wait on the barrier ---
    int rc = pthread_barrier_wait(&shared->barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) ERR("pthread_barrier_wait");

    // --- STAGE 3: Student processes open and map the memory object SHARED_MEM_NAME ---
    int shm_fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) ERR("shm_open student");
    
    double *keyboards = mmap(NULL, m * k * sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (keyboards == MAP_FAILED) ERR("mmap student");

    srand(getpid() ^ time(NULL)); 
    sem_t* sems[MAX_KEYBOARDS];
    char name[32];

    // --- STAGE 1: Each child opens m named semaphores (/sop-sem-1, etc.) ---
    for (int i = 0; i < m; i++) {
        sprintf(name, "/sop-sem-%d", i + 1);
        // O_CREAT initializes with KEYBOARD_CAP if it doesn't exist
        if ((sems[i] = sem_open(name, O_CREAT, 0666, KEYBOARD_CAP)) == SEM_FAILED)
            ERR("sem_open");
    }

    // --- STAGE 4: Students perform the cleaning process in an infinite loop ---
    while (1)
    {
        // --- STAGE 4: Check if another student announced a panic ---
        pthread_mutex_lock(&shared->flag_mutex);
        int should_panic = shared->panic_flag;
        pthread_mutex_unlock(&shared->flag_mutex);

        // --- STAGE 4: Run out of the room in panic (break the loop) ---
        if (should_panic) break; 

        // --- STAGE 1: Chooses a random keyboard ---
        int kb_idx = rand() % m;
        
        // --- STAGE 3: Draws a key number ---
        int key_idx = rand() % k;
        int global_key_idx = (kb_idx * k) + key_idx;
        
        // --- STAGE 1: Attempts to approach it, waiting if there is no space ---
        if (sem_wait(sems[kb_idx]) == -1) ERR("sem_wait");
        
        // --- STAGE 3: Protect against simultaneous cleaning of the same key ---
        rc = pthread_mutex_lock(&shared->key_mutexes[global_key_idx]);
        
        // --- STAGE 4: When a student attempts to clean a key another couldn't finish (EOWNERDEAD) ---
        if (rc == EOWNERDEAD) 
        {
            printf("Student %d: someone is lying here, help!!!\n", getpid());
            
            // --- STAGE 4: Announce it to all students using a shared flag ---
            pthread_mutex_lock(&shared->flag_mutex);
            shared->panic_flag = 1;
            pthread_mutex_unlock(&shared->flag_mutex);
            
            // Repair the broken mutex so we can unlock it properly
            pthread_mutex_consistent(&shared->key_mutexes[global_key_idx]);
            pthread_mutex_unlock(&shared->key_mutexes[global_key_idx]);
            sem_post(sems[kb_idx]);
            
            // --- STAGE 4: Run out of the room in panic ---
            break; 
        } 
        else if (rc != 0) 
        {
            ERR("pthread_mutex_lock");
        }
        
        // --- STAGE 1 & 3: Print message with keyboard and key number ---
        printf("Student %d: cleaning keyboard %d, key %d\n", getpid(), kb_idx + 1, key_idx + 1);
        
        // --- STAGE 1: Waits 300ms ---
        ms_sleep(300);
        
        // --- STAGE 4: 1% chance of falling from exhaustion ---
        if (rand() % 100 == 0) 
        {
            printf("Student %d: I have no more strength!\n", getpid());
            // --- STAGE 4: Releasing the semaphore, and calling abort() before updating dirt ---
            sem_post(sems[kb_idx]);
            abort(); 
        }

        // --- STAGE 3: Divide the value of the corresponding field by 3 ---
        keyboards[global_key_idx] /= 3.0;
        
        // Unlock key and release keyboard space
        if (pthread_mutex_unlock(&shared->key_mutexes[global_key_idx]) != 0) ERR("pthread_mutex_unlock");
        
        // --- STAGE 1: Releases the semaphore ---
        if (sem_post(sems[kb_idx]) == -1) ERR("sem_post");
    }

    // --- STAGE 1 & 3: Process finishes and cleans up local references ---
    for (int i = 0; i < m; i++) sem_close(sems[i]);
    munmap(keyboards, m * k * sizeof(double));
    close(shm_fd);
    
    exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) 
{
    // --- STAGE 1: Program takes 3 arguments ---
    if (argc != 4) usage(argv[0]);

    int n = atoi(argv[1]);
    int m = atoi(argv[2]);
    int k = atoi(argv[3]);

    if (n < MIN_STUDENTS || n > MAX_STUDENTS || m < MIN_KEYBOARDS || m > MAX_KEYBOARDS || k < MIN_KEYS || k > MAX_KEYS)
        usage(argv[0]);

    // --- STAGE 1: Immediately after start, remove semaphores if they exist ---
    char name[32];
    for (int i = 0; i < m; i++) {
        sprintf(name, "/sop-sem-%d", i + 1);
        sem_unlink(name);
    }
    // --- STAGE 3: Initial cleanup for shared memory if it exists ---
    shm_unlink(SHARED_MEM_NAME);

    // --- STAGE 2: Create anonymous shared memory BEFORE creating child processes ---
    shared_data_t *shared = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, 
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) ERR("mmap anonymous");
    shared->panic_flag = 0;

    // --- STAGE 2: Create barrier immediately after shared memory (wait for n+1 processes) ---
    pthread_barrierattr_t battr;
    pthread_barrierattr_init(&battr);
    pthread_barrierattr_setpshared(&battr, PTHREAD_PROCESS_SHARED);
    if (pthread_barrier_init(&shared->barrier, &battr, n + 1) != 0) ERR("pthread_barrier_init");
    pthread_barrierattr_destroy(&battr);

    // --- STAGE 3: Place mutexes in the anonymous shared memory ---
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    
    // --- STAGE 4: Make mutexes robust so we can catch EOWNERDEAD ---
    pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
    
    if (pthread_mutex_init(&shared->flag_mutex, &mattr) != 0) ERR("pthread_mutex_init");
    for (int i = 0; i < m * k; i++) {
        if (pthread_mutex_init(&shared->key_mutexes[i], &mattr) != 0) ERR("pthread_mutex_init");
    }
    pthread_mutexattr_destroy(&mattr);

    // --- STAGE 1: Create n child processes (students) ---
    for(int i = 0; i < n; i++) {
        pid_t pid = fork();
        if(pid < 0) ERR("fork");
        if(pid == 0) student_work(m, k, shared);
    }

    // --- STAGE 3: Perform creation of named memory object AFTER creating students ---
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) ERR("shm_open main");
    if (ftruncate(shm_fd, m * k * sizeof(double)) == -1) ERR("ftruncate");
    
    // --- STAGE 3: Map the memory object ---
    double *keyboards = mmap(NULL, m * k * sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (keyboards == MAP_FAILED) ERR("mmap main");

    // --- STAGE 3: Initialize by filling fields with ones (1.0) ---
    for (int i = 0; i < m * k; i++) {
        keyboards[i] = 1.0;
    }

    // --- STAGE 2: Main process sleeps for 500ms, then waits on the barrier ---
    ms_sleep(500);
    int rc = pthread_barrier_wait(&shared->barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) ERR("pthread_barrier_wait main");

    // --- STAGE 1: Main process waits for all child processes to finish ---
    int remaining = n;
    while(remaining > 0) {
        if(wait(NULL) == -1) {
            if (errno == EINTR) continue; // Ignore interrupts
            ERR("wait");
        }
        remaining--;
    }

    // --- STAGE 3: Main process prints the state of all keyboards ---
    print_keyboards_state(keyboards, m, k);

    // --- STAGE 1 & 3 & 4: Final cleanup of resources ---
    munmap(keyboards, m * k * sizeof(double));
    close(shm_fd);
    
    // --- STAGE 3: Removes the shared memory object ---
    shm_unlink(SHARED_MEM_NAME);

    // --- STAGE 1: Removes all created semaphores ---
    for (int i = 0; i < m; i++) {
        sprintf(name, "/sop-sem-%d", i + 1);
        sem_unlink(name);
    }
    
    pthread_mutex_destroy(&shared->flag_mutex);
    for (int i = 0; i < m * k; i++) {
        pthread_mutex_destroy(&shared->key_mutexes[i]);
    }
    pthread_barrier_destroy(&shared->barrier);
    munmap(shared, sizeof(shared_data_t));

    // --- STAGE 1: Prints Cleaning finished! ---
    printf("Cleaning finished!\n");
    return EXIT_SUCCESS;
}