#include "uthreads.h"
#include <csetjmp>
#include <csignal>
#include <sys/time.h>
#include <vector>
#include <stdlib.h>
#include <queue>
#include <setjmp.h>
#include <signal.h>
#include <thread>
#define FAILURE -1
#define SUCCESS 0
#define MICRO_STATIC 1000000
using namespace std;

class Thread {
 public:
  int id;
  sigjmp_buf env;
  void * stack_ptr;
  void (*func)(void);
  unsigned int quantum_num;
  bool is_blocked;
  bool is_mutex_locked;

  static struct itimerval quantum_timer;
  static int curr_running;
  static int mutex_possessor;
  static int first_available_place;
  static int active_threads_num;
  static long unsigned int quantum_counter;
  static priority_queue <int, vector<int>, greater<int> > free_queue;
  static queue<Thread **> ready_list;
  static queue<Thread **> mutex_list;
  static struct sigaction time_alrm;
};

Thread* threads_array[MAX_THREAD_NUM] = {nullptr};
Thread main_thread;

#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
  address_t ret;
  asm volatile("xor    %%fs:0x30,%0\n"
               "rol    $0x11,%0\n"
  : "=g" (ret)
  : "0" (addr));
  return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
		"rol    $0x9,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}

#endif

void RemoveFromQueue(int tid){
  int length = main_thread.ready_list.size();
  for (int i=0; i < length; i++) {
    if (((*(main_thread.ready_list.front()))->id) != tid) {
      main_thread.ready_list.push(main_thread.ready_list.front());
    }
    main_thread.ready_list.pop();
  }
}

void SwitchContext(int new_thread_id)
{
  sigsetjmp(threads_array[main_thread.curr_running]->env,1);
  main_thread.ready_list.push(&threads_array[main_thread.curr_running]);
  main_thread.curr_running = new_thread_id;
  threads_array[main_thread.curr_running]->quantum_num += 1;
  main_thread.quantum_num += 1;
  setitimer(ITIMER_VIRTUAL,&main_thread.quantum_timer,NULL);
  siglongjmp(threads_array[new_thread_id]->env,1);
}

void timer_handler()
{
  main_thread.ready_list.pop();
  main_thread.ready_list.push(&threads_array[uthread_get_tid()]);
  while ((*main_thread.ready_list.front())->is_blocked)
  {
    Thread** temp = main_thread.ready_list.front();
    main_thread.ready_list.pop();
    main_thread.ready_list.push(temp);
  }
  SwitchContext((*main_thread.ready_list.front())->id);
}


/*
 * Description: This function initializes the thread library.
 * You may assume that this function is called before any other thread library
 * function, and that it is called exactly once. The input to the function is
 * the length of a quantum in micro-seconds. It is an error to call this
 * function with non-positive quantum_usecs.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs){
  if (quantum_usecs <= 0){
    return FAILURE;
  }
  main_thread.stack_ptr = malloc(STACK_SIZE);
  if (!main_thread.stack_ptr){
    return FAILURE;
  }
  sigaddset(&main_thread.time_alrm.sa_mask,SIGVTALRM);
  main_thread.time_alrm.sa_handler = &SwitchContext;
  main_thread.id = 0;
  main_thread.is_blocked = false;
  main_thread.func = NULL;
  main_thread.quantum_num = 1;
  main_thread.quantum_timer.it_value.tv_usec = quantum_usecs % MICRO_STATIC;
  main_thread.quantum_timer.it_value.tv_sec = ((int) (quantum_usecs / MICRO_STATIC));
  main_thread.first_available_place = 1;
  main_thread.active_threads_num = 1;
  main_thread.quantum_counter = 1;
  main_thread.curr_running = 0;
  main_thread.mutex_possessor = -1;
  main_thread.is_mutex_locked = false;
  sigemptyset(&main_thread.env->__saved_mask);
  threads_array[0] = &main_thread;
  return SUCCESS;
}

/*
 * Description: This function creates a new thread, whose entry point is the
 * function f with the signature void f(void). The thread is added to the end
 * of the READY threads list. The uthread_spawn function should fail if it
 * would cause the number of concurrent threads to exceed the limit
 * (MAX_THREAD_NUM). Each thread should be allocated with a stack of size
 * STACK_SIZE bytes.
 * Return value: On success, return the ID of the created thread.
 * On failure, return -1.
*/
int uthread_spawn(void (*f)(void)){
  if ((!f)||(main_thread.active_threads_num >= 100))
  {
    return FAILURE;
  }


  Thread* curr_thread = (Thread*) malloc(sizeof(Thread));
  if (!curr_thread){
    return FAILURE;
  }


  curr_thread->stack_ptr = malloc(STACK_SIZE);
  if (!main_thread.stack_ptr){
    return FAILURE;
  }


  if (curr_thread->free_queue.empty()){
    curr_thread->id = main_thread.first_available_place;
    main_thread.first_available_place ++;
  }
  else{
    curr_thread->id = main_thread.free_queue.top();
    main_thread.free_queue.pop();
  }

  curr_thread->func = f;
  curr_thread->quantum_num = 1;
  curr_thread->quantum_counter++;
  threads_array[curr_thread->id] = curr_thread;
  main_thread.active_threads_num++;
  address_t sp, pc;
  sp = (address_t)curr_thread->stack_ptr + STACK_SIZE - sizeof(address_t);
  pc = (address_t)f;
  sigsetjmp(curr_thread->env, 1);
  (curr_thread->env->__jmpbuf)[JB_SP] = translate_address(sp);
  (curr_thread->env->__jmpbuf)[JB_PC] = translate_address(pc);
  curr_thread->is_blocked = false;
  curr_thread->is_mutex_locked = false;
  main_thread.ready_list.push(&threads_array[curr_thread->id]);
  sigemptyset(&curr_thread->env->__saved_mask);
  return SUCCESS;
}


/*
 * Description: This function terminates the thread with ID tid and deletes
 * it from all relevant control structures. All the resources allocated by
 * the library for this thread should be released. If no thread with ID tid
 * exists it is considered an error. Terminating the main thread
 * (tid == 0) will result in the termination of the entire process using
 * exit(0) [after releasing the assigned library memory].
 * Return value: The function returns 0 if the thread was successfully
 * terminated and -1 otherwise. If a thread terminates itself or the main
 * thread is terminated, the function does not return.
*/
int uthread_terminate(int tid){
  if ((tid == 0)||(tid >= MAX_THREAD_NUM)||(tid < 0)||(!threads_array[tid])){
    for (int i = 1; i < MAX_THREAD_NUM ; i++){
      if (threads_array[i]){
        free(threads_array[i]->stack_ptr);
        free(threads_array[i]);
        main_thread.free_queue.push(tid);
        main_thread.active_threads_num--;
      }
    }
    free(main_thread.stack_ptr);
    if ((tid >= MAX_THREAD_NUM)||(tid < 0)||(!threads_array[tid])) {
      return FAILURE;
    }
    exit(0);
  }
  Thread* curr_thread = threads_array[tid];
  free(curr_thread->stack_ptr);
  free(curr_thread);
  if (tid == (main_thread.first_available_place - 1)){
    main_thread.first_available_place--;
  }
  else{
    main_thread.free_queue.push(tid);
  }
  main_thread.active_threads_num--;
  threads_array[tid] = nullptr;
  return SUCCESS;
}


/*
 * Description: This function blocks the thread with ID tid. The thread may
 * be resumed later using uthread_resume. If no thread with ID tid exists it
 * is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision
 * should be made. Blocking a thread in BLOCKED state has no
 * effect and is not considered an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_block(int tid){

  if ((tid <= 0) || (tid>=MAX_THREAD_NUM)|| (!threads_array[tid])){
    return FAILURE;
  }
  if (threads_array[tid]->is_blocked){
    return SUCCESS;
  }
  threads_array[tid]->is_blocked = true;
  if (tid == main_thread.curr_running){
      timer_handler();
  }
  else{
    RemoveFromQueue(tid);
  }
  return SUCCESS;
}


/*
 * Description: This function resumes a blocked thread with ID tid and moves
 * it to the READY state if it's not synced. Resuming a thread in a RUNNING or READY state
 * has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid){
  if ((tid < 0) || (tid>=MAX_THREAD_NUM)|| (!threads_array[tid])){
    return FAILURE;
  }
  if (!threads_array[tid]->is_blocked){
    return SUCCESS;
  }
  threads_array[tid]->is_blocked = false;
  main_thread.ready_list.push(&threads_array[tid]);
  return SUCCESS;
}


/*
 * Description: This function tries to acquire a mutex.
 * If the mutex is unlocked, it locks it and returns.
 * If the mutex is already locked by different thread, the thread moves to BLOCK state.
 * In the future when this thread will be back to RUNNING state,
 * it will try again to acquire the mutex.
 * If the mutex is already locked by this thread, it is considered an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_mutex_lock()
{
  if ((main_thread.mutex_possessor == -1)&&(main_thread.mutex_list.empty())){
    main_thread.mutex_possessor = main_thread.curr_running;
    threads_array[main_thread.curr_running]->is_mutex_locked = true;
    return SUCCESS;
  }
  else if (main_thread.mutex_possessor != -1){
    int tid = main_thread.curr_running;
    threads_array[tid]->is_mutex_locked = true;
    main_thread.mutex_list.push(&threads_array[tid]);
    timer_handler();
    RemoveFromQueue(tid);
  }
  return SUCCESS;
}


/*
 * Description: This function releases a mutex.
 * If there are blocked threads waiting for this mutex,
 * one of them (no matter which one) moves to READY state.
 * If the mutex is already unlocked, it is considered an error.
 * Return value: On success, return 0. On failure, return -1.
*/
int uthread_mutex_unlock()
{
  if (!(threads_array[main_thread.curr_running]->is_mutex_locked)){
    return FAILURE;
  }
  if (main_thread.mutex_list.empty()){
    main_thread.mutex_possessor = -1;
  }
  else{
    main_thread.mutex_possessor = (*main_thread.mutex_list.front())->id;
    main_thread.mutex_list.pop();
    main_thread.ready_list.push(&threads_array[main_thread.curr_running]);
  }
  return SUCCESS;
}


/*
 * Description: This function returns the thread ID of the calling thread.
 * Return value: The ID of the calling thread.
*/
int uthread_get_tid(){
  return main_thread.curr_running;
}


/*
 * Description: This function returns the total number of quantums since
 * the library was initialized, including the current quantum.
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number
 * should be increased by 1.
 * Return value: The total number of quantums.
*/
int uthread_get_total_quantums(){
  return main_thread.quantum_num;
}


/*
 * Description: This function returns the number of quantums the thread with
 * ID tid was in RUNNING state. On the first time a thread runs, the function
 * should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state
 * when this function is called, include also the current quantum). If no
 * thread with ID tid exists it is considered an error.
 * Return value: On success, return the number of quantums of the thread with ID tid.
 * 			     On failure, return -1.
*/
int uthread_get_quantums(int tid){
  if ((tid<0) || (tid>=100) || (!threads_array[tid])){
    return FAILURE;
  }
  return threads_array[tid]->quantum_num;
}


