/*******************************************************************************************[Vec.h]
MiniSat -- Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#ifndef Vec_h
#define Vec_h

#include <cstdlib>
#include <cassert>
#include <new>
#include <initializer_list>
#include <utility>

//=================================================================================================
// Automatically resizable arrays
//
// NOTE! Don't use this vector on datatypes that cannot be re-located in memory (with realloc)

// Ugly hack to deal with gcc getting more aggressive about exploiting
// technically UB reallocs.
template<class T, bool B>
struct conservative_realloc_impl {
};
template<class T>
struct conservative_realloc_impl<T, false> {
  inline static T* _realloc(T* old_mem, int old_sz, int new_sz) {
    assert(old_sz < new_sz);
    T* mem = static_cast<T*>(malloc(sizeof(T) * new_sz));
    for(int ii = 0; ii < old_sz; ++ii) {
      new (mem + ii) T(std::move(old_mem[ii]));
      old_mem[ii].~T();
    }
    free(old_mem);
    return mem;
  }
};
template<class T>
struct conservative_realloc_impl<T, true> {
  inline static T* _realloc(T* old_mem, int old_sz, int new_sz) {
    return static_cast<T*>(realloc(old_mem, sizeof(T) * new_sz));
  }
};

template<class T>
inline T* conservative_realloc(T* old_mem, int old_sz, int new_sz) {
  return conservative_realloc_impl<T, std::is_trivially_copyable<T>::value>::_realloc(old_mem, old_sz, new_sz);
}

template<class T>
class vec {
    T*  data;
    int sz;
    int cap;

    void     init(int size, const T& pad);
    void     grow(int min_cap);

/*
    // Don't allow copying (error prone):
    vec<T>&  operator = (vec<T>& other) { assert(0); return *this; }
             vec        (vec<T>& other) { assert(0); }
             */

    static inline int imin(int x, int y) {
        int mask = (x-y) >> (sizeof(int)*8-1);
        return (x&mask) + (y&(~mask)); }

    static inline int imax(int x, int y) {
        int mask = (y-x) >> (sizeof(int)*8-1);
        return (x&mask) + (y&(~mask)); }

public:
    // Types:
    typedef int Key;
    typedef T   Datum;

    // Constructors:
    vec(void)                   : data(NULL) , sz(0)   , cap(0)    { }
    vec(int size)               : data(NULL) , sz(0)   , cap(0)    { growTo(size); }
    vec(int size, const T& pad) : data(NULL) , sz(0)   , cap(0)    { growTo(size, pad); }
    vec(T* array, int size)     : data(array), sz(size), cap(size) { }      // (takes ownership of array -- will be deallocated with 'free()')
   ~vec(void)                                                      { clear(true); }

    vec(std::initializer_list<T> elts)
      : data(NULL), sz(0), cap(0) {
      capacity(elts.size());
      int ii = 0;
      for(T x : elts) {
        new (&data[ii]) T(x);
        ++ii;
      }
      sz = ii;
    }

    vec(const T* begin, const T* end)
      : data(NULL), sz(0), cap(0) {
      capacity(imax(2, end - begin));
      int ii = 0;
      for(; begin < end; ++begin) {
        new (&data[ii]) T(*begin);
        ++ii;
      }
      sz = ii;
    }

    vec<T>& operator=(vec<T>&& o) {
      if(data) { free(data); }
      data = o.data;
      sz = o.sz;
      cap = o.cap;
      o.data = NULL;
      o.sz = 0;
      o.cap = 0;
      return *this;
    }
    vec<T>&  operator=(const vec<T>& other) {
      other.copyTo(*this);
      return *this;
    }
    vec(const vec<T>& other)
      : data(NULL), sz(0), cap(0) {
      other.copyTo(*this);
    }
    vec(vec<T>&& o)
      : data(o.data), sz(o.sz), cap(o.cap) {
        o.data = nullptr;
        o.sz = 0;
        o.cap = 0;
    }

    // Ownership of underlying array:
    T*       release  (void)           { T* ret = data; data = NULL; sz = 0; cap = 0; return ret; }
    operator T*       (void)           { return data; }     // (unsafe but convenient)
    operator const T* (void) const     { return data; }

    struct slice_t {
      T* begin(void) const { return b; }
      T* end(void) const { return e; }
      T* b;
      T* e;
    };
    slice_t slice(int b, int e) { return slice_t { data+b, data+e }; }
    slice_t slice(int k) { return slice_t { data, data+k }; }
    slice_t slice_from(int k) { return slice_t { data+k, data+sz }; }
    slice_t tail(void) { return slice_t { data+1, data+sz }; }

    // Size operations:
    int      size   (void) const       { return sz; }
    int&     _size  (void)             { return sz; }
    void     shrink (int nelems)       { assert(nelems <= sz); for (int i = 0; i < nelems; i++) sz--, data[sz].~T(); }
    void     shrink_(int nelems)       { assert(nelems <= sz); sz -= nelems; }
    void     pop    (void)             { sz--, data[sz].~T(); }
    void     growTo (int size);
    void     growTo (int size, const T& pad);
    void     clear  (bool dealloc = false);
    void     capacity (int size) { grow(size); }

    // Stack interface:
#if 1
  void     push  (void)              { if (sz == cap) { cap = imax(2, (cap*3+1)>>1); data = conservative_realloc(data, sz, cap); } new (&data[sz]) T(); sz++; }
  void     push  (const T& elem)     { if (sz == cap) { cap = imax(2, (cap*3+1)>>1); data = conservative_realloc(data, sz, cap); } new (&data[sz]) T(elem); sz++; }

  void     push  (T&& elem)     { if (sz == cap) { cap = imax(2, (cap*3+1)>>1); data = conservative_realloc(data, sz, cap); } new (&data[sz]) T(std::move(elem)); sz++; }
    void     push_ (const T& elem)     { assert(sz < cap); data[sz++] = elem; }
#else
    void     push  (void)              { if (sz == cap) grow(sz+1); new (&data[sz]) T()    ; sz++; }
    void     push  (const T& elem)     { if (sz == cap) grow(sz+1); new (&data[sz]) T(elem); sz++; }
#endif

    const T& last  (void) const        { return data[sz-1]; }
    T&       last  (void)              { return data[sz-1]; }

    T* begin(void) const { return data; }
    T* end(void)   const { return data+sz; }
    // Vector interface:
    const T& operator [] (int index) const  { return data[index]; }
    T&       operator [] (int index)        { return data[index]; }


    // Duplicatation (preferred instead):
    void copyTo(vec<T>& copy) const { copy.clear(); copy.growTo(sz); for (int i = 0; i < sz; i++) new (&copy[i]) T(data[i]); }
    void moveTo(vec<T>& dest) { dest.clear(true); dest.data = data; dest.sz = sz; dest.cap = cap; data = NULL; sz = 0; cap = 0; }
};

template<class T>
void vec<T>::grow(int min_cap) {
    if (min_cap <= cap) return;
    if (cap == 0) cap = (min_cap >= 2) ? min_cap : 2;
    else          do cap = (cap*3+1) >> 1; while (cap < min_cap);
    data = conservative_realloc(data, sz, cap * sizeof(T));
}

template<class T>
void vec<T>::growTo(int size, const T& pad) {
    if (sz >= size) return;
    grow(size);
    for (int i = sz; i < size; i++) new (&data[i]) T(pad);
    sz = size; }

template<class T>
void vec<T>::growTo(int size) {
    if (sz >= size) return;
    grow(size);
    for (int i = sz; i < size; i++) new (&data[i]) T();
    sz = size; }

template<class T>
void vec<T>::clear(bool dealloc) {
    if (data != NULL){
        for (int i = 0; i < sz; i++) data[i].~T();
        sz = 0;
        if (dealloc) free(data), data = NULL, cap = 0; } }


#endif
