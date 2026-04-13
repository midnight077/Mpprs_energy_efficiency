#ifndef QUILL_RUNTIME_H
#define QUILL_RUNTIME_H

#include <functional>
#include <pthread.h>

namespace quill {

struct Task {
    std::function<void()> func;
    Task(std::function<void()> &&f) : func(std::move(f)) {}
};

constexpr int DEQUE_SIZE = 64;

class Deque {
private:
    Task* tasks[DEQUE_SIZE];
    volatile int top;
    volatile int bottom;
    pthread_mutex_t lock;
public:
    Deque();
    ~Deque();
    void push(Task* task);
    Task* pop();
    Task* steal();
    bool is_empty();
};

struct RuntimeState {
    pthread_t* threads;
    Deque* deques;
    int num_workers;
    volatile bool shutdown;
    volatile int finish_counter;
    pthread_mutex_t global_lock;
    pthread_key_t worker_id_key;

    // For Daemon Thread
    pthread_t daemon_thread;
    pthread_cond_t* worker_cond;    // One per worker
    pthread_mutex_t* worker_mutex;  // One per worker 
    bool* is_sleeping;              // Sleep status per worker 
    int current_active_workers;     // Current DOP 
};

extern RuntimeState* runtime;

// Function declarations
void init_runtime();
void finalize_runtime();
void start_finish();
void end_finish();
void async(std::function<void()> &&lambda);
int get_worker_id();

} // namespace quill
#endif