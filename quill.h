#ifndef QUILL_H
#define QUILL_H

#include <functional>

namespace quill {

// Initialize quill runtime (thread pool, deques, TLS, etc.)
void init_runtime();

// Finalize runtime and free all resources
void finalize_runtime();

// Begin a flat-finish scope
void start_finish();

// End a flat-finish scope (wait until all async tasks finish)
void end_finish();

// Spawn an asynchronous task (C++11 lambda)
void async(std::function<void()> &&lambda);

} // namespace quill

#endif // QUILL_H