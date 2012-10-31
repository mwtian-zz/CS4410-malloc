/* Set to 0 to turn off debugging. */
#define DEBUG 1
#if DEBUG != 0
/* Do not printf inside free */
#include <stdio.h> 
#endif /* DEBUG != 0 */

/* Set to 0 to not compile with pthread */
#define PTHREAD_COMPILE  1
#if PTHREAD_COMPILE != 0
#include <pthread.h>
#endif /* PTHREAD_COMPILE != 0 */

#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <assert.h>

#include "malloc.h"
#include "memreq.h"

/* Memory overhead of a free node. */
#define NODE_SIZE (sizeof(struct fnode))
#define FENCE_SIZE (sizeof(struct fence))
#define NODE_OVERHEAD (NODE_SIZE+FENCE_SIZE)
#define FENCE_OVERHEAD (2*FENCE_SIZE)
#define DIFF_OVERHEAD (NODE_SIZE-FENCE_SIZE)

/* Assume sizeof(size_t) and sizeof(void*) are 8, the same */
#define SIZE_T_SIZE (sizeof(size_t))
#define ALIGN_SIZE (2*SIZE_T_SIZE)

/* Use one bit in size for marking the chunk in-use/free. */
#define SET_USED(x) ((x) |= 1)
#define SET_FREE(x) ((x) &= (~1))
#define ISUSED(x) ((x) & (1))
#define GETSIZE(x) (((x)>>1)<<1)

/* Round up to nearest sizes. */
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ROUNDUP_8(x) (((((x)-1)>>3)+1)<<3)
#define ROUNDUP_16(x) (((((x)-1)>>4)+1)<<4)
#define ROUNDUP_PAGE(x) (((((x)-1)/PAGE_SIZE)+1)*PAGE_SIZE)
#define ROUNDUP_CHUNK(x) ROUNDUP_16(MAX((x),DIFF_OVERHEAD)+FENCE_OVERHEAD) // ROUNDUP_16(MAX((x),NODE_OVERHEAD))

/* Get a pointer to the previous neighoring fence */
#define FENCE_BACKWARD(x) ((fence_t)(x)-1)

/* 
 * Data structures for boundary tags (fences) and free nodes. 
 *  'size' is the size of the whole chunk, including boundary overheads. 
 */
 
typedef struct fence {
    size_t size;
} *fence_t;

typedef struct fnode {
    size_t size;
    struct fnode *prev;
    struct fnode *next;
} *fnode_t;

/* Global variables */

/* Size of memory page in bytes */
static size_t PAGE_SIZE = 0;
/* Head of free nodes list */
static fnode_t flist = NULL;
/* Pointer to the start of the heap */
static char *HEAP_START = NULL;
/* Pointer to the break */
static char *HEAP_BREAK = NULL;
/* Mutex lock using pthread */
#if PTHREAD_COMPILE != 0
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif /* PTHREAD_COMPILE != 0 */

/* Helper-function declarations. Explained before each function definition. */

static fnode_t malloc_expand(size_t size);
static fnode_t malloc_fnode_assign_free(char *start, size_t size);
static void *malloc_fnode_assign_used(char *start, size_t size);
static fnode_t malloc_find_fit(fnode_t target, size_t size);
static fnode_t malloc_fnode_split(fnode_t *list, fnode_t node, size_t size);
static void malloc_fnode_release(fnode_t *list, fence_t item);
static fnode_t malloc_fnode_fuse_up(fnode_t *list, fnode_t node);
static fnode_t malloc_fnode_fuse_down(fnode_t *list, fnode_t node);

static void malloc_list_addr_insert(fnode_t *list, fnode_t item);
static void malloc_list_remove(fnode_t *list, fnode_t node);

/* Debugging */
#if DEBUG != 0
static int mark = 0;
static int malloc_count = 0;
static int calloc_count = 0;
static int realloc_count = 0;
static int free_count = 0;
static void malloc_print_free_chunks(fnode_t list);
static void malloc_print_all_chunks();
static void malloc_print_fnode(fnode_t front);
#endif /* DEBUG != 0 */

void *malloc(size_t size) 
{
    fnode_t fit;
    void *ret;
++malloc_count;
    /* The chunk size to be requested */
    size = ROUNDUP_CHUNK(size);
    
    #if PTHREAD_COMPILE != 0
    pthread_mutex_lock(&mutex);
    #endif /* PTHREAD_COMPILE != 0 */
    
    if ((fit = malloc_find_fit(flist, size)) == NULL) {
        if ((fit = malloc_expand(size)) != NULL) {
            malloc_list_addr_insert(&flist, fit);
        } else {
            errno = ENOMEM;
            #if PTHREAD_COMPILE != 0
            pthread_mutex_unlock(&mutex);
            #endif /* PTHREAD_COMPILE != 0 */
            return NULL;
        }
    }
    fit = malloc_fnode_split(&flist, fit, size);
    malloc_list_remove(&flist, fit);
    ret = malloc_fnode_assign_used((char*)fit, fit->size);
    
//malloc_print_all_chunks();

    #if PTHREAD_COMPILE != 0
    pthread_mutex_unlock(&mutex);
    #endif /* PTHREAD_COMPILE != 0 */
  
    return ret;
}

void free(void* ptr) 
{
    if (ptr) {
        #if PTHREAD_COMPILE != 0
        pthread_mutex_lock(&mutex);
        #endif /* PTHREAD_COMPILE != 0 */
        malloc_fnode_release(&flist, FENCE_BACKWARD(ptr));
        #if PTHREAD_COMPILE != 0
        pthread_mutex_unlock(&mutex);
        #endif /* PTHREAD_COMPILE != 0 */
    }
}

/* Find the first fit that can fit in size + new node overhead */
static fnode_t malloc_find_fit(fnode_t target, size_t size) 
{
    while (target != NULL) {
        if (target->size >= size) {
            return target;
        } else {
            target = target->next;
        }
    }
    return target;
}

/* Initialize and fence a free node. */
static fnode_t malloc_fnode_assign_free(char *start, size_t size) 
{
    fnode_t node = (fnode_t) start;
    fence_t end = FENCE_BACKWARD(start + size);
    node->size = size;
    SET_FREE(node->size);
    end->size = node->size;
    node->prev = NULL;
    node->next = NULL;
    
    return node;
}

/* Prepare node to be returned to the user. */
static void *malloc_fnode_assign_used(char *start, size_t size)
{
    fnode_t node = (fnode_t) start;
    fence_t end = FENCE_BACKWARD((start + size));
    node->size = size;
    SET_USED(node->size);
    end->size = node->size;
    node->prev = NULL;
    node->next = NULL;
 
    return start + FENCE_SIZE;
}

/* Increase break, return a free node at the new break. */
static fnode_t malloc_expand(size_t size)
{
    char *start;
    char init = 0;
    /* Two cases; getting initial memory or expanding memory */
    if (0 == PAGE_SIZE) {
        init = 1;
        PAGE_SIZE = sysconf(_SC_PAGESIZE);
        size = ROUNDUP_PAGE(size + FENCE_OVERHEAD);
    } else {
        size = ROUNDUP_PAGE(size);
    }
    if ((start = get_memory(size)) == NULL) {
        return NULL;
    }
    HEAP_BREAK += size;
    /* Put on initial fences */
    if (1 == init) {
        HEAP_START = start;
        HEAP_BREAK = start + size;
        ((fence_t) start)->size = 1;
        FENCE_BACKWARD(start + size)->size = 1;
        start += FENCE_SIZE;
        size -= 2 * FENCE_SIZE;
    } else {
        HEAP_BREAK += size;
        FENCE_BACKWARD(start + size)->size = 1;
        start -= FENCE_SIZE;
    }
    return malloc_fnode_assign_free(start, size);
}

/* Add item to the address-ordered list of free nodes */
static void malloc_list_addr_insert(fnode_t *list, fnode_t item)
{
    fnode_t front = *list;
    if (NULL == *list || item < *list) {
        item->prev = NULL;
        item->next = *list;
        *list = item;
    } else {
        while (front->next != NULL && front->next <= item) {
            front = front->next;
        }
        if (front->next == item) {
            return;
        }
        item->prev = front;
        item->next = front->next;
    }
    if (item->prev) {
        item->prev->next = item;
    }
    if (item->next) {
        item->next->prev = item;
    }
}

/* Split the node if possible. 'size' is the size requested (rounded up). */
static fnode_t malloc_fnode_split(fnode_t *list, fnode_t node, size_t size)
{
    char *start = (char*) node;
    char *split = ((char*) node) + size;
    size_t split_size = node->size - size;
    fnode_t node_new;

    if (split_size >= NODE_OVERHEAD) {
        //~ /* Enough space for a new free node. Insert into the free nodes list */
        malloc_list_remove(list, node);
        node = malloc_fnode_assign_free(start, size);
        node_new = malloc_fnode_assign_free(split, split_size);
        malloc_list_addr_insert(list, node);
        malloc_list_addr_insert(list, node_new);
    }
    
    return node;
}

/* Remove fnode from 'list' */
static void malloc_list_remove(fnode_t *list, fnode_t node)
{
    fnode_t front = *list;
    if (*list == node) {
        if ((*list = node->next)) {
            (*list)->prev = NULL;
        }
    } else {
        while (front->next != node) {
            front = front->next;
        }
        if ((front->next = node->next)) {
            front->next->prev = front;
        }
    }
}

/* Add the chunk back to the free list. */
static void malloc_fnode_release(fnode_t *list, fence_t target) 
{
    fnode_t node;
    SET_FREE(target->size);
    node = malloc_fnode_assign_free((char*)target, target->size);
    //malloc_list_addr_insert(list, node);
    node = malloc_fnode_fuse_up(list, node);
    //node = malloc_fnode_fuse_down(list, node);
//malloc_print_all_chunks();      

}

/* Fuse with the neighbor free nodes if possible. */
static fnode_t malloc_fnode_fuse_up(fnode_t *list, fnode_t node)
{
    fence_t prev_backfence = FENCE_BACKWARD(node);
    fnode_t prev_node;
    fence_t curr_backfence;
    if (ISUSED(prev_backfence->size)) {
        malloc_list_addr_insert(list, node);
        return node;
    }
    
    prev_node = (fnode_t) ((char*) node - prev_backfence->size);
    if (prev_node->size != prev_backfence->size) {
if (mark < 3) {
printf("Inconsistent node size discovered in fuse_up!\n");
//malloc_print_free_chunks(*list);
printf("number of malloc calls: %d\n", malloc_count);
printf("number of calloc calls: %d\n", calloc_count);
printf("number of realloc calls: %d\n", realloc_count);

printf("previous node shows size: %ld\n", prev_node->size);
printf("previous fence shows size: %ld\n", prev_backfence->size);
printf("previous node address: %p\n", prev_node);
malloc_print_all_chunks();
mark++;
}
        malloc_list_addr_insert(list, node);
        return node;
    }
    
    curr_backfence = FENCE_BACKWARD((char*) node + node->size);
    prev_node->size += node->size;
    curr_backfence->size = prev_node->size;
    return prev_node;
}

static fnode_t malloc_fnode_fuse_down(fnode_t *list, fnode_t node)
{
    fence_t curr_backfence = FENCE_BACKWARD((char*) node + node->size);
    fnode_t next_node = (fnode_t) (curr_backfence + 1); 
    fence_t next_backfence;
    if (ISUSED(next_node->size)) {
        return node;
    }
    
    next_backfence = FENCE_BACKWARD((char*) next_node + next_node->size);
    if (next_node->size != next_backfence->size) {
            return node;
    }
    
    node->size += next_node->size;
    next_backfence->size = node->size;
    
    if ((node->next = next_node->next)) {
        node->next->prev = node;
    }
    
    next_node->prev = NULL;
    next_node->next = NULL;
    return node;
}

#if DEBUG != 0
static void malloc_print_fnode(fnode_t front)
{
    size_t real_size = GETSIZE(front->size);
    size_t footer_size = FENCE_BACKWARD((char*) front + real_size)->size;
    printf("Listing a chunk...\n");
    printf("prev pointer %p. ", front->prev);
    printf("next pointer %p.\n", front->next);
    printf("Header shows size %ld. ", front->size);
    printf("Footer shows size %ld.\n", footer_size);
    if (front->size != footer_size) {
        printf("Inconsistent chunk size!\n");
    }
}

static void malloc_print_free_chunks(fnode_t front)
{
    int i = 0;
    size_t footer_size;
    printf("Listing each chunk... ");
    printf("Heap starts at %p, breaks at %p.\n", HEAP_START, HEAP_BREAK);
    while (front != NULL) {
        printf("Chunk %d: ", i++);
        printf("Header shows size %ld. ", front->size);
        footer_size = FENCE_BACKWARD((char*) front + front->size)->size;
        printf("Footer shows size %ld.\n", footer_size);
        if (front->size != footer_size) {
            printf("Inconsistent chunk size!\n");
        }
        front = front->next;
    }
}

static void malloc_print_all_chunks()
{
    char *start = HEAP_START + FENCE_SIZE;
    fence_t front = (fence_t) start;
    fence_t back;
    int i = 0;
    size_t real_size;
    size_t footer_size;
    printf("Listing each used/free chunk... ");
    printf("Heap starts at %p, breaks at %p.\n", HEAP_START, HEAP_BREAK);
    while (front->size != 1) {
        printf("Chunk %d, %p: ", i++, front);
        real_size = GETSIZE(front->size);
        if (ISUSED(front->size))
            printf("Used. ");
        else
            printf("Free. ");
        printf("Header shows real size %ld. ", real_size);
        if (front->size == 0 || front->size % 16 > 1) {
            printf("Error occurred.\n");
            return;
        }
        back = FENCE_BACKWARD((char*) front + real_size);
        footer_size = GETSIZE(back->size);
        printf("Footer shows real size %ld.\n", footer_size);
        if (real_size != footer_size) {
            printf("Inconsistent chunk size!\n");
            return;
        }
        front = back + 1;
    }
    printf("End reached.\n");
}
#endif /* DEBUG != 0 */

/***********************************************************************/

static inline size_t highest(size_t in) 
{
    size_t num_bits = 0;

    while (in != 0) {
        ++num_bits;
        in >>= 1;
    }

    return num_bits;
}

void* calloc(size_t number, size_t size) 
{
    size_t number_size = 0;
    size_t *target, *end;
    char *erase;
    size_t i;
    
calloc_count++;

    /* This prevents an integer overflow.  A size_t is a typedef to an integer
     * large enough to index all of memory.  If we cannot fit in a size_t, then
     * we need to fail.
     */
    if (highest(number) + highest(size) > sizeof(size_t) * CHAR_BIT) {
        errno = ENOMEM;
        return NULL;
    }

    number_size = number * size;
    void* ret = malloc(number_size);

if (ret) {
        memset(ret, 0, number_size);
}

    //~ if (ret) {
        //~ erase = ret;
        //~ for (i = 0; i < number_size; i++) {
            //~ erase[i] = 0;
        //~ }
        //end = target + ROUNDUP_8(number_size) / SIZE_T_SIZE - 1;
        //~ while (target < end) {
            //~ *(target++) = 0;
        //~ }
    //~ }

    return ret;
}

void* realloc(void *ptr, size_t size) 
{
    /* Set this to the size of the buffer pointed to by ptr */
    size_t old_size;
    void* ret;
    size_t *source, *target, *end;
realloc_count++;
    if (NULL == ptr) {
        return malloc(size);
    }
    if (0 == size) {
        free(ptr);
        return NULL;
    }
    
    old_size = GETSIZE(FENCE_BACKWARD(ptr)->size) - FENCE_OVERHEAD;
    if (old_size >= size)
        return ptr;

if ((ret = malloc(size))) {
    memmove(ret, ptr, old_size < size ? old_size : size);
    free(ptr);
    return ret;
} else {
    errno = ENOMEM;
    return NULL;
}

        //~ if ((ret = malloc(size))) {
        //~ source = ptr;
        //~ target = ret;
        //~ end = target + old_size / SIZE_T_SIZE;
        //~ while (target < end) {
            //~ *(target++) = *(source++);
        //~ }
        //~ free(ptr);
    //~ } 
    return ret;
    
}
