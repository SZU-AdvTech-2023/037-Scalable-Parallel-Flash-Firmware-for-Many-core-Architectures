#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "ReQ.h"

#define MEMSIZE 1024*1024*128 //128MB
#define QSIZE 128
#define BASEADDR_ReQ0 0
#define BASEADDR_ReQ1 32
#define BASEADDR_ReQ2 64
#define BASEADDR_ReQ3 96
#define BASEADDR_BUF0 128
#define BASEADDR_BUF1 640
#define BASEADDR_BUF2 1152
#define BASEADDR_BUF3 1664


int Test_Append_Fetch();
int Test_Append_Fetch_MT();
int Test_Append_Fetch_MT_more();

int main(){
    // TODO: multi-thread testing
    Test_Append_Fetch_MT_more();
    
}

int Test_Append_Fetch(){
    void* ADDR = malloc(MEMSIZE);
    if(!ADDR){
        printf("Memory Space Allocating Failed.\n\r");
    } 
    struct ReQ* req0 = (struct ReQ*)(ADDR+BASEADDR_ReQ0);
    printf("req0 before init: ---- ---- ----\n");
    ReQ_print(req0);

    ReQ_init(req0, (char*)(ADDR+BASEADDR_BUF0), QSIZE);
    printf("req0 after init: ---- ---- ----\n");
    ReQ_print(req0);

    for(int i = 0; i < 128; i++){
        ReQ_append(req0, i);
    }
    ReQ_print(req0);
    for(int i = 0; i < 128; i++){
        int a;
        ReQ_fetch(req0, &a);
        printf("Fetch the num: %d\n", a);
    }
    free(ADDR);
}

/*
* Put the elements into the ReQ.
*/
static void* P_thread(void* ADDR){
    struct ReQ* req0 = (struct ReQ*)(ADDR+BASEADDR_ReQ0);
    
    while(req0->init_flag != 1){}
    for(int i = 0; i < 256; i++){
        ReQ_append(req0, i);
        printf("Put the num: %d\n", i);
    }
    printf("P_thread finish.\n");
}

/*
* Init the ReQ. Then fetch elements from the ReQ.
*/
static void* C_thread(void* ADDR){
    struct ReQ* req0 = (struct ReQ*)(ADDR+BASEADDR_ReQ0);
    ReQ_init(req0, (char*)(ADDR+BASEADDR_BUF0), QSIZE);
    ReQ_print(req0);
    for(int i = 0; i < 256; i++){
        int a;
        ReQ_fetch(req0, &a);
        printf("Fetch the num: %d\n", a);
    }
    printf("C_thread finish.\n");
}

int Test_Append_Fetch_MT(){
    int Status;
    void* ADDR = malloc(MEMSIZE);
    if(!ADDR){
        printf("Memory Space Allocating Failed.\n");
    }
    pthread_t thread0, thread1;
    Status = pthread_create(&thread0, NULL, C_thread, ADDR);
    if(Status){
        printf("thread0 create failed.\n");
        return -1;
    }
    Status = pthread_create(&thread1, NULL, P_thread, ADDR);
    if(Status){
        printf("thread1 create failed.\n");
        return -1;
    }
    Status = pthread_join(thread0, NULL);
    if(Status){
        printf("thread0 join failed.\n");
        return -1;
    }
    Status = pthread_join(thread1, NULL);
    if(Status){
        printf("thread1 join failed.\n");
        return -1;
    }
    printf("Finished.\n");
    return 0;
}

/*
* Put the elements into the next ReQ.
*/
static void* Head_thread(void* ADDR){
    struct ReQ* req0 = (struct ReQ*)(ADDR+BASEADDR_ReQ0); // Put the element into this queue.
    
    while(req0->init_flag != 1){}
    for(int i = 0; i < 256; i++){
        ReQ_append(req0, i);
        //printf("[H] Put the num: %d\n", i);
    }
}

/*
* Fetch the elements from the ReQ.
* Put the elements into the next ReQ.
*/
static void* Mid_thread(void* ADDR){
    struct ReQ* req0 = (struct ReQ*)(ADDR+BASEADDR_ReQ0); // Get the element from this queue.
    struct ReQ* req1 = (struct ReQ*)(ADDR+BASEADDR_ReQ1); // Put the element into this queue.
    ReQ_init(req0, (char*)(ADDR+BASEADDR_BUF0), QSIZE);
    
    while(req1->init_flag != 1){}
    for(int i = 0; i < 256; i++){
        int a;
        ReQ_fetch(req0, &a);
        //printf("[M] Get the num: %d\n", a);
        a = -a;
        ReQ_append(req1, a);
        printf("[M] Put the num: %d\n", a);
    }
    ReQ_print(req0);
}

/*
* Fetch the elements from the ReQ.
*/
static void* Tail_thread(void* ADDR){
    struct ReQ* req1 = (struct ReQ*)(ADDR+BASEADDR_ReQ1); // Get the element from this queue.
    ReQ_init(req1, (char*)(ADDR+BASEADDR_BUF1), QSIZE);
    
    for(int i = 0; i < 256; i++){
        int a;
        ReQ_fetch(req1, &a);
        printf("[T] Get the num: %d\n", a);
    }
    ReQ_print(req1);
}

int Test_Append_Fetch_MT_more(){
    int Status;
    void* ADDR = malloc(MEMSIZE);
    if(!ADDR){
        printf("Memory Space Allocating Failed.\n");
    }
    pthread_t thread0, thread1, thread2, thread3;
    Status = pthread_create(&thread0, NULL, Head_thread, ADDR);
    if(Status){
        printf("thread0 create failed.\n");
        return -1;
    }
    Status = pthread_create(&thread1, NULL, Mid_thread, ADDR);
    if(Status){
        printf("thread1 create failed.\n");
        return -1;
    }
    Status = pthread_create(&thread2, NULL, Tail_thread, ADDR);
    if(Status){
        printf("thread2 create failed.\n");
        return -1;
    }
    Status = pthread_join(thread0, NULL);
    if(Status){
        printf("thread0 join failed.\n");
        return -1;
    }
    Status = pthread_join(thread1, NULL);
    if(Status){
        printf("thread1 join failed.\n");
        return -1;
    }
    Status = pthread_join(thread2, NULL);
    if(Status){
        printf("thread2 join failed.\n");
        return -1;
    }
    printf("Finished.\n");
    return 0;
}