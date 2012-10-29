#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "malloc.h"
#include "memreq.h"

/* Set to 0 to turn off debugging. */
#define DEBUG 0

/* Memory overhead of a free node. */
#define NODE_OVERHEAD (sizeof(struct fnode)+sizeof(struct fence))
#define FENCE_OVERHEAD (2 * sizeof(struct fence))
#define DIFF_OVERHEAD (NODE_OVERHEAD - FENCE_OVERHEAD)

/* Assume sizeof(size_t) and sizeof(void*) are 8, the same. */
#define SIZE_T_SIZE (sizeof(size_t))
#define ALIGN_SIZE 8
#define ALIGN8(x) (((((x)-1)>>3)+1)<<3)

/* Use one bit in size for marking the chunk in-use/free. */
#define SET_USED(x) ((x) |= 1)
#define SET_FREE(x) ((x) &= (~1))
#define ISUSED(x) ((x) & 1)
#define GETSIZE(x) (((x)>>1)<<1)

/* Round up to nearest sizes. */
#define ROUNDUP_8(x) (((((x)-1)>>3)+1)<<3)
#define ROUNDUP_PAGE(x) (((((x)-1)/PAGE_SIZE)+1)*PAGE_SIZE)
#define ROUNDUP_CHUNK(x) ROUNDUP_8((x)+FENCE_OVERHEAD)

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
static fnode_t malloc_find_fit(fnode_t target, size_t size);
static fnode_t malloc_expand(size_t size);
static void malloc_list_add(fnode_t *list, fnode_t item);
static void malloc_fnode_use(fnode_t *list, fnode_t node, size_t size);
static void malloc_fnode_remove(fnode_t *list, fnode_t node);

/* Debugging functions */
static void malloc_print_free_chunks(fnode_t list);

void *malloc(size_t size) 
{
    fnode_t fit;
    /* The chunk size to be requested */
    if (size < DIFF_OVERHEAD)
        size = DIFF_OVERHEAD;
    size = ROUNDUP_CHUNK(size);
    
    if ((fit = malloc_find_fit(flist, size)) == NULL) {
        fit = malloc_expand(size);
        malloc_list_add(&flist, fit);
    }
    malloc_fnode_use(&flist, fit, size);
    
    malloc_print_free_chunks(flist);
    
    return fit;
}

void free(void* ptr) 
{

}

/* Initialize and fence a free node. */
static fnode_t malloc_fnode_create(char *start, size_t size) 
{
    fnode_t node = (fnode_t) start;
    fence_t end = (fence_t) (start + size) - 1;
    node->size = size;
    SET_FREE(node->size);
    end->size = node->size;
    node->prev = NULL;
    node->next = NULL;
    return node;
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

/* Increase break, return a free node at the new break. */
static fnode_t malloc_expand(size_t size)
{
    char *start;
    char init = 0;
    fnode_t node;
    /* Two cases; getting initial memory or expanding memory */
    if (0 == PAGE_SIZE) {
        PAGE_SIZE = sysconf(_SC_PAGESIZE);
        init = 1;
        size = ROUNDUP_PAGE(size + 2 * SIZE_T_SIZE);
    } else {
        size = ROUNDUP_PAGE(size);
    }
    if ((start = get_memory(size)) == NULL) {
        return NULL;
    }
    /* Put on initial fences */
    if (1 == init) {
        ((fence_t) start)->size = 1;
        (((fence_t) (start + size)) - 1)->size = 1;
        start += sizeof(struct fence);
        size -= FENCE_OVERHEAD;
    }
    node = malloc_fnode_create(start, size);
    return node;
}

/* Add item to the address-ordered list of free nodes */
static void malloc_list_add(fnode_t *list, fnode_t item)
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

/* 
 * Prepare node to be returned to the user. Split the node if possible. 
 *  'size' is the size of the chunk to be returned.
 */
static void malloc_fnode_use(fnode_t *list, fnode_t node, size_t size)
{
    char *start = (char*) node;
    char *split = (char*) node + size;
    fnode_t node_new;
    size_t split_size = node->size - size;
    if (split_size >= NODE_OVERHEAD) {
        node_new = malloc_fnode_create(split, split_size);
        if ((node_new->prev = node->prev))
            node_new->prev->next = node_new;
        if ((node_new->next = node->next))
            node_new->next->prev = node_new;
        if (*list == node)
            *list = node_new;
    } else {
        malloc_fnode_remove(&flist, node);
    }
    malloc_fnode_create(start, size);
}

/* Remove fnode from 'list' */
static void malloc_fnode_remove(fnode_t *list, fnode_t node)
{
    fnode_t front = *list;
    if (*list == node) {
        *list = node->next;
    } else {
        while (front->next != node) {
            front = front->next;
        }
        front->next = node->next;
    }
    if (node->next) {
            node->next->prev = front;
    }
}

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
            exit(-1);
        }
        front = front->next;
    }
}

/***********************************************************************/

static size_t highest(size_t in) 
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

    return ret;
}

void* realloc(void *ptr, size_t size) 
{
    size_t old_size = 0; /* XXX Set this to the size of the buffer pointed to by ptr */
    void* ret = malloc(size);

    if (ret) {
        if (ptr) {
            memmove(ret, ptr, old_size < size ? old_size : size);
            free(ptr);
        }

        return ret;
    } else {
        errno = ENOMEM;
        return NULL;
    }
}
