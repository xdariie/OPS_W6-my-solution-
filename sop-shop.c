#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
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



typedef struct{
    pthread_mutex_t shelf_mutex[MAX_SHELVES];
    int n;
    int m;
}shared_data_t;

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

void swap(int* x, int* y)
{
    int tmp = *y;
    *y = *x;
    *x = tmp;
}

void shuffle(int* array, int n)
{
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap(&array[i], &array[j]);
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

int mutex_init(shared_data_t* shared_data, int n){
    pthread_mutexattr_t attr;

    if(pthread_mutexattr_init(&attr) !=0){
        ERR("pthread_mutexattr_init");
    }

    if(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)!=0){
        ERR("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&attr);
    }

    for(int i=0;i<n;i++){
        if(pthread_mutex_init(&shared_data->shelf_mutex[i], &attr)!=0){
            ERR("pthread_mutex_init");
            pthread_mutexattr_destroy(&attr);
        }
    }

    if(pthread_mutexattr_destroy(&attr)!=0){
        ERR("pthread_mutexattr_destroy");
    }

    return 0;
}

void childwork(int *shop, shared_data_t *shared_data){
    printf("[%d] Worker reports for a night shift.\n", getpid());
    fflush(stdout);

   
    int n=shared_data->n;
     
    unsigned int seed = (time(NULL)^getpid());
    
    for(int i=0;i<10; i++){
        
        int a,b;
        
        a=rand_r(&seed)%n;

        do{
            b=rand_r(&seed)%n;
        }while(a==b);

        int first, second;
        if(a<b){
            first=a;
            second=b;
        }else{
            first=b;
            second=a;
        }
        if(pthread_mutex_lock(&shared_data->shelf_mutex[first])!=0){
                ERR("pthread_mutex_lock");
            }
        if(pthread_mutex_lock(&shared_data->shelf_mutex[second])!=0){
            ERR("pthread_mutex_lock");
            pthread_mutex_unlock(&shared_data->shelf_mutex[first]);
        }
        if(shop[a]<shop[b]){
           

            int temp=shop[a];
            ms_sleep(100);
            shop[a]=shop[b];
            shop[b]=temp;

        }

        if(pthread_mutex_unlock(&shared_data->shelf_mutex[second])!=0){
            ERR("pthread_mutex_unlock");
        }
        if(pthread_mutex_unlock(&shared_data->shelf_mutex[first])!=0){
            ERR("pthread_mutex_unlock");
        }
            

    }
}

int main(int argc, char** argv) { 
    if(argc!=3){
        usage(argv[0]);
    }
    int n= atoi(argv[1]);
    if(n<MIN_SHELVES ||n>MAX_SHELVES){
        usage(argv[0]);
    }
    int m=atoi(argv[2]);
    if(m<MIN_WORKERS ||m>MAX_WORKERS){
        usage(argv[0]);
    }

    


    int fd = open(SHOP_FILENAME, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if(fd<0){
        ERR("open");
    }

    size_t shop_size = n* sizeof(int);
    if(ftruncate(fd, shop_size)<0){
        ERR("ftruncate");
        close(fd);
    }

    int* shop = mmap(NULL, shop_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(shop==MAP_FAILED){
        ERR("mmap");
    }

    for(int i=0; i<n; i++){
        shop[i]=i+1;
    }

    shared_data_t *shared_data= mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(shared_data==MAP_FAILED){
        ERR("mmap");
    }

    shared_data->n = n;
    shared_data->m=m;

    if(mutex_init(shared_data, n)<0){
        munmap(shared_data, sizeof(shared_data_t));
        munmap(shop, shop_size);
        close(fd);
    }

    shuffle(shop, n);

    print_array(shop, n);

    for(int i=0; i<m;i++){
        pid_t pid =fork();
        if(pid<0){
            ERR("fork");
        }
        if(pid==0){
            childwork(shop, shared_data);
            exit(EXIT_SUCCESS);
        }
        // else{
        //     parentwork();
        //     
        // }
        
    }
    while(wait(NULL)>0){}


    print_array(shop, n);
    printf("Night shift in Bitronka is over.\n");

    for(int i=0; i<n;i++){
        if(pthread_mutex_destroy(&shared_data->shelf_mutex[i])!=0){
            ERR("pthread_mutex_destroy");
        }
    }

    if(msync(shop, shop_size, MS_SYNC)<0){
        ERR("msync");
    }

    if(munmap(shop, shop_size)<0){
        ERR("munmap");
    }

    if(munmap(shared_data, sizeof(shared_data_t))<0){
        ERR("munmap");
    }

    close(fd);
    return 0;
}
