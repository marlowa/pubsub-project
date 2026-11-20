/*
 * Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Counts calls to the heap allocator, thread by thread, while a window is open. For BUG-0087: whether the order path
 * allocates from the heap can then be established on demand, by scripts/heap_allocation_count.py.
 *
 * It is loaded into a process with LD_PRELOAD and stands in front of malloc, calloc, realloc, free, posix_memalign,
 * aligned_alloc and memalign, passing every call on to the C library's own allocator (__libc_malloc and the others,
 * which glibc exports for exactly this), so the process behaves as before. It is written in C, not C++, because a
 * C++ declaration of malloc must match the exception specification glibc gives it, and a C file has none.
 *
 * Three environment variables control it, read once when it is loaded:
 *
 *   HEAP_COUNT_PROGRAMS     the names of the programs to count, separated by commas, such as "sequencer,matching_engine".
 *                           In any other program, a shell or a script that happens to inherit LD_PRELOAD, every call is
 *                           passed on and nothing else is done.
 *   HEAP_COUNT_WINDOW_FILE  the window is open while this file exists. Every program checks it at most every 100
 *                           milliseconds, on a call to the allocator, so a program starts counting within 100
 *                           milliseconds of its first call after the file is made, and stops likewise.
 *   HEAP_COUNT_REPORT_DIR   where each program writes its report, <program>.<pid>.txt, when the window closes, or when
 *                           it exits with the window still open.
 *
 * What is counted, for each thread that calls the allocator while the window is open: the calls of each kind, the
 * bytes asked for, and, for up to 64 different call stacks, how many calls came from each, with the six return
 * addresses nearest the call. The report ends with the program's executable mappings, so that the script can turn each
 * address into a function and line with addr2line.
 *
 * Nothing here may itself call the allocator while counting, or it would count itself and could recurse. Each thread
 * carries a flag that is set while this library is at work on it, and a call made while it is set is passed on
 * uncounted. The thread-local variables use the initial-exec model, so that reaching them never allocates.
 * backtrace() loads its unwinder, which allocates, on its first use, so it is called once when the library is loaded.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

extern void* __libc_malloc(size_t size);
extern void* __libc_calloc(size_t count, size_t size);
extern void* __libc_realloc(void* pointer, size_t size);
extern void __libc_free(void* pointer);
extern void* __libc_memalign(size_t alignment, size_t size);

enum { most_threads = 512, most_stacks_a_thread = 64, frames_kept = 6, frames_skipped = 2, path_length = 512 };
enum call_kind { kind_malloc, kind_calloc, kind_realloc, kind_aligned, kind_free, kind_count };
static const char* const kind_names[kind_count] = {"malloc", "calloc", "realloc", "aligned", "free"};

struct stack_count {
    void* frames[frames_kept];
    uint64_t calls;
};

struct thread_counts {
    _Atomic int thread_id;
    _Atomic uint64_t calls[kind_count];
    _Atomic uint64_t bytes;
    int stacks_used;
    struct stack_count stacks[most_stacks_a_thread];
    _Atomic uint64_t stacks_not_kept;
};

static struct thread_counts threads[most_threads];
static _Atomic int threads_used;

static int counting_this_program;
static char program_name[64];
static char window_file[path_length];
static char report_dir[path_length];

static _Atomic int window_open;
static _Atomic int64_t last_window_check_ns;
static _Atomic int report_written;
static int64_t window_opened_ns;

static __thread int this_thread_slot __attribute__((tls_model("initial-exec"))) = -1;
static __thread int at_work_here __attribute__((tls_model("initial-exec")));

static int64_t now_ns(clockid_t clock) {
    struct timespec now;
    clock_gettime(clock, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void write_text(int fd, const char* text) {
    size_t left = strlen(text);
    while (left > 0) {
        ssize_t written = write(fd, text, left);
        if (written <= 0) {
            return;
        }
        text += written;
        left -= (size_t)written;
    }
}

/* Copies the lines of /proc/self/maps for executable mappings, which is all addr2line needs. */
static void write_executable_mappings(int fd) {
    int maps = open("/proc/self/maps", O_RDONLY);
    if (maps < 0) {
        return;
    }
    char buffer[4096];
    char line[1024];
    size_t line_length = 0;
    ssize_t got;
    while ((got = read(maps, buffer, sizeof buffer)) > 0) {
        for (ssize_t i = 0; i < got; ++i) {
            if (line_length < sizeof line - 1) {
                line[line_length++] = buffer[i];
            }
            if (buffer[i] == '\n') {
                line[line_length] = '\0';
                if (strstr(line, " r-xp ") != NULL) {
                    write_text(fd, "map ");
                    write_text(fd, line);
                }
                line_length = 0;
            }
        }
    }
    close(maps);
}

/* The thread's name as the kernel holds it now, which may have been set after the thread first allocated. */
static void read_thread_name(int thread_id, char* name, size_t size) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", thread_id);
    snprintf(name, size, "?");
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    ssize_t got = read(fd, name, size - 1);
    close(fd);
    if (got > 0) {
        name[got] = '\0';
        char* newline = strchr(name, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
    }
}

static void write_report(void) {
    if (atomic_exchange(&report_written, 1) != 0) {
        return;
    }
    char path[path_length + 128];
    snprintf(path, sizeof path, "%s/%s.%d.txt", report_dir, program_name, (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    char line[512];
    double seconds = (double)(now_ns(CLOCK_MONOTONIC) - window_opened_ns) / 1e9;
    snprintf(line, sizeof line, "program %s pid %d window_seconds %.1f\n", program_name, (int)getpid(), seconds);
    write_text(fd, line);
    int used = atomic_load(&threads_used);
    if (used > most_threads) {
        used = most_threads;
    }
    for (int slot = 0; slot < used; ++slot) {
        struct thread_counts* counts = &threads[slot];
        int thread_id = atomic_load(&counts->thread_id);
        char name[32];
        read_thread_name(thread_id, name, sizeof name);
        int length = snprintf(line, sizeof line, "thread %d %s", thread_id, name);
        for (int kind = 0; kind < kind_count; ++kind) {
            length +=
                snprintf(line + length, sizeof line - (size_t)length, " %s %llu", kind_names[kind], (unsigned long long)atomic_load(&counts->calls[kind]));
        }
        snprintf(line + length, sizeof line - (size_t)length, " bytes %llu stacks_not_kept %llu\n", (unsigned long long)atomic_load(&counts->bytes),
                 (unsigned long long)atomic_load(&counts->stacks_not_kept));
        write_text(fd, line);
        for (int stack = 0; stack < counts->stacks_used; ++stack) {
            length = snprintf(line, sizeof line, "stack %d %llu", thread_id, (unsigned long long)counts->stacks[stack].calls);
            for (int frame = 0; frame < frames_kept; ++frame) {
                length += snprintf(line + length, sizeof line - (size_t)length, " %p", counts->stacks[stack].frames[frame]);
            }
            snprintf(line + length, sizeof line - (size_t)length, "\n");
            write_text(fd, line);
        }
    }
    write_executable_mappings(fd);
    close(fd);
}

/* Opens or closes the window by whether the file exists, looking at most every 100 milliseconds. */
static void check_window(void) {
    int64_t now = now_ns(CLOCK_MONOTONIC_COARSE);
    int64_t last = atomic_load_explicit(&last_window_check_ns, memory_order_relaxed);
    if (now - last < 100000000 || !atomic_compare_exchange_strong(&last_window_check_ns, &last, now)) {
        return;
    }
    int exists = access(window_file, F_OK) == 0;
    int was_open = atomic_load(&window_open);
    if (exists && !was_open) {
        window_opened_ns = now_ns(CLOCK_MONOTONIC);
        atomic_store(&window_open, 1);
    } else if (!exists && was_open) {
        atomic_store(&window_open, 0);
        write_report();
    }
}

static struct thread_counts* counts_for_this_thread(void) {
    if (this_thread_slot < 0) {
        int slot = atomic_fetch_add(&threads_used, 1);
        if (slot >= most_threads) {
            return NULL;
        }
        atomic_store(&threads[slot].thread_id, (int)syscall(SYS_gettid));
        this_thread_slot = slot;
    }
    return this_thread_slot < most_threads ? &threads[this_thread_slot] : NULL;
}

static void count(enum call_kind kind, size_t bytes) {
    if (!counting_this_program || at_work_here) {
        return;
    }
    at_work_here = 1;
    check_window();
    if (atomic_load_explicit(&window_open, memory_order_relaxed)) {
        struct thread_counts* counts = counts_for_this_thread();
        if (counts != NULL) {
            atomic_fetch_add_explicit(&counts->calls[kind], 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&counts->bytes, bytes, memory_order_relaxed);
            if (kind != kind_free) {
                void* frames[frames_kept + frames_skipped];
                int depth = backtrace(frames, frames_kept + frames_skipped);
                void* kept[frames_kept] = {0};
                for (int frame = frames_skipped; frame < depth; ++frame) {
                    kept[frame - frames_skipped] = frames[frame];
                }
                int found = -1;
                for (int stack = 0; stack < counts->stacks_used && found < 0; ++stack) {
                    if (memcmp(counts->stacks[stack].frames, kept, sizeof kept) == 0) {
                        found = stack;
                    }
                }
                if (found < 0 && counts->stacks_used < most_stacks_a_thread) {
                    found = counts->stacks_used;
                    memcpy(counts->stacks[found].frames, kept, sizeof kept);
                    counts->stacks[found].calls = 0;
                    counts->stacks_used = found + 1;
                }
                if (found >= 0) {
                    counts->stacks[found].calls++;
                } else {
                    atomic_fetch_add_explicit(&counts->stacks_not_kept, 1, memory_order_relaxed);
                }
            }
        }
    }
    at_work_here = 0;
}

static void copy_environment(const char* name, char* destination, size_t size) {
    extern char** environ;
    size_t name_length = strlen(name);
    for (char** entry = environ; entry != NULL && *entry != NULL; ++entry) {
        if (strncmp(*entry, name, name_length) == 0 && (*entry)[name_length] == '=') {
            snprintf(destination, size, "%s", *entry + name_length + 1);
            return;
        }
    }
    destination[0] = '\0';
}

__attribute__((constructor)) static void start(void) {
    at_work_here = 1;
    char executable[path_length];
    ssize_t got = readlink("/proc/self/exe", executable, sizeof executable - 1);
    executable[got > 0 ? got : 0] = '\0';
    const char* base = strrchr(executable, '/');
    snprintf(program_name, sizeof program_name, "%.63s", base != NULL ? base + 1 : executable);

    char programs[path_length];
    copy_environment("HEAP_COUNT_PROGRAMS", programs, sizeof programs);
    copy_environment("HEAP_COUNT_WINDOW_FILE", window_file, sizeof window_file);
    copy_environment("HEAP_COUNT_REPORT_DIR", report_dir, sizeof report_dir);
    size_t name_length = strlen(program_name);
    for (const char* entry = programs; *entry != '\0';) {
        const char* end = strchr(entry, ',');
        size_t length = end != NULL ? (size_t)(end - entry) : strlen(entry);
        if (length == name_length && strncmp(entry, program_name, length) == 0) {
            counting_this_program = window_file[0] != '\0' && report_dir[0] != '\0';
        }
        entry += length + (end != NULL ? 1 : 0);
    }
    if (counting_this_program) {
        void* frames[2];
        backtrace(frames, 2);
    }
    at_work_here = 0;
}

__attribute__((destructor)) static void finish(void) {
    at_work_here = 1;
    if (counting_this_program && atomic_load(&window_open)) {
        write_report();
    }
}

void* malloc(size_t size) {
    count(kind_malloc, size);
    return __libc_malloc(size);
}

void* calloc(size_t count_of, size_t size) {
    count(kind_calloc, count_of * size);
    return __libc_calloc(count_of, size);
}

void* realloc(void* pointer, size_t size) {
    count(kind_realloc, size);
    return __libc_realloc(pointer, size);
}

void free(void* pointer) {
    if (pointer != NULL) {
        count(kind_free, 0);
    }
    __libc_free(pointer);
}

void* memalign(size_t alignment, size_t size) {
    count(kind_aligned, size);
    return __libc_memalign(alignment, size);
}

void* aligned_alloc(size_t alignment, size_t size) {
    count(kind_aligned, size);
    return __libc_memalign(alignment, size);
}

int posix_memalign(void** result, size_t alignment, size_t size) {
    count(kind_aligned, size);
    if (alignment % sizeof(void*) != 0 || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void* pointer = __libc_memalign(alignment, size);
    if (pointer == NULL) {
        return ENOMEM;
    }
    *result = pointer;
    return 0;
}
