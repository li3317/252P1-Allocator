//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the allocator as indicated in the handout.
// 
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include "MyMalloc.h"

#define MIN_SIZE 56

static pthread_mutex_t mutex;

const int ArenaSize = 2097152;
const int NumberOfFreeLists = 1;

// Header of an object. Used both when the object is allocated and freed
struct ObjectHeader {
    size_t _objectSize;         // Real size of the object.
    int _allocated;             // 1 = yes, 0 = no 2 = sentinel
    struct ObjectHeader * _next;       // Points to the next object in the freelist (if free).
    struct ObjectHeader * _prev;       // Points to the previous object.
};

struct ObjectFooter {
    size_t _objectSize;
    int _allocated;
};

  //STATE of the allocator

  // Size of the heap
  static size_t _heapSize;

  // initial memory pool
  static void * _memStart;

  // number of chunks request from OS
  static int _numChunks;

  // True if heap has been initialized
  static int _initialized;

  // Verbose mode
  static int _verbose;

  // # malloc calls
  static int _mallocCalls;

  // # free calls
  static int _freeCalls;

  // # realloc calls
  static int _reallocCalls;
  
  // # realloc calls
  static int _callocCalls;

  // Free list is a sentinel
  static struct ObjectHeader _freeListSentinel; // Sentinel is used to simplify list operations
  static struct ObjectHeader *_freeList;


  //FUNCTIONS

  //Initializes the heap
  void initialize();

  // Allocates an object 
  void * allocateObject( size_t size );

  // Frees an object
  void freeObject( void * ptr );

  // Returns the size of an object
  size_t objectSize( void * ptr );

  // At exit handler
  void atExitHandler();

  //Prints the heap size and other information about the allocator
  void print();
  void print_list();

  // Gets memory from the OS
  void * getMemoryFromOS( size_t size );

  // Gets a nicely and initialized fenced memory chunk of the desired size
  struct ObjectHeader * getFencedChunk(size_t size);

  // Gets a valid free memory block from the list
  struct ObjectHeader * getValidBlock(size_t roundedSize);

  void increaseMallocCalls() { _mallocCalls++; }

  void increaseReallocCalls() { _reallocCalls++; }

  void increaseCallocCalls() { _callocCalls++; }

  void increaseFreeCalls() { _freeCalls++; }

extern void
atExitHandlerInC()
{
  atExitHandler();
}

void initialize()
{
  // Environment var VERBOSE prints stats at end and turns on debugging
  // Default is on
  _verbose = 1;
  const char * envverbose = getenv( "MALLOCVERBOSE" );
  if ( envverbose && !strcmp( envverbose, "NO") ) {
    _verbose = 0;
  }

  pthread_mutex_init(&mutex, NULL);
  void * _mem = getMemoryFromOS( ArenaSize + (2*sizeof(struct ObjectHeader)) + (2*sizeof(struct ObjectFooter)) );

  // In verbose mode register also printing statistics at exit
  atexit( atExitHandlerInC );

  // establish fence posts
  // footer
  struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;
  fencepost1->_allocated = 1;
  fencepost1->_objectSize = 123456789;

  // header
  char * temp = 
      (char *)_mem + (2*sizeof(struct ObjectFooter)) + sizeof(struct ObjectHeader) + ArenaSize;
  struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;
  fencepost2->_allocated = 1;
  fencepost2->_objectSize = 123456789;
  fencepost2->_next = NULL;
  fencepost2->_prev = NULL;

  // initialize the list to point to the _mem
  temp = (char *) _mem + sizeof(struct ObjectFooter);
  struct ObjectHeader * currentHeader = (struct ObjectHeader *) temp;
  temp = (char *)_mem + sizeof(struct ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
  struct ObjectFooter * currentFooter = (struct ObjectFooter *) temp;
  _freeList = &_freeListSentinel;
  currentHeader->_objectSize = ArenaSize + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter); // 2MB + header and footer size
  currentHeader->_allocated = 0;
  currentHeader->_next = _freeList;
  currentHeader->_prev = _freeList;
  currentFooter->_allocated = 0;
  currentFooter->_objectSize = currentHeader->_objectSize;
  _freeList->_prev = currentHeader;
  _freeList->_next = currentHeader; 
  _freeList->_allocated = 2; // sentinel. no coalescing.
  _freeList->_objectSize = 0;
  _memStart = (char*) currentHeader;
}

void * allocateObject( size_t size )
{
    //Make sure that allocator is initialized
    if ( !_initialized ) {
      _initialized = 1;
      initialize();
    }

    // Minimum size = 8 bytes
    if (size < 7) size = 8;

    // Add the ObjectHeader/Footer to the size and round the total size up to a multiple of
    // 8 bytes for alignment.
    size_t roundedSize = (size + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter) + 7) & ~7;

    // My code here.

    // Get a block from the freelist that's large enough
    struct ObjectHeader * o = getValidBlock(roundedSize);
    struct ObjectFooter * f = (struct ObjectFooter *)((char *)o + roundedSize - sizeof(struct ObjectFooter));

    // Now cut the block into the part we need and the remainder
    if (o->_objectSize > MIN_SIZE + roundedSize) {
        struct ObjectHeader * h = (struct ObjectHeader *)((char *)o + roundedSize);
        struct ObjectHeader * following = o->_next;
        following->_prev = h;
        h->_next = following;
        h->_prev = o;
        o->_next = h;
        h->_objectSize = o->_objectSize - roundedSize;
    }

    // Then remove the part we need from the freelist
    struct ObjectHeader * p = o->_prev;
    struct ObjectHeader * n = o->_next;
    p->_next = n;
    n->_prev = p;

    // Set allocated to true
    o->_allocated = 1;
    f->_allocated = 1;

    // Naively get memory from the OS every time
    //void * _mem = getMemoryFromOS( roundedSize );

    // Store the size in the header
    //struct ObjectHeader * o = (struct ObjectHeader *) _mem;

    o->_objectSize = roundedSize;

    pthread_mutex_unlock(&mutex);

    // Return a pointer to usable memory
    return (void *) (o + 1);

}

// Returns the header address of the
// first valid block from the list of available
// memory blocks that is at least size bytes large.
// Recall that headers contain the size of the memory block
// INCLUDING the size of the header and footer.
// Fortunately, this is already accounted for.
struct ObjectHeader * getValidBlock(size_t size)
{
    struct ObjectHeader * current = _freeList->_next;

    while (current != _freeList)
    {
        if (current->_objectSize >= size)
        {
            return current;
        }
        current = current->_next;
    }

    if (current == _freeList)
    {
        // No memory blocks large enough
        // Create a new one

        int request = ArenaSize + 2*sizeof(struct ObjectHeader) + 2*sizeof(struct ObjectFooter);
        if (size > request) request = size;
        struct ObjectHeader * chunk = getFencedChunk(request);
        // Add it to the free list
        
        if (chunk < _freeList->_next) {
            // Add to beginning of list
            struct ObjectHeader * n = _freeList->_next;
            _freeList->_next = chunk;
            chunk->_prev = _freeList;
            chunk->_next = n;
            n->_prev = chunk;

        } else if (chunk > _freeList->_prev) {
            // Add to end of list
            struct ObjectHeader * p = _freeList->_prev;
            _freeList->_prev = chunk;
            chunk->_next = _freeList;
            chunk->_prev = p;
            p->_next = chunk;
        } else {
            // Belongs somewhere in the middle
            // Should only happen if multiple chunks have been added
            struct ObjectHeader * following = _freeList->_next;
            while (chunk > following) {
                following = following->_next;
            }
            struct ObjectHeader * preceding = following->_prev;
            following->_prev = chunk;
            preceding->_next = chunk;
            chunk->_prev = preceding;
            chunk->_next = following;
        }

        current = chunk;
    }

    return current;
}

void freeObject( void * ptr )
{
    // Add this memory block back into the freelist
    // in the correct position, then coalesce it
    // with any unallocated memory blocks adjacent
    // to it
  
    // Get header and footer of object
    struct ObjectHeader * header = (struct ObjectHeader *)((char *)ptr - sizeof(struct ObjectHeader));
    struct ObjectFooter * footer = (struct ObjectFooter *)((char *)header + header->_objectSize - sizeof(struct ObjectFooter));

    // Deallocate memory
    header->_allocated = 0;
    footer->_allocated = 0;

    // Get first memory block in free list following this one
    struct ObjectHeader * following = _freeList->_next;

    while (following < header && following != _freeList) {
        following = following->_next;
    }

    // And the one before that
    struct ObjectHeader * preceding = following->_prev;

    // Now cram it in between them
    preceding->_next = header;
    following->_prev = header;
    header->_prev = preceding;
    header->_next = following;

    // and coalesce it if necessary
    // A note on this: memory blocks should never be allocated if
    // they are in the free list, but we still need to check anyway
    // to make sure that wedon' coalesce the sentinel. That would be
    // bad.
    if ((char *)preceding + preceding->_objectSize == header && !preceding->_allocated) {
        preceding->_objectSize += header->_objectSize;
        footer->_objectSize += preceding->_objectSize;
        preceding->_next = following;
        following->_prev = preceding;
        header = preceding;
    }

    if ((char *)header + header->_objectSize == following && !following->_allocated) {
        struct ObjectFooter * ff = (struct ObjectFooter *)((char *)following + following->_objectSize - sizeof(struct ObjectFooter));
        header->_objectSize += following->_objectSize;
        ff->_objectSize += header->_objectSize;
        header->_next = following->_next;
        following->_next->_prev = header;
    }

    return;

}

struct ObjectHeader * getFencedChunk(size_t size)
{
    void * _mem = getMemoryFromOS(size);

    // establish fence posts
    // footer
    struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;
    fencepost1->_allocated = 1;
    fencepost1->_objectSize = 123456789;

    // header
    char * temp = (char *)_mem + (size - sizeof(struct ObjectHeader));
    struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;
    fencepost2->_allocated = 1;
    fencepost2->_objectSize = 123456789;
    fencepost2->_next = NULL;
    fencepost2->_prev = NULL;

    // Now put in the actual header and footer and initialize them
    struct ObjectHeader * h = (struct ObjectHeader *)((char *)fencepost1 + sizeof(struct ObjectFooter));
    struct ObjectFooter * f = (struct ObjectFooter *)((char *)fencepost2 - sizeof(struct ObjectFooter));
    h->_objectSize = size - sizeof(struct ObjectHeader) - sizeof(struct ObjectFooter);
    h->_allocated = 0;
    f->_objectSize = h->_objectSize;
    h->_allocated = 0;
    h->_next = NULL;
    h->_prev = NULL;

    return h;
}

size_t objectSize( void * ptr )
{
  // Return the size of the object pointed by ptr. We assume that ptr is a valid obejct.
  struct ObjectHeader * o =
    (struct ObjectHeader *) ( (char *)ptr - sizeof(struct ObjectHeader) );

  // Subtract the size of the header (Edit: and footer?)
  // Why not already implemented?
  // Seems like you shouldn't, actually.
  // Only used in realloc() implementation below.
  // Looks like size of header and footer are important.
  return o->_objectSize;
}

void print()
{
  printf("\n-------------------\n");

  printf("HeapSize:\t%zd bytes\n", _heapSize );
  printf("# mallocs:\t%d\n", _mallocCalls );
  printf("# reallocs:\t%d\n", _reallocCalls );
  printf("# callocs:\t%d\n", _callocCalls );
  printf("# frees:\t%d\n", _freeCalls );

  printf("\n-------------------\n");
}

void print_list()
{
  printf("FreeList: ");
  if ( !_initialized ) {
    _initialized = 1;
    initialize();
  }
  struct ObjectHeader * ptr = _freeList->_next;
  while(ptr != _freeList){
      long offset = (long)ptr - (long)_memStart;
      printf("[offset:%ld,size:%zd]",offset,ptr->_objectSize);
      ptr = ptr->_next;
      if(ptr != NULL){
          printf("->");
      }
  }
  printf("\n");
}

void * getMemoryFromOS( size_t size )
{
  // Use sbrk() to get memory from OS
  _heapSize += size;
 
  void * _mem = sbrk( size );

  if(!_initialized){
      _memStart = _mem;
  }

  _numChunks++;

  return _mem;
}

void atExitHandler()
{
  // Print statistics when exit
  if ( _verbose ) {
    print();
  }
}

//
// C interface
//

extern void *
malloc(size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseMallocCalls();
  
  return allocateObject( size );
}

extern void
free(void *ptr)
{
  pthread_mutex_lock(&mutex);
  increaseFreeCalls();
  
  if ( ptr == 0 ) {
    // No object to free
    pthread_mutex_unlock(&mutex);
    return;
  }
  
  freeObject( ptr );
}

extern void *
realloc(void *ptr, size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseReallocCalls();
    
  // Allocate new object
  void * newptr = allocateObject( size );

  // Copy old object only if ptr != 0
  if ( ptr != 0 ) {
    
    // copy only the minimum number of bytes
    size_t sizeToCopy =  objectSize( ptr );
    if ( sizeToCopy > size ) {
      sizeToCopy = size;
    }
    
    memcpy( newptr, ptr, sizeToCopy );

    //Free old object
    freeObject( ptr );
  }

  return newptr;
}

extern void *
calloc(size_t nelem, size_t elsize)
{
  pthread_mutex_lock(&mutex);
  increaseCallocCalls();
    
  // calloc allocates and initializes
  size_t size = nelem * elsize;

  void * ptr = allocateObject( size );

  if ( ptr ) {
    // No error
    // Initialize chunk with 0s
    memset( ptr, 0, size );
  }

  return ptr;
}

