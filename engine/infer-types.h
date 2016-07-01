#ifndef PHAGE_INFER_TYPES__H
#define PHAGE_INFER_TYPES__H
/* Types for the inference engine */
#include <stdint.h>
#include <vector>
#include <mtl/Vec.h>

#include "utils/defs.h"
#include "engine/phage-types.h"

namespace phage {

class watch_node;

class clause_elt {
public:
  clause_elt(patom_t _at)
    : atom(_at), watch(nullptr)
  { }
  clause_elt(patom_t _at, watch_node* _watch)
    : atom(_at), watch(_watch)
  { }
  patom_t atom;
  // We cache the appropriate watch-list
  watch_node* watch;
};

class clause {
public:
  // As usual, don't use this directly...
  template<class T> clause(T& elts) {
    sz = 0;
    for(clause_elt e : elts)
      data[sz++] = e;
  }
  int size(void) const { return sz; }
  
  clause_elt& operator[](int ii) { return data[ii]; }
  clause_elt* begin(void) { return &(data[0]); }
  clause_elt* end(void) { return &(data[sz]); }
protected:
  int sz;
  clause_elt data[0];
};

// Use this instead
template<class T>
clause* clause_new(T& elts) {
  void* mem = malloc(sizeof(clause) + sizeof(clause_elt)*elts.size());
  return new (mem) clause(elts);
}

template<typename... Es>
clause* make_clause(clause_elt e, Es... rest) {
  vec<clause_elt> v;
  v.push(e);
  vec_push(v, rest...);
  return clause_new(v);
}

// If c == NULL, the clause is binary -- e0 is the 'other' literal
class clause_head {
public: 
  clause_head(patom_t _e0)
    : e0(_e0), c(nullptr) { }
  clause_head(patom_t _e0, clause* _c)
    : e0(_e0), c(_c) { }

  patom_t e0; // We can stop if e0 is true.
  clause* c;
};

// Watches for a given atom.
// Maintains a pointer to the next watch.
class watch_node {
public:
  watch_node(void)
    : succ(nullptr) { }
  patom_t atom; 
  watch_node* succ;  
  vec<clause_head> ws;
};

// For a given pid_t, map values to the corresponding
// watches.
// typedef std::map<pval_t, watch_node*> watch_map;
typedef uint64_triemap<uint64_t, watch_node*, UIntOps> watch_map;

// One of: a clause, a an atom, or 
class reason {
public:
  enum RKind { R_Clause = 0, R_Atom = 1, R_Thunk = 2 };

  reason(void)
    : kind(R_Clause), cl(nullptr) { }

  reason(patom_t _at)
    : kind(R_Atom), at(_at) { }

  reason(clause* _cl)
    : kind(R_Clause), cl(_cl) { }

  RKind kind; 
  union {
    patom_t at; 
    clause* cl;
    /* Deal with thunk. */
  };
};


}

#endif