#include "quill-runtime.h"
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sched.h>
#include <stdlib.h>

extern "C" {
    void profiler_init();
    void profiler_finalize();
    double calculate_JPI();
}

namespace quill {

RuntimeState* runtime = nullptr;

// ─────────────────────────────────────────────
// DEQUE
// ─────────────────────────────────────────────

Deque::Deque() : top(0), bottom(0) {
    pthread_mutex_init(&lock, nullptr);
}

Deque::~Deque() {
    pthread_mutex_destroy(&lock);
}

void Deque::push(Task* task) {
    pthread_mutex_lock(&lock);
    if (bottom - top >= DEQUE_SIZE) {
        fprintf(stderr, "ERROR: Deque full. Increase DEQUE_SIZE.\n");
        pthread_mutex_unlock(&lock);
        exit(1);
    }
    tasks[bottom % DEQUE_SIZE] = task;
    bottom++;
    pthread_mutex_unlock(&lock);
}

Task* Deque::pop() {
    pthread_mutex_lock(&lock);
    Task* task = nullptr;
    if (top < bottom) {
        bottom--;
        task = tasks[bottom % DEQUE_SIZE];
    }
    pthread_mutex_unlock(&lock);
    return task;
}

Task* Deque::steal() {
    pthread_mutex_lock(&lock);
    Task* task = nullptr;
    if (top < bottom) {
        task = tasks[top % DEQUE_SIZE];
        top++;
    }
    pthread_mutex_unlock(&lock);
    return task;
}

bool Deque::is_empty() {
    return bottom <= top;
}

// ─────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────

int get_worker_id() {
    void* ptr = pthread_getspecific(runtime->worker_id_key);
    return ptr ? *(int*)ptr : 0;
}

void decrement_finish_counter() {
    pthread_mutex_lock(&runtime->global_lock);
    runtime->finish_counter--;
    pthread_mutex_unlock(&runtime->global_lock);
}

Task* find_work(int id) {
    Task* t = runtime->deques[id].pop();
    if (t) return t;
    for (int i = 1; i < runtime->num_workers; i++) {
        int victim = (id + i) % runtime->num_workers;
        t = runtime->deques[victim].steal();
        if (t) return t;
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// DCT: configure_DOP
// ─────────────────────────────────────────────

void configure_DOP(double JPI_prev, double JPI_curr) {
    const int N = 2; // tune on server: try 1, 2, 4

    if (JPI_prev == 0.0) {
        // First call: unconditionally put N workers to sleep
        int target = runtime->current_active_workers - N;
        if (target < 1) target = 1;
        for (int i = runtime->current_active_workers - 1; i >= target; i--) {
            pthread_mutex_lock(&runtime->worker_mutex[i]);
            runtime->is_sleeping[i] = true;
            pthread_mutex_unlock(&runtime->worker_mutex[i]);
        }
        printf("[DCT] t=first  DOP: %d -> %d\n", runtime->current_active_workers, target);
        runtime->current_active_workers = target;
        return;
    }

    if (JPI_curr < JPI_prev) {
        // JPI improved -> put N more workers to sleep
        int target = runtime->current_active_workers - N;
        if (target < 1) target = 1;
        if (target < runtime->current_active_workers) {
            for (int i = runtime->current_active_workers - 1; i >= target; i--) {
                pthread_mutex_lock(&runtime->worker_mutex[i]);
                runtime->is_sleeping[i] = true;
                pthread_mutex_unlock(&runtime->worker_mutex[i]);
            }
            printf("[DCT] JPI %.6e -> %.6e (improved)  DOP: %d -> %d\n",
                   JPI_prev, JPI_curr, runtime->current_active_workers, target);
            runtime->current_active_workers = target;
        }
    } else if (JPI_curr > JPI_prev) {
        // JPI worsened -> wake N workers back up
        int target = runtime->current_active_workers + N;
        if (target > runtime->num_workers) target = runtime->num_workers;
        if (target > runtime->current_active_workers) {
            for (int i = runtime->current_active_workers; i < target; i++) {
                pthread_mutex_lock(&runtime->worker_mutex[i]);
                runtime->is_sleeping[i] = false;
                pthread_cond_signal(&runtime->worker_cond[i]);
                pthread_mutex_unlock(&runtime->worker_mutex[i]);
            }
            printf("[DCT] JPI %.6e -> %.6e (worsened) DOP: %d -> %d\n",
                   JPI_prev, JPI_curr, runtime->current_active_workers, target);
            runtime->current_active_workers = target;
        }
    }
}

// ─────────────────────────────────────────────
// DAEMON THREAD
// ─────────────────────────────────────────────

void* daemon_routine(void* arg) {
    const int interval_ms = 50; // tune on server: try 20, 50, 100

    usleep(100000); // 100ms warmup before first measurement

    double JPI_prev = 0.0; // sentinel for "first call"

    while (!runtime->shutdown) {
        double JPI_curr = calculate_JPI();
        configure_DOP(JPI_prev, JPI_curr);
        JPI_prev = JPI_curr;
        usleep(interval_ms * 1000);
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// WORKER THREAD
// ─────────────────────────────────────────────

void* worker_routine(void* arg) {
    int id = *(int*)arg;
    pthread_setspecific(runtime->worker_id_key, arg);

    while (true) {
        pthread_mutex_lock(&runtime->worker_mutex[id]);
        while (runtime->is_sleeping[id] && !runtime->shutdown) {
            pthread_cond_wait(&runtime->worker_cond[id], &runtime->worker_mutex[id]);
        }
        pthread_mutex_unlock(&runtime->worker_mutex[id]);

        if (runtime->shutdown) break;

        Task* t = find_work(id);
        if (t) {
            t->func();
            delete t;
            decrement_finish_counter();
        } else {
            usleep(100);
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// INIT / FINALIZE
// ─────────────────────────────────────────────

void init_runtime() {
    profiler_init();

    runtime = new RuntimeState();

    const char* env = getenv("QUILL_WORKERS");
    runtime->num_workers = env ? atoi(env) : 4;
    runtime->current_active_workers = runtime->num_workers;
    runtime->shutdown = false;
    runtime->finish_counter = 0;

    pthread_mutex_init(&runtime->global_lock, nullptr);
    pthread_key_create(&runtime->worker_id_key, nullptr);

    runtime->threads      = new pthread_t[runtime->num_workers];
    runtime->deques       = new Deque[runtime->num_workers];
    runtime->worker_cond  = new pthread_cond_t[runtime->num_workers];
    runtime->worker_mutex = new pthread_mutex_t[runtime->num_workers];
    runtime->is_sleeping  = new bool[runtime->num_workers];

    for (int i = 0; i < runtime->num_workers; i++) {
        pthread_cond_init(&runtime->worker_cond[i], nullptr);
        pthread_mutex_init(&runtime->worker_mutex[i], nullptr);
        runtime->is_sleeping[i] = false;
        int* id = new int(i);
        pthread_create(&runtime->threads[i], nullptr, worker_routine, id);
    }

    pthread_create(&runtime->daemon_thread, nullptr, daemon_routine, nullptr);
}

void finalize_runtime() {
    pthread_mutex_lock(&runtime->global_lock);
    runtime->shutdown = true;
    pthread_mutex_unlock(&runtime->global_lock);

    for (int i = 0; i < runtime->num_workers; i++) {
        pthread_mutex_lock(&runtime->worker_mutex[i]);
        runtime->is_sleeping[i] = false;
        pthread_cond_signal(&runtime->worker_cond[i]);
        pthread_mutex_unlock(&runtime->worker_mutex[i]);
    }

    pthread_join(runtime->daemon_thread, nullptr);
    for (int i = 0; i < runtime->num_workers; i++) {
        pthread_join(runtime->threads[i], nullptr);
        pthread_cond_destroy(&runtime->worker_cond[i]);
        pthread_mutex_destroy(&runtime->worker_mutex[i]);
    }

    profiler_finalize();

    delete[] runtime->threads;
    delete[] runtime->deques;
    delete[] runtime->worker_cond;
    delete[] runtime->worker_mutex;
    delete[] runtime->is_sleeping;
    pthread_mutex_destroy(&runtime->global_lock);
    pthread_key_delete(runtime->worker_id_key);
    delete runtime;
    runtime = nullptr;
}

// ─────────────────────────────────────────────
// PUBLIC API
// ─────────────────────────────────────────────

void start_finish() {
    pthread_mutex_lock(&runtime->global_lock);
    runtime->finish_counter = 0;
    pthread_mutex_unlock(&runtime->global_lock);
}

void end_finish() {
    int id = get_worker_id();
    while (true) {
        pthread_mutex_lock(&runtime->global_lock);
        int remaining = runtime->finish_counter;
        pthread_mutex_unlock(&runtime->global_lock);
        if (remaining == 0) break;
        Task* t = find_work(id);
        if (t) {
            t->func();
            delete t;
            decrement_finish_counter();
        } else {
            sched_yield();
        }
    }
}

void async(std::function<void()>&& lambda) {
    pthread_mutex_lock(&runtime->global_lock);
    runtime->finish_counter++;
    pthread_mutex_unlock(&runtime->global_lock);
    Task* t = new Task(std::move(lambda));
    runtime->deques[get_worker_id()].push(t);
}

} // namespace quill