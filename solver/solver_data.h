#ifndef PHAGE_SOLVER_IMPL__H
#define PHAGE_SOLVER_IMPL__H
#include "mtl/Vec.h"
#include "mtl/Queue.h"
#include "engine/phage-types.h"
#include "engine/state.h"
#include "engine/infer.h"
#include "engine/propagator.h"
#include "engine/conflict.h"

// #include "solver/solver.h"
#include "solver/options.h"

namespace phage {

class solver_data {
public:
  solver_data(const options& _opts)
    : opts(_opts)
  { }

  options opts;
   
  pred_state state;
  infer_info infer;
  conflict_info confl;

  vec< vec<watch_callback> > bool_callbacks;
  vec< vec<watch_callback> > pred_callbacks;

  Queue<pid_t> pred_queue;
  vec<bool> pred_queued;

  vec<pid_t> wake_queue;
  vec<bool> wake_queued;
  
  Queue<propagator*> prop_queue;
};

pid_t new_pred(solver_data& s);
bool propagate(solver_data& s);
bool enqueue(solver_data& s, patom_t p, reason r);

// Warning: Modifies elts in place.
bool add_clause(solver_data& s, vec<clause_elt>& elts);

template<typename... Ts>
bool add_clause(solver_data& s, patom_t e, Ts... args) {
  vec<clause_elt> elts;
  elts.push(e);
  vec_push(elts, args...);
  return add_clause(s, elts);  
}

}
#endif