/* Set to 0 to turn off debugging. */
#define DEBUG 0
#if DEBUG != 0
#include <string.h>
#include <stdio.h>
#endif /* DEBUG != 0 */

#include <errno.h>
#include <limits.h>
#include <unistd.h>

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
#define ISUSED(x) ((x) & 1)
#define GETSIZE(x) (((x)>>1)<<1)

/* Round up to nearest sizes. */
#define ROUNDUP_8(x) (((((x)-1)>>3)+1)<<3)
#define ROUNDUP_16(x) (((((x)-1)>>4)+1)<<4)
#define ROUNDUP_PAGE(x) (((((x)-1)/PAGE_SIZE)+1)*PAGE_SIZE)
#define ROUNDUP_CHUNK(x) ROUNDUP_16((x)+FENCE_OVERHEAD)

/* Get chunk size from fence */
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
static size_t PAGE_SIZE = 0;
static fnode_t flist = NULL;

/* Helper-function declarations. TExplained before each function definition. */
static fnode_t malloc_fnode_create(char *start, size_t size);
static void malloc_fnode_assign(char *start, size_t size);
static void malloc_fnode_release(fnode_t *list, fence_t item);
static fnode_t malloc_find_fit(fnode_t target, size_t size);
static fnode_t malloc_expand(size_t size);
static void malloc_list_addr_insert(fnode_t *list, fnode_t item);
static void *malloc_fnode_split(fnode_t *list, fnode_t node, size_t size);
static void malloc_list_remove(fnode_t *list, fnode_t node);
static fnode_t malloc_fnode_fuse_up(fnode_t *list, fnode_t node);
static fnode_t malloc_fnode_fuse_down(fnode_t *list, fnode_t node);

/* Debugging functions */
#if DEBUG != 0
static void malloc_print_free_chunks(fnode_t list);
#endif /* DEBUG != 0 */

void *malloc(size_t size) 
{
    fnode_t fit;
    /* The chunk size to be requested */
    if (size < DIFF_OVERHEAD) {
        size = DIFF_OVERHEAD;
    }
    size = ROUNDUP_CHUNK(size);
    
    if ((fit = malloc_find_fit(flist, size)) == NULL) {
        if ((fit = malloc_expand(size)) != NULL) {
            malloc_list_addr_insert(&flist, fit);
        } else {
            errno = ENOMEM;
            return NULL;
        }
    }

    return malloc_fnode_split(&flist, fit, size);
}

void free(void* ptr) 
{
    if (ptr) {
        malloc_fnode_release(&flist, FENCE_BACKWARD(ptr));
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
static fnode_t malloc_fnode_create(char *start, size_t size) 
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
    /* Put on initial fences */
    if (1 == init) {
        ((fence_t) start)->size = 1;
        FENCE_BACKWARD(start + size)->size = 1;
        start += FENCE_SIZE;
        size -= FENCE_OVERHEAD;
    } else {
        FENCE_BACKWARD(start + size)->size = 1;
        start -= FENCE_SIZE;
    }
    return malloc_fnode_create(start, size);
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
        while (front->next != NULL && front->next < item) {
            front = front->next;
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
static void *malloc_fnode_split(fnode_t *list, fnode_t node, size_t size)
{
    char *start = (char*) node;
    char *split = ((char*) node) + size;
    size_t split_size = node->size - size;
    fnode_t node_new;
    if (split_size >= NODE_OVERHEAD) {
        node_new = malloc_fnode_create(split, split_size);
        if ((node_new->prev = node->prev))
            node_new->prev->next = node_new;
        if ((node_new->next = node->next))
            node_new->next->prev = node_new;
        if (*list == node)
            *list = node_new;
        malloc_fnode_assign(start, size);
    } else {
        malloc_list_remove(&flist, node);
        malloc_fnode_assign(start, node->size);
    }
    
    
    return start + FENCE_SIZE;
}

/* Prepare node to be returned to the user. */
static void malloc_fnode_assign(char *start, size_t size)
{
    fnode_t node = (fnode_t) start;
    fence_t end = FENCE_BACKWARD((start + size));
    node->size = size;
    SET_USED(node->size);
    end->size = node->size;
    node->prev = NULL;
    node->next = NULL;
}

/* Add the chunk back to the free list. */
static void malloc_fnode_release(fnode_t *list, fence_t target) 
{
    fnode_t node;
    SET_FREE(target->size);
    node = malloc_fnode_create((char*)target, target->size);
    node = malloc_fnode_fuse_up(list, node);
    node = malloc_fnode_fuse_down(list, node);
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

/* Fuse with the neighbor free nodes if possible. */
static fnode_t malloc_fnode_fuse_up(fnode_t *list, fnode_t node)
{
    fnode_t prev_node; 
    fence_t prev_backfence = FENCE_BACKWARD(node);
    fence_t curr_backfence;
    if (ISUSED(prev_backfence->size)) {
        malloc_list_addr_insert(list, node);
        return node;
    }
    prev_node = (fnode_t) ((char*)node - prev_backfence->size);
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
static void malloc_print_free_chunks(fnode_t front)
{
    int i = 0;
    size_t footer_size;
    printf("Listing each chunk...\n");
    while (front != NULL) {
        printf("Chunk %d: ", i++);
        printf("Header shows size %ld. ", front->size);
        footer_size = ((fence_t)((char*) front + front->size) - 1)->size;
        printf("Footer shows size %ld.\n", footer_size);
        if (front->size != footer_size) {
            printf("Inconsistent chunk size!\n");
        }
        front = front->next;
    }
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
        target = ret;
        end = target + number_size / SIZE_T_SIZE;
        while (target != end) {
            *(target++) = 0;
        }
    }

    return ret;
}

void* realloc(void *ptr, size_t size) 
{
    /* Set this to the size of the buffer pointed to by ptr */
    size_t old_size;
    void* ret;
    size_t *source, *target, *end;
    if (NULL == ptr) {
        return malloc(size);
    }
    if (0 == size) {
        free(ptr);
        return ptr;
    }
    
    old_size = GETSIZE(FENCE_BACKWARD(ptr)->size) - FENCE_OVERHEAD;
    if (old_size >= size)
        return ptr;
    if ((ret = malloc(size))) {
        source = ptr;
        target = ret;
        end = target + size / SIZE_T_SIZE;
        while (target != end) {
            *(target++) = *(source++);
        }
        free(ptr);
    } 
    return ret;
    
}
