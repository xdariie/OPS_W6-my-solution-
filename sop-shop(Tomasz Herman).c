#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHOP_FILENAME "./shop"
#define MIN_SHELVES 8
#define MAX_SHELVES 256
#define MIN_WORKERS 1
#define MAX_WORKERS 64

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)

#define SWAP(x, y)         \
    do                     \
    {                      \
        typeof(x) __x = x; \
        typeof(y) __y = y; \
        x = __y;           \
        y = __x;           \
    } while (0)


typedef struct {
    int do_work;
    pthread_mutex_t do_work_mutex;
    int alive_workers;
    pthread_mutex_t alive_workers_mutex;
    pthread_mutex_t mutex[];
} shared_t;

typedef struct {
    int* shelves;
    int shelves_num;
    int employee_num;
    shared_t* shared;
} shop_t;

void usage(char* program_name)
{
    fprintf(stderr, "Usage: \n");
    fprintf(stderr, "\t%s n m\n", program_name);
    fprintf(stderr, "\t  n - number of items (shelves), %d <= n <= %d\n", MIN_SHELVES, MAX_SHELVES);
    fprintf(stderr, "\t  m - number of workers, %d <= m <= %d\n", MIN_WORKERS, MAX_WORKERS);
    exit(EXIT_FAILURE);
}

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

void shuffle(int* array, int n)
{
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        SWAP(array[i], array[j]);
    }
}

void print_array(int* array, int n)
{
    for (int i = 0; i < n; ++i)
    {
        printf("%3d ", array[i]);
    }
    printf("\n");
}

shop_t create_shop(int shelves_num, int employee_num)
{
    shop_t shop = { .shelves_num = shelves_num };
    int fd = open(SHOP_FILENAME, O_CREAT | O_RDWR, 0666);
    if(fd < 0)
        ERR("open");

    size_t shelves_size = shelves_num * sizeof(int);
    if(ftruncate(fd, shelves_size) < 0)
        ERR("ftruncate");

    shop.shelves = mmap(NULL, shelves_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shop.shelves == MAP_FAILED)
        ERR("mmap");

    close(fd);

    for(int i = 0; i < shelves_num; i++)
        shop.shelves[i] = i;

    shuffle(shop.shelves, shelves_num);

    shop.shared = mmap(NULL, shelves_num * sizeof(pthread_mutex_t) + sizeof(shared_t),
                       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shop.shared == MAP_FAILED)
        ERR("mmap");
    shop.shared->do_work = 1;
    shop.employee_num = employee_num;
    shop.shared->alive_workers = 2 * employee_num;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    for (int i = 0; i < shelves_num; ++i) {
        pthread_mutex_init(&shop.shared->mutex[i], &attr);
    }
    pthread_mutex_init(&shop.shared->do_work_mutex, &attr);
    pthread_mutex_init(&shop.shared->alive_workers_mutex, &attr);

    return shop;
}

void destroy_shop(shop_t shop)
{
    for (int i = 0; i < shop.shelves_num; ++i) {
        pthread_mutex_destroy(&shop.shared->mutex[i]);
    }
    pthread_mutex_destroy(&shop.shared->do_work_mutex);
    pthread_mutex_destroy(&shop.shared->alive_workers_mutex);

    munmap(shop.shared, sizeof(shared_t) + shop.shelves_num * sizeof(pthread_mutex_t));

    msync(shop.shelves, shop.shelves_num * sizeof(int), MS_SYNC);
    munmap(shop.shelves, shop.shelves_num * sizeof(int));
}

void lock_robust(pthread_mutex_t* mutex, shared_t* shared, int index)
{
    int ret = pthread_mutex_lock(mutex);
    if (ret == EOWNERDEAD)
    {
        printf("[%d] Found a dead body in aisle [%d]\n", getpid(), index);
        pthread_mutex_lock(&shared->alive_workers_mutex);
        shared->alive_workers--;
        pthread_mutex_unlock(&shared->alive_workers_mutex);
        pthread_mutex_consistent(mutex);
    }
}

void worker(shop_t shop) {
    printf("[%d] Worker reports for a night shift\n", getpid());
    srand(getpid());

    while (1) {
        pthread_mutex_lock(&shop.shared->do_work_mutex);
        if (shop.shared->do_work == 0) {
            pthread_mutex_unlock(&shop.shared->do_work_mutex);
            break;
        }
        pthread_mutex_unlock(&shop.shared->do_work_mutex);

        int k, l;
        k = rand() % shop.shelves_num;
        l = rand() % shop.shelves_num;
        while(k == l)
            l = rand() % shop.shelves_num;

        if (k > l) SWAP(l, k);

        lock_robust(&shop.shared->mutex[k], shop.shared, k);
        lock_robust(&shop.shared->mutex[l], shop.shared, l);

        if (shop.shelves[k] > shop.shelves[l])
            SWAP(shop.shelves[k], shop.shelves[l]);

        if (rand() % 10 == 0) {
            printf("[%d] Trips over pallet and dies\n", getpid());
            abort();
        }

        pthread_mutex_unlock(&shop.shared->mutex[k]);
        pthread_mutex_unlock(&shop.shared->mutex[l]);

        ms_sleep(100);
    }

    printf("Night shift in Bitronka is over\n");
}

void manager(shop_t shop) {
    printf("[%d] Manager reports for a night shift\n", getpid());

    while(1)
    {
        int is_sorted = 1;

        for (int i = 0; i < shop.shelves_num; ++i) {
            lock_robust(&shop.shared->mutex[i], shop.shared, i);
        }

        print_array(shop.shelves, shop.shelves_num);

        for (int i = 0; i < shop.shelves_num; ++i) {
            if (shop.shelves[i] != i){
                is_sorted = 0;
                break;
            }
        }

        for (int i = 0; i < shop.shelves_num; ++i) {
            pthread_mutex_unlock(&shop.shared->mutex[i]);
        }

        if (is_sorted) {
            pthread_mutex_lock(&shop.shared->do_work_mutex);
            shop.shared->do_work = 0;
            pthread_mutex_unlock(&shop.shared->do_work_mutex);
            break;
        }

        pthread_mutex_lock(&shop.shared->alive_workers_mutex);
        if (shop.shared->alive_workers == 0) {
            pthread_mutex_unlock(&shop.shared->alive_workers_mutex);
            break;
        }
        pthread_mutex_unlock(&shop.shared->alive_workers_mutex);

        msync(shop.shelves, shop.shelves_num * sizeof(int), MS_SYNC);
        ms_sleep(500);
    }

    printf("Night shift in Bitronka is over\n");
}

void create_workers(int num_employees, shop_t shop)
{
    for (int i = 0; i < num_employees; ++i) {
        pid_t pid = fork();
        if (pid == -1) ERR("fork");
        if (pid == 0) {
            worker(shop);
            exit(EXIT_SUCCESS);
        }
    }

    pid_t pid = fork();
    if (pid == -1) ERR("fork");
    if (pid == 0) {
        manager(shop);
        exit(EXIT_SUCCESS);
    }
}

int main(int argc, char** argv) {
    if (argc != 3)
        usage(argv[0]);
    int shelves_num = atoi(argv[1]);
    int num_employees = atoi(argv[2]);
    if (shelves_num < MIN_SHELVES || shelves_num > MAX_SHELVES || num_employees < MIN_WORKERS || num_employees > MAX_WORKERS)
        usage(argv[0]);

    shop_t shop = create_shop(shelves_num, num_employees);
    print_array(shop.shelves, shelves_num);

    create_workers(num_employees, shop);

    while(wait(NULL) > 0)
        ;

    print_array(shop.shelves, shelves_num);

    destroy_shop(shop);
}
