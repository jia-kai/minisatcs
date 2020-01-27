/***************************************************************************************[Solver.cc]
Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
Copyright (c) 2007-2010, Niklas Sorensson

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#include <math.h>

#include "minisat/core/Solver.h"
#include "minisat/mtl/Sort.h"
#include "minisat/utils/System.h"

using namespace Minisat;

//=================================================================================================
// Options:

static const char* _cat = "CORE";

static DoubleOption opt_var_decay(_cat, "var-decay",
                                  "The variable activity decay factor", 0.95,
                                  DoubleRange(0, false, 1, false));
static DoubleOption opt_clause_decay(_cat, "cla-decay",
                                     "The clause activity decay factor", 0.999,
                                     DoubleRange(0, false, 1, false));
static DoubleOption opt_random_var_freq(
        _cat, "rnd-freq",
        "The frequency with which the decision heuristic tries to choose a "
        "random variable",
        0, DoubleRange(0, true, 1, true));
static IntOption opt_random_seed(_cat, "rnd-seed",
                                 "Used by the random variable selection",
                                 92702102, IntRange(0, INT_MAX));
static IntOption opt_ccmin_mode(
        _cat, "ccmin-mode",
        "Controls conflict clause minimization (0=none, 1=basic, 2=deep)", 2,
        IntRange(0, 2));
static IntOption opt_phase_saving(
        _cat, "phase-saving",
        "Controls the level of phase saving (0=none, 1=limited, 2=full)", 2,
        IntRange(0, 2));
static BoolOption opt_rnd_pol(_cat, "rnd-pol",
                              "Randomize the polarity for decision", false);
static BoolOption opt_rnd_init_act(_cat, "rnd-init",
                                   "Randomize the initial activity", false);
static BoolOption opt_luby_restart(_cat, "luby",
                                   "Use the Luby restart sequence", true);
static IntOption opt_restart_first(_cat, "rfirst", "The base restart interval",
                                   100, IntRange(1, INT32_MAX));
static DoubleOption opt_restart_inc(_cat, "rinc",
                                    "Restart interval increase factor", 2,
                                    DoubleRange(1, false, HUGE_VAL, false));
static DoubleOption opt_garbage_frac(_cat, "gc-frac",
                                     "The fraction of wasted memory allowed "
                                     "before a garbage collection is triggered",
                                     0.20,
                                     DoubleRange(0, false, HUGE_VAL, false));

/* ================== LeqWatcher ================== */
//! watcher for LEQ clauses
struct Solver::LeqWatcher {
    //! bound of the LEQ
    uint32_t bound : 15;
    //! sign of this var in LEQ
    uint32_t sign : 1;
    //! number of lits in the LEQ
    uint32_t size : 16;

    CRef cref;

    //! offset of the corresponding LeqStatus in ClauseAllocator
    CRef status_ref() const {
        return cref + size + LeqStatus::OFFSET_IN_CLAUSE;
    }

    LeqStatus& status(ClauseAllocator& ca) const {
        return *ca.lea_as<LeqStatus>(status_ref());
    }

    //! LEQ = 0 <=> (nr_true >= bound_true)
    int bound_true() const { return bound + 1; }

    //! LEQ = 1 <=> (nr_false >= bound_false)
    int bound_false() const { return size - bound; }
};

/* ================== LeqStatusModLog ================== */
//! modification log of LeqStatus
struct Solver::LeqStatusModLog {
    //! whether nr_true in the status is added
    uint32_t is_true : 1;

    //! if set to 1, imply_type should be cleared during unwinding
    uint32_t imply_type_clear : 1;

    CRef status_ref : 30;

    LeqStatus& status(ClauseAllocator& ca) const {
        return *ca.lea_as<LeqStatus>(status_ref);
    }
};

//=================================================================================================
// Constructor/Destructor:

Solver::Solver()
        :

          // Parameters (user settable):
          //
          verbosity(0),
          var_decay(opt_var_decay),
          clause_decay(opt_clause_decay),
          random_var_freq(opt_random_var_freq),
          luby_restart(opt_luby_restart),
          ccmin_mode(opt_ccmin_mode),
          phase_saving(opt_phase_saving),
          rnd_pol(opt_rnd_pol),
          rnd_init_act(opt_rnd_init_act),
          garbage_frac(opt_garbage_frac),
          restart_first(opt_restart_first),
          restart_inc(opt_restart_inc)

          // Parameters (the rest):
          //
          ,
          learntsize_factor((double)1 / (double)3),
          learntsize_inc(1.1)

          // Parameters (experimental):
          //
          ,
          learntsize_adjust_start_confl(100),
          learntsize_adjust_inc(1.5)

          // Statistics: (formerly in 'SolverStats')
          //
          ,
          solves(0),
          starts(0),
          decisions(0),
          rnd_decisions(0),
          propagations(0),
          conflicts(0),
          dec_vars(0),
          clauses_literals(0),
          learnts_literals(0),
          max_literals(0),
          tot_literals(0)

          ,
          ok(true),
          cla_inc(1),
          var_inc(1),
          watches{ca},
          leq_watches{ca},
          qhead(0),
          simpDB_assigns(-1),
          simpDB_props(0),
          order_heap(VarOrderLt{this}),
          progress_estimate(0),
          remove_satisfied(true)

          // Resource constraints:
          //
          ,
          conflict_budget(-1),
          propagation_budget(-1),
          asynch_interrupt(false),

          random_state{static_cast<uint64_t>(opt_random_seed)}

{
    static_assert(sizeof(LeqWatcher) == sizeof(uint64_t));
    static_assert(sizeof(LeqStatusModLog) == sizeof(uint32_t));
}

Solver::~Solver() = default;

//=================================================================================================
// Minor methods:

// Creates a new SAT variable in the solver. If 'decision' is cleared, variable
// will not be used as a decision variable (NOTE! This has effects on the
// meaning of a SATISFIABLE result).
//
Var Solver::newVar(bool sign, bool dvar) {
    int v = nVars();
    watches.init(mkLit(v, false));
    watches.init(mkLit(v, true));
    leq_watches.init(v);
    assigns.push(l_Undef);
    vardata.push(VarData{CRef_Undef, 0});
    activity.push(rnd_init_act ? random_state.uniform() * 0.00001 : 0);
    var_preference.push(0);
    seen.push(0);
    polarity.push(sign);
    decision.push();
    trail.capacity(v + 1);
    setDecisionVar(v, dvar);
    return v;
}

bool Solver::addClause_(vec<Lit>& ps) {
    assert(decisionLevel() == 0);
    if (!ok)
        return false;

    // Check if clause is satisfied and remove false/duplicate literals:
    sort(ps);
    Lit p;
    int i, j;
    for (i = j = 0, p = lit_Undef; i < ps.size(); i++) {
        if (value(ps[i]) == l_True || ps[i] == ~p)
            return true;
        else if (value(ps[i]) != l_False && ps[i] != p)
            ps[j++] = p = ps[i];
    }
    ps.shrink(i - j);

    if (ps.size() == 0)
        return ok = false;
    else if (ps.size() == 1) {
        uncheckedEnqueue(ps[0]);
        return ok = (propagate() == CRef_Undef);
    } else {
        CRef cr = ca.alloc(ps, false);
        clauses.push(cr);
        attachClause(cr);
    }

    return true;
}

bool Solver::addLeqAssign_(vec<Lit>& ps, int bound, Lit dst) {
    assert(decisionLevel() == 0);
    if (!ok)
        return false;

    canonize_leq_clause(ps, bound);

    if (auto r = try_leq_clause_const_prop(ps, dst, bound); r.has_value()) {
        return r.value();
    }
    if (bound == 0) {
        // We do not add watchers on dst, so we handle the case when bound is
        // zero because dst = 1 can imply all lits in this case
        vec<Lit> tmp;
        ps.copyTo(tmp);
        ps.push(dst);
        if (!addClause_(ps)) {
            return false;
        }
        for (Lit i : tmp) {
            if (!addClause(~i, ~dst)) {
                return false;
            }
        }
        return true;
    }
    assert(1 <= bound && bound < ps.size());

    if (ps.size() == 1) {
        Lit a = dst, b = ~ps[0];
        // now the constraint is a == b
        return addClause(~a, b) && addClause(~b, a);
    }

    add_leq_and_setup_watchers(ps, dst, bound);
    return true;
}

void Solver::canonize_leq_clause(vec<Lit>& ps, int& bound) {
    sort(ps);
    Lit p;
    int i, j;
    for (i = j = 0, p = lit_Undef; i < ps.size(); i++) {
        if (value(ps[i]) == l_True) {
            --bound;
            continue;
        }

        if (value(ps[i]) == l_False) {
            continue;
        }

        if (ps[i] == ~p) {
            --j;  // remove previous literal
            --bound;
            if (j > 0) {
                p = ps[j - 1];
            } else {
                p = lit_Undef;
            }
            continue;
        }

        ps[j++] = p = ps[i];
    }
    ps.shrink(i - j);
}

std::optional<bool> Solver::try_leq_clause_const_prop(const vec<Lit>& ps,
                                                      Lit dst, int bound) {
    lbool val = l_Undef;
    if (ps.size() <= bound) {
        val = l_True;
    } else if (bound < 0) {
        val = l_False;
    }
    if (val != l_Undef) {
        if (value(dst) == l_Undef) {
            // setup the value for dst
            uncheckedEnqueue(val == l_True ? dst : ~dst);
            return ok = (propagate() == CRef_Undef);
        }
        if (value(dst) == val) {
            return true;
        }
        return ok = false;
    }
    return std::nullopt;
}

void Solver::add_leq_and_setup_watchers(vec<Lit>& ps, Lit dst, int bound) {
    if (ps.size() >= (1 << 14) - 10) {
        throw std::runtime_error{"LEQ too large"};
    }
    CRef cr = ca.alloc(ps, false, dst, bound);
    clauses.push(cr);
    assert(ca.ael(&ca[cr].leq_status()) - cr ==
           ps.size() + LeqStatus::OFFSET_IN_CLAUSE);

    // note that duplicated lits are naturally handled by adding multiple
    // watchers

    for (int i = 0; i < ps.size(); ++i) {
        Lit p = ps[i];
        LeqWatcher watcher = {
                .bound = static_cast<uint32_t>(bound),
                .sign = sign(p),
                .size = static_cast<uint32_t>(ps.size()),
                .cref = cr,
        };
        leq_watches[var(p)].push(watcher);
    }

    clauses_literals += ps.size() + 1;
}

void Solver::attachClause(CRef cr) {
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    assert(!c.is_leq());
    watches[~c[0]].push(Watcher(cr, c[1]));
    watches[~c[1]].push(Watcher(cr, c[0]));
    if (c.learnt())
        learnts_literals += c.size();
    else
        clauses_literals += c.size();
}

void Solver::detachClause(CRef cr, bool strict) {
    const Clause& c = ca[cr];
    assert(!c.is_leq());
    assert(c.size() > 1);

    if (strict) {
        remove(watches[~c[0]], Watcher(cr, c[1]));
        remove(watches[~c[1]], Watcher(cr, c[0]));
    } else {
        // Lazy detaching: (NOTE! Must clean all watcher lists before garbage
        // collecting this clause)
        watches.smudge(~c[0]);
        watches.smudge(~c[1]);
    }

    if (c.learnt())
        learnts_literals -= c.size();
    else
        clauses_literals -= c.size();
}

void Solver::removeClause(CRef cr) {
    Clause& c = ca[cr];
    if (c.is_leq()) {
        auto remove_self_reason_ref = [this, cr](Var var) {
            CRef& reason = vardata[var].reason;
            if (reason == cr) {
                reason = CRef_Undef;
            }
        };
        for (int i = 0, it = c.size(); i < it; ++i) {
            Var v = var(c[i]);
            leq_watches.smudge(v);
            remove_self_reason_ref(v);
        }
        remove_self_reason_ref(var(c.leq_dst()));
        clauses_literals -= c.size() + 1;
    } else {
        detachClause(cr);
        // Don't leave pointers to free'd memory!
        if (locked_disj(c))
            vardata[var(c[0])].reason = CRef_Undef;
    }
    c.mark(1);
    ca.free(cr);
}

bool Solver::satisfied(const Clause& c) const {
    if (c.is_leq()) {
        auto vdst = value(c.leq_dst());
        if (vdst.is_not_undef()) {
            LeqStatus s = c.leq_status();
            int bound = c.leq_bound();
            bool vleq;
            if (s.nr_true >= bound + 1) {
                vleq = false;
            } else if (s.nr_decided - s.nr_true >= c.size() - bound) {
                vleq = true;
            } else {
                return false;
            }
            return vdst.val_is(vleq);
        }
        return false;
    }
    for (int i = 0; i < c.size(); i++) {
        if (value(c[i]) == l_True)
            return true;
    }
    return false;
}

void Solver::cancelUntil(int level) {
    if (decisionLevel() > level) {
        TrailSep sep = trail_lim[level];
        for (int c = trail.size() - 1; c >= sep.lit; --c) {
            Var x = var(trail[c]);
            assigns[x] = l_Undef;
            if (phase_saving > 1 ||
                ((phase_saving == 1) && c > trail_lim.last().lit)) {
                polarity[x] = sign(trail[c]);
            }
            insertVarOrder(x);
        }

        for (int i = trail_leq_stat.size() - 1; i >= sep.leq; --i) {
            LeqStatusModLog log = trail_leq_stat[i];
            LeqStatus& s = log.status(ca);
            s.decr(log.is_true, 1);
            s.clear_imply_type_with(log.imply_type_clear);
        }

        qhead = trail_lim[level].lit;
        trail.shrink(trail.size() - sep.lit);
        trail_leq_stat.shrink(trail_leq_stat.size() - sep.leq);
        trail_lim.shrink(trail_lim.size() - level);
    }
}

//=================================================================================================
// Major methods:

Lit Solver::pickBranchLit() {
    Var next = var_Undef;

    // Random decision:
    if (random_var_freq && !order_heap.empty() &&
        random_state.binomial(random_var_freq)) {
        next = order_heap[random_state.randint(order_heap.size())];
        if (value(next) == l_Undef && decision[next])
            rnd_decisions++;
    }

    // Activity based decision:
    while (next == var_Undef || value(next) != l_Undef || !decision[next])
        if (order_heap.empty()) {
            next = var_Undef;
            break;
        } else
            next = order_heap.removeMin();

    return next == var_Undef ? lit_Undef
                             : mkLit(next, rnd_pol ? random_state.binomial(0.5)
                                                   : polarity[next]);
}

/*_________________________________________________________________________________________________
|
|  analyze : (confl : Clause*) (out_learnt : vec<Lit>&) (out_btlevel : int&)  ->
[void]
|
|  Description:
|    Analyze conflict and produce a reason clause.
|
|    Pre-conditions:
|      * 'out_learnt' is assumed to be cleared.
|      * Current decision level must be greater than root level.
|
|    Post-conditions:
|      * 'out_learnt[0]' is the asserting literal at level 'out_btlevel'.
|      * If out_learnt.size() > 1 then 'out_learnt[1]' has the greatest decision
level of the |        rest of literals. There may be others from the same level
though.
|
|________________________________________________________________________________________________@*/
void Solver::analyze(CRef confl, vec<Lit>& out_learnt, int& out_btlevel) {
    /*
     * See http://satassociation.org/articles/FAIA185-0131.pdf for a formal
     * description of CDCL, and a demonstration of graph building process is
     * available at
     * https://cse442-17f.github.io/Conflict-Driven-Clause-Learning/
     *
     * Key properties in the implication graph:
     *  1. node + its antecedents = clause that implied this node
     *  2. a node has opposite signs in incoming and outgoing edges
     *
     * Original clause learning:
     *      Goal: find a vertex cut consisting of nodes from other levels on the
     *      graph that leads to the conflict. Negation of literals of the cut
     *      must be true.
     *
     *      S := set of literals in the conflict clause
     *      invariance: S must be true, but not satisfied now
     *      while (i in S such that (this_level(i) and precedents(i))) {
     *          S := S - {i} + precedents(i)
     *          // correct because i has opposite signs (see properties above)
     *      }
     *
     * With UIP (Unit Implication Points): break while loop if i is the only
     * node in S at this decision level.
     *
     * Clause learning can be naturally extended to handle clauses other than
     * disjunction as long as the implication graph can be built. When a
     * conflict is encountered where p_1, ..., p_m are the antecedents, we can
     * prove that {-p_1, ..., -p_m} must be true, and -p_i can be replaced by
     * the disjunction negation of antecedents of p_i, until a cut of literals
     * on earlier decision levels is found.
     *
     * Below is a very clever implmentation of clause learning without UIP.
     */

    int pathC = 0;
    Lit p = lit_Undef;

    // Generate conflict clause:
    //
    out_learnt.push();  // (leave room for the asserting literal)
    int index = trail.size() - 1;

    auto add_antecedent = [&](Lit q) __attribute__((always_inline)) {
        // add ~q as a visited antecident (in the graph it is ~q, and q is added
        // to the learnt clause)

        if (!seen[var(q)] && level(var(q)) > 0) {
            varBumpActivity(var(q));
            seen[var(q)] = 1;
            if (level(var(q)) >= decisionLevel()) {
                // Only the decision var at current level should be added to the
                // learnt clause. We keep a counter here instead of adding the
                // var, so it would be processed later.
                pathC++;
            } else {
                out_learnt.push(q);
            }
        }
    };

    do {
        assert(confl != CRef_Undef);  // (otherwise should be UIP)
        Clause& c = ca[confl];

        if (c.is_leq()) {
            // note: this code is duplicated in litRedundant
            LeqStatus status = c.leq_status();
            assert(status.imply_type);
            int is_true = status.precond_is_true,
                size = is_true ? status.nr_true
                               : status.nr_decided - status.nr_true;
            for (int i = 0; i < size; ++i) {
                add_antecedent(c[i] ^ is_true);
            }
            if (status.imply_type != LeqStatus::IMPLY_DST) {
                add_antecedent(c.leq_dst() ^ is_true);
            }
        } else {
            if (c.learnt())
                claBumpActivity(c);

            for (int j = (p == lit_Undef) ? 0 : 1; j < c.size(); j++) {
                // note: c[0] is the implied value (see propagate())
                add_antecedent(c[j]);
            }
        }

        // Select next clause to look at:
        while (!seen[var(trail[index--])])
            ;
        p = trail[index + 1];
        confl = reason(var(p));
        seen[var(p)] = 0;
        pathC--;

    } while (pathC > 0);
    out_learnt[0] = ~p;

    // Simplify conflict clause:
    //
    int i, j;
    out_learnt.copyTo(analyze_toclear);
    if (ccmin_mode == 2) {
        abstract_level_set_t abstract_level = 0;
        for (i = 1; i < out_learnt.size(); i++)
            abstract_level |= abstractLevel(
                    var(out_learnt[i]));  // (maintain an abstraction of levels
                                          // involved in conflict)

        for (i = j = 1; i < out_learnt.size(); i++)
            if (reason(var(out_learnt[i])) == CRef_Undef ||
                !litRedundant(out_learnt[i], abstract_level))
                out_learnt[j++] = out_learnt[i];

    } else if (ccmin_mode == 1) {
        for (i = j = 1; i < out_learnt.size(); i++) {
            Var x = var(out_learnt[i]);

            if (reason(x) == CRef_Undef)
                out_learnt[j++] = out_learnt[i];
            else {
                Clause& c = ca[reason(var(out_learnt[i]))];
                if (c.is_leq()) {
                    throw std::runtime_error{
                            "ccmin=1 for LEQ clause unimplemented"};
                }
                for (int k = 1; k < c.size(); k++)
                    if (!seen[var(c[k])] && level(var(c[k])) > 0) {
                        out_learnt[j++] = out_learnt[i];
                        break;
                    }
            }
        }
    } else
        i = j = out_learnt.size();

    max_literals += out_learnt.size();
    out_learnt.shrink(i - j);
    tot_literals += out_learnt.size();

    // Find correct backtrack level:
    //
    if (out_learnt.size() == 1)
        out_btlevel = 0;
    else {
        int max_i = 1;
        // Find the first literal assigned at the next-highest level:
        for (int i = 2; i < out_learnt.size(); i++)
            if (level(var(out_learnt[i])) > level(var(out_learnt[max_i])))
                max_i = i;
        // Swap-in this literal at index 1:
        Lit p = out_learnt[max_i];
        out_learnt[max_i] = out_learnt[1];
        out_learnt[1] = p;
        out_btlevel = level(var(p));
    }

    for (int j = 0; j < analyze_toclear.size(); j++)
        seen[var(analyze_toclear[j])] = 0;  // ('seen[]' is now cleared)
}

// Check if 'p' can be removed. 'abstract_levels' is used to abort early if the
// algorithm is visiting literals at levels that cannot be removed later.
bool Solver::litRedundant(Lit p, abstract_level_set_t abstract_levels) {
    // A lit is redundant if all seen vars can form a cut to isolate this lit
    // (i.e. it can be implied from other seen vars).
    // If a lit is in an unvisited level, it can not be redundant
    analyze_stack.clear();
    analyze_stack.push(p);
    auto add_antecedent =
            [ this, top = analyze_toclear.size(), abstract_levels ](Lit p)
                    __attribute__((always_inline)) {
        if (!seen[var(p)] && level(var(p)) > 0) {
            if (reason(var(p)) != CRef_Undef &&
                (abstractLevel(var(p)) & abstract_levels) != 0) {
                seen[var(p)] = 1;
                analyze_stack.push(p);
                analyze_toclear.push(p);
            } else {
                for (int j = top; j < analyze_toclear.size(); j++)
                    seen[var(analyze_toclear[j])] = 0;
                analyze_toclear.shrink(analyze_toclear.size() - top);
                return false;
            }
        }
        return true;
    };
    while (analyze_stack.size() > 0) {
        assert(reason(var(analyze_stack.last())) != CRef_Undef);
        Clause& c = ca[reason(var(analyze_stack.last()))];
        analyze_stack.pop();

        if (c.is_leq()) {
            LeqStatus status = c.leq_status();
            assert(status.imply_type);
            int is_true = status.precond_is_true,
                size = is_true ? status.nr_true
                               : status.nr_decided - status.nr_true;
            for (int i = 0; i < size; ++i) {
                if (!add_antecedent(c[i] ^ is_true)) {
                    return false;
                }
            }
            if (status.imply_type != LeqStatus::IMPLY_DST) {
                if (!add_antecedent(c.leq_dst() ^ is_true)) {
                    return false;
                }
            }
        } else {
            for (int i = 1; i < c.size(); i++) {
                if (!add_antecedent(c[i])) {
                    return false;
                }
            }
        }
    }

    // note that we do not clear seen[] because all visited lits are redundant
    // and can be used to block other lits

    return true;
}

/*_________________________________________________________________________________________________
|
|  analyzeFinal : (p : Lit)  ->  [void]
|
|  Description:
|    Specialized analysis procedure to express the final conflict in terms of
assumptions. |    Calculates the (possibly empty) set of assumptions that led to
the assignment of 'p', and |    stores the result in 'out_conflict'.
|________________________________________________________________________________________________@*/
void Solver::analyzeFinal(Lit p, vec<Lit>& out_conflict) {
    out_conflict.clear();
    out_conflict.push(p);

    if (decisionLevel() == 0)
        return;

    seen[var(p)] = 1;

    for (int i = trail.size() - 1; i >= trail_lim[0].lit; i--) {
        Var x = var(trail[i]);
        if (seen[x]) {
            if (reason(x) == CRef_Undef) {
                assert(level(x) > 0);
                out_conflict.push(~trail[i]);
            } else {
                Clause& c = ca[reason(x)];
                if (c.is_leq()) {
                    throw std::runtime_error(
                            "assumptions with LEQ clause not implmented");
                }
                for (int j = 1; j < c.size(); j++)
                    if (level(var(c[j])) > 0)
                        seen[var(c[j])] = 1;
            }
            seen[x] = 0;
        }
    }

    seen[var(p)] = 0;
}

void Solver::uncheckedEnqueue(Lit p, CRef from) {
    assert(value(p) == l_Undef);
    assigns[var(p)] = lbool(!sign(p));
    vardata[var(p)] = VarData{from, decisionLevel()};
    trail.push_(p);
}

void Solver::dequeueUntil(int target_size) {
    for (int i = target_size; i < trail.size(); ++i) {
        assigns[var(trail[i])] = l_Undef;
    }
    trail.shrink(trail.size() - target_size);
}

/*_________________________________________________________________________________________________
|
|  propagate : [void]  ->  [Clause*]
|
|  Description:
|    Propagates all enqueued facts. If a conflict arises, the conflicting clause
is returned, |    otherwise CRef_Undef.
|
|    Post-conditions:
|      * the propagation queue is empty, even if there was a conflict.
|________________________________________________________________________________________________@*/
CRef Solver::propagate() {
    CRef confl = CRef_Undef;
    int num_props = 0;
    watches.cleanAll();

    while (qhead < trail.size()) {
        Lit p = trail[qhead++];  // 'p' is enqueued fact to propagate.
        num_props++;

        // propagate for disjunction clauses
        vec<Watcher>& ws = watches[p];
        Watcher *i, *j, *end;
        for (i = j = ws.begin(), end = ws.end(); i != end;) {
            // Try to avoid inspecting the clause:
            Lit blocker = i->blocker;
            if (value(blocker) == l_True) {
                *j++ = *i++;
                continue;
            }

            // Make sure the false literal is data[1]:
            CRef cr = i->cref;
            Clause& c = ca[cr];
            Lit false_lit = ~p;
            if (c[0] == false_lit)
                c[0] = c[1], c[1] = false_lit;
            assert(c[1] == false_lit);
            i++;

            // If 0th watch is true, then clause is already satisfied.
            Lit first = c[0];
            Watcher w = Watcher(cr, first);
            if (first != blocker && value(first) == l_True) {
                *j++ = w;
                continue;
            }

            // Look for new watch:
            for (int k = 2; k < c.size(); k++) {
                if (value(c[k]) != l_False) {
                    c[1] = c[k];
                    c[k] = false_lit;
                    watches[~c[1]].push(w);
                    goto NextClause;
                }
            }

            // Did not find watch -- clause is unit under assignment:
            *j++ = w;
            if (value(first) == l_False) {
                confl = cr;
                qhead = trail.size();
                // Copy the remaining watches:
                while (i < end)
                    *j++ = *i++;
            } else
                uncheckedEnqueue(first, cr);

        NextClause:;
        }
        ws.shrink(i - j);

        if (confl == CRef_Undef) {
            confl = propagate_leq(p);
        }
    }
    propagations += num_props;
    simpDB_props -= num_props;

    return confl;
}

CRef Solver::propagate_leq(Lit new_fact) {
    int fact_is_true = sign(new_fact) ^ 1;

    const vec<LeqWatcher>& watcher_list = leq_watches[var(new_fact)];
    int watcher_size = watcher_list.size();
    for (int watcher_idx = 0; watcher_idx < watcher_size; ++watcher_idx) {
        if (watcher_idx % 4 == 0 && watcher_idx + 4 < watcher_size) {
            __builtin_prefetch(&watcher_list[watcher_idx + 1].status(ca), 1, 1);
            __builtin_prefetch(&watcher_list[watcher_idx + 2].status(ca), 1, 1);
            __builtin_prefetch(&watcher_list[watcher_idx + 3].status(ca), 1, 1);
            __builtin_prefetch(&watcher_list[watcher_idx + 4].status(ca), 1, 1);
        }
        LeqWatcher watch = watcher_list[watcher_idx];
        LeqStatus& stat = watch.status(ca);
        if (stat.imply_type) {
            // already used for implication, skip this clause
            continue;
        }

        if (watch.status_ref() >= (1u << 30)) {
            throw std::runtime_error{"status ref addr too large"};
        }

        LeqStatusModLog mod_log{
                .is_true = static_cast<uint32_t>(fact_is_true ^ watch.sign),
                .imply_type_clear = 0,
                .status_ref = watch.status_ref()};

        stat.incr(mod_log.is_true, 1);

#define SETUP_IMPLY(pre, type)        \
    do {                              \
        stat.precond_is_true = pre;   \
        stat.imply_type = type;       \
        mod_log.imply_type_clear = 1; \
    } while (0)

#define RETURN_ON_CONFL(imply_pre)                      \
    do {                                                \
        SETUP_IMPLY(imply_pre, LeqStatus::IMPLY_CONFL); \
        trail_leq_stat.push(mod_log);                   \
        qhead = trail.size();                           \
        return cref;                                    \
    } while (0)

        int nr_true = stat.nr_true, nr_false = stat.nr_decided - nr_true,
            bound_true = watch.bound_true(), bound_false = watch.bound_false();

        if (nr_true < bound_true - 1 && nr_false < bound_false - 1) {
            // nothing can be implied in this case
            trail_leq_stat.push(mod_log);
            continue;
        }

        CRef cref = watch.cref;
        Clause& c = ca[cref];
        assert(c.is_leq());
        Lit dst = c.leq_dst();
        if (lbool dst_val = value(dst); dst_val.is_not_undef()) {
            // truth value of the LEQ is known, and we can try to imply lits
            if (dst_val == l_True) {
                if (nr_true >= bound_true) {
                    // LEQ is false but dst is true
                    select_known_lits<true>(c, nr_true);
                    RETURN_ON_CONFL(1);
                } else if (nr_true == bound_true - 1) {
                    // all unknown vars must be false
                    if (select_known_and_imply_unknown<true>(cref, c,
                                                             nr_true)) {
                        SETUP_IMPLY(1, LeqStatus::IMPLY_LITS);
                    } else {
                        // push the log of the newly found var (which must be an
                        // unprocessed var in the queue)
                        stat.incr(1, 1);
                        LeqStatusModLog tmp = mod_log;
                        tmp.is_true = 1;
                        trail_leq_stat.push(tmp);
                        RETURN_ON_CONFL(1);
                    }
                }
            } else {
                assert(dst_val == l_False);
                if (nr_false >= bound_false) {
                    // LEQ is true but dst is false
                    select_known_lits<false>(c, nr_false);
                    RETURN_ON_CONFL(0);
                } else if (nr_false == bound_false - 1) {
                    // all unknown vars must be true
                    if (select_known_and_imply_unknown<false>(cref, c,
                                                              nr_false)) {
                        SETUP_IMPLY(0, LeqStatus::IMPLY_LITS);
                    } else {
                        stat.incr(0, 1);
                        LeqStatusModLog tmp = mod_log;
                        tmp.is_true = 0;
                        trail_leq_stat.push(tmp);
                        RETURN_ON_CONFL(0);
                    }
                }
            }
        } else {
            // dst val is unknown, try to imply it
            if (nr_true >= bound_true) {
                select_known_lits<true>(c, nr_true);
                uncheckedEnqueue(~dst, cref);
                SETUP_IMPLY(1, LeqStatus::IMPLY_DST);

            } else if (nr_false >= bound_false) {
                select_known_lits<false>(c, nr_false);
                uncheckedEnqueue(dst, cref);
                SETUP_IMPLY(0, LeqStatus::IMPLY_DST);
            }
        }

        trail_leq_stat.push(mod_log);
#undef SETUP_IMPLY
#undef RETURN_ON_CONFL
    }
    return CRef_Undef;
}

template <bool sel_true>
void Solver::select_known_lits(Clause& c, int num) {
    for (int i = 0, j = c.size() - 1; i < num;) {
        if (value(c[i]).val_is(sel_true)) {
            ++i;
        } else {
            while (value(c[j]).val_is(!sel_true)) {
                --j;
                assert(j > i);
            }
            std::swap(c[i], c[j]);
            --j;
        }
    }
}

template <bool sel_true>
bool Solver::select_known_and_imply_unknown(CRef cr, Clause& c, int nr_known) {
    int orig_top = trail.size();
    int i = 0, j = c.size() - 1;
    // c[0:i] are true, and c[i:c.size()] are false
    while (i <= j && i <= nr_known) {
        Lit q = c[i];
        lbool v = value(q);
        if (v.is_not_undef()) {
            if (v.val_is(sel_true)) {
                ++i;
                continue;
            }
            // v is false
        } else {
            // v is unkown, and can be inferred to be false
            uncheckedEnqueue(q ^ sel_true, cr);
        }
        // put all false and inferred variables at the end
        std::swap(c[i], c[j]);
        --j;
    }
    if (i > nr_known) {
        assert(i == nr_known + 1);
        dequeueUntil(orig_top);
        return false;
    }
    assert(i == j + 1 && i == nr_known);
    return true;
}

/*_________________________________________________________________________________________________
|
|  reduceDB : ()  ->  [void]
|
|  Description:
|    Remove half of the learnt clauses, minus the clauses locked by the current
assignment. Locked |    clauses are clauses that are reason to some assignment.
Binary clauses are never removed.
|________________________________________________________________________________________________@*/
struct reduceDB_lt {
    ClauseAllocator& ca;
    reduceDB_lt(ClauseAllocator& ca_) : ca(ca_) {}
    bool operator()(CRef x, CRef y) {
        return ca[x].size() > 2 &&
               (ca[y].size() == 2 || ca[x].activity() < ca[y].activity());
    }
};
void Solver::reduceDB() {
    int i, j;
    double extra_lim =
            cla_inc / learnts.size();  // Remove any clause below this activity

    sort(learnts, reduceDB_lt(ca));
    // Don't delete binary or locked clauses. From the rest, delete clauses from
    // the first half and clauses with activity smaller than 'extra_lim':
    for (i = j = 0; i < learnts.size(); i++) {
        Clause& c = ca[learnts[i]];
        if (c.size() > 2 && !locked_disj(c) &&
            (i < learnts.size() / 2 || c.activity() < extra_lim))
            removeClause(learnts[i]);
        else
            learnts[j++] = learnts[i];
    }
    learnts.shrink(i - j);
    checkGarbage();
}

void Solver::removeSatisfied(vec<CRef>& cs) {
    int i, j;
    for (i = j = 0; i < cs.size(); i++) {
        Clause& c = ca[cs[i]];
        if (satisfied(c))
            removeClause(cs[i]);
        else
            cs[j++] = cs[i];
    }
    cs.shrink(i - j);
}

void Solver::rebuildOrderHeap() {
    vec<Var> vs;
    for (Var v = 0; v < nVars(); v++)
        if (decision[v] && value(v) == l_Undef)
            vs.push(v);
    order_heap.build(vs);
}

/*_________________________________________________________________________________________________
|
|  simplify : [void]  ->  [bool]
|
|  Description:
|    Simplify the clause database according to the current top-level assigment.
Currently, the only |    thing done here is the removal of satisfied clauses,
but more things can be put here.
|________________________________________________________________________________________________@*/
bool Solver::simplify() {
    assert(decisionLevel() == 0);

    if (!ok || propagate() != CRef_Undef)
        return ok = false;

    if (nAssigns() == simpDB_assigns || (simpDB_props > 0))
        return true;

    // Remove satisfied clauses:
    removeSatisfied(learnts);
    if (remove_satisfied) {
        // controlled by an option that can be turned off

        removeSatisfied(clauses);
        // we will never need to backtrace below 0, so it's safe to clear the
        // stats; this is also necessary because their pointers to stat would
        // become dangling after garbage collection
        trail_leq_stat.clear();
        // remove watchers on removed clauses
        leq_watches.cleanAll();
    }
    checkGarbage();
    rebuildOrderHeap();

    simpDB_assigns = nAssigns();
    simpDB_props = clauses_literals +
                   learnts_literals;  // (shouldn't depend on stats really, but
                                      // it will do for now)

    return true;
}

/*_________________________________________________________________________________________________
|
|  search : (nof_conflicts : int) (params : const SearchParams&)  ->  [lbool]
|
|  Description:
|    Search for a model the specified number of conflicts.
|    NOTE! Use negative value for 'nof_conflicts' indicate infinity.
|
|  Output:
|    'l_True' if a partial assigment that is consistent with respect to the
clauseset is found. If |    all variables are decision variables, this means
that the clause set is satisfiable. 'l_False' |    if the clause set is
unsatisfiable. 'l_Undef' if the bound on number of conflicts is reached.
|________________________________________________________________________________________________@*/
lbool Solver::search(int nof_conflicts) {
    assert(ok);
    int backtrack_level;
    int conflictC = 0;
    vec<Lit> learnt_clause;
    starts++;

    for (;;) {
        CRef confl = propagate();
        if (confl != CRef_Undef) {
            // CONFLICT
            conflicts++;
            conflictC++;
            if (decisionLevel() == 0)
                return l_False;

            learnt_clause.clear();
            analyze(confl, learnt_clause, backtrack_level);
            cancelUntil(backtrack_level);

            if (learnt_clause.size() == 1) {
                uncheckedEnqueue(learnt_clause[0]);
            } else {
                CRef cr = ca.alloc(learnt_clause, true);
                learnts.push(cr);
                attachClause(cr);
                claBumpActivity(ca[cr]);
                uncheckedEnqueue(learnt_clause[0], cr);
            }

            varDecayActivity();
            claDecayActivity();

            if (--learntsize_adjust_cnt == 0) {
                learntsize_adjust_confl *= learntsize_adjust_inc;
                learntsize_adjust_cnt = (int)learntsize_adjust_confl;
                max_learnts *= learntsize_inc;

                if (verbosity >= 1)
                    printf("| %9d | %7d %8d %8d | %8d %8d %6.0f | %6.3f %% |\n",
                           (int)conflicts,
                           (int)dec_vars - (trail_lim.size() == 0
                                                    ? trail.size()
                                                    : trail_lim[0].lit),
                           nClauses(), (int)clauses_literals, (int)max_learnts,
                           nLearnts(), (double)learnts_literals / nLearnts(),
                           progressEstimate() * 100);
            }

        } else {
            // NO CONFLICT
            if ((nof_conflicts >= 0 && conflictC >= nof_conflicts) ||
                !withinBudget()) {
                // Reached bound on number of conflicts:
                progress_estimate = progressEstimate();
                cancelUntil(0);
                return l_Undef;
            }

            // Simplify the set of problem clauses:
            if (decisionLevel() == 0 && !simplify())
                return l_False;

            if (learnts.size() - nAssigns() >= max_learnts)
                // Reduce the set of learnt clauses:
                reduceDB();

            Lit next = lit_Undef;
            while (decisionLevel() < assumptions.size()) {
                // Perform user provided assumption:
                Lit p = assumptions[decisionLevel()];
                if (value(p) == l_True) {
                    // Dummy decision level:
                    newDecisionLevel();
                } else if (value(p) == l_False) {
                    analyzeFinal(~p, conflict);
                    return l_False;
                } else {
                    next = p;
                    break;
                }
            }

            if (next == lit_Undef) {
                // New variable decision:
                decisions++;
                next = pickBranchLit();

                if (next == lit_Undef)
                    // Model found:
                    return l_True;
            }

            // Increase decision level and enqueue 'next'
            newDecisionLevel();
            uncheckedEnqueue(next);
        }
    }
}

double Solver::progressEstimate() const {
    double progress = 0;
    double F = 1.0 / nVars();

    for (int i = 0; i <= decisionLevel(); i++) {
        int beg = i == 0 ? 0 : trail_lim[i - 1].lit;
        int end = i == decisionLevel() ? trail.size() : trail_lim[i].lit;
        progress += pow(F, i) * (end - beg);
    }

    return progress / nVars();
}

/*
  Finite subsequences of the Luby-sequence:

  0: 1
  1: 1 1 2
  2: 1 1 2 1 1 2 4
  3: 1 1 2 1 1 2 4 1 1 2 1 1 2 4 8
  ...


 */

static double luby(double y, int x) {
    // Find the finite subsequence that contains index 'x', and the
    // size of that subsequence:
    int size, seq;
    for (size = 1, seq = 0; size < x + 1; seq++, size = 2 * size + 1)
        ;

    while (size - 1 != x) {
        size = (size - 1) >> 1;
        seq--;
        x = x % size;
    }

    return pow(y, seq);
}

// NOTE: assumptions passed in member-variable 'assumptions'.
lbool Solver::solve_() {
    model.clear();
    conflict.clear();
    if (!ok)
        return l_False;

    double cpu_time_begin = 0;
    if (verbosity > 0) {
        cpu_time_begin = cpuTime();
        printf("============================[ Problem Statistics "
               "]=============================\n");
        printf("|  Number of variables:  %12d                                  "
               "       |\n",
               nVars());
        printf("|  Number of clauses:    %12d                                  "
               "       |\n",
               nClauses());
    }

    // first try simplify() for unit propagation
    {
        bool simplify_result = simplify();
        if (verbosity > 0) {
            printf("|  Simplified: (result=%d)%12d                             "
                   "            |\n",
                   simplify_result, nClauses());
        }
        if (!simplify_result) {
            return l_False;
        }
    }

    solves++;

    max_learnts = nClauses() * learntsize_factor;
    learntsize_adjust_confl = learntsize_adjust_start_confl;
    learntsize_adjust_cnt = (int)learntsize_adjust_confl;
    lbool status = l_Undef;

    if (verbosity >= 1) {
        printf("============================[ Search Statistics "
               "]==============================\n");
        printf("| Conflicts |          ORIGINAL         |          LEARNT      "
               "    | Progress |\n");
        printf("|           |    Vars  Clauses Literals |    Limit  Clauses "
               "Lit/Cl |          |\n");
        printf("==============================================================="
               "================\n");
    }

    // Search:
    int curr_restarts = 0;
    while (status == l_Undef) {
        double rest_base = luby_restart ? luby(restart_inc, curr_restarts)
                                        : pow(restart_inc, curr_restarts);
        status = search(rest_base * restart_first);
        if (!withinBudget())
            break;
        curr_restarts++;
    }

    if (verbosity >= 1) {
        double cpu_time = cpuTime() - cpu_time_begin;
        printf("==============================================================="
               "================\n");
        printf("restarts              : %" PRIu64 "\n", starts);
        printf("conflicts             : %-12" PRIu64 "   (%.0f /sec)\n",
               conflicts, conflicts / cpu_time);
        printf("decisions             : %-12" PRIu64
               "   (%4.2f %% random) (%.0f /sec)\n",
               decisions, (float)rnd_decisions * 100 / (float)decisions,
               decisions / cpu_time);
        printf("propagations          : %-12" PRIu64 "   (%.0f /sec)\n",
               propagations, propagations / cpu_time);
        printf("conflict literals     : %-12" PRIu64 "   (%4.2f %% deleted)\n",
               tot_literals,
               (max_literals - tot_literals) * 100 / (double)max_literals);
    }

    if (status == l_True) {
        // Extend & copy model:
        model.growTo(nVars());
        for (int i = 0; i < nVars(); i++)
            model[i] = value(i);
    } else if (status == l_False && conflict.size() == 0)
        ok = false;

    cancelUntil(0);
    return status;
}

//=================================================================================================
// Writing CNF to DIMACS:
//
// FIXME: this needs to be rewritten completely.

static Var mapVar(Var x, vec<Var>& map, Var& max) {
    if (map.size() <= x || map[x] == -1) {
        map.growTo(x + 1, -1);
        map[x] = max++;
    }
    return map[x];
}

void Solver::toDimacs(FILE* f, Clause& c, vec<Var>& map, Var& max) {
    if (satisfied(c))
        return;

    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) != l_False)
            fprintf(f, "%s%d ", sign(c[i]) ? "-" : "",
                    mapVar(var(c[i]), map, max) + 1);
    fprintf(f, "0\n");
}

void Solver::toDimacs(const char* file, const vec<Lit>& assumps) {
    FILE* f = fopen(file, "wr");
    if (f == NULL)
        fprintf(stderr, "could not open file %s\n", file), exit(1);
    toDimacs(f, assumps);
    fclose(f);
}

void Solver::toDimacs(FILE* f, const vec<Lit>& /*assumps*/) {
    // Handle case when solver is in contradictory state:
    if (!ok) {
        fprintf(f, "p cnf 1 2\n1 0\n-1 0\n");
        return;
    }

    vec<Var> map;
    Var max = 0;

    // Cannot use removeClauses here because it is not safe
    // to deallocate them at this point. Could be improved.
    int cnt = 0;
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]]))
            cnt++;

    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]])) {
            Clause& c = ca[clauses[i]];
            for (int j = 0; j < c.size(); j++)
                if (value(c[j]) != l_False)
                    mapVar(var(c[j]), map, max);
        }

    // Assumptions are added as unit clauses:
    cnt += assumptions.size();

    fprintf(f, "p cnf %d %d\n", max, cnt);

    for (int i = 0; i < assumptions.size(); i++) {
        assert(value(assumptions[i]) != l_False);
        fprintf(f, "%s%d 0\n", sign(assumptions[i]) ? "-" : "",
                mapVar(var(assumptions[i]), map, max) + 1);
    }

    for (int i = 0; i < clauses.size(); i++)
        toDimacs(f, ca[clauses[i]], map, max);

    if (verbosity > 0)
        printf("Wrote %d clauses with %d variables.\n", cnt, max);
}

//=================================================================================================
// Garbage Collection methods:

void Solver::relocAll(ClauseAllocator& to) {
    // Remove watchers for deleted clauses
    watches.cleanAll();
    leq_watches.cleanAll();

    // All original:
    // note that we move original clauses first so LEQ clauses would be placed
    // near the beginning
    for (CRef& i : clauses)
        ca.reloc(i, to);

    // All refs to clause status in LeqStatusModLog entries:
    //
    for (LeqStatusModLog& i : trail_leq_stat) {
        Clause& cnew = to[i.status(ca).get_cref_after_reloc()];
        assert(i.status(ca) == cnew.leq_status());
        assert(cnew.is_leq() && i.status(ca) == cnew.leq_status());
        i.status_ref = to.ael(&cnew.leq_status());
    }

    // All watcher refs:
    //
    for (int v = 0; v < nVars(); v++) {
        for (int s = 0; s < 2; s++) {
            Lit p = mkLit(v, s);
            // printf(" >>> RELOCING: %s%d\n", sign(p)?"-":"", var(p)+1);
            for (Watcher& w : watches[p]) {
                ca.reloc(w.cref, to);
            }
        }
        for (LeqWatcher& w : leq_watches[v]) {
            ca.reloc(w.cref, to);
        }
    }

    // All reasons:
    // note: reasons only meaningful for vars in the trail
    for (int i = 0; i < trail.size(); i++) {
        Var v = var(trail[i]);

        if (CRef& r = vardata[v].reason; r != CRef_Undef) {
            ca.reloc(r, to);
        }
    }

    // All learnt:
    //
    for (int i = 0; i < learnts.size(); i++)
        ca.reloc(learnts[i], to);
}

void Solver::garbageCollect() {
    // Initialize the next region to a size corresponding to the estimated
    // utilization degree. This is not precise but should avoid some unnecessary
    // reallocations for the new region:
    ClauseAllocator to(ca.size() - ca.wasted());

    relocAll(to);
    if (verbosity >= 2) {
        printf("|  Garbage collection:   %12d bytes => %12d bytes             "
               "|\n",
               ca.size() * ClauseAllocator::Unit_Size,
               to.size() * ClauseAllocator::Unit_Size);
    }
    to.moveTo(ca);
}
