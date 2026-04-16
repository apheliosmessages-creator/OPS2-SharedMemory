#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>

// Shared memory structure to aggregate results
typedef struct {
    pthread_mutex_t lock;
    int counts[256];
    int failure;
} shared_data_t;

// Stage 1 & 2: Print file using mmap and write (NO streams or read)
void print_file_contents(const char* filepath, size_t file_size) {
    if (file_size == 0) return;

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { perror("open for print"); exit(EXIT_FAILURE); }

    char *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap for print"); exit(EXIT_FAILURE); }

    size_t written = 0;
    printf("--- File Contents ---\n");
    fflush(stdout); // Flush before using raw write

    while (written < file_size) {
        ssize_t res = write(STDOUT_FILENO, map + written, file_size - written);
        if (res < 0) {
            if (errno == EINTR) continue;
            perror("write"); exit(EXIT_FAILURE);
        }
        written += res;
    }
    write(STDOUT_FILENO, "\n\n", 2);

    munmap(map, file_size);
    close(fd);
}

// Stages 4, 5, 6, 7 & 9: Child worker logic
void child_worker(int id, int N, const char* filepath, size_t file_size, shared_data_t* shared) {
    srand(getpid() ^ time(NULL)); // Seed random for the 3% chance

    // Stage 5: Move the file opening operation to the child process
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { perror("child open"); exit(EXIT_FAILURE); }

    unsigned char *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("child mmap"); exit(EXIT_FAILURE); }

    // Stage 4: Distribute workload
    size_t chunk_size = file_size / N;
    size_t start = id * chunk_size;
    size_t end = (id == N - 1) ? file_size : start + chunk_size;

    // Stage 6: Count characters independently
    int local_counts[256] = {0};
    for (size_t i = start; i < end; i++) {
        local_counts[map[i]]++;
    }

    // Unmap early to save resources before waiting on lock
    munmap(map, file_size);
    close(fd);

    // Stage 7: Use shared memory to transfer results to the parent
    int rc = pthread_mutex_lock(&shared->lock);
    
    // Stage 9 (Part 1): Check if another child died while holding the lock
    if (rc == EOWNERDEAD) {
        shared->failure = 1;
        pthread_mutex_consistent(&shared->lock); // Repair state
        pthread_mutex_unlock(&shared->lock);
        exit(EXIT_FAILURE); // Cascade the failure
    } else if (rc != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }

    // Check if a failure was previously flagged
    if (shared->failure) {
        pthread_mutex_unlock(&shared->lock);
        exit(EXIT_FAILURE);
    }

    // Stage 9 (Part 2): 3% chance of sudden termination when reporting
    if (rand() % 100 < 3) {
        // Abort while holding the lock. 
        // PTHREAD_MUTEX_ROBUST guarantees the next process will get EOWNERDEAD.
        abort(); 
    }

    // Apply local counts to shared memory
    for (int i = 0; i < 256; i++) {
        shared->counts[i] += local_counts[i];
    }

    pthread_mutex_unlock(&shared->lock);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <file_path> <N_processes>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* filepath = argv[1];
    int N = atoi(argv[2]);
    if (N <= 0) {
        fprintf(stderr, "N must be > 0\n");
        exit(EXIT_FAILURE);
    }

    struct stat sb;
    if (stat(filepath, &sb) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }
    size_t file_size = sb.st_size;

    // Stages 1 & 2: Print file using mmap and write
    print_file_contents(filepath, file_size);

    // Create Anonymous Shared Memory for aggregation
    shared_data_t *shared = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, 
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap shared");
        exit(EXIT_FAILURE);
    }

    memset(shared->counts, 0, sizeof(shared->counts));
    shared->failure = 0;

    // Initialize the robust process-shared mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&shared->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    // Fork N child processes
    for (int i = 0; i < N; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            child_worker(i, N, filepath, file_size, shared);
        }
    }

    // Wait for all children and monitor for sudden terminations
    int child_crashed = 0;
    for (int i = 0; i < N; i++) {
        int status;
        wait(&status);
        
        // If a child was killed by a signal (e.g., SIGABRT) or exited with an error
        if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0)) {
            child_crashed = 1;
        }
    }

    // Stage 8 & 9: Print summary OR failure message
    if (child_crashed || shared->failure) {
        printf("Computation failed.\n");
    } else {
        printf("--- Character Count Summary ---\n");
        for (int i = 0; i < 256; i++) {
            if (shared->counts[i] > 0) {
                // Make non-printable characters readable in the summary
                char display_char = (i >= 32 && i <= 126) ? (char)i : '.'; 
                printf("Byte %3d ('%c') : %d occurrences\n", i, display_char, shared->counts[i]);
            }
        }
    }

    // Cleanup
    pthread_mutex_destroy(&shared->lock);
    munmap(shared, sizeof(shared_data_t));

    return 0;
}