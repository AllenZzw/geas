#ifndef PHAGE_SOLVER_IMPL__H
#define PHAGE_SOLVER_IMPL__H
#include "mtl/Vec.h"
#include "mtl/Heap.h"
#include "mtl/Queue.h"
#include "engine/phage-types.h"
#include "engine/state.h"
#include "engine/infer.h"
#include "engine/persist.h"
#include "engine/propagator.h"
#include "engine/conflict.h"

// #include "solver/solver.h"
#include "solver/branch.h"
#include "solver/options.h"
#include "solver/model.h"

namespace phage {

struct act_cmp {
  bool operator()(int i, int j) { return act[i] > act[j]; }; 
  vec<double>& act;
};

class solver_data {
public:
  solver_data(const options& _opts);
  ~solver_data(void);

  model incumbent;

  options opts;
   
  pred_state state;
  infer_info infer;
  persistence persist;
  conflict_info confl;

  vec< vec<watch_callback> > pred_callbacks;

  Queue<pid_t> pred_queue;
  vec<bool> pred_queued;
  
  // Used for dynamic idempotence
  // handling
  // propagator* active_prop;
  // vec<propagator*> pred_origin;

  vec<pid_t> wake_queue;
  vec<bool> wake_queued;
  
  Queue<propagator*> prop_queue;

  vec<propagator*> propagators;
  vec<brancher*> branchers;
  brancher* last_branch;

  Heap<act_cmp> pred_heap;

  vec<patom_t> assumptions;
  vec<int> assump_level;
  int assump_end;
  
  double learnt_act_inc;
  double pred_act_inc;
  int learnt_dbmax;
};

inline int num_preds(solver_data* s) { return s->pred_callbacks.size(); }

pid_t new_pred(solver_data& s, pval_t lb = 0, pval_t ub = pval_max);
pid_t new_pred(solver_data& s, pred_init init);

patom_t new_bool(solver_data& s);
patom_t new_bool(solver_data& s, pred_init init);

inline pval_t pred_val(solver_data* s, pid_t p) { return s->state.p_vals[p]; }
inline bool pred_fixed(solver_data* s, pid_t p) { return pval_max - pred_val(s, p) == pred_val(s, p^1); }

bool propagate(solver_data& s);
bool enqueue(solver_data& s, patom_t p, reason r);

// Warning: Modifies elts in place.
bool add_clause(solver_data& s, vec<clause_elt>& elts);

void attach(solver_data* s, patom_t p, const watch_callback& c);

template<typename... Ts>
bool add_clause(solver_data* s, patom_t e, Ts... args) {
  vec<clause_elt> elts;
  elts.push(e);
  vec_push(elts, args...);
  return add_clause(*s, elts);  
}

// For debugging
std::ostream& operator<<(std::ostream& o, const patom_t& at);

std::ostream& operator<<(std::ostream& o, const clause_elt& e);

}
#endif
