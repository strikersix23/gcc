/* Vector API for GNU compiler.
   Copyright (C) 2004-2025 Free Software Foundation, Inc.
   Contributed by Nathan Sidwell <nathan@codesourcery.com>
   Re-implemented in C++ by Diego Novillo <dnovillo@google.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#ifndef GCC_VEC_H
#define GCC_VEC_H

/* Some gen* file have no ggc support as the header file gtype-desc.h is
   missing.  Provide these definitions in case ggc.h has not been included.
   This is not a problem because any code that runs before gengtype is built
   will never need to use GC vectors.*/

extern void ggc_free (void *);
extern size_t ggc_round_alloc_size (size_t requested_size);
extern void *ggc_realloc (void *, size_t MEM_STAT_DECL);

/* Templated vector type and associated interfaces.

   The interface functions are typesafe and use inline functions,
   sometimes backed by out-of-line generic functions.  The vectors are
   designed to interoperate with the GTY machinery.

   There are both 'index' and 'iterate' accessors.  The index accessor
   is implemented by operator[].  The iterator returns a boolean
   iteration condition and updates the iteration variable passed by
   reference.  Because the iterator will be inlined, the address-of
   can be optimized away.

   Each operation that increases the number of active elements is
   available in 'quick' and 'safe' variants.  The former presumes that
   there is sufficient allocated space for the operation to succeed
   (it dies if there is not).  The latter will reallocate the
   vector, if needed.  Reallocation causes an exponential increase in
   vector size.  If you know you will be adding N elements, it would
   be more efficient to use the reserve operation before adding the
   elements with the 'quick' operation.  This will ensure there are at
   least as many elements as you ask for, it will exponentially
   increase if there are too few spare slots.  If you want reserve a
   specific number of slots, but do not want the exponential increase
   (for instance, you know this is the last allocation), use the
   reserve_exact operation.  You can also create a vector of a
   specific size from the get go.

   You should prefer the push and pop operations, as they append and
   remove from the end of the vector. If you need to remove several
   items in one go, use the truncate operation.  The insert and remove
   operations allow you to change elements in the middle of the
   vector.  There are two remove operations, one which preserves the
   element ordering 'ordered_remove', and one which does not
   'unordered_remove'.  The latter function copies the end element
   into the removed slot, rather than invoke a memmove operation.  The
   'lower_bound' function will determine where to place an item in the
   array using insert that will maintain sorted order.

   Vectors are template types with three arguments: the type of the
   elements in the vector, the allocation strategy, and the physical
   layout to use

   Four allocation strategies are supported:

	- Heap: allocation is done using malloc/free.  This is the
	  default allocation strategy.

  	- GC: allocation is done using ggc_alloc/ggc_free.

  	- GC atomic: same as GC with the exception that the elements
	  themselves are assumed to be of an atomic type that does
	  not need to be garbage collected.  This means that marking
	  routines do not need to traverse the array marking the
	  individual elements.  This increases the performance of
	  GC activities.

   Two physical layouts are supported:

	- Embedded: The vector is structured using the trailing array
	  idiom.  The last member of the structure is an array of size
	  1.  When the vector is initially allocated, a single memory
	  block is created to hold the vector's control data and the
	  array of elements.  These vectors cannot grow without
	  reallocation (see discussion on embeddable vectors below).

	- Space efficient: The vector is structured as a pointer to an
	  embedded vector.  This is the default layout.  It means that
	  vectors occupy a single word of storage before initial
	  allocation.  Vectors are allowed to grow (the internal
	  pointer is reallocated but the main vector instance does not
	  need to relocate).

   The type, allocation and layout are specified when the vector is
   declared.

   If you need to directly manipulate a vector, then the 'address'
   accessor will return the address of the start of the vector.  Also
   the 'space' predicate will tell you whether there is spare capacity
   in the vector.  You will not normally need to use these two functions.

   Not all vector operations support non-POD types and such restrictions
   are enforced through static assertions.  Some operations which often use
   memmove to move elements around like quick_insert, safe_insert,
   ordered_remove, unordered_remove, block_remove etc. require trivially
   copyable types.  Sorting operations, qsort, sort and stablesort, require
   those too but as an extension allow also std::pair of 2 trivially copyable
   types which happens to work even when std::pair itself isn't trivially
   copyable.  The quick_grow and safe_grow operations require trivially
   default constructible types.  One can use quick_grow_cleared or
   safe_grow_cleared for non-trivially default constructible types if needed
   (but of course such operation is more expensive then).  The pop operation
   returns reference to the last element only for trivially destructible
   types, for non-trivially destructible types one should use last operation
   followed by pop which in that case returns void.
   And finally, the GC and GC atomic vectors should always be used with
   trivially destructible types, as nothing will invoke destructors when they
   are freed during GC.

   Notes on the different layout strategies

   * Embeddable vectors (vec<T, A, vl_embed>)

     These vectors are suitable to be embedded in other data
     structures so that they can be pre-allocated in a contiguous
     memory block.

     Embeddable vectors are implemented using the trailing array
     idiom, thus they are not resizeable without changing the address
     of the vector object itself.  This means you cannot have
     variables or fields of embeddable vector type -- always use a
     pointer to a vector.  The one exception is the final field of a
     structure, which could be a vector type.

     You will have to use the embedded_size & embedded_init calls to
     create such objects, and they will not be resizeable (so the
     'safe' allocation variants are not available).

     Properties of embeddable vectors:

	  - The whole vector and control data are allocated in a single
	    contiguous block.  It uses the trailing-vector idiom, so
	    allocation must reserve enough space for all the elements
	    in the vector plus its control data.
	  - The vector cannot be re-allocated.
	  - The vector cannot grow nor shrink.
	  - No indirections needed for access/manipulation.
	  - It requires 2 words of storage (prior to vector allocation).


   * Space efficient vector (vec<T, A, vl_ptr>)

     These vectors can grow dynamically and are allocated together
     with their control data.  They are suited to be included in data
     structures.  Prior to initial allocation, they only take a single
     word of storage.

     These vectors are implemented as a pointer to embeddable vectors.
     The semantics allow for this pointer to be NULL to represent
     empty vectors.  This way, empty vectors occupy minimal space in
     the structure containing them.

     Properties:

	- The whole vector and control data are allocated in a single
	  contiguous block.
  	- The whole vector may be re-allocated.
  	- Vector data may grow and shrink.
  	- Access and manipulation requires a pointer test and
	  indirection.
  	- It requires 1 word of storage (prior to vector allocation).

   An example of their use would be,

   struct my_struct {
     // A space-efficient vector of tree pointers in GC memory.
     vec<tree, va_gc, vl_ptr> v;
   };

   struct my_struct *s;

   if (s->v.length ()) { we have some contents }
   s->v.safe_push (decl); // append some decl onto the end
   for (ix = 0; s->v.iterate (ix, &elt); ix++)
     { do something with elt }
*/

/* Support function for statistics.  */
extern void dump_vec_loc_statistics (void);

/* Hashtable mapping vec addresses to descriptors.  */
extern htab_t vec_mem_usage_hash;

/* Destruct N elements in DST.  */

template <typename T>
inline void
vec_destruct (T *dst, unsigned n)
{
  for ( ; n; ++dst, --n)
    dst->~T ();
}

/* Control data for vectors.  This contains the number of allocated
   and used slots inside a vector.  */

struct vec_prefix
{
  /* FIXME - These fields should be private, but we need to cater to
	     compilers that have stricter notions of PODness for types.  */

  /* Memory allocation support routines in vec.cc.  */
  void register_overhead (void *, size_t, size_t CXX_MEM_STAT_INFO);
  void release_overhead (void *, size_t, size_t, bool CXX_MEM_STAT_INFO);
  static unsigned calculate_allocation (vec_prefix *, unsigned, bool);
  static unsigned calculate_allocation_1 (unsigned, unsigned);

  /* Note that vec_prefix should be a base class for vec, but we use
     offsetof() on vector fields of tree structures (e.g.,
     tree_binfo::base_binfos), and offsetof only supports base types.

     To compensate, we make vec_prefix a field inside vec and make
     vec a friend class of vec_prefix so it can access its fields.  */
  template <typename, typename, typename> friend struct vec;

  /* The allocator types also need access to our internals.  */
  friend struct va_gc;
  friend struct va_gc_atomic;
  friend struct va_heap;

  unsigned m_alloc : 31;
  unsigned m_using_auto_storage : 1;
  unsigned m_num;
};

/* Calculate the number of slots to reserve a vector, making sure that
   RESERVE slots are free.  If EXACT grow exactly, otherwise grow
   exponentially.  PFX is the control data for the vector.  */

inline unsigned
vec_prefix::calculate_allocation (vec_prefix *pfx, unsigned reserve,
				  bool exact)
{
  if (exact)
    return (pfx ? pfx->m_num : 0) + reserve;
  else if (!pfx)
    return MAX (4, reserve);
  return calculate_allocation_1 (pfx->m_alloc, pfx->m_num + reserve);
}

template<typename, typename, typename> struct vec;

/* Valid vector layouts

   vl_embed	- Embeddable vector that uses the trailing array idiom.
   vl_ptr	- Space efficient vector that uses a pointer to an
		  embeddable vector.  */
struct vl_embed { };
struct vl_ptr { };


/* Types of supported allocations

   va_heap	- Allocation uses malloc/free.
   va_gc	- Allocation uses ggc_alloc.
   va_gc_atomic	- Same as GC, but individual elements of the array
		  do not need to be marked during collection.  */

/* Allocator type for heap vectors.  */
struct va_heap
{
  /* Heap vectors are frequently regular instances, so use the vl_ptr
     layout for them.  */
  typedef vl_ptr default_layout;

  template<typename T>
  static void reserve (vec<T, va_heap, vl_embed> *&, unsigned, bool
		       CXX_MEM_STAT_INFO);

  template<typename T>
  static void release (vec<T, va_heap, vl_embed> *&);
};


/* Allocator for heap memory.  Ensure there are at least RESERVE free
   slots in V.  If EXACT is true, grow exactly, else grow
   exponentially.  As a special case, if the vector had not been
   allocated and RESERVE is 0, no vector will be created.  */

template<typename T>
inline void
va_heap::reserve (vec<T, va_heap, vl_embed> *&v, unsigned reserve, bool exact
		  MEM_STAT_DECL)
{
  size_t elt_size = sizeof (T);
  unsigned alloc
    = vec_prefix::calculate_allocation (v ? &v->m_vecpfx : 0, reserve, exact);
  gcc_checking_assert (alloc);

  if (GATHER_STATISTICS && v)
    v->m_vecpfx.release_overhead (v, elt_size * v->allocated (),
				  v->allocated (), false);

  size_t size = vec<T, va_heap, vl_embed>::embedded_size (alloc);
  unsigned nelem = v ? v->length () : 0;
  v = static_cast <vec<T, va_heap, vl_embed> *> (xrealloc (v, size));
  v->embedded_init (alloc, nelem);

  if (GATHER_STATISTICS)
    v->m_vecpfx.register_overhead (v, alloc, elt_size PASS_MEM_STAT);
}


#if GCC_VERSION >= 4007
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

/* Free the heap space allocated for vector V.  */

template<typename T>
void
va_heap::release (vec<T, va_heap, vl_embed> *&v)
{
  size_t elt_size = sizeof (T);
  if (v == NULL)
    return;

  if (!std::is_trivially_destructible <T>::value)
    vec_destruct (v->address (), v->length ());

  if (GATHER_STATISTICS)
    v->m_vecpfx.release_overhead (v, elt_size * v->allocated (),
				  v->allocated (), true);
  ::free (v);
  v = NULL;
}

#if GCC_VERSION >= 4007
#pragma GCC diagnostic pop
#endif

/* Allocator type for GC vectors.  Notice that we need the structure
   declaration even if GC is not enabled.  */

struct va_gc
{
  /* Use vl_embed as the default layout for GC vectors.  Due to GTY
     limitations, GC vectors must always be pointers, so it is more
     efficient to use a pointer to the vl_embed layout, rather than
     using a pointer to a pointer as would be the case with vl_ptr.  */
  typedef vl_embed default_layout;

  template<typename T, typename A>
  static void reserve (vec<T, A, vl_embed> *&, unsigned, bool
		       CXX_MEM_STAT_INFO);

  template<typename T, typename A>
  static void release (vec<T, A, vl_embed> *&v);
};


/* Free GC memory used by V and reset V to NULL.  */

template<typename T, typename A>
inline void
va_gc::release (vec<T, A, vl_embed> *&v)
{
  if (v)
    ::ggc_free (v);
  v = NULL;
}


/* Allocator for GC memory.  Ensure there are at least RESERVE free
   slots in V.  If EXACT is true, grow exactly, else grow
   exponentially.  As a special case, if the vector had not been
   allocated and RESERVE is 0, no vector will be created.  */

template<typename T, typename A>
void
va_gc::reserve (vec<T, A, vl_embed> *&v, unsigned reserve, bool exact
		MEM_STAT_DECL)
{
  unsigned alloc
    = vec_prefix::calculate_allocation (v ? &v->m_vecpfx : 0, reserve, exact);
  if (!alloc)
    {
      ::ggc_free (v);
      v = NULL;
      return;
    }

  /* Calculate the amount of space we want.  */
  size_t size = vec<T, A, vl_embed>::embedded_size (alloc);

  /* Ask the allocator how much space it will really give us.  */
  size = ::ggc_round_alloc_size (size);

  /* Adjust the number of slots accordingly.  */
  size_t vec_offset = sizeof (vec_prefix);
  size_t elt_size = sizeof (T);
  alloc = (size - vec_offset) / elt_size;

  /* And finally, recalculate the amount of space we ask for.  */
  size = vec_offset + alloc * elt_size;

  unsigned nelem = v ? v->length () : 0;
  v = static_cast <vec<T, A, vl_embed> *> (::ggc_realloc (v, size
							       PASS_MEM_STAT));
  v->embedded_init (alloc, nelem);
}


/* Allocator type for GC vectors.  This is for vectors of types
   atomics w.r.t. collection, so allocation and deallocation is
   completely inherited from va_gc.  */
struct va_gc_atomic : va_gc
{
};


/* Generic vector template.  Default values for A and L indicate the
   most commonly used strategies.

   FIXME - Ideally, they would all be vl_ptr to encourage using regular
           instances for vectors, but the existing GTY machinery is limited
	   in that it can only deal with GC objects that are pointers
	   themselves.

	   This means that vector operations that need to deal with
	   potentially NULL pointers, must be provided as free
	   functions (see the vec_safe_* functions above).  */
template<typename T,
         typename A = va_heap,
         typename L = typename A::default_layout>
struct GTY((user)) vec
{
};

/* Allow C++11 range-based 'for' to work directly on vec<T>*.  */
template<typename T, typename A, typename L>
T* begin (vec<T,A,L> *v) { return v ? v->begin () : nullptr; }
template<typename T, typename A, typename L>
T* end (vec<T,A,L> *v) { return v ? v->end () : nullptr; }
template<typename T, typename A, typename L>
const T* begin (const vec<T,A,L> *v) { return v ? v->begin () : nullptr; }
template<typename T, typename A, typename L>
const T* end (const vec<T,A,L> *v) { return v ? v->end () : nullptr; }

/* Generic vec<> debug helpers.

   These need to be instantiated for each vec<TYPE> used throughout
   the compiler like this:

    DEFINE_DEBUG_VEC (TYPE)

   The reason we have a debug_helper() is because GDB can't
   disambiguate a plain call to debug(some_vec), and it must be called
   like debug<TYPE>(some_vec).  */

template<typename T>
void
debug_helper (vec<T> &ref)
{
  unsigned i;
  for (i = 0; i < ref.length (); ++i)
    {
      fprintf (stderr, "[%d] = ", i);
      debug_slim (ref[i]);
      fputc ('\n', stderr);
    }
}

/* We need a separate va_gc variant here because default template
   argument for functions cannot be used in c++-98.  Once this
   restriction is removed, those variant should be folded with the
   above debug_helper.  */

template<typename T>
void
debug_helper (vec<T, va_gc> &ref)
{
  unsigned i;
  for (i = 0; i < ref.length (); ++i)
    {
      fprintf (stderr, "[%d] = ", i);
      debug_slim (ref[i]);
      fputc ('\n', stderr);
    }
}

/* Macro to define debug(vec<T>) and debug(vec<T, va_gc>) helper
   functions for a type T.  */

#define DEFINE_DEBUG_VEC(T) \
  template void debug_helper (vec<T> &);		\
  template void debug_helper (vec<T, va_gc> &);		\
  /* Define the vec<T> debug functions.  */		\
  DEBUG_FUNCTION void					\
  debug (vec<T> &ref)					\
  {							\
    debug_helper <T> (ref);				\
  }							\
  DEBUG_FUNCTION void					\
  debug (vec<T> *ptr)					\
  {							\
    if (ptr)						\
      debug (*ptr);					\
    else						\
      fprintf (stderr, "<nil>\n");			\
  }							\
  /* Define the vec<T, va_gc> debug functions.  */	\
  DEBUG_FUNCTION void					\
  debug (vec<T, va_gc> &ref)				\
  {							\
    debug_helper <T> (ref);				\
  }							\
  DEBUG_FUNCTION void					\
  debug (vec<T, va_gc> *ptr)				\
  {							\
    if (ptr)						\
      debug (*ptr);					\
    else						\
      fprintf (stderr, "<nil>\n");			\
  }

/* Default-construct N elements in DST.  */

template <typename T>
inline void
vec_default_construct (T *dst, unsigned n)
{
  for ( ; n; ++dst, --n)
    ::new (static_cast<void*>(dst)) T ();
}

/* Copy-construct N elements in DST from *SRC.  */

template <typename T>
inline void
vec_copy_construct (T *dst, const T *src, unsigned n)
{
  for ( ; n; ++dst, ++src, --n)
    ::new (static_cast<void*>(dst)) T (*src);
}

/* Type to provide zero-initialized values for vec<T, A, L>.  This is
   used to  provide nil initializers for vec instances.  Since vec must
   be a trivially copyable type that can be copied by memcpy and zeroed
   out by memset, it must have defaulted default and copy ctor and copy
   assignment.  To initialize a vec either use value initialization
   (e.g., vec() or vec v{ };) or assign it the value vNULL.  This isn't
   needed for file-scope and function-local static vectors, which are
   zero-initialized by default.  */
struct vnull { };
constexpr vnull vNULL{ };


/* Embeddable vector.  These vectors are suitable to be embedded
   in other data structures so that they can be pre-allocated in a
   contiguous memory block.

   Embeddable vectors are implemented using the trailing array idiom,
   thus they are not resizeable without changing the address of the
   vector object itself.  This means you cannot have variables or
   fields of embeddable vector type -- always use a pointer to a
   vector.  The one exception is the final field of a structure, which
   could be a vector type.

   You will have to use the embedded_size & embedded_init calls to
   create such objects, and they will not be resizeable (so the 'safe'
   allocation variants are not available).

   Properties:

	- The whole vector and control data are allocated in a single
	  contiguous block.  It uses the trailing-vector idiom, so
	  allocation must reserve enough space for all the elements
  	  in the vector plus its control data.
  	- The vector cannot be re-allocated.
  	- The vector cannot grow nor shrink.
  	- No indirections needed for access/manipulation.
  	- It requires 2 words of storage (prior to vector allocation).  */

template<typename T, typename A>
struct GTY((user)) vec<T, A, vl_embed>
{
public:
  unsigned allocated (void) const { return m_vecpfx.m_alloc; }
  unsigned length (void) const { return m_vecpfx.m_num; }
  bool is_empty (void) const { return m_vecpfx.m_num == 0; }
  T *address (void) { return reinterpret_cast <T *> (this + 1); }
  const T *address (void) const
    { return reinterpret_cast <const T *> (this + 1); }
  T *begin () { return address (); }
  const T *begin () const { return address (); }
  T *end () { return address () + length (); }
  const T *end () const { return address () + length (); }
  const T &operator[] (unsigned) const;
  T &operator[] (unsigned);
  const T &last (void) const;
  T &last (void);
  bool space (unsigned) const;
  bool iterate (unsigned, T *) const;
  bool iterate (unsigned, T **) const;
  vec *copy (ALONE_CXX_MEM_STAT_INFO) const;
  void splice (const vec &);
  void splice (const vec *src);
  T *quick_push (const T &);
  using pop_ret_type
    = typename std::conditional <std::is_trivially_destructible <T>::value,
				 T &, void>::type;
  pop_ret_type pop (void);
  void truncate (unsigned);
  void quick_insert (unsigned, const T &);
  void ordered_remove (unsigned);
  void unordered_remove (unsigned);
  void block_remove (unsigned, unsigned);
  void qsort (int (*) (const void *, const void *));
  void sort (int (*) (const void *, const void *, void *), void *);
  void stablesort (int (*) (const void *, const void *, void *), void *);
  T *bsearch (const void *key, int (*compar) (const void *, const void *));
  T *bsearch (const void *key,
	      int (*compar)(const void *, const void *, void *), void *);
  unsigned lower_bound (const T &, bool (*) (const T &, const T &)) const;
  bool contains (const T &search) const;
  static size_t embedded_size (unsigned);
  void embedded_init (unsigned, unsigned = 0, unsigned = 0);
  void quick_grow (unsigned len);
  void quick_grow_cleared (unsigned len);

  /* vec class can access our internal data and functions.  */
  template <typename, typename, typename> friend struct vec;

  /* The allocator types also need access to our internals.  */
  friend struct va_gc;
  friend struct va_gc_atomic;
  friend struct va_heap;

  /* FIXME - This field should be private, but we need to cater to
	     compilers that have stricter notions of PODness for types.  */
  /* Align m_vecpfx to simplify address ().  */
  alignas (T) alignas (vec_prefix) vec_prefix m_vecpfx;
};


/* Convenience wrapper functions to use when dealing with pointers to
   embedded vectors.  Some functionality for these vectors must be
   provided via free functions for these reasons:

	1- The pointer may be NULL (e.g., before initial allocation).

  	2- When the vector needs to grow, it must be reallocated, so
  	   the pointer will change its value.

   Because of limitations with the current GC machinery, all vectors
   in GC memory *must* be pointers.  */


/* If V contains no room for NELEMS elements, return false. Otherwise,
   return true.  */
template<typename T, typename A>
inline bool
vec_safe_space (const vec<T, A, vl_embed> *v, unsigned nelems)
{
  return v ? v->space (nelems) : nelems == 0;
}


/* If V is NULL, return 0.  Otherwise, return V->length().  */
template<typename T, typename A>
inline unsigned
vec_safe_length (const vec<T, A, vl_embed> *v)
{
  return v ? v->length () : 0;
}


/* If V is NULL, return NULL.  Otherwise, return V->address().  */
template<typename T, typename A>
inline T *
vec_safe_address (vec<T, A, vl_embed> *v)
{
  return v ? v->address () : NULL;
}


/* If V is NULL, return true.  Otherwise, return V->is_empty().  */
template<typename T, typename A>
inline bool
vec_safe_is_empty (vec<T, A, vl_embed> *v)
{
  return v ? v->is_empty () : true;
}

/* If V does not have space for NELEMS elements, call
   V->reserve(NELEMS, EXACT).  */
template<typename T, typename A>
inline bool
vec_safe_reserve (vec<T, A, vl_embed> *&v, unsigned nelems, bool exact = false
		  CXX_MEM_STAT_INFO)
{
  bool extend = nelems ? !vec_safe_space (v, nelems) : false;
  if (extend)
    A::reserve (v, nelems, exact PASS_MEM_STAT);
  return extend;
}

template<typename T, typename A>
inline bool
vec_safe_reserve_exact (vec<T, A, vl_embed> *&v, unsigned nelems
			CXX_MEM_STAT_INFO)
{
  return vec_safe_reserve (v, nelems, true PASS_MEM_STAT);
}


/* Allocate GC memory for V with space for NELEMS slots.  If NELEMS
   is 0, V is initialized to NULL.  */

template<typename T, typename A>
inline void
vec_alloc (vec<T, A, vl_embed> *&v, unsigned nelems CXX_MEM_STAT_INFO)
{
  v = NULL;
  vec_safe_reserve (v, nelems, false PASS_MEM_STAT);
}


/* Free the GC memory allocated by vector V and set it to NULL.  */

template<typename T, typename A>
inline void
vec_free (vec<T, A, vl_embed> *&v)
{
  A::release (v);
}


/* Grow V to length LEN.  Allocate it, if necessary.  */
template<typename T, typename A>
inline void
vec_safe_grow (vec<T, A, vl_embed> *&v, unsigned len,
	       bool exact = false CXX_MEM_STAT_INFO)
{
  unsigned oldlen = vec_safe_length (v);
  gcc_checking_assert (len >= oldlen);
  vec_safe_reserve (v, len - oldlen, exact PASS_MEM_STAT);
  v->quick_grow (len);
}


/* If V is NULL, allocate it.  Call V->safe_grow_cleared(LEN).  */
template<typename T, typename A>
inline void
vec_safe_grow_cleared (vec<T, A, vl_embed> *&v, unsigned len,
		       bool exact = false CXX_MEM_STAT_INFO)
{
  unsigned oldlen = vec_safe_length (v);
  gcc_checking_assert (len >= oldlen);
  vec_safe_reserve (v, len - oldlen, exact PASS_MEM_STAT);
  v->quick_grow_cleared (len);
}


/* Assume V is not NULL.  */

template<typename T>
inline void
vec_safe_grow_cleared (vec<T, va_heap, vl_ptr> *&v,
		       unsigned len, bool exact = false CXX_MEM_STAT_INFO)
{
  v->safe_grow_cleared (len, exact PASS_MEM_STAT);
}

/* If V does not have space for NELEMS elements, call
   V->reserve(NELEMS, EXACT).  */

template<typename T>
inline bool
vec_safe_reserve (vec<T, va_heap, vl_ptr> *&v, unsigned nelems, bool exact = false
		  CXX_MEM_STAT_INFO)
{
  return v->reserve (nelems, exact);
}


/* If V is NULL return false, otherwise return V->iterate(IX, PTR).  */
template<typename T, typename A>
inline bool
vec_safe_iterate (const vec<T, A, vl_embed> *v, unsigned ix, T **ptr)
{
  if (v)
    return v->iterate (ix, ptr);
  else
    {
      *ptr = 0;
      return false;
    }
}

template<typename T, typename A>
inline bool
vec_safe_iterate (const vec<T, A, vl_embed> *v, unsigned ix, T *ptr)
{
  if (v)
    return v->iterate (ix, ptr);
  else
    {
      *ptr = 0;
      return false;
    }
}


/* If V has no room for one more element, reallocate it.  Then call
   V->quick_push(OBJ).  */
template<typename T, typename A>
inline T *
vec_safe_push (vec<T, A, vl_embed> *&v, const T &obj CXX_MEM_STAT_INFO)
{
  vec_safe_reserve (v, 1, false PASS_MEM_STAT);
  return v->quick_push (obj);
}


/* if V has no room for one more element, reallocate it.  Then call
   V->quick_insert(IX, OBJ).  */
template<typename T, typename A>
inline void
vec_safe_insert (vec<T, A, vl_embed> *&v, unsigned ix, const T &obj
		 CXX_MEM_STAT_INFO)
{
  vec_safe_reserve (v, 1, false PASS_MEM_STAT);
  v->quick_insert (ix, obj);
}


/* If V is NULL, do nothing.  Otherwise, call V->truncate(SIZE).  */
template<typename T, typename A>
inline void
vec_safe_truncate (vec<T, A, vl_embed> *v, unsigned size)
{
  if (v)
    v->truncate (size);
}


/* If SRC is not NULL, return a pointer to a copy of it.  */
template<typename T, typename A>
inline vec<T, A, vl_embed> *
vec_safe_copy (vec<T, A, vl_embed> *src CXX_MEM_STAT_INFO)
{
  return src ? src->copy (ALONE_PASS_MEM_STAT) : NULL;
}

/* Copy the elements from SRC to the end of DST as if by memcpy.
   Reallocate DST, if necessary.  */
template<typename T, typename A>
inline void
vec_safe_splice (vec<T, A, vl_embed> *&dst, const vec<T, A, vl_embed> *src
		 CXX_MEM_STAT_INFO)
{
  unsigned src_len = vec_safe_length (src);
  if (src_len)
    {
      vec_safe_reserve_exact (dst, vec_safe_length (dst) + src_len
			      PASS_MEM_STAT);
      dst->splice (*src);
    }
}

/* Return true if SEARCH is an element of V.  Note that this is O(N) in the
   size of the vector and so should be used with care.  */

template<typename T, typename A>
inline bool
vec_safe_contains (vec<T, A, vl_embed> *v, const T &search)
{
  return v ? v->contains (search) : false;
}

/* Index into vector.  Return the IX'th element.  IX must be in the
   domain of the vector.  */

template<typename T, typename A>
inline const T &
vec<T, A, vl_embed>::operator[] (unsigned ix) const
{
  gcc_checking_assert (ix < m_vecpfx.m_num);
  return address ()[ix];
}

template<typename T, typename A>
inline T &
vec<T, A, vl_embed>::operator[] (unsigned ix)
{
  gcc_checking_assert (ix < m_vecpfx.m_num);
  return address ()[ix];
}


/* Get the final element of the vector, which must not be empty.  */

template<typename T, typename A>
inline const T &
vec<T, A, vl_embed>::last (void) const
{
  gcc_checking_assert (m_vecpfx.m_num > 0);
  return (*this)[m_vecpfx.m_num - 1];
}

template<typename T, typename A>
inline T &
vec<T, A, vl_embed>::last (void)
{
  gcc_checking_assert (m_vecpfx.m_num > 0);
  return (*this)[m_vecpfx.m_num - 1];
}


/* If this vector has space for NELEMS additional entries, return
   true.  You usually only need to use this if you are doing your
   own vector reallocation, for instance on an embedded vector.  This
   returns true in exactly the same circumstances that vec::reserve
   will.  */

template<typename T, typename A>
inline bool
vec<T, A, vl_embed>::space (unsigned nelems) const
{
  return m_vecpfx.m_alloc - m_vecpfx.m_num >= nelems;
}


/* Return iteration condition and update *PTR to (a copy of) the IX'th
   element of this vector.  Use this to iterate over the elements of a
   vector as follows,

     for (ix = 0; v->iterate (ix, &val); ix++)
       continue;  */

template<typename T, typename A>
inline bool
vec<T, A, vl_embed>::iterate (unsigned ix, T *ptr) const
{
  if (ix < m_vecpfx.m_num)
    {
      *ptr = address ()[ix];
      return true;
    }
  else
    {
      *ptr = 0;
      return false;
    }
}


/* Return iteration condition and update *PTR to point to the
   IX'th element of this vector.  Use this to iterate over the
   elements of a vector as follows,

     for (ix = 0; v->iterate (ix, &ptr); ix++)
       continue;

   This variant is for vectors of objects.  */

template<typename T, typename A>
inline bool
vec<T, A, vl_embed>::iterate (unsigned ix, T **ptr) const
{
  if (ix < m_vecpfx.m_num)
    {
      *ptr = CONST_CAST (T *, &address ()[ix]);
      return true;
    }
  else
    {
      *ptr = 0;
      return false;
    }
}


/* Return a pointer to a copy of this vector.  */

template<typename T, typename A>
inline vec<T, A, vl_embed> *
vec<T, A, vl_embed>::copy (ALONE_MEM_STAT_DECL) const
{
  vec<T, A, vl_embed> *new_vec = NULL;
  unsigned len = length ();
  if (len)
    {
      vec_alloc (new_vec, len PASS_MEM_STAT);
      new_vec->embedded_init (len, len);
      vec_copy_construct (new_vec->address (), address (), len);
    }
  return new_vec;
}


/* Copy the elements from SRC to the end of this vector as if by memcpy.
   The vector must have sufficient headroom available.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::splice (const vec<T, A, vl_embed> &src)
{
  unsigned len = src.length ();
  if (len)
    {
      gcc_checking_assert (space (len));
      vec_copy_construct (end (), src.address (), len);
      m_vecpfx.m_num += len;
    }
}

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::splice (const vec<T, A, vl_embed> *src)
{
  if (src)
    splice (*src);
}


/* Push OBJ (a new element) onto the end of the vector.  There must be
   sufficient space in the vector.  Return a pointer to the slot
   where OBJ was inserted.  */

template<typename T, typename A>
inline T *
vec<T, A, vl_embed>::quick_push (const T &obj)
{
  gcc_checking_assert (space (1));
  T *slot = &address ()[m_vecpfx.m_num++];
  ::new (static_cast<void*>(slot)) T (obj);
  return slot;
}


/* Pop and return a reference to the last element off the end of the
   vector.  If T has non-trivial destructor, this method just pops
   the element and returns void type.  */

template<typename T, typename A>
inline typename vec<T, A, vl_embed>::pop_ret_type
vec<T, A, vl_embed>::pop (void)
{
  gcc_checking_assert (length () > 0);
  T &last = address ()[--m_vecpfx.m_num];
  if (!std::is_trivially_destructible <T>::value)
    last.~T ();
  return static_cast <pop_ret_type> (last);
}


/* Set the length of the vector to SIZE.  The new length must be less
   than or equal to the current length.  This is an O(1) operation.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::truncate (unsigned size)
{
  unsigned l = length ();
  gcc_checking_assert (l >= size);
  if (!std::is_trivially_destructible <T>::value)
    vec_destruct (address () + size, l - size);
  m_vecpfx.m_num = size;
}


/* Insert an element, OBJ, at the IXth position of this vector.  There
   must be sufficient space.  This operation is not suitable for non-trivially
   copyable types.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::quick_insert (unsigned ix, const T &obj)
{
  gcc_checking_assert (length () < allocated ());
  gcc_checking_assert (ix <= length ());
#if GCC_VERSION >= 5000
  /* GCC 4.8 and 4.9 only implement std::is_trivially_destructible,
     but not std::is_trivially_copyable nor
     std::is_trivially_default_constructible.  */
  static_assert (std::is_trivially_copyable <T>::value, "");
#endif
  T *slot = &address ()[ix];
  memmove (slot + 1, slot, (m_vecpfx.m_num++ - ix) * sizeof (T));
  *slot = obj;
}


/* Remove an element from the IXth position of this vector.  Ordering of
   remaining elements is preserved.  This is an O(N) operation due to
   memmove.  Not suitable for non-trivially copyable types.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::ordered_remove (unsigned ix)
{
  gcc_checking_assert (ix < length ());
#if GCC_VERSION >= 5000
  static_assert (std::is_trivially_copyable <T>::value, "");
#endif
  T *slot = &address ()[ix];
  memmove (slot, slot + 1, (--m_vecpfx.m_num - ix) * sizeof (T));
}


/* Remove elements in [START, END) from VEC for which COND holds.  Ordering of
   remaining elements is preserved.  This is an O(N) operation.  */

#define VEC_ORDERED_REMOVE_IF_FROM_TO(vec, read_index, write_index,	\
				      elem_ptr, start, end, cond)	\
  {									\
    gcc_assert ((end) <= (vec).length ());				\
    for (read_index = write_index = (start); read_index < (end);	\
	 ++read_index)							\
      {									\
	elem_ptr = &(vec)[read_index];					\
	bool remove_p = (cond);						\
	if (remove_p)							\
	  continue;							\
									\
	if (read_index != write_index)					\
	  (vec)[write_index] = (vec)[read_index];			\
									\
	write_index++;							\
      }									\
									\
    if (read_index - write_index > 0)					\
      (vec).block_remove (write_index, read_index - write_index);	\
  }


/* Remove elements from VEC for which COND holds.  Ordering of remaining
   elements is preserved.  This is an O(N) operation.  */

#define VEC_ORDERED_REMOVE_IF(vec, read_index, write_index, elem_ptr,	\
			      cond)					\
  VEC_ORDERED_REMOVE_IF_FROM_TO ((vec), read_index, write_index,	\
				 elem_ptr, 0, (vec).length (), (cond))

/* Remove an element from the IXth position of this vector.  Ordering of
   remaining elements is destroyed.  This is an O(1) operation.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::unordered_remove (unsigned ix)
{
  gcc_checking_assert (ix < length ());
#if GCC_VERSION >= 5000
  static_assert (std::is_trivially_copyable <T>::value, "");
#endif
  T *p = address ();
  p[ix] = p[--m_vecpfx.m_num];
}


/* Remove LEN elements starting at the IXth.  Ordering is retained.
   This is an O(N) operation due to memmove.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::block_remove (unsigned ix, unsigned len)
{
  gcc_checking_assert (ix + len <= length ());
#if GCC_VERSION >= 5000
  static_assert (std::is_trivially_copyable <T>::value, "");
#endif
  T *slot = &address ()[ix];
  m_vecpfx.m_num -= len;
  memmove (slot, slot + len, (m_vecpfx.m_num - ix) * sizeof (T));
}


#if GCC_VERSION >= 5000
namespace vec_detail
{
  /* gcc_{qsort,qsort_r,stablesort_r} implementation under the hood
     uses memcpy/memmove to reorder the array elements.
     We want to assert these methods aren't used on types for which
     that isn't appropriate, but unfortunately std::pair of 2 trivially
     copyable types isn't trivially copyable and we use qsort on many
     such std::pair instantiations.  Let's allow both trivially
     copyable types and std::pair of 2 trivially copyable types as
     exception for qsort/sort/stablesort.  */
  template<typename T>
  struct is_trivially_copyable_or_pair : std::is_trivially_copyable<T> { };

  template<typename T, typename U>
  struct is_trivially_copyable_or_pair<std::pair<T, U> >
  : std::integral_constant<bool, std::is_trivially_copyable<T>::value
    && std::is_trivially_copyable<U>::value> { };
}
#endif

/* Sort the contents of this vector with qsort.  CMP is the comparison
   function to pass to qsort.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::qsort (int (*cmp) (const void *, const void *))
{
#if GCC_VERSION >= 5000
  static_assert (vec_detail::is_trivially_copyable_or_pair <T>::value, "");
#endif
  if (length () > 1)
    gcc_qsort (address (), length (), sizeof (T), cmp);
}

/* Sort the contents of this vector with qsort.  CMP is the comparison
   function to pass to qsort.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::sort (int (*cmp) (const void *, const void *, void *),
			   void *data)
{
#if GCC_VERSION >= 5000
  static_assert (vec_detail::is_trivially_copyable_or_pair <T>::value, "");
#endif
  if (length () > 1)
    gcc_sort_r (address (), length (), sizeof (T), cmp, data);
}

/* Sort the contents of this vector with gcc_stablesort_r.  CMP is the
   comparison function to pass to qsort.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::stablesort (int (*cmp) (const void *, const void *,
					     void *), void *data)
{
#if GCC_VERSION >= 5000
  static_assert (vec_detail::is_trivially_copyable_or_pair <T>::value, "");
#endif
  if (length () > 1)
    gcc_stablesort_r (address (), length (), sizeof (T), cmp, data);
}

/* Search the contents of the sorted vector with a binary search.
   CMP is the comparison function to pass to bsearch.  */

template<typename T, typename A>
inline T *
vec<T, A, vl_embed>::bsearch (const void *key,
			      int (*compar) (const void *, const void *))
{
  const void *base = this->address ();
  size_t nmemb = this->length ();
  size_t size = sizeof (T);
  /* The following is a copy of glibc stdlib-bsearch.h.  */
  size_t l, u, idx;
  const void *p;
  int comparison;

  l = 0;
  u = nmemb;
  while (l < u)
    {
      idx = (l + u) / 2;
      p = (const void *) (((const char *) base) + (idx * size));
      comparison = (*compar) (key, p);
      if (comparison < 0)
	u = idx;
      else if (comparison > 0)
	l = idx + 1;
      else
	return (T *)const_cast<void *>(p);
    }

  return NULL;
}

/* Search the contents of the sorted vector with a binary search.
   CMP is the comparison function to pass to bsearch.  */

template<typename T, typename A>
inline T *
vec<T, A, vl_embed>::bsearch (const void *key,
			      int (*compar) (const void *, const void *,
					     void *), void *data)
{
  const void *base = this->address ();
  size_t nmemb = this->length ();
  size_t size = sizeof (T);
  /* The following is a copy of glibc stdlib-bsearch.h.  */
  size_t l, u, idx;
  const void *p;
  int comparison;

  l = 0;
  u = nmemb;
  while (l < u)
    {
      idx = (l + u) / 2;
      p = (const void *) (((const char *) base) + (idx * size));
      comparison = (*compar) (key, p, data);
      if (comparison < 0)
	u = idx;
      else if (comparison > 0)
	l = idx + 1;
      else
	return (T *)const_cast<void *>(p);
    }

  return NULL;
}

/* Return true if SEARCH is an element of V.  Note that this is O(N) in the
   size of the vector and so should be used with care.  */

template<typename T, typename A>
inline bool
vec<T, A, vl_embed>::contains (const T &search) const
{
  unsigned int len = length ();
  const T *p = address ();
  for (unsigned int i = 0; i < len; i++)
    {
      const T *slot = &p[i];
      if (*slot == search)
	return true;
    }

  return false;
}

/* Find and return the first position in which OBJ could be inserted
   without changing the ordering of this vector.  LESSTHAN is a
   function that returns true if the first argument is strictly less
   than the second.  */

template<typename T, typename A>
unsigned
vec<T, A, vl_embed>::lower_bound (const T &obj,
				  bool (*lessthan)(const T &, const T &))
  const
{
  unsigned int len = length ();
  unsigned int half, middle;
  unsigned int first = 0;
  while (len > 0)
    {
      half = len / 2;
      middle = first;
      middle += half;
      const T &middle_elem = address ()[middle];
      if (lessthan (middle_elem, obj))
	{
	  first = middle;
	  ++first;
	  len = len - half - 1;
	}
      else
	len = half;
    }
  return first;
}


/* Return the number of bytes needed to embed an instance of an
   embeddable vec inside another data structure.

   Use these methods to determine the required size and initialization
   of a vector V of type T embedded within another structure (as the
   final member):

   size_t vec<T, A, vl_embed>::embedded_size (unsigned alloc);
   void v->embedded_init (unsigned alloc, unsigned num);

   These allow the caller to perform the memory allocation.  */

template<typename T, typename A>
inline size_t
vec<T, A, vl_embed>::embedded_size (unsigned alloc)
{
  struct alignas (T) U { char data[sizeof (T)]; };
  typedef vec<U, A, vl_embed> vec_embedded;
  typedef typename std::conditional<std::is_standard_layout<T>::value,
				    vec, vec_embedded>::type vec_stdlayout;
  static_assert (sizeof (vec_stdlayout) == sizeof (vec), "");
  static_assert (alignof (vec_stdlayout) == alignof (vec), "");
  return sizeof (vec_stdlayout) + alloc * sizeof (T);
}


/* Initialize the vector to contain room for ALLOC elements and
   NUM active elements.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::embedded_init (unsigned alloc, unsigned num, unsigned aut)
{
  m_vecpfx.m_alloc = alloc;
  m_vecpfx.m_using_auto_storage = aut;
  m_vecpfx.m_num = num;
}


/* Grow the vector to a specific length.  LEN must be as long or longer than
   the current length.  The new elements are uninitialized.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::quick_grow (unsigned len)
{
  gcc_checking_assert (length () <= len && len <= m_vecpfx.m_alloc);
#if GCC_VERSION >= 5000
  static_assert (std::is_trivially_default_constructible <T>::value, "");
#endif
  m_vecpfx.m_num = len;
}


/* Grow the vector to a specific length.  LEN must be as long or longer than
   the current length.  The new elements are initialized to zero.  */

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::quick_grow_cleared (unsigned len)
{
  unsigned oldlen = length ();
  size_t growby = len - oldlen;
  gcc_checking_assert (length () <= len && len <= m_vecpfx.m_alloc);
  m_vecpfx.m_num = len;
  if (growby != 0)
    vec_default_construct (address () + oldlen, growby);
}

/* Garbage collection support for vec<T, A, vl_embed>.  */

template<typename T>
void
gt_ggc_mx (vec<T, va_gc> *v)
{
  static_assert (std::is_trivially_destructible <T>::value, "");
  extern void gt_ggc_mx (T &);
  for (unsigned i = 0; i < v->length (); i++)
    gt_ggc_mx ((*v)[i]);
}

template<typename T>
void
gt_ggc_mx (vec<T, va_gc_atomic, vl_embed> *v ATTRIBUTE_UNUSED)
{
  static_assert (std::is_trivially_destructible <T>::value, "");
  /* Nothing to do.  Vectors of atomic types wrt GC do not need to
     be traversed.  */
}


/* PCH support for vec<T, A, vl_embed>.  */

template<typename T, typename A>
void
gt_pch_nx (vec<T, A, vl_embed> *v)
{
  extern void gt_pch_nx (T &);
  for (unsigned i = 0; i < v->length (); i++)
    gt_pch_nx ((*v)[i]);
}

template<typename T>
void
gt_pch_nx (vec<T, va_gc_atomic, vl_embed> *)
{
  /* No pointers to note.  */
}

template<typename T, typename A>
void
gt_pch_nx (vec<T *, A, vl_embed> *v, gt_pointer_operator op, void *cookie)
{
  for (unsigned i = 0; i < v->length (); i++)
    op (&((*v)[i]), NULL, cookie);
}

template<typename T, typename A>
void
gt_pch_nx (vec<T, A, vl_embed> *v, gt_pointer_operator op, void *cookie)
{
  extern void gt_pch_nx (T *, gt_pointer_operator, void *);
  for (unsigned i = 0; i < v->length (); i++)
    gt_pch_nx (&((*v)[i]), op, cookie);
}

template<typename T>
void
gt_pch_nx (vec<T, va_gc_atomic, vl_embed> *, gt_pointer_operator, void *)
{
  /* No pointers to note.  */
}


/* Space efficient vector.  These vectors can grow dynamically and are
   allocated together with their control data.  They are suited to be
   included in data structures.  Prior to initial allocation, they
   only take a single word of storage.

   These vectors are implemented as a pointer to an embeddable vector.
   The semantics allow for this pointer to be NULL to represent empty
   vectors.  This way, empty vectors occupy minimal space in the
   structure containing them.

   Properties:

	- The whole vector and control data are allocated in a single
	  contiguous block.
  	- The whole vector may be re-allocated.
  	- Vector data may grow and shrink.
  	- Access and manipulation requires a pointer test and
	  indirection.
	- It requires 1 word of storage (prior to vector allocation).


   Limitations:

   These vectors must be PODs because they are stored in unions.
   (http://en.wikipedia.org/wiki/Plain_old_data_structures).
   As long as we use C++03, we cannot have constructors nor
   destructors in classes that are stored in unions.  */

template<typename T, size_t N = 0>
class auto_vec;

template<typename T>
struct vec<T, va_heap, vl_ptr>
{
public:
  /* Default ctors to ensure triviality.  Use value-initialization
     (e.g., vec() or vec v{ };) or vNULL to create a zero-initialized
     instance.  */
  vec () = default;
  vec (const vec &) = default;
  /* Initialization from the generic vNULL.  */
  vec (vnull): m_vec () { }
  /* Same as default ctor: vec storage must be released manually.  */
  ~vec () = default;

  /* Defaulted same as copy ctor.  */
  vec& operator= (const vec &) = default;

  /* Prevent implicit conversion from auto_vec.  Use auto_vec::to_vec()
     instead.  */
  template <size_t N>
  vec (auto_vec<T, N> &) = delete;

  template <size_t N>
  void operator= (auto_vec<T, N> &) = delete;

  /* Memory allocation and deallocation for the embedded vector.
     Needed because we cannot have proper ctors/dtors defined.  */
  void create (unsigned nelems CXX_MEM_STAT_INFO);
  void release (void);

  /* Vector operations.  */
  bool exists (void) const
  { return m_vec != NULL; }

  bool is_empty (void) const
  { return m_vec ? m_vec->is_empty () : true; }

  unsigned allocated (void) const
  { return m_vec ? m_vec->allocated () : 0; }

  unsigned length (void) const
  { return m_vec ? m_vec->length () : 0; }

  T *address (void)
  { return m_vec ? m_vec->address () : NULL; }

  const T *address (void) const
  { return m_vec ? m_vec->address () : NULL; }

  T *begin () { return address (); }
  const T *begin () const { return address (); }
  T *end () { return begin () + length (); }
  const T *end () const { return begin () + length (); }
  const T &operator[] (unsigned ix) const
  { return (*m_vec)[ix]; }
  const T &last (void) const
  { return m_vec->last (); }

  bool operator!=(const vec &other) const
  { return !(*this == other); }

  bool operator==(const vec &other) const
  { return address () == other.address (); }

  T &operator[] (unsigned ix)
  { return (*m_vec)[ix]; }

  T &last (void)
  { return m_vec->last (); }

  bool space (int nelems) const
  { return m_vec ? m_vec->space (nelems) : nelems == 0; }

  bool iterate (unsigned ix, T *p) const;
  bool iterate (unsigned ix, T **p) const;
  vec copy (ALONE_CXX_MEM_STAT_INFO) const;
  bool reserve (unsigned, bool = false CXX_MEM_STAT_INFO);
  bool reserve_exact (unsigned CXX_MEM_STAT_INFO);
  void splice (const vec &);
  void safe_splice (const vec & CXX_MEM_STAT_INFO);
  T *quick_push (const T &);
  T *safe_push (const T &CXX_MEM_STAT_INFO);
  using pop_ret_type
    = typename std::conditional <std::is_trivially_destructible <T>::value,
				 T &, void>::type;
  pop_ret_type pop (void);
  void truncate (unsigned);
  void safe_grow (unsigned, bool = false CXX_MEM_STAT_INFO);
  void safe_grow_cleared (unsigned, bool = false CXX_MEM_STAT_INFO);
  void quick_grow (unsigned);
  void quick_grow_cleared (unsigned);
  void quick_insert (unsigned, const T &);
  void safe_insert (unsigned, const T & CXX_MEM_STAT_INFO);
  void ordered_remove (unsigned);
  void unordered_remove (unsigned);
  void block_remove (unsigned, unsigned);
  void qsort (int (*) (const void *, const void *));
  void sort (int (*) (const void *, const void *, void *), void *);
  void stablesort (int (*) (const void *, const void *, void *), void *);
  T *bsearch (const void *key, int (*compar)(const void *, const void *));
  T *bsearch (const void *key,
	      int (*compar)(const void *, const void *, void *), void *);
  unsigned lower_bound (T, bool (*)(const T &, const T &)) const;
  bool contains (const T &search) const;
  void reverse (void);

  bool using_auto_storage () const;

  /* FIXME - This field should be private, but we need to cater to
	     compilers that have stricter notions of PODness for types.  */
  vec<T, va_heap, vl_embed> *m_vec;
};


/* auto_vec is a subclass of vec that automatically manages creating and
   releasing the internal vector. If N is non zero then it has N elements of
   internal storage.  The default is no internal storage, and you probably only
   want to ask for internal storage for vectors on the stack because if the
   size of the vector is larger than the internal storage that space is wasted.
   */
template<typename T, size_t N /* = 0 */>
class auto_vec : public vec<T, va_heap>
{
public:
  auto_vec ()
  {
    m_auto.embedded_init (N, 0, 1);
    /* ???  Instead of initializing m_vec from &m_auto directly use an
       expression that avoids refering to a specific member of 'this'
       to derail the -Wstringop-overflow diagnostic code, avoiding
       the impression that data accesses are supposed to be to the
       m_auto member storage.  */
    size_t off = (char *) &m_auto - (char *) this;
    this->m_vec = (vec<T, va_heap, vl_embed> *) ((char *) this + off);
  }

  auto_vec (size_t s CXX_MEM_STAT_INFO)
  {
    if (s > N)
      {
	this->create (s PASS_MEM_STAT);
	return;
      }

    m_auto.embedded_init (N, 0, 1);
    /* ???  See above.  */
    size_t off = (char *) &m_auto - (char *) this;
    this->m_vec = (vec<T, va_heap, vl_embed> *) ((char *) this + off);
  }

  ~auto_vec ()
  {
    this->release ();
  }

  /* Explicitly convert to the base class.  There is no conversion
     from a const auto_vec because a copy of the returned vec can
     be used to modify *THIS.
     This is a legacy function not to be used in new code.  */
  vec<T, va_heap> to_vec_legacy () {
    return *static_cast<vec<T, va_heap> *>(this);
  }

private:
  vec<T, va_heap, vl_embed> m_auto;
  unsigned char m_data[sizeof (T) * N];
};

/* auto_vec is a sub class of vec whose storage is released when it is
  destroyed. */
template<typename T>
class auto_vec<T, 0> : public vec<T, va_heap>
{
public:
  auto_vec () { this->m_vec = NULL; }
  auto_vec (size_t n CXX_MEM_STAT_INFO) { this->create (n PASS_MEM_STAT); }
  ~auto_vec () { this->release (); }

  auto_vec (vec<T, va_heap>&& r)
    {
      gcc_assert (!r.using_auto_storage ());
      this->m_vec = r.m_vec;
      r.m_vec = NULL;
    }

  auto_vec (auto_vec<T> &&r)
    {
      gcc_assert (!r.using_auto_storage ());
      this->m_vec = r.m_vec;
      r.m_vec = NULL;
    }

  auto_vec& operator= (vec<T, va_heap>&& r)
    {
      if (this == &r)
	return *this;

      gcc_assert (!r.using_auto_storage ());
      this->release ();
      this->m_vec = r.m_vec;
      r.m_vec = NULL;
      return *this;
    }

  auto_vec& operator= (auto_vec<T> &&r)
    {
      if (this == &r)
	return *this;

      gcc_assert (!r.using_auto_storage ());
      this->release ();
      this->m_vec = r.m_vec;
      r.m_vec = NULL;
      return *this;
    }

  /* Explicitly convert to the base class.  There is no conversion
     from a const auto_vec because a copy of the returned vec can
     be used to modify *THIS.
     This is a legacy function not to be used in new code.  */
  vec<T, va_heap> to_vec_legacy () {
    return *static_cast<vec<T, va_heap> *>(this);
  }

  // You probably don't want to copy a vector, so these are deleted to prevent
  // unintentional use.  If you really need a copy of the vectors contents you
  // can use copy ().
  auto_vec (const auto_vec &) = delete;
  auto_vec &operator= (const auto_vec &) = delete;
};


/* Allocate heap memory for pointer V and create the internal vector
   with space for NELEMS elements.  If NELEMS is 0, the internal
   vector is initialized to empty.  */

template<typename T>
inline void
vec_alloc (vec<T> *&v, unsigned nelems CXX_MEM_STAT_INFO)
{
  v = new vec<T>;
  v->create (nelems PASS_MEM_STAT);
}


/* A subclass of auto_vec <char *> that frees all of its elements on
   deletion.  */

class auto_string_vec : public auto_vec <char *>
{
 public:
  ~auto_string_vec ();
};

/* A subclass of auto_vec <T *> that deletes all of its elements on
   destruction.

   This is a crude way for a vec to "own" the objects it points to
   and clean up automatically.

   For example, no attempt is made to delete elements when an item
   within the vec is overwritten.

   We can't rely on gnu::unique_ptr within a container,
   since we can't rely on move semantics in C++98.  */

template <typename T>
class auto_delete_vec : public auto_vec <T *>
{
 public:
  auto_delete_vec () {}
  auto_delete_vec (size_t s) : auto_vec <T *> (s) {}

  ~auto_delete_vec ();

private:
  DISABLE_COPY_AND_ASSIGN(auto_delete_vec);
};

/* Conditionally allocate heap memory for VEC and its internal vector.  */

template<typename T>
inline void
vec_check_alloc (vec<T, va_heap> *&vec, unsigned nelems CXX_MEM_STAT_INFO)
{
  if (!vec)
    vec_alloc (vec, nelems PASS_MEM_STAT);
}


/* Free the heap memory allocated by vector V and set it to NULL.  */

template<typename T>
inline void
vec_free (vec<T> *&v)
{
  if (v == NULL)
    return;

  v->release ();
  delete v;
  v = NULL;
}


/* Return iteration condition and update PTR to point to the IX'th
   element of this vector.  Use this to iterate over the elements of a
   vector as follows,

     for (ix = 0; v.iterate (ix, &ptr); ix++)
       continue;  */

template<typename T>
inline bool
vec<T, va_heap, vl_ptr>::iterate (unsigned ix, T *ptr) const
{
  if (m_vec)
    return m_vec->iterate (ix, ptr);
  else
    {
      *ptr = 0;
      return false;
    }
}


/* Return iteration condition and update *PTR to point to the
   IX'th element of this vector.  Use this to iterate over the
   elements of a vector as follows,

     for (ix = 0; v->iterate (ix, &ptr); ix++)
       continue;

   This variant is for vectors of objects.  */

template<typename T>
inline bool
vec<T, va_heap, vl_ptr>::iterate (unsigned ix, T **ptr) const
{
  if (m_vec)
    return m_vec->iterate (ix, ptr);
  else
    {
      *ptr = 0;
      return false;
    }
}


/* Convenience macro for forward iteration.  */
#define FOR_EACH_VEC_ELT(V, I, P)			\
  for (I = 0; (V).iterate ((I), &(P)); ++(I))

#define FOR_EACH_VEC_SAFE_ELT(V, I, P)			\
  for (I = 0; vec_safe_iterate ((V), (I), &(P)); ++(I))

/* Likewise, but start from FROM rather than 0.  */
#define FOR_EACH_VEC_ELT_FROM(V, I, P, FROM)		\
  for (I = (FROM); (V).iterate ((I), &(P)); ++(I))

/* Convenience macro for reverse iteration.  */
#define FOR_EACH_VEC_ELT_REVERSE(V, I, P)		\
  for (I = (V).length () - 1;				\
       (V).iterate ((I), &(P));				\
       (I)--)

#define FOR_EACH_VEC_SAFE_ELT_REVERSE(V, I, P)		\
  for (I = vec_safe_length (V) - 1;			\
       vec_safe_iterate ((V), (I), &(P));	\
       (I)--)

/* auto_string_vec's dtor, freeing all contained strings, automatically
   chaining up to ~auto_vec <char *>, which frees the internal buffer.  */

inline
auto_string_vec::~auto_string_vec ()
{
  int i;
  char *str;
  FOR_EACH_VEC_ELT (*this, i, str)
    free (str);
}

/* auto_delete_vec's dtor, deleting all contained items, automatically
   chaining up to ~auto_vec <T*>, which frees the internal buffer.  */

template <typename T>
inline
auto_delete_vec<T>::~auto_delete_vec ()
{
  int i;
  T *item;
  FOR_EACH_VEC_ELT (*this, i, item)
    delete item;
}


/* Return a copy of this vector.  */

template<typename T>
inline vec<T, va_heap, vl_ptr>
vec<T, va_heap, vl_ptr>::copy (ALONE_MEM_STAT_DECL) const
{
  vec<T, va_heap, vl_ptr> new_vec{ };
  if (length ())
    new_vec.m_vec = m_vec->copy (ALONE_PASS_MEM_STAT);
  return new_vec;
}


/* Ensure that the vector has at least RESERVE slots available (if
   EXACT is false), or exactly RESERVE slots available (if EXACT is
   true).

   This may create additional headroom if EXACT is false.

   Note that this can cause the embedded vector to be reallocated.
   Returns true iff reallocation actually occurred.  */

template<typename T>
inline bool
vec<T, va_heap, vl_ptr>::reserve (unsigned nelems, bool exact MEM_STAT_DECL)
{
  if (space (nelems))
    return false;

  /* For now play a game with va_heap::reserve to hide our auto storage if any,
     this is necessary because it doesn't have enough information to know the
     embedded vector is in auto storage, and so should not be freed.  */
  vec<T, va_heap, vl_embed> *oldvec = m_vec;
  unsigned int oldsize = 0;
  bool handle_auto_vec = m_vec && using_auto_storage ();
  if (handle_auto_vec)
    {
      m_vec = NULL;
      oldsize = oldvec->length ();
      nelems += oldsize;
    }

  va_heap::reserve (m_vec, nelems, exact PASS_MEM_STAT);
  if (handle_auto_vec)
    {
      vec_copy_construct (m_vec->address (), oldvec->address (), oldsize);
      m_vec->m_vecpfx.m_num = oldsize;
    }

  return true;
}


/* Ensure that this vector has exactly NELEMS slots available.  This
   will not create additional headroom.  Note this can cause the
   embedded vector to be reallocated.  Returns true iff reallocation
   actually occurred.  */

template<typename T>
inline bool
vec<T, va_heap, vl_ptr>::reserve_exact (unsigned nelems MEM_STAT_DECL)
{
  return reserve (nelems, true PASS_MEM_STAT);
}


/* Create the internal vector and reserve NELEMS for it.  This is
   exactly like vec::reserve, but the internal vector is
   unconditionally allocated from scratch.  The old one, if it
   existed, is lost.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::create (unsigned nelems MEM_STAT_DECL)
{
  m_vec = NULL;
  if (nelems > 0)
    reserve_exact (nelems PASS_MEM_STAT);
}


/* Free the memory occupied by the embedded vector.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::release (void)
{
  if (!m_vec)
    return;

  if (using_auto_storage ())
    {
      m_vec->truncate (0);
      return;
    }

  va_heap::release (m_vec);
}

/* Copy the elements from SRC to the end of this vector as if by memcpy.
   SRC and this vector must be allocated with the same memory
   allocation mechanism. This vector is assumed to have sufficient
   headroom available.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::splice (const vec<T, va_heap, vl_ptr> &src)
{
  if (src.length ())
    m_vec->splice (*(src.m_vec));
}


/* Copy the elements in SRC to the end of this vector as if by memcpy.
   SRC and this vector must be allocated with the same mechanism.
   If there is not enough headroom in this vector, it will be reallocated
   as needed.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::safe_splice (const vec<T, va_heap, vl_ptr> &src
				      MEM_STAT_DECL)
{
  if (src.length ())
    {
      reserve_exact (src.length ());
      splice (src);
    }
}


/* Push OBJ (a new element) onto the end of the vector.  There must be
   sufficient space in the vector.  Return a pointer to the slot
   where OBJ was inserted.  */

template<typename T>
inline T *
vec<T, va_heap, vl_ptr>::quick_push (const T &obj)
{
  return m_vec->quick_push (obj);
}


/* Push a new element OBJ onto the end of this vector.  Reallocates
   the embedded vector, if needed.  Return a pointer to the slot where
   OBJ was inserted.  */

template<typename T>
inline T *
vec<T, va_heap, vl_ptr>::safe_push (const T &obj MEM_STAT_DECL)
{
  reserve (1, false PASS_MEM_STAT);
  return quick_push (obj);
}


/* Pop and return a reference to the last element off the end of the
   vector.  If T has non-trivial destructor, this method just pops
   last element and returns void.  */

template<typename T>
inline typename vec<T, va_heap, vl_ptr>::pop_ret_type
vec<T, va_heap, vl_ptr>::pop (void)
{
  return m_vec->pop ();
}


/* Set the length of the vector to LEN.  The new length must be less
   than or equal to the current length.  This is an O(1) operation.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::truncate (unsigned size)
{
  if (m_vec)
    m_vec->truncate (size);
  else
    gcc_checking_assert (size == 0);
}


/* Grow the vector to a specific length.  LEN must be as long or
   longer than the current length.  The new elements are
   uninitialized.  Reallocate the internal vector, if needed.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::safe_grow (unsigned len, bool exact MEM_STAT_DECL)
{
  unsigned oldlen = length ();
  gcc_checking_assert (oldlen <= len);
  reserve (len - oldlen, exact PASS_MEM_STAT);
  if (m_vec)
    m_vec->quick_grow (len);
  else
    gcc_checking_assert (len == 0);
}


/* Grow the embedded vector to a specific length.  LEN must be as
   long or longer than the current length.  The new elements are
   initialized to zero.  Reallocate the internal vector, if needed.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::safe_grow_cleared (unsigned len, bool exact
					    MEM_STAT_DECL)
{
  unsigned oldlen = length ();
  gcc_checking_assert (oldlen <= len);
  reserve (len - oldlen, exact PASS_MEM_STAT);
  if (m_vec)
    m_vec->quick_grow_cleared (len);
  else
    gcc_checking_assert (len == 0);
}


/* Same as vec::safe_grow but without reallocation of the internal vector.
   If the vector cannot be extended, a runtime assertion will be triggered.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::quick_grow (unsigned len)
{
  gcc_checking_assert (m_vec);
  m_vec->quick_grow (len);
}


/* Same as vec::quick_grow_cleared but without reallocation of the
   internal vector. If the vector cannot be extended, a runtime
   assertion will be triggered.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::quick_grow_cleared (unsigned len)
{
  gcc_checking_assert (m_vec);
  m_vec->quick_grow_cleared (len);
}


/* Insert an element, OBJ, at the IXth position of this vector.  There
   must be sufficient space.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::quick_insert (unsigned ix, const T &obj)
{
  m_vec->quick_insert (ix, obj);
}


/* Insert an element, OBJ, at the IXth position of the vector.
   Reallocate the embedded vector, if necessary.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::safe_insert (unsigned ix, const T &obj MEM_STAT_DECL)
{
  reserve (1, false PASS_MEM_STAT);
  quick_insert (ix, obj);
}


/* Remove an element from the IXth position of this vector.  Ordering of
   remaining elements is preserved.  This is an O(N) operation due to
   a memmove.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::ordered_remove (unsigned ix)
{
  m_vec->ordered_remove (ix);
}


/* Remove an element from the IXth position of this vector.  Ordering
   of remaining elements is destroyed.  This is an O(1) operation.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::unordered_remove (unsigned ix)
{
  m_vec->unordered_remove (ix);
}


/* Remove LEN elements starting at the IXth.  Ordering is retained.
   This is an O(N) operation due to memmove.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::block_remove (unsigned ix, unsigned len)
{
  m_vec->block_remove (ix, len);
}


/* Sort the contents of this vector with qsort.  CMP is the comparison
   function to pass to qsort.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::qsort (int (*cmp) (const void *, const void *))
{
  if (m_vec)
    m_vec->qsort (cmp);
}

/* Sort the contents of this vector with qsort.  CMP is the comparison
   function to pass to qsort.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::sort (int (*cmp) (const void *, const void *,
					   void *), void *data)
{
  if (m_vec)
    m_vec->sort (cmp, data);
}

/* Sort the contents of this vector with gcc_stablesort_r.  CMP is the
   comparison function to pass to qsort.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::stablesort (int (*cmp) (const void *, const void *,
						 void *), void *data)
{
  if (m_vec)
    m_vec->stablesort (cmp, data);
}

/* Search the contents of the sorted vector with a binary search.
   CMP is the comparison function to pass to bsearch.  */

template<typename T>
inline T *
vec<T, va_heap, vl_ptr>::bsearch (const void *key,
				  int (*cmp) (const void *, const void *))
{
  if (m_vec)
    return m_vec->bsearch (key, cmp);
  return NULL;
}

/* Search the contents of the sorted vector with a binary search.
   CMP is the comparison function to pass to bsearch.  */

template<typename T>
inline T *
vec<T, va_heap, vl_ptr>::bsearch (const void *key,
				  int (*cmp) (const void *, const void *,
					      void *), void *data)
{
  if (m_vec)
    return m_vec->bsearch (key, cmp, data);
  return NULL;
}


/* Find and return the first position in which OBJ could be inserted
   without changing the ordering of this vector.  LESSTHAN is a
   function that returns true if the first argument is strictly less
   than the second.  */

template<typename T>
inline unsigned
vec<T, va_heap, vl_ptr>::lower_bound (T obj,
				      bool (*lessthan)(const T &, const T &))
    const
{
  return m_vec ? m_vec->lower_bound (obj, lessthan) : 0;
}

/* Return true if SEARCH is an element of V.  Note that this is O(N) in the
   size of the vector and so should be used with care.  */

template<typename T>
inline bool
vec<T, va_heap, vl_ptr>::contains (const T &search) const
{
  return m_vec ? m_vec->contains (search) : false;
}

/* Reverse content of the vector.  */

template<typename T>
inline void
vec<T, va_heap, vl_ptr>::reverse (void)
{
  unsigned l = length ();
  T *ptr = address ();

  for (unsigned i = 0; i < l / 2; i++)
    std::swap (ptr[i], ptr[l - i - 1]);
}

template<typename T>
inline bool
vec<T, va_heap, vl_ptr>::using_auto_storage () const
{
  return m_vec ? m_vec->m_vecpfx.m_using_auto_storage : false;
}

/* Release VEC and call release of all element vectors.  */

template<typename T>
inline void
release_vec_vec (vec<vec<T> > &vec)
{
  for (unsigned i = 0; i < vec.length (); i++)
    vec[i].release ();

  vec.release ();
}

// Provide a subset of the std::span functionality.  (We can't use std::span
// itself because it's a C++20 feature.)
//
// In addition, provide an invalid value that is distinct from all valid
// sequences (including the empty sequence).  This can be used to return
// failure without having to use std::optional.
//
// There is no operator bool because it would be ambiguous whether it is
// testing for a valid value or an empty sequence.
template<typename T>
class array_slice
{
  template<typename OtherT> friend class array_slice;

public:
  using value_type = T;
  using iterator = T *;
  using const_iterator = const T *;

  array_slice () : m_base (nullptr), m_size (0) {}

  template<typename OtherT>
  array_slice (array_slice<OtherT> other)
    : m_base (other.m_base), m_size (other.m_size) {}

  array_slice (iterator base, unsigned int size)
    : m_base (base), m_size (size) {}

  template<size_t N>
  array_slice (T (&array)[N]) : m_base (array), m_size (N) {}

  template<typename OtherT>
  array_slice (const vec<OtherT> &v)
    : m_base (v.address ()), m_size (v.length ()) {}

  template<typename OtherT>
  array_slice (vec<OtherT> &v)
    : m_base (v.address ()), m_size (v.length ()) {}

  template<typename OtherT, typename A>
  array_slice (const vec<OtherT, A, vl_embed> *v)
    : m_base (v ? v->address () : nullptr), m_size (v ? v->length () : 0) {}

  template<typename OtherT, typename A>
  array_slice (vec<OtherT, A, vl_embed> *v)
    : m_base (v ? v->address () : nullptr), m_size (v ? v->length () : 0) {}

  iterator begin () {  gcc_checking_assert (is_valid ()); return m_base; }
  iterator end () {  gcc_checking_assert (is_valid ()); return m_base + m_size; }

  const_iterator begin () const { gcc_checking_assert (is_valid ()); return m_base; }
  const_iterator end () const { gcc_checking_assert (is_valid ()); return m_base + m_size; }

  value_type &front ();
  value_type &back ();
  value_type &operator[] (unsigned int i);

  const value_type &front () const;
  const value_type &back () const;
  const value_type &operator[] (unsigned int i) const;

  unsigned size () const { return m_size; }
  size_t size_bytes () const { return m_size * sizeof (T); }
  bool empty () const { return m_size == 0; }

  // An invalid array_slice that represents a failed operation.  This is
  // distinct from an empty slice, which is a valid result in some contexts.
  static array_slice invalid () { return { nullptr, ~0U }; }

  // True if the array is valid, false if it is an array like INVALID.
  bool is_valid () const { return m_base || m_size == 0; }

private:
  iterator m_base;
  unsigned int m_size;
};

template<typename T>
inline typename array_slice<T>::value_type &
array_slice<T>::front ()
{
  gcc_checking_assert (m_size);
  return m_base[0];
}

template<typename T>
inline const typename array_slice<T>::value_type &
array_slice<T>::front () const
{
  gcc_checking_assert (m_size);
  return m_base[0];
}

template<typename T>
inline typename array_slice<T>::value_type &
array_slice<T>::back ()
{
  gcc_checking_assert (m_size);
  return m_base[m_size - 1];
}

template<typename T>
inline const typename array_slice<T>::value_type &
array_slice<T>::back () const
{
  gcc_checking_assert (m_size);
  return m_base[m_size - 1];
}

template<typename T>
inline typename array_slice<T>::value_type &
array_slice<T>::operator[] (unsigned int i)
{
  gcc_checking_assert (i < m_size);
  return m_base[i];
}

template<typename T>
inline const typename array_slice<T>::value_type &
array_slice<T>::operator[] (unsigned int i) const
{
  gcc_checking_assert (i < m_size);
  return m_base[i];
}

template<typename T>
array_slice<T>
make_array_slice (T *base, unsigned int size)
{
  return array_slice<T> (base, size);
}

#if (GCC_VERSION >= 3000)
# pragma GCC poison m_vec m_vecpfx m_vecdata
#endif

/* string_slice inherits from array_slice, specifically to refer to a substring
   of a character array.
   It includes some string like helpers.  */
class string_slice : public array_slice<const char>
{
public:
  string_slice () : array_slice<const char> () {}
  string_slice (const char *str) : array_slice (str, strlen (str)) {}
  explicit string_slice (const char *str, size_t len)
    : array_slice (str, len) {}
  explicit string_slice (const char *start, const char *end)
    : array_slice (start, end - start) {}

  friend bool operator== (const string_slice &lhs, const string_slice &rhs)
  {
    if (!lhs.is_valid () || !rhs.is_valid ())
      return false;
    if (lhs.size () != rhs.size ())
      return false;
    /* Case where either is a NULL pointer and therefore, as both are valid,
       both are empty slices with length 0.  */
    if (lhs.size () == 0)
      return true;
    return memcmp (lhs.begin (), rhs.begin (), lhs.size ()) == 0;
  }

  friend bool operator!= (const string_slice &lhs, const string_slice &rhs)
  {
    return !(lhs == rhs);
  }

  /* Returns an invalid string_slice.  */
  static string_slice invalid ()
  {
    return string_slice (nullptr, ~0U);
  }

  /* tokenize is used to split a string by some deliminator into
     string_slice's.  Similarly to the posix strtok_r.but without modifying the
     input string, and returning all tokens which may be empty in the case
     of an empty input string of consecutive deliminators.  */
  static string_slice tokenize (string_slice *str, string_slice delims);

  /* Removes white space from the front and back of the string_slice.  */
  string_slice strip ();

  /* Compares two string_slices in lexographical ordering.  */
  static int strcmp (string_slice str1, string_slice str2);
};

#endif // GCC_VEC_H
