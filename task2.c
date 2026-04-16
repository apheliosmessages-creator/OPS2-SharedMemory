#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), kill(0, SIGKILL), exit(EXIT_FAILURE))

#define SHM_NAME "/mc_shm"
#define SEM_NAME "/mc_init_sem"

// Shared memory structure
typedef struct {
    pthread_mutex_t mtx;
    int process_count;
    uint64_t total_points;
    uint64_t hit_points;
    float a;
    float b;
} shm_data_t;

volatile sig_atomic_t keep_running = 1;

void sigint_handler(int sig)
{
    keep_running = 0;
}

// Values of this function are in range (0,1]
double func(double x)
{
    usleep(2000);
    return exp(-x * x);
}

/**
 * It counts hit points by Monte Carlo method.
 * Use it to process one batch of computation.
 * @param N Number of points to randomize
 * @param a Lower bound of integration
 * @param b Upper bound of integration
 * @return Number of points which was hit.
 */
int randomize_points(int N, float a, float b)
{
    int result = 0;
    for (int i = 0; i < N; ++i)
    {
        double rand_x = ((double)rand() / RAND_MAX) * (b - a) + a;
        double rand_y = ((double)rand() / RAND_MAX);
        double real_y = func(rand_x);

        if (rand_y <= real_y)
            result++;
    }
    return result;
}

/**
 * This function calculates approximation of integral from counters of hit and total points.
 * @param total_randomized_points Number of total randomized points.
 * @param hit_points Number of hit points.
 * @param a Lower bound of integration
 * @param b Upper bound of integration
 * @return The approximation of integral
 */
double summarize_calculations(uint64_t total_randomized_points, uint64_t hit_points, float a, float b)
{
    if (total_randomized_points == 0) return 0.0;
    return (b - a) * ((double)hit_points / (double)total_randomized_points);
}

/**
 * This function locks mutex and can sometime die (it has 2% chance to die).
 * It cannot die if lock would return an error.
 * It doesn't handle any errors. It's users responsibility.
 * Use it only in STAGE 4.
 *
 * @param mtx Mutex to lock
 * @return Value returned from pthread_mutex_lock.
 */
int random_death_lock(pthread_mutex_t* mtx)
{
    int ret = pthread_mutex_lock(mtx);
    if (ret)
        return ret;

    // 2% chance to die
    if (rand() % 50 == 0)
        abort();
    return ret;
}

void usage(char* argv[])
{
    printf("%s a b N - calculating integral with multiple processes\n", argv[0]);
    printf("a - Start of segment for integral (default: -1)\n");
    printf("b - End of segment for integral (default: 1)\n");
    printf("N - Size of batch to calculate before reporting to shared memory (default: 1000)\n");
}

int main(int argc, char* argv[])
{
    // Default arguments
    float a = -1.0f;
    float b = 1.0f;
    int N = 1000;

    if (argc > 1) a = atof(argv[1]);
    if (argc > 2) b = atof(argv[2]);
    if (argc > 3) N = atoi(argv[3]);

    srand(getpid() ^ time(NULL));

    // Setup SIGINT handler (Stage 3)
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) ERR("sigaction");

    // Initialization Phase (Stage 1)
    // Create/Open semaphore to prevent race conditions during shared memory creation
    sem_t *init_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (init_sem == SEM_FAILED) ERR("sem_open");

    if (sem_wait(init_sem) == -1) ERR("sem_wait");

    int is_creator = 0;
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0) {
        is_creator = 1;
        if (ftruncate(fd, sizeof(shm_data_t)) == -1) ERR("ftruncate");
    } else if (errno == EEXIST) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd == -1) ERR("shm_open existing");
    } else {
        ERR("shm_open creation");
    }

    shm_data_t *data = mmap(NULL, sizeof(shm_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) ERR("mmap");

    if (is_creator) {
        // Init Robust Mutex
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        
        if (pthread_mutex_init(&data->mtx, &attr) != 0) ERR("pthread_mutex_init");
        pthread_mutexattr_destroy(&attr);

        data->process_count = 1;
        data->total_points = 0;
        data->hit_points = 0;
        data->a = a;
        data->b = b;
    } else {
        // Joiner checks limits and increments count
        data->process_count++;
        if (data->a != a || data->b != b) {
            fprintf(stderr, "Warning: Process bounds differ from shared memory bounds. Overriding local bounds.\n");
            a = data->a;
            b = data->b;
        }
    }

    printf("Collaborating processes: %d\n", data->process_count);
    if (sem_post(init_sem) == -1) ERR("sem_post");

    // Stage 1 required a 2 sec sleep. Keeping it briefly before loop starts.
    sleep(2);

    // Computation Loop (Stages 2, 3, 4)
    while (keep_running) {
        int hits = randomize_points(N, a, b);

        // Stage 4: Use random_death_lock to simulate crashes
        int ret = random_death_lock(&data->mtx);
        if (ret == EOWNERDEAD) {
            fprintf(stderr, "\n[Process %d] Detected previous owner death. Decrementing ghost process count.\n", getpid());
            data->process_count--; // Decrement because the dead process couldn't do it
            pthread_mutex_consistent(&data->mtx);
        } else if (ret != 0) {
            ERR("random_death_lock");
        }

        // Inside Critical Section
        data->hit_points += hits;
        data->total_points += N;
        printf("PID: %d | Computed Batch! Total Samples: %lu | Hits: %lu\n", getpid(), data->total_points, data->hit_points);

        if (pthread_mutex_unlock(&data->mtx) != 0) ERR("pthread_mutex_unlock");
    }

    // Finalization / Clean up
    printf("\nProcess %d detaching...\n", getpid());

    int ret = pthread_mutex_lock(&data->mtx);
    if (ret == EOWNERDEAD) {
        data->process_count--;
        pthread_mutex_consistent(&data->mtx);
    } else if (ret != 0) {
        ERR("pthread_mutex_lock cleanup");
    }

    data->process_count--;
    int current_count = data->process_count;
    uint64_t final_total = data->total_points;
    uint64_t final_hits = data->hit_points;
    float final_a = data->a;
    float final_b = data->b;

    pthread_mutex_unlock(&data->mtx);

    // If this is the absolute last process attached, print results and destroy SHM
    if (current_count == 0) {
        double result = summarize_calculations(final_total, final_hits, final_a, final_b);
        printf("\n==================================================\n");
        printf("ALL PROCESSES DETACHED. FINAL INTEGRATION RESULTS:\n");
        printf("Lower Bound (a) : %f\n", final_a);
        printf("Upper Bound (b) : %f\n", final_b);
        printf("Total Samples   : %lu\n", final_total);
        printf("Total Hits      : %lu\n", final_hits);
        printf("Integral Approx : %lf\n", result);
        printf("==================================================\n");

        if (munmap(data, sizeof(shm_data_t)) == -1) ERR("munmap");
        if (shm_unlink(SHM_NAME) == -1) ERR("shm_unlink");
        if (sem_unlink(SEM_NAME) == -1) ERR("sem_unlink");
        
        printf("Shared memory properly destroyed.\n");
    } else {
        if (munmap(data, sizeof(shm_data_t)) == -1) ERR("munmap");
    }

    return EXIT_SUCCESS;
}