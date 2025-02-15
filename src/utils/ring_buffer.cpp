/**
 * @file   utils/ring_buffer.cpp
 * @author Martin Pulec     <pulec@cesnet.cz>
 * @author Martin Piatka    <piatka@cesnet.cz>
 */
/*
 * Copyright (c) 2011-2019 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "utils/ring_buffer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <atomic>

struct ring_buffer {
        std::unique_ptr<char[]> data;
        int len;
        /* Start and end markers for the buffer.
         *
         * Valid values are in the range (0, 2 * ring->len). This is because in
         * the simple implementation, where the values are in range
         * (0, ring->len), it would be impossible to tell apart empty buffer
         * and completely full buffer (both would have start == end). The
         * simple workaround of always having at least one byte free would make
         * it impossible to correctly align multi-byte elements for direct
         * zero-copy access.
         *
         * When the range is doubled, full buffer has start == end in modulo
         * ring->len, but not in modulo 2*ring->len.
         */
        std::atomic<int> start, end;
};

struct ring_buffer *ring_buffer_init(int size) {
        assert(size > 0);
        auto ring = new ring_buffer();
        
        ring->data = std::make_unique<char[]>(size);
        ring->len = size;
        ring->start = 0;
        ring->end = 0;
        return ring;
}

void ring_buffer_destroy(struct ring_buffer *ring) {
        if(ring) {
                delete ring;
        }
}

static int calculate_avail_read(int start, int end, int buf_len) {
        int avail = (end - start + 2 * buf_len) % buf_len;
        if(avail == 0 && (end >= buf_len) != (start >= buf_len))
                avail += buf_len;

        return avail;
}

static int calculate_avail_write(int start, int end, int buf_len) {
        return buf_len - calculate_avail_read(start, end, buf_len); 
}


int ring_get_read_regions(struct ring_buffer *ring, int max_len,
                void **ptr1, int *size1,
                void **ptr2, int *size2)
{
        /* end index is modified by the writer thread, use acquire order to ensure
         * that all writes by the writer thread made before the modification are
         * observable in this (reader) thread */
        int end = std::atomic_load_explicit(&ring->end, std::memory_order_acquire);
        // start index is modified only by this (reader) thread, so relaxed is enough
        int start = std::atomic_load_explicit(&ring->start, std::memory_order_relaxed);

        int read_len = calculate_avail_read(start, end, ring->len);
        if(read_len > max_len)
                read_len = max_len;

        int start_idx = start % ring->len;
        int to_end = ring->len - start_idx;
        *ptr1 = ring->data.get() + start_idx;
        if(read_len <= to_end) {
                *size1 = read_len;
                *ptr2 = nullptr;
                *size2 = 0;
        } else {
                *size1 = to_end;
                *ptr2 = ring->data.get();
                *size2 = read_len - to_end;
        }

        return read_len;
}

void ring_advance_read_idx(struct ring_buffer *ring, int amount) {
        // start index is modified only by this (reader) thread, so relaxed is enough
        int start = std::atomic_load_explicit(&ring->start, std::memory_order_relaxed);

        /* Use release order to ensure that all reads are completed (no reads
         * or writes in the current thread can be reordered after this store).
         */
        std::atomic_store_explicit(&ring->start,
                        (start + amount) % (2*ring->len), std::memory_order_release);
}

int ring_buffer_read(struct ring_buffer * ring, char *out, int max_len) {
        void *ptr1;
        int size1;
        void *ptr2;
        int size2;

        int read_len = ring_get_read_regions(ring, max_len, &ptr1, &size1, &ptr2, &size2);
        
        memcpy(out, ptr1, size1);
        if(ptr2) {
                memcpy(out + size1, ptr2, size2);
        }

        ring_advance_read_idx(ring, read_len);
        return read_len;
}

void ring_buffer_flush(struct ring_buffer * buf) {
        /* This should only be called while the buffer is not being read or
         * written. The only way to safely flush without locking is by reading
         * all available data from the reader thread.
         */
        buf->start = 0;
        buf->end = 0;
}

int ring_get_write_regions(struct ring_buffer *ring, int requested_len,
                void **ptr1, int *size1,
                void **ptr2, int *size2)
{
        *ptr1 = nullptr;
        *size1 = 0;
        *ptr2 = nullptr;
        *size2 = 0;

        // end index is modified only by this (writer) thread, so relaxed is enough
        int end = std::atomic_load_explicit(&ring->end, std::memory_order_relaxed);

        if(requested_len > ring->len) {
                return 0;
        }

        int end_idx = end % ring->len;
        int to_end = ring->len - end_idx;
        *ptr1 = ring->data.get() + end_idx;
        *size1 = requested_len < to_end ? requested_len : to_end;
        if(*size1 < requested_len){
                *ptr2 = ring->data.get();
                *size2 = requested_len - *size1;
        }

        return *size1 + *size2;
}

bool ring_advance_write_idx(struct ring_buffer *ring, int amount) {
        const int start = std::atomic_load_explicit(&ring->start, std::memory_order_acquire);
        // end index is modified only by this (writer) thread, so relaxed is enough
        const int end = std::atomic_load_explicit(&ring->end, std::memory_order_relaxed);

        /* Use release order to ensure that all writes to the buffer are
         * completed before advancing the end index (no reads or writes in the
         * current thread can be reordered after this store).
         */
        std::atomic_store_explicit(&ring->end,
                        (end + amount) % (2*ring->len), std::memory_order_release);

        return amount > calculate_avail_write(start, end, ring->len);
}

void ring_buffer_write(struct ring_buffer * ring, const char *in, int len) {
        void *ptr1;
        int size1;
        void *ptr2;
        int size2;
        if(!ring_get_write_regions(ring, len, &ptr1, &size1, &ptr2, &size2)){
                fprintf(stderr, "Warning: too long write request for ring buffer (%d B)!!!\n", len);
                return;
        }

        memcpy(ptr1, in, size1);
        if(ptr2){
                memcpy(ptr2, in + size1, size2);
        }

        if(ring_advance_write_idx(ring, len)) {
                fprintf(stderr, "Warning: ring buffer overflow!!!\n");
        }
}

int ring_get_size(struct ring_buffer * ring) {
        return ring->len;
}

void ring_fill(struct ring_buffer *ring, int c, int size){
        void *ptr1;
        int size1;
        void *ptr2;
        int size2;
        if(!ring_get_write_regions(ring, size, &ptr1, &size1, &ptr2, &size2)){
                fprintf(stderr, "Warning: too long write request for ring buffer (%d B)!!!\n", size);
                return;
        }

        memset(ptr1, c, size1);
        if(ptr2){
                memset(ptr2, c, size2);
        }

        if(ring_advance_write_idx(ring, size)) {
                fprintf(stderr, "Warning: ring buffer overflow!!!\n");
        }
}

/* ring_get_current_size and ring_get_available_write_size can be called from
 * both reader and writer threads.
 *
 * Writer case:
 * If the reader modifies start index under our feet, it doesn't
 * matter, because reader can only make the current size smaller. That
 * means the writer may calculate less free space, but never more than
 * really available.
 *
 * Reader case:
 * If the writer modifies end index under our feet, it doesn't matter,
 * because the writer can only make current size bigger. That means the
 * reader may calculate less size for reading, but the read data is
 * always valid.
 */
int ring_get_current_size(struct ring_buffer * ring) {
        int start = std::atomic_load_explicit(&ring->start, std::memory_order_acquire);
        int end = std::atomic_load_explicit(&ring->end, std::memory_order_acquire);
        return calculate_avail_read(start, end, ring->len);
}

int ring_get_available_write_size(struct ring_buffer * ring) {
        int start = std::atomic_load_explicit(&ring->start, std::memory_order_acquire);
        int end = std::atomic_load_explicit(&ring->end, std::memory_order_acquire);
        return calculate_avail_write(start, end, ring->len);
}

struct audio_buffer_api ring_buffer_fns = {
        (void (*)(void *)) ring_buffer_destroy,
        (int (*)(void *, char *, int)) ring_buffer_read,
        (void (*)(void *, const char *, int)) ring_buffer_write
};

