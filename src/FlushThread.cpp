// FlushThread.cpp
//
// The flush thread lifecycle (FR-604) is implemented as member functions of
// beacon::Tracker in Tracker.cpp because the thread needs direct access to
// the Tracker's private state (memory queue, disk queue, curl handle,
// condition variables, etc.).
//
// Key methods:
//   Tracker::flush_thread_loop()  - Main loop with condition_variable wait
//   Tracker::drain_disk_queue()   - Reads from SQLite, sends via HTTP, deletes on success
//   Tracker::drain_memory_queue() - Dequeues from std::deque, sends via HTTP with retry
//
// This file exists as a translation unit entry point per the PRD file list.
// It includes FlushThread.hpp which documents the architecture decision.

#include "FlushThread.hpp"
