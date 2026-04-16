// 2. Shared Memory Allocation
// A. Anonymous Shared Memory (For Mutexes/Barriers)
// Used when parent and children share memory, but no external processes need access.
// Allocation
my_struct_t *shared = mmap(NULL, sizeof(my_struct_t), 
                           PROT_READ | PROT_WRITE, 
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
if (shared == MAP_FAILED) ERR("mmap anonymous");

// Cleanup
munmap(shared, sizeof(my_struct_t));

// B. Named Shared Memory (For Data Arrays)

// Used when independent processes need to attach to the same memory block using a string name.

// 1. Open / Create
int shm_fd = shm_open("/my_shm_name", O_CREAT | O_RDWR, 0666);
if (shm_fd == -1) ERR("shm_open");

// 2. Set Size (CRITICAL: Do this only once in the creator process!)
if (ftruncate(shm_fd, sizeof(double) * array_size) == -1) ERR("ftruncate");

// 3. Map into memory
double *data = mmap(NULL, sizeof(double) * array_size, 
                    PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
if (data == MAP_FAILED) ERR("mmap named");

// Cleanup
munmap(data, sizeof(double) * array_size);
close(shm_fd);
shm_unlink("/my_shm_name"); // Only the very last process does this

// 3. Robust Mutexes (Process-Shared & Crash-Proof)

pthread_mutexattr_t mattr;
pthread_mutexattr_init(&mattr);
pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED); // Share across forks
pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);    // Handle sudden deaths

if (pthread_mutex_init(&shared->mtx, &mattr) != 0) ERR("mutex_init");
pthread_mutexattr_destroy(&mattr);

// Locking and Handling Dead Owners:
int rc = pthread_mutex_lock(&shared->mtx);
if (rc == EOWNERDEAD) {
    // A process died holding the lock! We now own it, but state is inconsistent.
    printf("Recovering from a dead process!\n");
    
    // TODO: Fix any broken shared variables here (e.g., decrement process counter)
    
    // Repair the mutex so future locks work normally
    pthread_mutex_consistent(&shared->mtx); 
} else if (rc != 0) {
    ERR("mutex_lock");
}

// ... Do critical section work ...

pthread_mutex_unlock(&shared->mtx);


// 4. Process-Shared Barriers

pthread_barrierattr_t battr;
pthread_barrierattr_init(&battr);
pthread_barrierattr_setpshared(&battr, PTHREAD_PROCESS_SHARED);

// Wait for exactly (N_CHILDREN + 1_PARENT) processes
if (pthread_barrier_init(&shared->barrier, &battr, n + 1) != 0) ERR("barrier_init");
pthread_barrierattr_destroy(&battr);

//Waiting
int rc = pthread_barrier_wait(&shared->barrier);
if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) ERR("barrier_wait");


// 5. Named Semaphores

// Open or Create (Initializes to CAPACITY)
sem_t *my_sem = sem_open("/my_sem_name", O_CREAT, 0666, CAPACITY);
if (my_sem == SEM_FAILED) ERR("sem_open");

// Use
if (sem_wait(my_sem) == -1) ERR("sem_wait");
// ... work ...
if (sem_post(my_sem) == -1) ERR("sem_post");

// Cleanup
sem_close(my_sem);
sem_unlink("/my_sem_name"); // Only call this once at the end of the program

// 6. Waiting for Children (Interrupt-Safe)
// When waiting for children, wait() might return an error if a signal (like SIGINT) interrupts it. Always handle EINTR.

int remaining = n;
while (remaining > 0) {
    if (wait(NULL) == -1) {
        if (errno == EINTR) continue; // It was just a signal, keep waiting
        ERR("wait");
    }
    remaining--;
}

// 7. Signal Handling (Graceful Shutdown)
// Use sigaction instead of the older signal() function for POSIX compliance.

volatile sig_atomic_t keep_running = 1;

void sigint_handler(int sig) {
    keep_running = 0;
}

// In main():
struct sigaction sa;
sa.sa_handler = sigint_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
if (sigaction(SIGINT, &sa, NULL) == -1) ERR("sigaction");

// Then use `while(keep_running)` for your main worker loops.
