// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef SLIDING_QUEUE_H_
#define SLIDING_QUEUE_H_

#include <algorithm>
#include "shmem.h"
#include "platform_atomics.h"


/*
GAP Benchmark Suite
Class:  SlidingQueue
Author: Scott Beamer

Double-buffered queue so appends aren't seen until SlideWindow() called
 - Use QueueBuffer when used in parallel to avoid false sharing by doing
   bulk appends from thread-local storage

    Reworked in such a way that an individual PE does the work of a thread
    When the sliding queue in symmetric memory is updated by one PE, other PEs must wait to access
*/


template <typename T>
class QueueBuffer;

template <typename T>
class SlidingQueue {
  T *shared;
  size_t shared_in;
  size_t shared_out_start;
  size_t shared_out_end;
  friend class QueueBuffer<T>;

 public:
  explicit SlidingQueue(size_t shared_size) {
    shared = (T *) shmem_calloc(shared_size, sizeof(T));
    reset();
  }

  ~SlidingQueue() {
    shfree(shared);
  }

  void push_back(T to_add) {
    shared[shared_in++] = to_add;
  }

  bool empty() const {
    return shared_out_start == shared_out_end;
  }

  void reset() {
    shared_out_start = 0;
    shared_out_end = 0;
    shared_in = 0;
  }

  void slide_window() {
    shared_out_start = shared_out_end;
    shared_out_end = shared_in;
  }

  typedef T* iterator;

  iterator begin() const {
    return shared + shared_out_start;
  }

  iterator end() const {
    return shared + shared_out_end;
  }

  size_t size() const {
    return end() - begin();
  }
};


template <typename T>
class QueueBuffer {
  size_t in;
  T *local_queue;
  SlidingQueue<T> &sq;
  const size_t local_size;
  int pe;
  int npes; 
  long* QLOCK;

 public:
  explicit QueueBuffer(SlidingQueue<T> &master, long* QL, size_t given_size = 16384)
      : sq(master), QLOCK(QL), local_size(given_size) {
    pe = shmem_my_pe();
    npes = shmem_n_pes();
    in = 0;
    local_queue = new T[local_size];
  }

  ~QueueBuffer() {
    delete[] local_queue;
  }

  void push_back(T to_add) {
    if (in == local_size)
      flush();
    local_queue[in++] = to_add;
  }

  void flush() {
    shmem_set_lock(QLOCK);                                                      // Lock critical region (the frontier) to avoid simultaneous flushes
    T *shared_queue = sq.shared;
    size_t copy_start = shmem_ulong_atomic_fetch_add(&(sq.shared_in), in, pe);          // Get start of shared queue incoming region, update local copy of incoming region start position
    size_t copy_end = copy_start + local_size;
    std::copy(local_queue, local_queue+in, shared_queue+copy_start);                    // Update local copy of shared queue
    for (int i = 0; i < npes; i++){
        if (i != pe){
            shmem_put64(shared_queue+copy_start, local_queue, local_size, i);           // Update shared queue on all PEs (An Edge<int, int> consumes 8 bytes of memory)
            shmem_ulong_put(&(sq.shared_in), &copy_end, (long unsigned) 1, i);          // Move start of incoming region to end of copied elements
        }
    }
    in = 0;
    shmem_clear_lock(QLOCK);
  }
};

#endif  // SLIDING_QUEUE_H_
