#include <algorithm>
#include <climits>
#include "engine/propagator.h"
#include "engine/propagator_ext.h"
#include "solver/solver_data.h"
#include "vars/intvar.h"
#include "mtl/bool-set.h"
#include "mtl/p-sparse-set.h"
#include "utils/interval.h"

// using max = std::max;
// using min = std::min;

namespace phage {

// Deciding whether to decompose
bool is_small(solver_data* s, intvar x) {
  return x.ub(s) - x.lb(s) < s->opts.eager_threshold;
}

class iprod_nonneg : public propagator, public prop_inst<iprod_nonneg> {
  // Queueing
  enum Status { S_Red = 2 };

  watch_result wake(int _xi) {
    if(status & S_Red)
      return Wt_Keep;

    queue_prop();
    return Wt_Keep;
  }

  // Explanations
  void ex_z_lb(int _xi, pval_t pval, vec<clause_elt>& expl) {
    int z_lb = z.lb_of_pval(pval);
    // Need to pick k <= lb(x), k' <= lb(y) s.t. k * k' >= z_lb.
//    int x_lb = lb(xs[0]); int y_lb = lb(xs[1]);

    // Check if we can get by with just one atom.
    for(int xi : irange(2)) {
      int x_lb0 = lb_0(xs[xi]);
      int y_lb = lb(xs[1 - xi]);
      if(x_lb0 * y_lb >= z_lb) {
        expl.push(xs[1 - xi] < iceil(z_lb, x_lb0));
        return;
      }
    }

    int ex = iceil(z_lb, lb(xs[1]));
    int ey = iceil(z_lb, ex);
    expl.push(xs[0] < ex);
    expl.push(xs[1] < ey);
  }

  void ex_z_ub(int _xi, pval_t pval, vec<clause_elt>& expl) {
    int z_ub = z.ub_of_pval(pval);
//    int x_ub = ub(xs[0]); int y_ub = ub(xs[1]);

    for(int xi : irange(2)) {
      int x_ub0 = ub_0(xs[xi]);
      int y_ub = ub(xs[1 - xi]);
      if(x_ub0 * y_ub <= z_ub) {
        expl.push(xs[1 - xi] > iceil(z_ub,x_ub0));
        return;
      }
    }
    int y_ub = ub(xs[1]);
    int ex = iceil(z_ub,y_ub);
    int ey = iceil(z_ub,ex);
    expl.push(xs[0] > ex);
    expl.push(xs[1] > ey);
  }

  void ex_x_lb(int xi, pval_t pval, vec<clause_elt>& expl) {
    int x_lb = xs[xi].lb_of_pval(pval);
    
    int yi = 1 - xi; 
    int y_ub = ub(xs[yi]);
    int z_lb = lb(z);

    int y_ub0 = ub_0(xs[yi]);
    if(iceil(z_lb, y_ub0) >= x_lb) {
      expl.push(z <= (x_lb-1) * y_ub0);
      return;
    }
    int z_lb0 = lb_0(z);
    if((x_lb-1) * y_ub < z_lb0) {
      expl.push(xs[yi] > iceil(z_lb0-1, x_lb-1));
      return;
    }
    // Choose largest ey s.t. (x_lb-1) * ey < z_lb.
    int ey = (z_lb-1)/(x_lb-1);
    // And smallest ez s.t. (ey * (x_lb-1)) < ez
    int ez = (x_lb-1) * ey + 1;
    assert((x_lb-1) * ey <= ez);
    expl.push(xs[yi] > ey);
    expl.push(z < ez);
  }

  void ex_x_ub(int xi, pval_t pval, vec<clause_elt>& expl) {
    int x_ub = xs[xi].ub_of_pval(pval);
    
    int yi = 1 - xi; 
    int y_lb = lb(xs[yi]);
    int z_ub = ub(z);
    
    int y_lb0 = lb_0(xs[yi]);
    if(y_lb0 > 0 && (x_ub+1) * y_lb0 > z_ub) {
      expl.push(z >= (x_ub+1) * y_lb0);
      return;
    }
    int z_ub0 = ub_0(z);
    if((x_ub+1) * y_lb > z_ub0) {
      expl.push(xs[yi] < iceil(z_ub0+1, x_ub+1));
      return;
    }

    // Pick smallest ey s.t. ((x_ub + 1) * ey) > z_ub.
    int ey = iceil(z_ub+1, x_ub+1);
    int ez = ey * (x_ub+1)-1;
    assert((x_ub+1) * ey > ez);
    expl.push(xs[yi] < ey);
    expl.push(z > ez);
  }

public:
  iprod_nonneg(solver_data* s, patom_t _r, intvar _z, intvar _x, intvar _y)
    : propagator(s), r(_r), z(_z), status(0) {
      assert(s->state.is_entailed_l0(r));
      xs[0] = _x; xs[1] = _y; 

//    fprintf(stderr, "Constructing [iprod_nonneg]\n");
    z.attach(E_LU, watch_callback(wake_default, this, 2));
    for(int ii : irange(2))
      xs[ii].attach(E_LU, watch_callback(wake_default, this, ii));
  }

  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cerr << "[[Running iprod(+)]]" << std::endl;
#endif
    int z_low = lb(xs[0]) * lb(xs[1]);
    if(z_low > lb(z)) {
      if(!set_lb(z, z_low, ex_thunk(ex<&P::ex_z_lb>,0, expl_thunk::Ex_BTPRED)))
        return false;
    }
    int z_high = ub(xs[0]) * ub(xs[1]);
    if(z_high < ub(z)) {
      if(!set_ub(z, z_high, ex_thunk(ex<&P::ex_z_ub>,0, expl_thunk::Ex_BTPRED)))
        return false;
    }

    for(int xi : irange(2)) {
      if(ub(xs[1 - xi]) <= 0)
        continue;
      int x_low = iceil(lb(z), ub(xs[1 - xi]));
      if(x_low > lb(xs[xi])) {
        if(!set_lb(xs[xi], x_low, ex_thunk(ex<&P::ex_x_lb>,xi, expl_thunk::Ex_BTPRED)))
          return false;
      }
      int y_lb = lb(xs[1 - xi]);
      if(y_lb > 0) {
        int x_high = ub(z)/lb(xs[1 - xi]);
        if(x_high < ub(xs[xi])) {
          if(!set_ub(xs[xi], x_high, ex_thunk(ex<&P::ex_x_ub>,xi, expl_thunk::Ex_BTPRED)))
            return false;
        }
      }
    }
    return true;
  }

  patom_t r;
  intvar z;
  intvar xs[2];

  char status;
};

// Propagator:
// non-incremental, with naive eager explanations
class iprod : public propagator {
  static watch_result wake_z(void* ptr, int xi) {
    iprod* p(static_cast<iprod*>(ptr)); 
    p->queue_prop();
    return Wt_Keep;
  }

  static watch_result wake_xs(void* ptr, int xi) {
    iprod* p(static_cast<iprod*>(ptr)); 
    p->queue_prop();
    return Wt_Keep;
  }

public:
  iprod(solver_data* s, intvar _z, intvar _x, intvar _y)
    : propagator(s), z(_z), x(_x), y(_y)
  {
    z.attach(E_LU, watch_callback(wake_z, this, 0));
    x.attach(E_LU, watch_callback(wake_xs, this, 0));
    y.attach(E_LU, watch_callback(wake_xs, this, 1));
  }
  
  template<class T>
  void push_expl(int_itv iz, int_itv ix, int_itv iy, T& ex) {
    ex.push(z < iz.lb);
    ex.push(z > iz.ub);
    ex.push(x < ix.lb);
    ex.push(x > ix.ub);
    ex.push(y < iy.lb);
    ex.push(y > iy.ub);
  }

  clause* make_expl(int_itv iz, int_itv ix, int_itv iy) {
    expl_builder ex(s->persist.alloc_expl(7)); 
    push_expl(iz, ix, iy, ex);
    return *ex;
  }
  
  bool propagate(vec<clause_elt>& confl) {
    // Non-incremental propagator
#ifdef LOG_PROP
    std::cerr << "[[Running iprod]]" << std::endl;
#endif
    int_itv z_supp(var_unsupp(s, z));          
    int_itv x_supp(var_unsupp(s, x));
    int_itv y_supp(var_unsupp(s, y));

    int_itv z_itv(var_range(s, z));
    int_itv x_itv(var_range(s, x));
    int_itv y_itv(var_range(s, y));
    if(z_itv.elem(0)) {
      if(x_itv.elem(0)) {
        z_supp = int_itv { 0, 0 };
        y_supp = y_itv;
        x_supp = int_itv { 0, 0 };
      }
      if(y_itv.elem(0)) {
        z_supp = int_itv { 0, 0 };
        x_supp = x_itv;
        y_supp |= int_itv { 0, 0 };
      }
    }

    if(x_itv.ub > 0) {
      int_itv x_pos(pos(var_range(s, x)));
      if(y_itv.ub > 0) {
        // + * + 
        int_itv y_pos(pos(var_range(s, y)));
        int_itv xy { x_pos.lb * y_pos.lb, x_pos.ub * y_pos.ub };
        int_itv z_feas(z_itv & xy);
        if(!z_feas.empty()) {
          z_supp |= z_feas;
          // Propagate back to x, y
          x_supp |= (x_itv & int_itv {(z_feas.lb + y_pos.ub - 1)/ y_pos.ub,
                                      z_feas.ub / y_pos.lb});
          y_supp |= (y_itv & int_itv {(z_feas.lb + x_pos.ub - 1) / x_pos.ub,
                                      z_feas.ub / x_pos.lb});
        }
      }
      if(y_itv.lb < 0) {
        // + * -  
        int_itv y_neg(neg(var_range(s, y)));
        int_itv xy { x_pos.ub * y_neg.lb, x_pos.lb * y_neg.ub };
        int_itv z_feas(z_itv & xy);
        if(!z_feas.empty()) {
          z_supp |= z_feas;
          // Propagate back to x, y
          x_supp |= (x_itv & int_itv {z_feas.lb / y_neg.lb,
                                      (z_feas.ub + y_neg.ub + 1)/y_neg.ub});
          y_supp |= (y_itv & int_itv {z_feas.lb / x_pos.lb,
                                      (z_feas.ub - x_pos.ub + 1)/x_pos.ub});
        }
      }
    }
    if(x_itv.lb < 0) {
      int_itv x_neg(neg(var_range(s, x)));
      if(y_itv.ub > 0) {
        // - * +  
        int_itv y_pos(pos(var_range(s, y)));
        int_itv xy { x_neg.lb * y_pos.lb, x_neg.ub * y_pos.lb };
        int_itv z_feas(z_itv & xy);
        if(!z_feas.empty()) {
          z_supp |= z_feas;
          // Propagate back to x, y
          x_supp |= (x_itv & int_itv {z_feas.lb / y_pos.lb,
                                      (z_feas.ub - y_pos.ub + 1)/y_pos.ub});
          y_supp |= (y_itv & int_itv {(z_feas.ub + x_neg.ub + 1)/x_neg.ub,
                                      z_feas.lb / x_neg.lb});
        }
      }
      if(y_itv.lb < 0) {
        // - * - 
        int_itv y_neg(neg(var_range(s, y)));
        int_itv xy { x_neg.ub * y_neg.ub, x_neg.lb * y_neg.lb };
        int_itv z_feas(z_itv & xy);
        if(!z_feas.empty()) {
          z_supp |= z_feas;
          // Propagate back to x, y
          x_supp |= (x_itv & int_itv {z_feas.ub / y_neg.ub,
                                      (z_feas.lb+y_neg.lb+1)/ y_neg.lb});
          y_supp |= (y_itv & int_itv {z_feas.ub / x_neg.ub,
                                      (z_feas.lb+x_neg.lb+1)/ x_neg.lb});
        }
      }
    }

    if(z_supp.ub < z_supp.lb) {
      // Infeasible
      push_expl(z_itv, x_itv, y_itv, confl);
      return false;
    }
    assert(x_supp.lb <= x_supp.ub);
    assert(y_supp.lb <= y_supp.ub);

    if(z_supp.lb > lb(z)) {
      clause* cl(make_expl(z_itv, x_itv, y_itv));
      (*cl)[0] = (z >= z_supp.lb);
      if(!set_lb(z, z_supp.lb, cl))
        return false;
    }
    if(z_supp.ub < ub(z)) {
      clause* cl(make_expl(z_itv, x_itv, y_itv));
      (*cl)[0] = (z <= z_supp.ub);
      if(!set_lb(z, z_supp.lb, cl))
        return false;
    }
    if(x_supp.lb > lb(x)) {
      clause* cl(make_expl(z_itv, x_itv, y_itv));
      (*cl)[0] = (x >= x_supp.lb);
      if(!set_lb(x, x_supp.lb, cl))
        return false;
    }
    if(x_supp.ub < ub(x)) {
      clause* cl(make_expl(z_itv, x_itv, y_itv));
      (*cl)[0] = (x <= x_supp.ub);
      if(!set_ub(x, x_supp.ub, cl))
        return false;
    }
    if(y_supp.lb > lb(y)) {
      clause* cl(make_expl(z_itv, x_itv, y_itv));
      (*cl)[0] = (y >= y_supp.lb);
      if(!set_lb(y, y_supp.lb, cl))
        return false;
    }
    if(y_supp.ub < ub(y)) {
      clause* cl(make_expl(z_itv, x_itv, y_itv));
      (*cl)[0] = (y <= y_supp.ub);
      if(!set_ub(y, y_supp.ub, cl))
        return false;
    }

    return true;
  }

  bool check_sat(void) {
    return true;
  }

  void root_simplify(void) { }

  void cleanup(void) { is_queued = false; }

protected:
  intvar z;
  intvar x;
  intvar y;
};

irange pos_range(solver_data* s, intvar z) { return irange(std::max(1, (int) z.lb(s)), z.ub(s)+1); }
irange neg_range(solver_data* s, intvar z) { return irange(z.lb(s), std::min(-1, (int) z.ub(s))); }

// Decomposition of z = x * y.
void imul_decomp(solver_data* s, intvar z, intvar x, intvar y) {
  // Establish lower bounds on abs(z).
  // Case splits. x pos:
  if(x.ub(s) > 0) {
    // y pos
    if(y.ub(s) > 0) {
      for(int kx : pos_range(s, x)) {
        for(int ky : pos_range(s, y)) {
          add_clause(s, x < kx, y < ky, z >= kx*ky);  
          add_clause(s, x > kx, y > ky, x < -kx, y < -ky, z <= kx*ky);
        }
      }
    }
    // y neg
    if(y.lb(s) < 0) {
      for(int kx : pos_range(s, x)) {
        for(int ky : neg_range(s, y)) {
          add_clause(s, x < kx, y > ky, z <= kx*ky);
          add_clause(s, x > kx, y < ky, x < -kx, y > -ky, z >= kx*ky);
        }
      }
    }
  }
  // x neg
  if(x.lb(s) < 0) {
    if(y.ub(s) > 0) {
      for(int kx : neg_range(s, x)) {
        for(int ky : pos_range(s, y)) {
          add_clause(s, x > kx, y < ky, z <= kx*ky);
          add_clause(s, x < kx, y > ky, x > -kx, y < -ky, z >= kx*ky);
        }
      }
    }
    if(y.lb(s) < 0) {
      for(int kx : neg_range(s, x)) {
        for(int ky : neg_range(s, y)) {
          add_clause(s, x > kx, y > ky, z >= kx*ky);
          add_clause(s, x < kx, y < ky, x > -kx, y > -ky, z <= kx*ky);
        }
      }
    }
  }
}

class iabs : public propagator {
  static watch_result wake(void* ptr, int xi) {
    iabs* p(static_cast<iabs*>(ptr)); 
    p->queue_prop();
    return Wt_Keep;
  }

  // Explanation functions  
  // Annoyingly, for upper bounds, we need invert the
  // value manually.
  static void ex_z_lb(void* ptr, int sign,
                        pval_t val, vec<clause_elt>& expl) {
    iabs* p(static_cast<iabs*>(ptr));
    if(sign) {
      expl.push(p->x < to_int(val));
    } else {
      expl.push(p->x > -to_int(val));
    }
  }
  static void ex_z_ub(void* ptr, int _xi, pval_t val,
                        vec<clause_elt>& expl) {
    iabs* p(static_cast<iabs*>(ptr));
    intvar::val_t ival(to_int(pval_max - val));

    expl.push(p->x > ival);
    expl.push(p->x < -ival);
  }

  static void ex_lb(void* ptr, int sign,
                        pval_t val, vec<clause_elt>& expl) {
    iabs* p(static_cast<iabs*>(ptr));
    intvar::val_t ival(p->x.lb_of_pval(val));
    if(sign) {
      intvar::val_t v = ival < 1 ? -1 : -ival;
      expl.push(p->x <= v);
      expl.push(p->z < ival);
    } else {
      expl.push(p->z > -ival);
    }
  }
  static void ex_ub(void* ptr, int sign,
                        pval_t val, vec<clause_elt>& expl) {
    iabs* p(static_cast<iabs*>(ptr));
    intvar::val_t ival(p->x.ub_of_pval(val));

    if(sign) {
      expl.push(p->z > ival);
    } else {
      intvar::val_t v = ival > -1 ? 1 : -ival;
      expl.push(p->x >= v);
      expl.push(p->z < ival);
    }
  }

public:
  iabs(solver_data* s, intvar _z, intvar _x)
    : propagator(s), z(_z), x(_x)
  {
    z.attach(E_LU, watch_callback(wake, this, 0));
    x.attach(E_LU, watch_callback(wake, this, 1));
  }
 
  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cerr << "[[Running iabs]]" << std::endl;
#endif
    // Case split
    int_itv z_itv { ub(z)+1, lb(z)-1 };
    int_itv x_itv { ub(x)+1, lb(x)-1 };
//    int z_min = ub(z)+1;
//    int z_max = lb(z)-1;

//    int x_min = ub(x)+1;
//    int x_max = lb(x)-1;

    if(lb(x) < 0) {
      int_itv neg { std::max(lb(x), -ub(z)),
                    std::max(ub(x), -lb(z)) };
      if(!neg.empty()) {
        x_itv |= neg;
        z_itv |= -neg;
      }
    }
    if(ub(x) >= 0) {
      int_itv pos { std::max(lb(z), lb(z)),
                    std::min(ub(x), ub(z)) }; 
      if(!pos.empty()) {
        x_itv |= pos;
        z_itv |= pos;
      }
    }

    if(z_itv.ub < ub(z)) {
      if(!set_ub(z, z_itv.ub, expl_thunk { ex_z_ub, this, 0 }))
        return false;
    }
    if(z_itv.lb > lb(z)) {
      if(!set_lb(z, z_itv.lb, expl_thunk { ex_z_lb, this, x_itv.lb >= 0 }))
        return false;
    }
    if(x_itv.ub < ub(x)) {
      if(!set_ub(x, x_itv.ub, expl_thunk { ex_ub, this, x_itv.ub >= 0 }))
        return false;
    }
    if(x_itv.lb > lb(x)) {
      if(!set_lb(x, x_itv.lb, expl_thunk { ex_lb, this, x_itv.lb >= 0}))
        return false;
    }
    return true;
#if 0
    int z_min = lb(z);
    if(lb(x) > -z_min) {
      // z = x
      if(z_min > lb(x)) {
        if(!x.set_lb(z_min, expl_thunk ex_x_lb, this, 1))
          return false;
      }
      if(ub(z) < ub(x)) {
         
      }
    } else if(ub(x) < z_min) {
      // z = -x
      
    }
#endif
    return true;
  }

  bool check_sat(void) {
    if(lb(x) <= 0) {
      int low = std::max((int) lb(z), std::max(0, (int) -ub(x)));
      int high = std::min(ub(z), -lb(x));
      if(low <= high)
        return true;
    }
    if(ub(x) >= 0) {
      int low = std::max((int) lb(z), std::max(0, (int) lb(x)));
      int high = std::min(ub(z), ub(x));
      if(low <= high)
        return true;
    }
    return false;
  }

  void cleanup(void) { is_queued = false; }

protected:
  intvar z;
  intvar x; 
};

// Only use for small domains
void iabs_decomp(solver_data* s, intvar z, intvar x) {
  // Set up initial bounds
  if(z.lb(s) < 0)
    enqueue(*s, z >= 0, reason());
//    z.set_lb(0, reason());
  if(x.lb(s) < -z.ub(s))
    enqueue(*s, z >= -z.ub(s), reason());
//    x.set_lb(-ub(z), reason());
  if(z.ub(s) < x.ub(s))
    enqueue(*s, x <= z.ub(s), reason());
//    x.set_ub(ub(z), reason());

  for(intvar::val_t k : z.domain(s)) {
    add_clause(s, x < -k, x > k, z <= k);
    add_clause(s, x > -k, z >= k);
    add_clause(s, x < k, z >= k);
  }
}

// Incremental version
#if 1
class imax : public propagator, public prop_inst<imax> {
  static watch_result wake_z(void* ptr, int k) {
    imax* p(static_cast<imax*>(ptr));
    p->z_change |= k;
    p->queue_prop();
    return Wt_Keep;
  }

  static watch_result wake_x(void* ptr, int xi) {
    imax* p(static_cast<imax*>(ptr));
    assert((xi>>1) < p->xs.size());
    if(xi&1) { // UB
      if(xi>>1 == p->ub_supp) {
        p->supp_change = E_UB;
        p->queue_prop();
      }
    } else {
      if(!p->lb_change.elem(xi>>1))
        p->lb_change.add(xi>>1);
      p->queue_prop();
    }
    return Wt_Keep;
  }

  static void expl_z_lb(imax* p, int xi, intvar::val_t v,
                          vec<clause_elt>& expl) {
    expl.push(p->xs[xi] < v+p->xs[xi].off);
  }

  static void expl_z_ub(imax* p, int xi, intvar::val_t v,
                          vec<clause_elt>& expl) {
    v = v + p->z.off;
    for(intvar x : p->xs) {
      expl.push(x > v + x.off);
    }
  }

  static void expl_xi_lb(imax* p, int xi, intvar::val_t v,
                          vec<clause_elt>& expl) {
    v = v + p->xs[xi].off;
    intvar::val_t sep = std::max(v, p->sep_val);
    expl.push(p->z < sep);
    for(int x : irange(xi)) {
      expl.push(p->xs[x] >= sep);
    }
    for(int x : irange(xi+1,p->xs.size())) {
      expl.push(p->xs[x] >= sep);
    }
  }

  static void expl_xi_ub(imax* p, int xi, intvar::val_t v,
                          vec<clause_elt>& expl) {
    v = v + p->xs[xi].off;
    expl.push(p->z > v);
  }

public:
  imax(solver_data* s, intvar _z, vec<intvar>& _xs)
    : propagator(s), z(_z), xs(_xs),
      sep_val(lb(z)), z_change(0), supp_change(0) { 
    z.attach(E_LB, watch_callback(wake_z, this, 0, true));
    z.attach(E_UB, watch_callback(wake_z, this, 1, true));

    lb_supp = ub_supp = 0;
    int lb = xs[lb_supp].lb(s);
    int ub = xs[ub_supp].ub(s);
    for(int ii = 0; ii < xs.size(); ii++) {
      if(xs[ii].lb(s) < lb) {
        lb_supp = ii;
        lb = xs[ii].lb(s);
      }
      if(xs[ii].ub(s) > ub) {
        ub_supp = ii;
        ub = xs[ii].ub(s);
      }
      xs[ii].attach(E_LB, watch_callback(wake_x, this, ii<<1, true));
      xs[ii].attach(E_UB, watch_callback(wake_x, this, (ii<<1)|1, true));
    }

    maybe_max.growTo(xs.size());
    for(int xi : irange(0, xs.size()))
      maybe_max.insert(xi);

    lb_change.growTo(xs.size()); 
  }

  inline void mm_remove(int k, bool& mm_trailed) {
    if(!mm_trailed)
      trail_push(s->persist, maybe_max.sz);
    mm_trailed = true;
    maybe_max.remove(k);
  }

  inline bool propagate_z_ub(vec<clause_elt>& confl, bool& mm_trailed) {
    unsigned int seen_var = ub_supp;
    int seen_ub = xs[ub_supp].ub(s);
    for(int xi : maybe_max) {
      assert(xi < xs.size());
      if(seen_ub < xs[xi].ub(s)) {
        seen_var = xi;
        seen_ub = xs[xi].ub(s);
      }
    }

    if(seen_ub < ub(z)) {
      /*
      expl_builder e(s->persist.alloc_expl(1 + xs.size()));
      for(intvar x : xs)
        e.push(x > seen_ub);
      if(!z.set_ub(seen_ub, *e))
        */
      if(!set_ub(z, seen_ub, ex_thunk(ex_ub<expl_z_ub>, 0)))
        return false;
    }
    if(seen_var != ub_supp)
      trail_change(s->persist, ub_supp, seen_var);

    return true; 
  }

  inline bool propagate_xs_lb(vec<clause_elt>& confl, bool& mm_trailed) {
    unsigned int *xi = maybe_max.begin();
    int z_lb = lb(z);
    while(xi != maybe_max.end()) {
      if(xs[*xi].ub(s) < z_lb) {
        // xs[*xi] cannot be the maximum
        if(!mm_trailed) {
          mm_trailed = true;
          trail_push(s->persist, maybe_max.sz);
        }
        if(sep_val <= xs[*xi].ub(s))
          sep_val = xs[*xi].ub(s)+1;
        maybe_max.remove(*xi);
      } else {
        // lb(z) won't change anyway
        if(xs[*xi].lb(s) == z_lb)
          return true;

        goto first_lb_found;
      }
    }
    // Every variable is below lb_max.
    // Set up conflict and bail
    confl.push(z < z_lb);
    for(intvar x : xs)
      confl.push(x >= z_lb);
    return false;

first_lb_found:
    unsigned int supp = *xi;
    ++xi;
    while(xi != maybe_max.end()) {
      if(xs[*xi].ub(s) < z_lb) {
        if(!mm_trailed) {
          mm_trailed = true;
          trail_push(s->persist, maybe_max.sz);
        }
        if(sep_val <= xs[*xi].ub(s))
          sep_val = xs[*xi].ub(s)+1;
        maybe_max.remove(*xi);
      } else {
        // Second support found
        return true;
      }
    }
    // Exactly one support found
    assert(xs[supp].lb(s) < z_lb);
    /*
    expl_builder e(s->persist.alloc_expl(1 + xs.size()));
    e.push(z < z_lb);
    for(int ii = 0; ii < xs.size(); ii++) {
        if(ii != supp)
          e.push(xs[ii] >= z_lb);
    }
    if(!xs[supp].set_lb(z_lb, *e))
    */
    if(sep_val > lb(z))
      sep_val = lb(z);
    if(!set_lb(xs[supp], z_lb, ex_thunk(ex_lb<expl_xi_lb>, supp)))
      return false;

    return true;
  }

  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cout << "[[Running imax]]" << std::endl;
#endif
    bool maybe_max_trailed = false;
    
    // forall x, ub(x) <= ub(z).
    if(z_change&E_UB) {
      int z_ub = ub(z);
      for(int ii : maybe_max) {
        if(z_ub < xs[ii].ub(s)) {
          // if(!xs[ii].set_ub(z_ub, z > z_ub))
          if(!set_ub(xs[ii], z_ub, ex_thunk(ex_ub<expl_xi_ub>, ii)))
            return false; 
        }
      }
    }

    // forall x, lb(z) >= lb(x).
    int z_lb = lb(z);
    for(int xi : lb_change) {
      if(xs[xi].lb(s) > z_lb) {
        z_lb = xs[xi].lb(s);
        // if(!z.set_lb(z_lb, xs[xi] < z_lb))
        if(!set_lb(z, z_lb, ex_thunk(ex_lb<expl_z_lb>, xi)))
          return false;
      }
    }

    if(supp_change&E_UB) {
      if(!propagate_z_ub(confl, maybe_max_trailed))
        return false;
    }

    if(z_change&E_LB) {
      // Identify if any variable LBs must change
      if(!propagate_xs_lb(confl, maybe_max_trailed))
        return false;
    }
    return true;
  }

  bool check_sat(void) {
    int zlb = INT_MIN; 
    int zub = INT_MIN;
    for(intvar x : xs) {
      zlb = std::max(zlb, (int) lb(x));
      zub = std::max(zub, (int) ub(x));
    }
    return zlb <= ub(z) && lb(z) <= zub;
  }

  void root_simplify(void) { }

  void cleanup(void) {
    z_change = 0;
    supp_change = 0;
    lb_change.clear();
    is_queued = false;
  }

protected:
  intvar z;
  vec<intvar> xs;

  // Persistent state
  unsigned int lb_supp;
  unsigned int ub_supp;
  p_sparseset maybe_max; // The set of vars (possibly) above lb(z)
  intvar::val_t sep_val;

  // Transient state
  char z_change;
  char supp_change;
  boolset lb_change;
};
#else
// FIXME: Finish implementing
class max_lb : public propagator, public prop_inst<max_lb> {
  enum { S_Active = 1, S_Red = 2 };

  inline void add_cb(int xi, pid_t p) {
    if(!attached[xi]) {
      s->watch_callbacks[p].push(watch<&P::watch_act>, 0, Wt_IDEM);
      attached[xi] = true;
    }
  }

  watch_result watch_xi(int xi) {
      // Update watches
        
      // No watches remaining.
      trail_change(s->persist, status, (char) S_Active);
      add_cb(0, xs[0]^1); // ub(x) decreases
      add_cb(1, z); // lb(z) increases
  }

  watch_result watch_act(int xi) {
    if(!(status&S_Active)) {
      attached[xi] = 0;
      return Wt_Drop; 
    }

    queue_prop(); 
  }

  void ex_z(int is_z, pval_t pval, vec<clause_elt>& expl) {
    pval_t v;
    if(is_z) {
      // Explaining ub(z)
      v = pval_inv(pval);
      expl.push(ge_lit(xs[xi], v+1));
    } else {
      // Explaining lb(xs[0])
      v = pval;
      expl.push(le_lit(z, v-1));
    }
    pval_t gap = std::max(v, sep+1);
    for(int ii = 1; ii < xs.size(); ii++)
      expl.push(ge_lit(xs[ii], gap));
  }

public:
  malb(xsolver_data* _s, vec<pid_t>& _xs, pid_t z)
    : propagator(_s), xs(_xs), z(_z) {
    for(int ii : irange(2))
      attached[ii] = 0; 
  }
  bool propagate(void) {
    
  }

  void simplify_at_root(void) {
    
  }

  vec<pid_t> xs;
  pid_t z;

  pval_t sep;
  
  char status;

  // Keeping track of active watches
  bool attached[2];
};
#endif

// Avoids introducing equality lits
#if 0
class ine_bound : public propagator, public prop_inst<ine_bound> {
  enum Vtag { Var_X = 1, Var_Z = 2 };

  static watch_result wake_fix(void* ptr, int k) {
    ine_bound* p(static_cast<ine_bound*>(ptr));
    p->new_fix |= k;
    p->queue_prop();
    return Wt_Keep;
  }

  static watch_result wake_bound(void* ptr, int k) {
    ine_bound* p(static_cast<ine_bound*>(ptr));
    if(p->fixed) 
      p->queue_prop();
    return Wt_Keep;
  }

   watch_result wake_r(int k) {
     if(fixed)
       queue_prop();
     return Wt_Keep;
   }

  static void expl(void* ptr, int xi, pval_t v, vec<clause_elt>& ex) {
    ine_bound* p(static_cast<ine_bound*>(ptr));
    if(xi != 0) {
//      ex.push(p->x != p->lb(x));
      ex.push(p->x < p->lb(x));
      ex.push(p->x > p->ub(x));
    }
    if(xi != 1) {
      // ex.push(p->z != p->lb(z));
      ex.push(p->z < p->lb(z));
      ex.push(p->z > p->lb(z));
    }
    if(xi != 2)
      ex.push(~p->r);
  }

public:
  ine_bound(solver_data* s, intvar _z, intvar _x, patom_t _r)
    : propagator(s), z(_z), x(_x), r(_r),
      fixed(0), new_fix(0) {
    attach(s, r, watch<&P::wake_r>(0));
    z.attach(E_FIX, watch_callback(wake_fix, this, 0));
//    z.attach(E_LU, watch_callback(wake_bound, this, 0));

    x.attach(E_FIX, watch_callback(wake_fix, this, 1));
//    x.attach(E_LU, watch_callback(wake_bound, this, 1));
  }


  inline bool prop_bound(intvar a, intvar b) {
    int k = a.lb(s);
    if(b.lb(s) == k) {
      if(!b.set_lb(k+1, s->persist.create_expl(a < k, a > k, b < k)))
        return false;
    }
    if(b.ub(s) == k) {
      if(!b.set_ub(k-1, s->persist.create_expl(a < k, a > k, b > k)))
        return false;
    }
    return true;
  }

  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cout << "[[Running ineq]]" << std::endl;
#endif
    trail_change(s->persist, fixed, (char) (fixed|new_fix));
    
    switch(fixed) {
      case Var_X:
        // return prop_bound(x, z);
        return enqueue(*s, z != lb(x), ex_thunk(expl, 0));
      case Var_Z:
//        return prop_bound(z, x);
        return enqueue(*s, x != lb(z), ex_thunk(expl, 1));
      default:
        if(lb(z) == lb(x)) {
          /*
          int k = lb(z);
          // vec_push(confl, z < k, z > k, x < k, x > k);
          vec_push(confl, z != k, x != k);
          return false;
          */
          return enqueue(*s, ~r, ex_thunk(expl, 2));
        }
        return true;
    }

    return true;
  }

  bool check_sat(void) {
    int lb = std::min(lb(z), lb(x));
    int ub = std::max(ub(z), ub(x));
    return lb < ub;
  }

  void root_simplify(void) { }

  void cleanup(void) {
    new_fix = 0;
    is_queued = false;
  }

protected:
  intvar z;
  intvar x;
  patom_t r;

  // Persistent state
  char fixed;

  // Transient state
  char new_fix;
};
#endif
class ineq : public propagator, public prop_inst<ineq> {
  enum Vtag { Var_X = 1, Var_Z = 2 };
  enum TrigKind { T_Atom, T_Var };
  enum Status { S_None = 0, S_Active = 1 };

  struct trigger {
    TrigKind kind;
    int idx;
  };

  inline bool is_active(trigger t) {
    switch(t.kind) {
      case T_Atom:
        return s->state.is_entailed(r);
      case T_Var:
      default:
        return pred_fixed(s, vs[t.idx].p);
    }
  }

  inline void attach_trigger(trigger t, int ii) {
    switch(t.kind) {
      case T_Atom: 
        attach(s, r, watch<&P::wake_trig>(ii, Wt_IDEM));
        break;
      case T_Var:
        vs[t.idx].attach(E_FIX, watch<&P::wake_trig>(ii, Wt_IDEM));
        break;
    }
  }

  watch_result wake_lb(int wake_gen) {
    if(wake_gen != gen || !(status&S_Active))
      return Wt_Drop;

//    fprintf(stderr, "{%p,lb: %d %d %d\n", this, is_active(trigs[0]), is_active(trigs[1]), is_active(trigs[2]));
    assert(is_active(trigs[1 - active]));
    queue_prop();
    return Wt_Keep;
  }

  watch_result wake_ub(int wake_gen) {
    if(wake_gen != gen || !(status&S_Active))
      return Wt_Drop;

//    fprintf(stderr, "{%p,ub: %d %d %d\n", this, is_active(trigs[0]), is_active(trigs[1]), is_active(trigs[2]));
    assert(is_active(trigs[1 - active]));
    queue_prop(); 
    return Wt_Keep;
  }

  inline bool enqueue_trigger(trigger t, int ii, vec<clause_elt>& confl) {
    if(is_active(t)) {
      assert(vs[0].is_fixed(s));
      assert(vs[1].is_fixed(s));
      assert(vs[0].lb(s) == vs[1].lb(s));
      intvar::val_t val = vs[0].lb(s);
      confl.push(~r);
      /*
      confl.push(vs[0] != val);
      confl.push(vs[1] != val);
      */
      confl.push(vs[0] < val);
      confl.push(vs[0] > val);
      confl.push(vs[1] < val);
      confl.push(vs[1] > val);
      return false; 
    }
    switch(t.kind) {
      case T_Atom:
        return enqueue(*s, ~r, ex_thunk(ex_nil<&P::expl>,ii));
      case T_Var:
      {
        intvar::val_t val = vs[1 - t.idx].lb(s);
        prop_val = val;

        if(vs[t.idx].lb(s) == val) {
          return set_lb(vs[t.idx], val+1, ex_thunk(ex_nil<&P::expl_lb>, ii));
        }
        if(vs[t.idx].ub(s) == val) {
          return set_ub(vs[t.idx], val-1, ex_thunk(ex_nil<&P::expl_ub>, ii));
        }
        // Otherwise, add LB and UB watches
        ++gen;
        trail_change(s->persist, status, (char) S_Active);
        attach(s, vs[t.idx] >= val, watch<&P::wake_lb>(gen, Wt_IDEM));
        attach(s, vs[t.idx] <= val, watch<&P::wake_ub>(gen, Wt_IDEM));
        /*
        return enqueue(*s, vs[t.idx] != val, ex_thunk(ex_nil<&P::expl>,ii)); 
        */
      }
    }
    return true;
  }

  watch_result wake_trig(int wi) {
    assert(is_active(trigs[wi]));
    if(!is_active(trigs[2])) {
      std::swap(trigs[2], trigs[wi]); 
      attach_trigger(trigs[wi], wi);
      return Wt_Drop;
    }
    if(!is_active(trigs[1 - wi]))
      active = 1 - wi;

//    fprintf(stderr, "{%p(%d): %d %d %d\n", this, wi, is_active(trigs[0]), is_active(trigs[1]), is_active(trigs[2]));
    assert(is_active(trigs[1 - active]));
    queue_prop();
    return Wt_Keep;
  }

  void expl(int xi, vec<clause_elt>& ex) {
    trigger t = trigs[active];
    switch(t.kind) {
      case T_Atom:  
      /*
        ex.push(vs[0] != vs[0].lb());
        ex.push(vs[1] != vs[1].lb());
        */
        ex.push(vs[0] < vs[0].lb(s));
        ex.push(vs[0] > vs[0].ub(s));
        ex.push(vs[1] < vs[1].lb(s));
        ex.push(vs[1] > vs[1].ub(s));
        return;
      case T_Var:
        ex.push(~r);
      /*
        ex.push(vs[1 - t.idx] != vs[1 - t.idx].lb());
        */
        ex.push(vs[1 - t.idx] < vs[1 - t.idx].lb(s));
        ex.push(vs[1 - t.idx] > vs[1 - t.idx].ub(s));
        return;
    }
  }

  void expl_lb(int xi, vec<clause_elt>& ex) {
    trigger t = trigs[active];
    ex.push(~r);
    ex.push(vs[t.idx] < prop_val);
    ex.push(vs[1 - t.idx] < prop_val);
    ex.push(vs[1 - t.idx] > prop_val);
    return;
  }

  void expl_ub(int xi, vec<clause_elt>& ex) {
    trigger t = trigs[active];
    ex.push(~r);
    ex.push(vs[t.idx] > prop_val);
    ex.push(vs[1 - t.idx] < prop_val);
    ex.push(vs[1 - t.idx] > prop_val);
    return;
  }

public:
  ineq(solver_data* s, intvar _z, intvar _x, patom_t _r)
    : propagator(s), r(_r), active(0), prop_val(0), gen(0), status(0) {
    vs[0] = _z;
    vs[1] = _x; 

    trigs[0] = { T_Var, 0 };
    trigs[1] = { T_Var, 1 };
    trigs[2] = { T_Atom };

    attach_trigger(trigs[0], 0);
    attach_trigger(trigs[1], 1);
  }


  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cout << "[[Running ineq]]" << std::endl;
#endif
    assert(is_active(trigs[1 - active]));
    assert(is_active(trigs[2]));
    if(vs[0].ub(s) < vs[1].lb(s) || vs[0].lb(s) > vs[1].ub(s))
      return true;
    if(s->state.is_inconsistent(r))
      return true;

    return enqueue_trigger(trigs[active], active, confl);
  }

  void root_simplify(void) { }

  void cleanup(void) {
    is_queued = false;
  }

protected:
  intvar vs[2];
  patom_t r;

  // Persistent state
  trigger trigs[3];
  int active;
  intvar::val_t prop_val;

  unsigned int gen;
  char status;
};

void imax_decomp(solver_data* s, intvar z, vec<intvar>& xs) {
  vec<clause_elt> elts;
  for(int k : irange(z.lb(s), z.ub(s)+1)) {
    elts.clear();
    elts.push(z <= k);
    for(intvar x : xs) {
      add_clause(s, x < k, z >= k);
      elts.push(x > k);
    }
    add_clause(*s, elts);
  }
  
  elts.clear();
  for(intvar x : xs) {
    if(x.ub(s) > z.ub(s))
      enqueue(*s, x <= z.ub(s), reason());
    elts.push(x >= z.lb(s));
  }
  add_clause(*s, elts);
}

bool int_max(solver_data* s, intvar z, vec<intvar>& xs, patom_t r) {
  // FIXME: Choose whether to use propagator or decomposition
  // imax_decomp(s, z, xs);
  if(!s->state.is_entailed_l0(r))
    WARN("Half-reified int_max not yet implemented.");

  new imax(s, z, xs);
  return true;
}

// Half-reified disequality
bool int_ne(solver_data* s, intvar x, intvar y, patom_t r) {
  intvar::val_t lb = std::max(x.lb(s), y.lb(s));
  intvar::val_t ub = std::min(x.ub(s), y.ub(s));
  if(ub < lb)
    return true;

  if(ub - lb < s->opts.eager_threshold)
  // if(0)
  {
    for(int k : irange(lb, ub+1)) {
      if(!add_clause(s, ~r, x != k, y != k))
        return false;
    }
  } else {
    new ineq(s, x, y, r);
  }
  return true;
}

#if 1
class pred_le_hr : public propagator, public prop_inst<pred_le_hr> {
  enum { gen_mask = ~(1<<31) };
  enum status { S_Active = 1, S_Red = 2 };
  enum mode { P_None = 0, P_LB = 1, P_UB = 2, P_LU = 3, P_Deact = 4 };

  // Misc helper functions
  inline bool watch_expired(int xi) {
    return ((unsigned int) xi)>>1 != fwatch_gen;
  }
  inline pval_t choose_cut(void) {
    return pred_lb(s, x) + kx + (pred_ub(s, y) + ky - pred_lb(s, x) -kx)/2;
  }
  inline pval_t lb(int pi) { return pred_lb(s, pi); }
  inline pval_t ub(int pi) { return pred_ub(s, pi); }
  // inline spval_t lb(int pi) { return pred_lb(s, pi); }
  // inline spval_t ub(int pi) { return pred_ub(s, pi); }

  // Deactivation triggers
  watch_result wake_fail(int xi) {
    // If this is an expired watch, drop it.
    if(watch_expired(xi)) {
      return Wt_Drop;
    }
    // If the propagator is already enabled,
    // ignore this.
    if(state&S_Active)
      return Wt_Keep;

    // Enqueue the propagator, to set ~r
    if(lb(x) + kx > ub(y) + ky) {
      mode = P_Deact;
      queue_prop();
      return Wt_Keep;
    }
    
    // Otherwise, find replacement watches
    // GKG: What's a good strategy?
    fwatch_gen = (fwatch_gen+1)&gen_mask;
    pval_t cut = choose_cut();
    attach(s, ge_atom(x, cut-kx+1), watch<&P::wake_fail>(fwatch_gen<<1, Wt_IDEM));
    attach(s, le_atom(y, cut-ky-1), watch<&P::wake_fail>((fwatch_gen<<1)|1, Wt_IDEM));
    return Wt_Drop;
  }

  watch_result wake_r(int _xi) {
    if(state & S_Red)
      return Wt_Keep;
    
    // If the constraint is activated, add watches on lb(x) and ub(y).
    if(!attached[0]) {
      s->pred_callbacks[x].push(watch<&P::wake_xs>(0, Wt_IDEM));
      attached[0] = true;
    }
    if(!attached[1]) {
      s->pred_callbacks[y^1].push(watch<&P::wake_xs>(1, Wt_IDEM));
      attached[1] = true;
    }

    trail_change(s->persist, state, (char) S_Active);
    mode = P_LU;
    queue_prop();
    return Wt_Keep;
  }

  watch_result wake_xs(int xi) {
    if(state&S_Red)
      return Wt_Keep;
    // If we've backtracked beyond the activation,
    // drop the watcher.
    if(!(state&S_Active)) {
      attached[xi] = false;
      return Wt_Drop;
    }
    mode |= xi ? P_UB : P_LB; 
    queue_prop();
    return Wt_Keep;
  }

  static void ex_r(void* ptr, int _p, pval_t _val,
    vec<clause_elt>& expl) {
    pred_le_hr* p(static_cast<pred_le_hr*>(ptr));
    vec_push(expl, le_atom(p->x, p->sep-p->kx -1), ge_atom(p->y, p->sep-p->ky));
  }

  static void ex_var(void* ptr, int var,
                        pval_t val, vec<clause_elt>& expl) {
    pred_le_hr* p(static_cast<pred_le_hr*>(ptr));
    expl.push(~(p->r));
    if(var) {
      expl.push(le_atom(p->x, val + p->ky - p->kx - 1));
    } else {
      expl.push(ge_atom(p->y, pval_inv(val) - p->ky + p->kx + 1));
    }
  }
public:
  pred_le_hr(solver_data* s, pid_t _x, pid_t _y, int _k, patom_t _r)
    : propagator(s), r(_r), x(_x), y(_y),
      kx(_k < 0 ? -_k : 0), ky(_k > 0 ? _k : 0),
      fwatch_gen(0),
      mode(P_None), state(0) {
    assert(x < s->state.p_vals.size());
    assert(y < s->state.p_vals.size());
    /*
    s->pred_callbacks[x].push(watch<&P::wake_xs>(0, Wt_IDEM));
    s->pred_callbacks[y^1].push(watch<&P::wake_xs>(1, Wt_IDEM));
    */
    // Pick an initial cut
//    x.attach(E_LB, watch_callback(wake_xs, this, 0, 1));
//    y.attach(E_UB, watch_callback(wake_xs, this, 1, 1));
    attached[0] = false; attached[1] = false;
    pval_t cut = choose_cut();
    attach(s, ge_atom(x, cut), watch<&P::wake_fail>(fwatch_gen<<1, Wt_IDEM));
    attach(s, le_atom(y, cut), watch<&P::wake_fail>((fwatch_gen<<1)|1, Wt_IDEM));

    attach(s, r, watch<&P::wake_r>(0, Wt_IDEM));
  }
  
  /*
  forceinline pval_t lb(pid_t p) { return s->state.p_vals[p]; }
  forceinline pval_t ub(pid_t p) { return pval_inv(s->state.p_vals[p^1]); }
  */
  
  bool propagate(vec<clause_elt>& confl) {
    // Non-incremental propagator
#ifdef LOG_PROP
    std::cerr << "[[Running ile]]" << std::endl;
#endif
    if(state&S_Red)
      return true;

    if(mode&P_Deact) {
      // Deactivation
      sep = pred_lb(s, x) + kx;
      assert(sep > pred_ub(s, y) + ky);
      if(!enqueue(*s, ~r, ex_thunk(ex_r, 0))) {
        return false;
      }
      trail_change(s->persist, state, (char) S_Red);
      return true;
    }

    if(!(state&S_Active))
      return true;

    assert(s->state.is_entailed(r));

    if(mode&P_LB) {
      // FIXME: Overflow problems abound
      if(lb(x) + kx > lb(y) + ky) {
        if(!enqueue(*s, ge_atom(y, lb(x) + kx - ky), expl_thunk { ex_var, this, 1 }))
          return false;
      }
    }
    if(mode&P_UB) {
      if(ub(y) + ky < ub(x) + kx) {
        if(!enqueue(*s, le_atom(x, ub(y) + ky - kx), expl_thunk { ex_var, this, 0}))
          return false;
      }
    }
    return true;
  }

  void root_simplify(void) {
    if(ub(x) + kx <= lb(y) + ky || s->state.is_inconsistent(r)) {
      state = S_Red;
      return;
    }

    if(s->state.is_entailed(r)) {
      // FIXME: Instead, disable the propagator
      // and swap in a pred_le builtin.
      state = S_Active; 
    }
  }

  void cleanup(void) { mode = P_None; is_queued = false; }

protected:
  // Parameters
  patom_t r;
  pid_t x;
  pid_t y;
  pval_t kx;
  pval_t ky;

  // Transient bookkeeping
  unsigned int fwatch_gen; // For watch expiration
  pval_t sep; // For explanation
  bool attached[2];

  // Persistent state
  char mode;
  char state;

};
#else
class pred_le_hr : public propagator, public prop_inst<pred_le_hr> {
  enum status { S_None = 0, S_Active = 1, S_Red = 2 };
  enum mode { P_LB = 1, P_UB = 2, P_LU = 3 };

  inline pval_t lb(int pi) { return pred_lb(s, pi); }
  inline pval_t ub(int pi) { return pred_ub(s, pi); }

  watch_result wake_r(int _xi) {
    if(state&S_Red)
      return Wt_Keep;
    
    set(status, S_Active);
    if(lb(y) + ky < lb(x) + kx) {
      mode |= P_LB;
      queue_prop();
    }
    if(ub(y) + ky < ub(x) + kx) {
    return Wt_Keep;
  }

  watch_result wake_x(int xi) {
    if(state&S_Red)
      return Wt_Keep;
    
    pval_t vx = lb(x);
    pval_t vy = (status&S_Active) ? pred_lb(y) : pred_ub(y);
    if(pred_lb(s, x) + kx > pred_lb(s, y) + ky) {
      queue_prop();
    }
    return Wt_Keep;
  }

  void ex_r(int _p, pval_t _val, vec<clause_elt>& expl) {
    vec_push(expl, le_atom(x, sep-p->kx - 1), ge_atom(y, sep-p->ky));
  }

  void ex_var(int var, pval_t val, vec<clause_elt>& expl) {
    expl.push(~r);
    if(var) {
      expl.push(le_atom(x, val + ky - kx - 1));
    } else {
      expl.push(ge_atom(y, pval_inv(val) - ky + kx + 1));
    }
  }
public:
  pred_le_hr(solver_data* s, pid_t _x, pid_t _y, int _k, patom_t _r)
    : propagator(s), r(_r), x(_x), y(_y),
      kx(_k < 0 ? -_k : 0), ky(_k > 0 ? _k : 0),
      fwatch_gen(0),
      mode(P_None), state(0) {
    assert(x < s->state.p_vals.size());
    assert(y < s->state.p_vals.size());

    s->pred_callbacks[x].push(watch<&P::wake_xs>(0, Wt_IDEM));
    s->pred_callbacks[y^1].push(watch<&P::wake_xs>(1, Wt_IDEM));

    attach(s, r, watch<&P::wake_r>(0, Wt_IDEM));
  }
  
  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cerr << "[[Running ile]]" << std::endl;
#endif
    if(state&S_Red)
      return true;

    if(pred_lb(s, x) + kx > pred_ub(s, y) + ky) {
      if(!enqueue(*s, ~r, ex_thunk(ex<&P::ex_r>, 0)))
        return false;
      set(state, S_Red);
      return true;
    }
    if(mode&P_Deact) {
      // Deactivation
      sep = pred_lb(s, x) + kx;
      assert(sep > pred_ub(s, y) + ky);
      if(!enqueue(*s, ~r, ex_thunk(ex_r, 0))) {
        return false;
      }
      trail_change(s->persist, state, (char) S_Red);
      return true;
    }

    if(!(state&S_Active))
      return true;

    assert(s->state.is_entailed(r));

    if(mode&P_LB) {
      // FIXME: Overflow problems abound
      if(lb(x) + kx > lb(y) + ky) {
        if(!enqueue(*s, ge_atom(y, lb(x) + kx - ky), expl_thunk { ex_var, this, 1 }))
          return false;
      }
    }
    if(mode&P_UB) {
      if(ub(y) + ky < ub(x) + kx) {
        if(!enqueue(*s, le_atom(x, ub(y) + ky - kx), expl_thunk { ex_var, this, 0}))
          return false;
      }
    }
    return true;
  }

  void root_simplify(void) {
    if(ub(x) + kx <= lb(y) + ky || s->state.is_inconsistent(r)) {
      state = S_Red;
      return;
    }

    if(s->state.is_entailed(r)) {
      // FIXME: Instead, disable the propagator
      // and swap in a pred_le builtin.
      state = S_Active; 
    }
  }

  void cleanup(void) { mode = P_None; is_queued = false; }

protected:
  // Parameters
  patom_t r;
  pid_t x;
  pid_t y;
  pval_t kx;
  pval_t ky;

  // Transient bookkeeping
  Tchar state;
};
#endif

class pred_le_hr_s : public propagator, public prop_inst<pred_le_hr_s> {
  enum status { S_Active = 1, S_Red = 2 };
  enum mode { P_None = 0, P_LB = 1, P_UB = 2, P_LU = 3, P_Deact = 4 };

  inline pval_t lb(int pi) { return pred_lb(s, pi); }
  inline pval_t ub(int pi) { return pred_ub(s, pi); }

  watch_result wake_r(int _xi) {
    if(state & S_Red)
      return Wt_Keep;
    
    trail_change(s->persist, state, (char) S_Active);
    mode = P_LU;
    queue_prop();
    return Wt_Keep;
  }

  watch_result wake_xs(int xi) {
    if(state&S_Red)
      return Wt_Keep;

    if(xi) {
      if(pred_ub(s, y) + ky < pred_ub(s, x) + kx) {
        mode |= P_UB;
        queue_prop();
      }
    } else {
      if(pred_lb(s, x) + kx > pred_lb(s, y) + ky) {
        mode |= P_LB;
        queue_prop();
      }
    }
    return Wt_Keep;
  }

  static void ex_r(void* ptr, int _x, pval_t p, vec<clause_elt>& elt) {
    return static_cast<P*>(ptr)->ex_r(_x, p, elt);
  }
  void ex_r(int _p, pval_t _val,
    vec<clause_elt>& expl) {
    vec_push(expl, le_atom(x, sep-kx -1), ge_atom(y, sep-ky));
  }

  static void ex_var(void* ptr, int x, pval_t p, vec<clause_elt>& elt) {
    return static_cast<P*>(ptr)->ex_var(x, p, elt);
  }
  void ex_var(int var, pval_t val, vec<clause_elt>& expl) {
    expl.push(~r);
    if(var) {
      expl.push(le_atom(x, val + ky - kx - 1));
    } else {
      expl.push(ge_atom(y, pval_inv(val) - ky + kx + 1));
    }
  }
public:
  pred_le_hr_s(solver_data* s, pid_t _x, pid_t _y, int _k, patom_t _r)
    : propagator(s), r(_r), x(_x), y(_y),
      kx(_k < 0 ? -_k : 0), ky(_k > 0 ? _k : 0),
      mode(P_None), state(0) {
    assert(x < s->state.p_vals.size());
    assert(y < s->state.p_vals.size());
    s->pred_callbacks[x].push(watch<&P::wake_xs>(0, Wt_IDEM));
    s->pred_callbacks[y^1].push(watch<&P::wake_xs>(1, Wt_IDEM));

    attach(s, r, watch<&P::wake_r>(0, Wt_IDEM));
  }
  
  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cerr << "[[Running ile_s]]" << std::endl;
#endif
    if(state&S_Red)
      return true;

    // if(mode&P_Deact) {
    if(pred_lb(s, x) + kx > pred_ub(s, y) + ky) {
      // Deactivation
      sep = pred_lb(s, x) + kx;
      assert(sep > pred_ub(s, y) + ky);
      if(!enqueue(*s, ~r, ex_thunk(ex_r, 0))) {
        return false;
      }
      trail_change(s->persist, state, (char) S_Red);
      return true;
    }

    if(!(state&S_Active))
      return true;

    assert(s->state.is_entailed(r));

    if(mode&P_LB) {
      // FIXME: Overflow problems abound
      if(lb(x) + kx > lb(y) + ky) {
        if(!enqueue(*s, ge_atom(y, lb(x) + kx - ky), expl_thunk { ex_var, this, 1 }))
          return false;
      }
    }
    if(mode&P_UB) {
      if(ub(y) + ky < ub(x) + kx) {
        if(!enqueue(*s, le_atom(x, ub(y) + ky - kx), expl_thunk { ex_var, this, 0}))
          return false;
      }
    }
    return true;
  }

  void root_simplify(void) {
    if(ub(x) + kx <= lb(y) + ky || s->state.is_inconsistent(r)) {
      state = S_Red;
      return;
    }

    if(s->state.is_entailed(r)) {
      // FIXME: Instead, disable the propagator
      // and swap in a pred_le builtin.
      state = S_Active; 
    }
  }

  void cleanup(void) { mode = P_None; is_queued = false; }

protected:
  // Parameters
  patom_t r;
  pid_t x;
  pid_t y;
  pval_t kx;
  pval_t ky;

  pval_t sep;

  // Persistent state
  char mode;
  char state;

};


/*
class ile_hr : public propagator {
  enum status { S_Active = 1, S_Red = 2 };

  static void wake_r(void* ptr, int _xi) {
    ile_hr* p(static_cast<ile_hr*>(ptr));
    if(p->state & S_Red)
      return;
    trail_change(p->s->persist, p->state, (char) S_Active);
    p->queue_prop();
  }

  static void wake_xs(void* ptr, int xi) {
    ile_hr* p(static_cast<ile_hr*>(ptr));
    if(p->state&S_Red)
      return;
    p->queue_prop();
  }

  static void ex_r(void* ptr, int sep, pval_t _val,
    vec<clause_elt>& expl) {
    ile_hr* p(static_cast<ile_hr*>(ptr));
    vec_push(expl, p->x < sep, p->y >= sep - p->k);
  }

  static void ex_var(void* ptr, int var,
                        pval_t val, vec<clause_elt>& expl) {
    ile_hr* p(static_cast<ile_hr*>(ptr));
    expl.push(~(p->r));
    if(var) {
      expl.push(p->x > to_int(val) + p->k);
    } else {
      expl.push(p->y < to_int(pval_inv(val)) - p->k);
    }
  }
public:
  ile_hr(solver_data* s, intvar _x, intvar _y, int _k, patom_t _r)
    : propagator(s), r(_r), x(_x), y(_y), k(_k) {
    x.attach(E_LB, watch_callback(wake_xs, this, 0, 1));
    y.attach(E_UB, watch_callback(wake_xs, this, 1, 1));
    attach(s, r, watch_callback(wake_r, this, 0, 1));
  }
  
  bool propagate(vec<clause_elt>& confl) {
    // Non-incremental propagator
#ifdef LOG_ALL
    std::cerr << "[[Running ile]]" << std::endl;
#endif
    if(state & S_Active) {
      if(lb(x) > lb(y) + k) {
        if(!y.set_lb(lb(x) - k, expl_thunk { ex_var, this, 1 }))
          return false;
      }
      if(ub(y) + k < ub(x)) {
        if(!x.set_ub(ub(y) + k, expl_thunk { ex_var, this, 0}))
          return false;
      }
    } else {
      if(lb(x) > ub(y) + k) {
        if(!enqueue(*s, ~r, expl_thunk { ex_r, this, (int) lb(x) }))
          return false;
        trail_change(s->persist, state, (char) S_Red);
      }
    }
    return true;
  }

  void root_simplify(void) {
    if(ub(x) <= lb(y) + k || s->state.is_inconsistent(r)) {
      state = S_Red;
      return;
    }

    if(s->state.is_entailed(r)) {
      state = S_Active; 
    }
  }

  void cleanup(void) { is_queued = false; }

protected:
  // Parameters
  patom_t r;
  intvar x;
  intvar y;
  int k;

  // Persistent state
  char state;
};
*/

bool pred_leq(solver_data* s, pid_t x, pid_t y, int k) {
  if(pred_ub(s, y) + k < pred_lb(s, x))
    return false;

  if(!enqueue(*s, ge_atom(y, pred_lb(s, x) - k), reason()))
    return false;
  if(!enqueue(*s, le_atom(x, pred_ub(s, y) + k), reason()))
    return false;

  s->infer.pred_ineqs[x].push({y, k});
  s->infer.pred_ineqs[y^1].push({x^1, k});
  return true;
}

bool int_leq(solver_data* s, intvar x, intvar y, int k) {
  return pred_leq(s, x.p, y.p, k);
  /*
  if(ub(y) + k < lb(x))
    return false;
  
  if(!enqueue(*s, y >= lb(x) - k, reason()))
    return false;
  if(!enqueue(*s, x <= ub(y) + k, reason()))
    return false;

  pid_t px = x.pid;
  pid_t py = y.pid;  
  s->infer.pred_ineqs[px].push({py, k});
  s->infer.pred_ineqs[py^1].push({px^1, k});
  return true;
  */
}

bool int_le(solver_data* s, intvar x, intvar y, int k, patom_t r) {
  if(s->state.is_entailed(r) && y.ub(s) + k < x.lb(s))
    return false;

  if(s->state.is_entailed(r))
    return int_leq(s, x, y, k);

  // new pred_le_hr(s, x.p, y.p, k, r);
  new pred_le_hr_s(s, x.p, y.p, k, r);
  return true;
}

bool pred_le(solver_data* s, pid_t x, pid_t y, int k, patom_t r) {
  if(s->state.is_entailed(r))
    return pred_leq(s, x, y, k);
  pval_t lb = std::max(pred_lb(s, x), pred_lb(s, y)+k);
  pval_t ub = std::min(pred_ub(s, x), pred_ub(s, y)+k);

  if(ub < lb) {
    if(pred_lb(s, x) > pred_ub(s, y) + k)
      return enqueue(*s, ~r, reason());
    return true;
  }

  if(ub - lb < s->opts.eager_threshold)
  // if(0)
  {
    if(pred_lb(s, y)+k < lb) {   
      if(!add_clause(s, ~r, ge_atom(y, lb-k)))
        return false;
    }
    if(pred_ub(s, x) > ub) {
      if(!add_clause(s, ~r, le_atom(x, ub)))
        return false;
    }
    for(pval_t v = lb; v < ub; ++v) {
      if(!add_clause(s, ~r, le_atom(x, v), ge_atom(y, v-k+1)))
        return false;
    }
  } else {
    // new pred_le_hr(s, x, y, k, r);
    new pred_le_hr_s(s, x, y, k, r);
  }
  return true;
}

bool int_abs(solver_data* s, intvar z, intvar x, patom_t r) {
  // iabs_decomp(s, z, x);
  if(!s->state.is_entailed_l0(r))
    WARN("Half-reified int_abs not yet implemented.");

  if(z.lb(s) < 0) {
    if(!enqueue(*s, z >= 0, reason ()))
      return false;
  }
  if(z.ub(s) < x.ub(s)) {
    if(!enqueue(*s, x <= z.ub(s), reason ()))
      return false;
  }
/*
  new iabs(s, z, x);
  */
  // x <= z
  /*
  pred_le(s, x.pid^1, z.pid, -2, at_True);
  pred_le(s, z.pid, x.pid^1, 2, at_True);
  */
  pred_le(s, x.p, z.p, 0, at_True);
  // (WARNING: Offsets here are fragile wrt offset changes)
  // (-x) <= z
  pred_le(s, x.p^1, z.p, -2, at_True);

  // x >= 0 -> (z <= x)
  pred_le(s, z.p, x.p, 0, x >= 0);
  // x <= 0 -> (z <= -x)
  if(!enqueue(*s, x <= z.ub(s), reason ()))
     return false;
   /*
   pred_le(s, x.pid^1, z.pid, -2, at_True);
   pred_le(s, z.pid, x.pid^1, 2, at_True);
   */
  pred_le(s, x.p, z.p, 0, at_True);
  // (WARNING: Offsets here are fragile wrt offset changes)
  // (-x) <= z
  pred_le(s, x.p^1, z.p, -2, at_True);

  // x >= 0 -> (z <= x)
  pred_le(s, z.p, x.p, 0, x >= 0);
  // x <= 0 -> (z <= -x)
  pred_le(s, z.p, x.p^1, 2, x <= 0);
  return true;
}

bool is_binary(solver_data* s, intvar x) { return x.lb(s) == 0 && x.ub(s) == 1; }

bool mul_bool(solver_data* s, intvar z, intvar x, patom_t y) {
  if(!add_clause(s, y, z == 0))
    return false;

  return int_le(s, z, x, 0, x.lb(s) >= 0 ? at_True : y)
    && int_le(s, x, z, 0, x.ub(s) <= 0 ? at_True : y);
}

#if 0
class isquare : public propagator, public prop_inst<isquare> {
  watch_result wake_x(int xi) {
    queue_prop();
    return  Wt_Keep;
  }


  isquare(solver_data* _s, intvar _z, intvar _x)
    : s(_s), z(_z), x(_x) {
      if(lb(z) < 0)
        set_lb(z, 0, reason());
      if(lb(x) >= 0) {
        if(lb(z) < lb(x) * lb(x)) {
          set_lb(z, lb(x) * lb(x), reason());
        }
        if(ub(z) > ub(x) * ub(x)) {
          set_ub(x, 
  }

  void expl_z(int xi, pval_t pval, vec<clause_elt>& expl) {
    if(!xi) {
      // lb(z)
      intvar::val_t v = std::max(1, to_int(pval));
    }
  }
  bool propagate(void) {

  }

  void cleanup(void) { changes = 0; is_queued = false; }

  char changes;
}
#endif

bool square_decomp(solver_data* s, intvar z, intvar x) {
  vec<intvar::val_t> abs_vals;
  vec<intvar::val_t> z_vals;
  for(intvar::val_t v : x.domain(s)) {
    z_vals.push(v*v);
    if(v < 0)
      abs_vals.push(-v);
    else
      abs_vals.push(v);
  }
  make_sparse(z, z_vals);

  uniq(abs_vals);
  for(intvar::val_t v : abs_vals) {
    if(!add_clause(s, z > v*v, x <= v))
      return false;
    if(!add_clause(s, z > v*v, x >= -v))
      return false;
    if(!add_clause(s, z < v*v, x <= -v, x >= v))
      return false;
  }
  return true;
}

bool int_mul(solver_data* s, intvar z, intvar x, intvar y, patom_t r) {
  if(!s->state.is_entailed_l0(r))
    WARN("Half-reified int_mul not yet implemented.");

  if(is_binary(s, x)) {
    return mul_bool(s, z, y, x >= 1);
  } else if(is_binary(s, y)) {
    return mul_bool(s, z, x, y >= 1);
  }

  if(x.p == y.p) {
    if(x.ub(s) - x.lb(s) < s->opts.eager_threshold) {
      return square_decomp(s, z, x);
    }
  }

  // imul_decomp(s, z, x, y);
  if(z.lb(s) >= 0) {
    if(x.lb(s) >= 0 || y.lb(s) >= 0) {
      new iprod_nonneg(s, r, z, x, y);
      return true;
    } else if(x.ub(s) <= 0) {
      new iprod_nonneg(s, r, z, -x, y);
      return true;
    } else if(y.ub(s) <= 0) {
      new iprod_nonneg(s, r, z, x, -y);
      return true;
    }
  } else if(z.ub(s) <= 0) {
    if(x.lb(s) >= 0 || y.ub(s) <= 0) {
      new iprod_nonneg(s, r, -z, x, -y);
      return true;
    }  else if(x.ub(s) <= 0 || y.lb(s) >= 0) {
      new iprod_nonneg(s, r, -z, -x, y);
      return true;
    } 
  }
  new iprod(s, z, x, y);
  return true;
}

class idiv_nonneg : public propagator, public prop_inst<idiv_nonneg> {
  // Queueing
  enum Status { S_Red = 2 };

  watch_result wake(int _xi) {
    if(status & S_Red)
      return Wt_Keep;

    queue_prop();
    return Wt_Keep;
  }

  // Explanations. Naive for now
  // z >= ceil[ lb(x)+1, ub(y) ] - 1
  void ex_z_lb(int _xi, pval_t p, vec<clause_elt>& expl) {
    int z_lb = z.lb_of_pval(p);
    int x_lb = (iceil(lb_prev(x)+1, ub(y)) - 1 >= z_lb) ? lb_prev(x) : lb(x);
    int y_ub = (iceil(x_lb+1, ub_prev(y)) - 1 >= z_lb) ? ub_prev(y) : ub(y);
    expl.push(x < x_lb);
    expl.push(y > y_ub);
  }

  // z <= ub(x)/lb(y); 
  void ex_z_ub(int _xi, pval_t pval, vec<clause_elt>& expl) {
    int z_ub = z.ub_of_pval(pval);
    
    int x_ub = (ub_prev(x) / lb(y) <= z_ub) ? ub_prev(x) : ub(x);
    int y_lb = (x_ub / lb_prev(y) <= z_ub) ? lb_prev(y) : lb(y);
    // Can probably weaken further
    expl.push(x > x_ub);
    expl.push(y < y_lb);
  }

  // x >= lb(z) * lb(y)
  void ex_x_lb(int xi, pval_t p, vec<clause_elt>& expl) {
    int x_lb = x.lb_of_pval(p);

    int z_lb = (lb_prev(z) * lb(y) >= x_lb) ? lb_prev(z) : lb(z);
    int y_lb = (z_lb * lb_prev(y) >= x_lb) ? lb_prev(y) : lb(y);
    expl.push(z < z_lb);
    expl.push(y < y_lb);
  }
  // x <= (ub(z)+1) * ub(y) - 1;
  void ex_x_ub(int xi, pval_t p, vec<clause_elt>& expl) {
    int x_ub = x.ub_of_pval(p) + 1;
    
    int z_ub = ((ub_prev(z)+1) * ub(y) <= x_ub) ? ub_prev(z) : ub(z);
    int y_ub = ((z_ub+1) * ub_prev(y) <= x_ub) ? ub_prev(y) : ub(y);
    expl.push(z > z_ub);
    expl.push(y > y_ub);
  }

  // y >= iceil(lb(x)+1, ub(z)+1);
  void ex_y_lb(int xi, pval_t p, vec<clause_elt>& expl) {
    int y_lb = y.lb_of_pval(p);

    int x_lb = (iceil(lb_prev(x)+1, ub(z)+1) >= y_lb) ? lb_prev(x) : lb(x);
    int z_ub = (iceil(x_lb+1, ub_prev(z)+1) >= y_lb) ? ub_prev(z) : ub(z);
    expl.push(z > z_ub);
    expl.push(x < x_lb);
  }
  // y <= ub(x)/lb(z);
  void ex_y_ub(int xi, pval_t p, vec<clause_elt>& expl) {
    int y_ub = y.ub_of_pval(p);

    int x_ub = (ub_prev(x)/lb(z) <= y_ub) ? ub_prev(x) : ub(x);
    int z_lb = (x_ub/lb_prev(z) <= y_ub) ? lb_prev(z) : lb(z);
    expl.push(z < z_lb);
    expl.push(x > x_ub);
  }

public:
  idiv_nonneg(solver_data* s, patom_t _r, intvar _z, intvar _x, intvar _y)
    : propagator(s), r(_r), z(_z), x(_x), y(_y), status(0) {
      assert(s->state.is_entailed_l0(r));
    z.attach(E_LU, watch_callback(wake_default, this, 2));
    x.attach(E_LU, watch_callback(wake_default, this, 0));
    y.attach(E_LU, watch_callback(wake_default, this, 1));
  }

  bool propagate(vec<clause_elt>& confl) {
#ifdef LOG_PROP
    std::cerr << "[[Running idiv(+)]]" << std::endl;
#endif
    // What do the constraints look like?
    // (1) x >= z * y
    // (2) x < (z+1) * y
    // Propagate x
    int x_low = lb(z) * lb(y);
    int x_high = (ub(z)+1) * ub(y) - 1;
    if(x_low > lb(x) && !set_lb(x, x_low, ex_thunk(ex<&P::ex_x_lb>, 0, expl_thunk::Ex_BTPRED)))
      return false;
    if(x_high < ub(x) && !set_ub(x, x_high, ex_thunk(ex<&P::ex_x_ub>, 0, expl_thunk::Ex_BTPRED)))
      return false;

    // ... and y
    int y_low = iceil(lb(x)+1, ub(z)+1);
    int y_high = ub(x)/lb(z);
    if(y_low > lb(y) &&
      !set_lb(y, y_low, ex_thunk(ex<&P::ex_y_lb>, 0, expl_thunk::Ex_BTPRED)))
      return false;
    if(y_high < ub(y) &&
      !set_ub(y, y_low, ex_thunk(ex<&P::ex_y_ub>, 0, expl_thunk::Ex_BTPRED)))
      return false;

    // ... and z
    int z_low = iceil(lb(x)+1, ub(y)) - 1;
    int z_high = ub(x)/lb(y); 
    if(z_low > lb(z)
      && !set_lb(z, z_low, ex_thunk(ex<&P::ex_z_lb>, 0, expl_thunk::Ex_BTPRED)))
      return false;

    if(z_high > ub(z)
      && !set_ub(z, z_high, ex_thunk(ex<&P::ex_z_ub>, 0, expl_thunk::Ex_BTPRED)))
      return false;
    /*
    int z_low = lb(x) / ub(y);
    if(z_low > lb(z)) {
      if(!set_lb(z, z_low, ex_thunk(ex<&P::ex_z_lb>,0, expl_thunk::Ex_BTPRED)))
        return false;
    }
    int z_high = ub(x) / lb(y);
    if(z_high < ub(z)) {
      if(!set_ub(z, z_high, ex_thunk(ex<&P::ex_z_ub>,0, expl_thunk::Ex_BTPRED)))
        return false;
    }

    // Now x: smallest x s.t. x / lb(y) >= lb(z)
    int x_low = lb(z) * lb(y);
    if(x_low > lb(x)) {
      if(!set_lb(x, x_low, ex_thunk(ex<&P::ex_x_lb>, 0, expl_thunk::Ex_BTPRED)))
        return false;
    }
    // Greatest x s.t. x / ub(y) <= ub(z)
    int x_high = (ub(z)+1) * ub(y) - 1;
    if(x_high < ub(x)) {
      if(!set_ub(x, x_high, ex_thunk(ex<&P::ex_x_ub>, 0, expl_thunk::Ex_BTPRED)))
        return false;
    }

    // Same for y: smallest y s.t. lb(x) / y <= ub(z)
    int y_low = iceil(lb(x), ub(z));
    if(y_low > lb(y)) {
      if(!set_lb(y, y_low, ex_thunk(ex<&P::ex_y_lb>, 0, expl_thunk::Ex_BTPRED)))
        return false;
    }
    int y_high = iceil(ub(x), lb(z));
    if(y_high < ub(y)) {
      if(!set_ub(y, y_high, ex_thunk(ex<&P::ex_y_lb>, 0, expl_thunk::Ex_BTPRED)))
        return false;
    }
    */

    return true;
  }

  patom_t r;
  intvar z;
  intvar x;
  intvar y;

  char status;
};

bool post_idiv_nonneg(solver_data* s, intvar z, intvar x, intvar y) {
  // FIXME: Cheapo implementation appears to be wrong
#if 0
  intvar x_low(new_intvar(s, z.lb(s) * y.lb(s), z.ub(s) * y.ub(s)));
  intvar x_high(new_intvar(s, z.lb(s) * y.lb(s) - 1, (z.ub(s)+1) * y.ub(s) - 1));
  new iprod_nonneg(s, at_True, x_low, z, y);
  new iprod_nonneg(s, at_True, x_high+1, z+1, y);
  int_leq(s, x_low, x, 0);
  int_leq(s, x, x_high, 0);
#else
  if(z.lb(s) < 0 && !enqueue(*s, z >= 0, reason()))
    return false;
  new idiv_nonneg(s, at_True, z, x, y);
#endif
  return true;
}

bool int_div(solver_data* s, intvar z, intvar x, intvar y, patom_t r) {
  assert(r == at_True);

  if(!enqueue(*s, y != 0, reason()))
    return false;

  // Check the sign cases.
  // TODO: This doesn't handle the case when, say, z & x are fixed-sign.
  if(x.lb(s) >= 0) {
    if(y.lb(s) >= 0) {
      return post_idiv_nonneg(s, z, x, y);
    } else if(y.ub(s) <= 0) {
      return post_idiv_nonneg(s, -z, x, -y);
    }
  } else if(x.ub(s) <= 0) {
    if(y.lb(s) >= 0) {
      return post_idiv_nonneg(s, -z, -x, y);
    } else if(y.ub(s) <= 0) {
      return post_idiv_nonneg(s, z, -x, -y);
    }
  }
  // TODO: Implement non-sign-fixed case.
  NOT_YET;
  return false;
}

}
