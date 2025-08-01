/* If-conversion for vectorizer.
   Copyright (C) 2004-2025 Free Software Foundation, Inc.
   Contributed by Devang Patel <dpatel@apple.com>

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

/* This pass implements a tree level if-conversion of loops.  Its
   initial goal is to help the vectorizer to vectorize loops with
   conditions.

   A short description of if-conversion:

     o Decide if a loop is if-convertible or not.
     o Walk all loop basic blocks in breadth first order (BFS order).
       o Remove conditional statements (at the end of basic block)
         and propagate condition into destination basic blocks'
	 predicate list.
       o Replace modify expression with conditional modify expression
         using current basic block's condition.
     o Merge all basic blocks
       o Replace phi nodes with conditional modify expr
       o Merge all basic blocks into header

     Sample transformation:

     INPUT
     -----

     # i_23 = PHI <0(0), i_18(10)>;
     <L0>:;
     j_15 = A[i_23];
     if (j_15 > 41) goto <L1>; else goto <L17>;

     <L17>:;
     goto <bb 3> (<L3>);

     <L1>:;

     # iftmp.2_4 = PHI <0(8), 42(2)>;
     <L3>:;
     A[i_23] = iftmp.2_4;
     i_18 = i_23 + 1;
     if (i_18 <= 15) goto <L19>; else goto <L18>;

     <L19>:;
     goto <bb 1> (<L0>);

     <L18>:;

     OUTPUT
     ------

     # i_23 = PHI <0(0), i_18(10)>;
     <L0>:;
     j_15 = A[i_23];

     <L3>:;
     iftmp.2_4 = j_15 > 41 ? 42 : 0;
     A[i_23] = iftmp.2_4;
     i_18 = i_23 + 1;
     if (i_18 <= 15) goto <L19>; else goto <L18>;

     <L19>:;
     goto <bb 1> (<L0>);

     <L18>:;
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "cfghooks.h"
#include "tree-pass.h"
#include "ssa.h"
#include "expmed.h"
#include "expr.h"
#include "optabs-tree.h"
#include "gimple-pretty-print.h"
#include "alias.h"
#include "fold-const.h"
#include "stor-layout.h"
#include "gimple-iterator.h"
#include "gimple-fold.h"
#include "gimplify.h"
#include "gimplify-me.h"
#include "tree-cfg.h"
#include "tree-into-ssa.h"
#include "tree-ssa.h"
#include "cfgloop.h"
#include "tree-data-ref.h"
#include "tree-scalar-evolution.h"
#include "tree-ssa-loop.h"
#include "tree-ssa-loop-niter.h"
#include "tree-ssa-loop-ivopts.h"
#include "tree-ssa-address.h"
#include "dbgcnt.h"
#include "tree-hash-traits.h"
#include "varasm.h"
#include "builtins.h"
#include "cfganal.h"
#include "internal-fn.h"
#include "fold-const.h"
#include "tree-ssa-sccvn.h"
#include "tree-cfgcleanup.h"
#include "tree-ssa-dse.h"
#include "tree-vectorizer.h"
#include "tree-eh.h"
#include "cgraph.h"

/* For lang_hooks.types.type_for_mode.  */
#include "langhooks.h"

/* Only handle PHIs with no more arguments unless we are asked to by
   simd pragma.  */
#define MAX_PHI_ARG_NUM \
  ((unsigned) param_max_tree_if_conversion_phi_args)

/* True if we've converted a statement that was only executed when some
   condition C was true, and if for correctness we need to predicate the
   statement to ensure that it is a no-op when C is false.  See
   predicate_statements for the kinds of predication we support.  */
static bool need_to_predicate;

/* True if we have to rewrite stmts that may invoke undefined behavior
   when a condition C was false so it doesn't if it is always executed.
   See predicate_statements for the kinds of predication we support.  */
static bool need_to_rewrite_undefined;

/* Indicate if there are any complicated PHIs that need to be handled in
   if-conversion.  Complicated PHI has more than two arguments and can't
   be degenerated to two arguments PHI.  See more information in comment
   before phi_convertible_by_degenerating_args.  */
static bool any_complicated_phi;

/* True if we have bitfield accesses we can lower.  */
static bool need_to_lower_bitfields;

/* True if there is any ifcvting to be done.  */
static bool need_to_ifcvt;

/* Hash for struct innermost_loop_behavior.  It depends on the user to
   free the memory.  */

struct innermost_loop_behavior_hash : nofree_ptr_hash <innermost_loop_behavior>
{
  static inline hashval_t hash (const value_type &);
  static inline bool equal (const value_type &,
			    const compare_type &);
};

inline hashval_t
innermost_loop_behavior_hash::hash (const value_type &e)
{
  hashval_t hash;

  hash = iterative_hash_expr (e->base_address, 0);
  hash = iterative_hash_expr (e->offset, hash);
  hash = iterative_hash_expr (e->init, hash);
  return iterative_hash_expr (e->step, hash);
}

inline bool
innermost_loop_behavior_hash::equal (const value_type &e1,
				     const compare_type &e2)
{
  if ((e1->base_address && !e2->base_address)
      || (!e1->base_address && e2->base_address)
      || (!e1->offset && e2->offset)
      || (e1->offset && !e2->offset)
      || (!e1->init && e2->init)
      || (e1->init && !e2->init)
      || (!e1->step && e2->step)
      || (e1->step && !e2->step))
    return false;

  if (e1->base_address && e2->base_address
      && !operand_equal_p (e1->base_address, e2->base_address, 0))
    return false;
  if (e1->offset && e2->offset
      && !operand_equal_p (e1->offset, e2->offset, 0))
    return false;
  if (e1->init && e2->init
      && !operand_equal_p (e1->init, e2->init, 0))
    return false;
  if (e1->step && e2->step
      && !operand_equal_p (e1->step, e2->step, 0))
    return false;

  return true;
}

/* List of basic blocks in if-conversion-suitable order.  */
static basic_block *ifc_bbs;

/* Hash table to store <DR's innermost loop behavior, DR> pairs.  */
static hash_map<innermost_loop_behavior_hash,
		data_reference_p> *innermost_DR_map;

/* Hash table to store <base reference, DR> pairs.  */
static hash_map<tree_operand_hash, data_reference_p> *baseref_DR_map;

/* List of redundant SSA names: the first should be replaced by the second.  */
static vec< std::pair<tree, tree> > redundant_ssa_names;

/* Structure used to predicate basic blocks.  This is attached to the
   ->aux field of the BBs in the loop to be if-converted.  */
struct bb_predicate {

  /* The condition under which this basic block is executed.  */
  tree predicate;

  /* PREDICATE is gimplified, and the sequence of statements is
     recorded here, in order to avoid the duplication of computations
     that occur in previous conditions.  See PR44483.  */
  gimple_seq predicate_gimplified_stmts;

  /* Records the number of statements recorded into
     PREDICATE_GIMPLIFIED_STMTS.   */
  unsigned no_predicate_stmts;
};

/* Returns true when the basic block BB has a predicate.  */

static inline bool
bb_has_predicate (basic_block bb)
{
  return bb->aux != NULL;
}

/* Returns the gimplified predicate for basic block BB.  */

static inline tree
bb_predicate (basic_block bb)
{
  return ((struct bb_predicate *) bb->aux)->predicate;
}

/* Sets the gimplified predicate COND for basic block BB.  */

static inline void
set_bb_predicate (basic_block bb, tree cond)
{
  auto aux = (struct bb_predicate *) bb->aux;
  gcc_assert ((TREE_CODE (cond) == TRUTH_NOT_EXPR
	       && is_gimple_val (TREE_OPERAND (cond, 0)))
	      || is_gimple_val (cond));
  aux->predicate = cond;
  aux->no_predicate_stmts++;

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "Recording block %d value %d\n", bb->index,
	     aux->no_predicate_stmts);
}

/* Returns the sequence of statements of the gimplification of the
   predicate for basic block BB.  */

static inline gimple_seq
bb_predicate_gimplified_stmts (basic_block bb)
{
  return ((struct bb_predicate *) bb->aux)->predicate_gimplified_stmts;
}

/* Sets the sequence of statements STMTS of the gimplification of the
   predicate for basic block BB.  If PRESERVE_COUNTS then don't clear the predicate
   counts.  */

static inline void
set_bb_predicate_gimplified_stmts (basic_block bb, gimple_seq stmts,
				   bool preserve_counts)
{
  ((struct bb_predicate *) bb->aux)->predicate_gimplified_stmts = stmts;
  if (stmts == NULL && !preserve_counts)
    ((struct bb_predicate *) bb->aux)->no_predicate_stmts = 0;
}

/* Adds the sequence of statements STMTS to the sequence of statements
   of the predicate for basic block BB.  */

static inline void
add_bb_predicate_gimplified_stmts (basic_block bb, gimple_seq stmts)
{
  /* We might have updated some stmts in STMTS via force_gimple_operand
     calling fold_stmt and that producing multiple stmts.  Delink immediate
     uses so update_ssa after loop versioning doesn't get confused for
     the not yet inserted predicates.
     ???  This should go away once we reliably avoid updating stmts
     not in any BB.  */
  for (gimple_stmt_iterator gsi = gsi_start (stmts);
       !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gimple *stmt = gsi_stmt (gsi);
      delink_stmt_imm_use (stmt);
      gimple_set_modified (stmt, true);
      ((struct bb_predicate *) bb->aux)->no_predicate_stmts++;
    }
  gimple_seq_add_seq_without_update
    (&(((struct bb_predicate *) bb->aux)->predicate_gimplified_stmts), stmts);
}

/* Return the number of statements the predicate of the basic block consists
   of.  */

static inline unsigned
get_bb_num_predicate_stmts (basic_block bb)
{
  return ((struct bb_predicate *) bb->aux)->no_predicate_stmts;
}

/* Initializes to TRUE the predicate of basic block BB.  */

static inline void
init_bb_predicate (basic_block bb)
{
  bb->aux = XNEW (struct bb_predicate);
  set_bb_predicate_gimplified_stmts (bb, NULL, false);
  set_bb_predicate (bb, boolean_true_node);
}

/* Release the SSA_NAMEs associated with the predicate of basic block BB.  */

static inline void
release_bb_predicate (basic_block bb)
{
  gimple_seq stmts = bb_predicate_gimplified_stmts (bb);
  if (stmts)
    {
      /* Ensure that these stmts haven't yet been added to a bb.  */
      if (flag_checking)
	for (gimple_stmt_iterator i = gsi_start (stmts);
	     !gsi_end_p (i); gsi_next (&i))
	  gcc_assert (! gimple_bb (gsi_stmt (i)));

      /* Discard them.  */
      gimple_seq_discard (stmts);
      set_bb_predicate_gimplified_stmts (bb, NULL, false);
    }
}

/* Free the predicate of basic block BB.  */

static inline void
free_bb_predicate (basic_block bb)
{
  if (!bb_has_predicate (bb))
    return;

  release_bb_predicate (bb);
  free (bb->aux);
  bb->aux = NULL;
}

/* Reinitialize predicate of BB with the true predicate.  */

static inline void
reset_bb_predicate (basic_block bb)
{
  if (!bb_has_predicate (bb))
    init_bb_predicate (bb);
  else
    {
      release_bb_predicate (bb);
      set_bb_predicate (bb, boolean_true_node);
    }
}

/* Returns a new SSA_NAME of type TYPE that is assigned the value of
   the expression EXPR.  Inserts the statement created for this
   computation before GSI and leaves the iterator GSI at the same
   statement.  */

static tree
ifc_temp_var (tree type, tree expr, gimple_stmt_iterator *gsi)
{
  tree new_name = make_temp_ssa_name (type, NULL, "_ifc_");
  gimple *stmt = gimple_build_assign (new_name, expr);
  gimple_set_vuse (stmt, gimple_vuse (gsi_stmt (*gsi)));
  gsi_insert_before (gsi, stmt, GSI_SAME_STMT);
  return new_name;
}

/* Return true when COND is a false predicate.  */

static inline bool
is_false_predicate (tree cond)
{
  return (cond != NULL_TREE
	  && (cond == boolean_false_node
	      || integer_zerop (cond)));
}

/* Return true when COND is a true predicate.  */

static inline bool
is_true_predicate (tree cond)
{
  return (cond == NULL_TREE
	  || cond == boolean_true_node
	  || integer_onep (cond));
}

/* Returns true when BB has a predicate that is not trivial: true or
   NULL_TREE.  */

static inline bool
is_predicated (basic_block bb)
{
  return !is_true_predicate (bb_predicate (bb));
}

/* Parses the predicate COND and returns its comparison code and
   operands OP0 and OP1.  */

static enum tree_code
parse_predicate (tree cond, tree *op0, tree *op1)
{
  gimple *s;

  if (TREE_CODE (cond) == SSA_NAME
      && is_gimple_assign (s = SSA_NAME_DEF_STMT (cond)))
    {
      if (TREE_CODE_CLASS (gimple_assign_rhs_code (s)) == tcc_comparison)
	{
	  *op0 = gimple_assign_rhs1 (s);
	  *op1 = gimple_assign_rhs2 (s);
	  return gimple_assign_rhs_code (s);
	}

      else if (gimple_assign_rhs_code (s) == TRUTH_NOT_EXPR)
	{
	  tree op = gimple_assign_rhs1 (s);
	  tree type = TREE_TYPE (op);
	  enum tree_code code = parse_predicate (op, op0, op1);

	  return code == ERROR_MARK ? ERROR_MARK
	    : invert_tree_comparison (code, HONOR_NANS (type));
	}

      return ERROR_MARK;
    }

  if (COMPARISON_CLASS_P (cond))
    {
      *op0 = TREE_OPERAND (cond, 0);
      *op1 = TREE_OPERAND (cond, 1);
      return TREE_CODE (cond);
    }

  return ERROR_MARK;
}

/* Returns the fold of predicate C1 OR C2 at location LOC.  */

static tree
fold_or_predicates (location_t loc, tree c1, tree c2)
{
  tree op1a, op1b, op2a, op2b;
  enum tree_code code1 = parse_predicate (c1, &op1a, &op1b);
  enum tree_code code2 = parse_predicate (c2, &op2a, &op2b);

  if (code1 != ERROR_MARK && code2 != ERROR_MARK)
    {
      tree t = maybe_fold_or_comparisons (boolean_type_node, code1, op1a, op1b,
					  code2, op2a, op2b);
      if (t)
	return t;
    }

  return fold_build2_loc (loc, TRUTH_OR_EXPR, boolean_type_node, c1, c2);
}

/* Returns either a COND_EXPR or the folded expression if the folded
   expression is a MIN_EXPR, a MAX_EXPR, an ABS_EXPR,
   a constant or a SSA_NAME. */

static tree
fold_build_cond_expr (tree type, tree cond, tree rhs, tree lhs)
{
  /* Short cut the case where both rhs and lhs are the same. */
  if (operand_equal_p (rhs, lhs))
    return rhs;

  /* If COND is comparison r != 0 and r has boolean type, convert COND
     to SSA_NAME to accept by vect bool pattern.  */
  if (TREE_CODE (cond) == NE_EXPR)
    {
      tree op0 = TREE_OPERAND (cond, 0);
      tree op1 = TREE_OPERAND (cond, 1);
      if (TREE_CODE (op0) == SSA_NAME
	  && TREE_CODE (TREE_TYPE (op0)) == BOOLEAN_TYPE
	  && (integer_zerop (op1)))
	cond = op0;
    }

  gimple_match_op cexpr (gimple_match_cond::UNCOND, COND_EXPR,
			 type, cond, rhs, lhs);
  if (cexpr.resimplify (NULL, follow_all_ssa_edges))
    {
      if (gimple_simplified_result_is_gimple_val (&cexpr))
	return cexpr.ops[0];
      else if (cexpr.code == ABS_EXPR)
	return build1 (ABS_EXPR, type, cexpr.ops[0]);
      else if (cexpr.code == MIN_EXPR
	       || cexpr.code == MAX_EXPR)
	return build2 ((tree_code)cexpr.code, type, cexpr.ops[0], cexpr.ops[1]);
    }

  return build3 (COND_EXPR, type, cond, rhs, lhs);
}

/* Add condition NC to the predicate list of basic block BB.  LOOP is
   the loop to be if-converted. Use predicate of cd-equivalent block
   for join bb if it exists: we call basic blocks bb1 and bb2
   cd-equivalent if they are executed under the same condition.  */

static inline void
add_to_predicate_list (class loop *loop, basic_block bb, tree nc)
{
  tree bc, *tp;
  basic_block dom_bb;

  if (is_true_predicate (nc))
    return;

  /* If dominance tells us this basic block is always executed,
     don't record any predicates for it.  */
  if (dominated_by_p (CDI_DOMINATORS, loop->latch, bb))
    return;

  dom_bb = get_immediate_dominator (CDI_DOMINATORS, bb);
  /* We use notion of cd equivalence to get simpler predicate for
     join block, e.g. if join block has 2 predecessors with predicates
     p1 & p2 and p1 & !p2, we'd like to get p1 for it instead of
     p1 & p2 | p1 & !p2.  */
  if (dom_bb != loop->header
      && get_immediate_dominator (CDI_POST_DOMINATORS, dom_bb) == bb)
    {
      gcc_assert (flow_bb_inside_loop_p (loop, dom_bb));
      bc = bb_predicate (dom_bb);
      if (!is_true_predicate (bc))
	set_bb_predicate (bb, bc);
      else
	gcc_assert (is_true_predicate (bb_predicate (bb)));
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "Use predicate of bb#%d for bb#%d\n",
		 dom_bb->index, bb->index);
      return;
    }

  if (!is_predicated (bb))
    bc = nc;
  else
    {
      bc = bb_predicate (bb);
      bc = fold_or_predicates (EXPR_LOCATION (bc), nc, bc);
      if (is_true_predicate (bc))
	{
	  reset_bb_predicate (bb);
	  return;
	}
    }

  /* Allow a TRUTH_NOT_EXPR around the main predicate.  */
  if (TREE_CODE (bc) == TRUTH_NOT_EXPR)
    tp = &TREE_OPERAND (bc, 0);
  else
    tp = &bc;
  if (!is_gimple_val (*tp))
    {
      gimple_seq stmts;
      *tp = force_gimple_operand (*tp, &stmts, true, NULL_TREE);
      add_bb_predicate_gimplified_stmts (bb, stmts);
    }
  set_bb_predicate (bb, bc);
}

/* Add the condition COND to the previous condition PREV_COND, and add
   this to the predicate list of the destination of edge E.  LOOP is
   the loop to be if-converted.  */

static void
add_to_dst_predicate_list (class loop *loop, edge e,
			   tree prev_cond, tree cond)
{
  if (!flow_bb_inside_loop_p (loop, e->dest))
    return;

  if (!is_true_predicate (prev_cond))
    cond = fold_build2 (TRUTH_AND_EXPR, boolean_type_node,
			prev_cond, cond);

  if (!dominated_by_p (CDI_DOMINATORS, loop->latch, e->dest))
    add_to_predicate_list (loop, e->dest, cond);
}

/* Return true if one of the successor edges of BB exits LOOP.  */

static bool
bb_with_exit_edge_p (const class loop *loop, basic_block bb)
{
  edge e;
  edge_iterator ei;

  FOR_EACH_EDGE (e, ei, bb->succs)
    if (loop_exit_edge_p (loop, e))
      return true;

  return false;
}

/* Given PHI which has more than two arguments, this function checks if
   it's if-convertible by degenerating its arguments.  Specifically, if
   below two conditions are satisfied:

     1) Number of PHI arguments with different values equals to 2 and one
	argument has the only occurrence.
     2) The edge corresponding to the unique argument isn't critical edge.

   Such PHI can be handled as PHIs have only two arguments.  For example,
   below PHI:

     res = PHI <A_1(e1), A_1(e2), A_2(e3)>;

   can be transformed into:

     res = (predicate of e3) ? A_2 : A_1;

   Return TRUE if it is the case, FALSE otherwise.  */

static bool
phi_convertible_by_degenerating_args (gphi *phi)
{
  edge e;
  tree arg, t1 = NULL, t2 = NULL;
  unsigned int i, i1 = 0, i2 = 0, n1 = 0, n2 = 0;
  unsigned int num_args = gimple_phi_num_args (phi);

  gcc_assert (num_args > 2);

  for (i = 0; i < num_args; i++)
    {
      arg = gimple_phi_arg_def (phi, i);
      if (t1 == NULL || operand_equal_p (t1, arg, 0))
	{
	  n1++;
	  i1 = i;
	  t1 = arg;
	}
      else if (t2 == NULL || operand_equal_p (t2, arg, 0))
	{
	  n2++;
	  i2 = i;
	  t2 = arg;
	}
      else
	return false;
    }

  if (n1 != 1 && n2 != 1)
    return false;

  /* Check if the edge corresponding to the unique arg is critical.  */
  e = gimple_phi_arg_edge (phi, (n1 == 1) ? i1 : i2);
  if (EDGE_COUNT (e->src->succs) > 1)
    return false;

  return true;
}

/* Return true when PHI is if-convertible.  PHI is part of loop LOOP
   and it belongs to basic block BB.  Note at this point, it is sure
   that PHI is if-convertible.  This function updates global variable
   ANY_COMPLICATED_PHI if PHI is complicated.  */

static bool
if_convertible_phi_p (class loop *loop, basic_block bb, gphi *phi)
{
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "-------------------------\n");
      print_gimple_stmt (dump_file, phi, 0, TDF_SLIM);
    }

  if (bb != loop->header
      && gimple_phi_num_args (phi) > 2
      && !phi_convertible_by_degenerating_args (phi))
    any_complicated_phi = true;

  return true;
}

/* Records the status of a data reference.  This struct is attached to
   each DR->aux field.  */

struct ifc_dr {
  bool rw_unconditionally;
  bool w_unconditionally;
  bool written_at_least_once;

  tree rw_predicate;
  tree w_predicate;
  tree base_w_predicate;
};

#define IFC_DR(DR) ((struct ifc_dr *) (DR)->aux)
#define DR_BASE_W_UNCONDITIONALLY(DR) (IFC_DR (DR)->written_at_least_once)
#define DR_RW_UNCONDITIONALLY(DR) (IFC_DR (DR)->rw_unconditionally)
#define DR_W_UNCONDITIONALLY(DR) (IFC_DR (DR)->w_unconditionally)

/* Iterates over DR's and stores refs, DR and base refs, DR pairs in
   HASH tables.  While storing them in HASH table, it checks if the
   reference is unconditionally read or written and stores that as a flag
   information.  For base reference it checks if it is written atlest once
   unconditionally and stores it as flag information along with DR.
   In other words for every data reference A in STMT there exist other
   accesses to a data reference with the same base with predicates that
   add up (OR-up) to the true predicate: this ensures that the data
   reference A is touched (read or written) on every iteration of the
   if-converted loop.  */
static void
hash_memrefs_baserefs_and_store_DRs_read_written_info (data_reference_p a)
{

  data_reference_p *master_dr, *base_master_dr;
  tree base_ref = DR_BASE_OBJECT (a);
  innermost_loop_behavior *innermost = &DR_INNERMOST (a);
  tree ca = bb_predicate (gimple_bb (DR_STMT (a)));
  bool exist1, exist2;

  master_dr = &innermost_DR_map->get_or_insert (innermost, &exist1);
  if (!exist1)
    *master_dr = a;

  if (DR_IS_WRITE (a))
    {
      IFC_DR (*master_dr)->w_predicate
	= fold_or_predicates (UNKNOWN_LOCATION, ca,
			      IFC_DR (*master_dr)->w_predicate);
      if (is_true_predicate (IFC_DR (*master_dr)->w_predicate))
	DR_W_UNCONDITIONALLY (*master_dr) = true;
    }
  IFC_DR (*master_dr)->rw_predicate
    = fold_or_predicates (UNKNOWN_LOCATION, ca,
			  IFC_DR (*master_dr)->rw_predicate);
  if (is_true_predicate (IFC_DR (*master_dr)->rw_predicate))
    DR_RW_UNCONDITIONALLY (*master_dr) = true;

  if (DR_IS_WRITE (a))
    {
      base_master_dr = &baseref_DR_map->get_or_insert (base_ref, &exist2);
      if (!exist2)
	*base_master_dr = a;
      IFC_DR (*base_master_dr)->base_w_predicate
	= fold_or_predicates (UNKNOWN_LOCATION, ca,
			      IFC_DR (*base_master_dr)->base_w_predicate);
      if (is_true_predicate (IFC_DR (*base_master_dr)->base_w_predicate))
	DR_BASE_W_UNCONDITIONALLY (*base_master_dr) = true;
    }
}

/* Return TRUE if can prove the index IDX of an array reference REF is
   within array bound.  Return false otherwise.  */

static bool
idx_within_array_bound (tree ref, tree *idx, void *dta)
{
  wi::overflow_type overflow;
  widest_int niter, valid_niter, delta, wi_step;
  tree ev, init, step;
  tree low, high;
  class loop *loop = (class loop*) dta;

  /* Only support within-bound access for array references.  */
  if (TREE_CODE (ref) != ARRAY_REF)
    return false;

  /* For arrays that might have flexible sizes, it is not guaranteed that they
     do not extend over their declared size.  */
  if (array_ref_flexible_size_p (ref))
    return false;

  ev = analyze_scalar_evolution (loop, *idx);
  ev = instantiate_parameters (loop, ev);
  init = initial_condition (ev);
  step = evolution_part_in_loop_num (ev, loop->num);

  if (!init || TREE_CODE (init) != INTEGER_CST
      || (step && TREE_CODE (step) != INTEGER_CST))
    return false;

  low = array_ref_low_bound (ref);
  high = array_ref_up_bound (ref);

  /* The case of nonconstant bounds could be handled, but it would be
     complicated.  */
  if (TREE_CODE (low) != INTEGER_CST
      || !high || TREE_CODE (high) != INTEGER_CST)
    return false;

  /* Check if the intial idx is within bound.  */
  if (wi::to_widest (init) < wi::to_widest (low)
      || wi::to_widest (init) > wi::to_widest (high))
    return false;

  /* The idx is always within bound.  */
  if (!step || integer_zerop (step))
    return true;

  if (!max_loop_iterations (loop, &niter))
    return false;

  if (wi::to_widest (step) < 0)
    {
      delta = wi::to_widest (init) - wi::to_widest (low);
      wi_step = -wi::to_widest (step);
    }
  else
    {
      delta = wi::to_widest (high) - wi::to_widest (init);
      wi_step = wi::to_widest (step);
    }

  valid_niter = wi::div_floor (delta, wi_step, SIGNED, &overflow);
  /* The iteration space of idx is within array bound.  */
  if (!overflow && niter <= valid_niter)
    return true;

  return false;
}

/* Return TRUE if ref is a within bound array reference.  */

bool
ref_within_array_bound (gimple *stmt, tree ref)
{
  class loop *loop = loop_containing_stmt (stmt);

  gcc_assert (loop != NULL);
  return for_each_index (&ref, idx_within_array_bound, loop);
}


/* Given a memory reference expression T, return TRUE if base object
   it refers to is writable.  The base object of a memory reference
   is the main object being referenced, which is returned by function
   get_base_address.  */

static bool
base_object_writable (tree ref)
{
  tree base_tree = get_base_address (ref);

  return (base_tree
	  && DECL_P (base_tree)
	  && decl_binds_to_current_def_p (base_tree)
	  && !TREE_READONLY (base_tree));
}

/* Return true when the memory references of STMT won't trap in the
   if-converted code.  There are two things that we have to check for:

   - writes to memory occur to writable memory: if-conversion of
   memory writes transforms the conditional memory writes into
   unconditional writes, i.e. "if (cond) A[i] = foo" is transformed
   into "A[i] = cond ? foo : A[i]", and as the write to memory may not
   be executed at all in the original code, it may be a readonly
   memory.  To check that A is not const-qualified, we check that
   there exists at least an unconditional write to A in the current
   function.

   - reads or writes to memory are valid memory accesses for every
   iteration.  To check that the memory accesses are correctly formed
   and that we are allowed to read and write in these locations, we
   check that the memory accesses to be if-converted occur at every
   iteration unconditionally.

   Returns true for the memory reference in STMT, same memory reference
   is read or written unconditionally atleast once and the base memory
   reference is written unconditionally once.  This is to check reference
   will not write fault.  Also retuns true if the memory reference is
   unconditionally read once then we are conditionally writing to memory
   which is defined as read and write and is bound to the definition
   we are seeing.  */
static bool
ifcvt_memrefs_wont_trap (gimple *stmt, vec<data_reference_p> drs)
{
  /* If DR didn't see a reference here we can't use it to tell
     whether the ref traps or not.  */
  if (gimple_uid (stmt) == 0)
    return false;

  data_reference_p *master_dr, *base_master_dr;
  data_reference_p a = drs[gimple_uid (stmt) - 1];

  tree base = DR_BASE_OBJECT (a);
  innermost_loop_behavior *innermost = &DR_INNERMOST (a);

  gcc_assert (DR_STMT (a) == stmt);
  gcc_assert (DR_BASE_ADDRESS (a) || DR_OFFSET (a)
              || DR_INIT (a) || DR_STEP (a));

  master_dr = innermost_DR_map->get (innermost);
  gcc_assert (master_dr != NULL);

  base_master_dr = baseref_DR_map->get (base);

  /* If a is unconditionally written to it doesn't trap.  */
  if (DR_W_UNCONDITIONALLY (*master_dr))
    return true;

  /* If a is unconditionally accessed then ...

     Even a is conditional access, we can treat it as an unconditional
     one if it's an array reference and all its index are within array
     bound.  */
  if (DR_RW_UNCONDITIONALLY (*master_dr)
      || ref_within_array_bound (stmt, DR_REF (a)))
    {
      /* an unconditional read won't trap.  */
      if (DR_IS_READ (a))
	return true;

      /* an unconditionaly write won't trap if the base is written
         to unconditionally.  */
      if ((base_master_dr
	   && DR_BASE_W_UNCONDITIONALLY (*base_master_dr))
	  /* or the base is known to be not readonly.  */
	  || base_object_writable (DR_REF (a)))
	return !ref_can_have_store_data_races (base);
    }

  return false;
}

/* Return true if STMT could be converted into a masked load or store
   (conditional load or store based on a mask computed from bb predicate).  */

static bool
ifcvt_can_use_mask_load_store (gimple *stmt)
{
  /* Check whether this is a load or store.  */
  tree lhs = gimple_assign_lhs (stmt);
  bool is_load;
  tree ref;
  if (gimple_store_p (stmt))
    {
      if (!is_gimple_val (gimple_assign_rhs1 (stmt)))
	return false;
      is_load = false;
      ref = lhs;
    }
  else if (gimple_assign_load_p (stmt))
    {
      is_load = true;
      ref = gimple_assign_rhs1 (stmt);
    }
  else
    return false;

  if (may_be_nonaddressable_p (ref))
    return false;

  /* Mask should be integer mode of the same size as the load/store
     mode.  */
  machine_mode mode = TYPE_MODE (TREE_TYPE (lhs));
  if (!int_mode_for_mode (mode).exists () || VECTOR_MODE_P (mode))
    return false;

  if (can_vec_mask_load_store_p (mode, VOIDmode, is_load))
    return true;

  return false;
}

/* Return true if STMT could be converted from an operation that is
   unconditional to one that is conditional on a bb predicate mask.  */

static bool
ifcvt_can_predicate (gimple *stmt)
{
  basic_block bb = gimple_bb (stmt);

  if (!(flag_tree_loop_vectorize || bb->loop_father->force_vectorize)
      || bb->loop_father->dont_vectorize
      || gimple_has_volatile_ops (stmt))
    return false;

  if (gimple_assign_single_p (stmt))
    return ifcvt_can_use_mask_load_store (stmt);

  tree_code code = gimple_assign_rhs_code (stmt);
  tree lhs_type = TREE_TYPE (gimple_assign_lhs (stmt));
  tree rhs_type = TREE_TYPE (gimple_assign_rhs1 (stmt));
  if (!types_compatible_p (lhs_type, rhs_type))
    return false;
  internal_fn cond_fn = get_conditional_internal_fn (code);
  return (cond_fn != IFN_LAST
	  && vectorized_internal_fn_supported_p (cond_fn, lhs_type));
}

/* Return true when STMT is if-convertible.

   GIMPLE_ASSIGN statement is not if-convertible if,
   - it is not movable,
   - it could trap,
   - LHS is not var decl.  */

static bool
if_convertible_gimple_assign_stmt_p (gimple *stmt,
				     vec<data_reference_p> refs)
{
  tree lhs = gimple_assign_lhs (stmt);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "-------------------------\n");
      print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);
    }

  if (!is_gimple_reg_type (TREE_TYPE (lhs)))
    return false;

  /* Some of these constrains might be too conservative.  */
  if (stmt_ends_bb_p (stmt)
      || gimple_has_volatile_ops (stmt)
      || (TREE_CODE (lhs) == SSA_NAME
          && SSA_NAME_OCCURS_IN_ABNORMAL_PHI (lhs))
      || gimple_has_side_effects (stmt))
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
        fprintf (dump_file, "stmt not suitable for ifcvt\n");
      return false;
    }

  /* tree-into-ssa.cc uses GF_PLF_1, so avoid it, because
     in between if_convertible_loop_p and combine_blocks
     we can perform loop versioning.  */
  gimple_set_plf (stmt, GF_PLF_2, false);

  if ((! gimple_vuse (stmt)
       || gimple_could_trap_p_1 (stmt, false, false)
       || ! ifcvt_memrefs_wont_trap (stmt, refs))
      && gimple_could_trap_p (stmt))
    {
      if (ifcvt_can_predicate (stmt))
	{
	  gimple_set_plf (stmt, GF_PLF_2, true);
	  need_to_predicate = true;
	  return true;
	}
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "tree could trap...\n");
      return false;
    }
  else if (gimple_needing_rewrite_undefined (stmt))
    /* We have to rewrite stmts with undefined overflow.  */
    need_to_rewrite_undefined = true;

  /* When if-converting stores force versioning, likewise if we
     ended up generating store data races.  */
  if (gimple_vdef (stmt))
    need_to_predicate = true;

  return true;
}

/* Return true when SW switch statement is equivalent to cond, that
   all non default labels point to the same label.

   Fallthrough is not checked for and could even happen
   with cond (using goto), so is handled.

   This is intended for switches created by the if-switch-conversion
   pass, but can handle some programmer supplied cases too. */

static bool
if_convertible_switch_p (gswitch *sw)
{
  if (gimple_switch_num_labels (sw) <= 1)
    return false;
  tree label = CASE_LABEL (gimple_switch_label (sw, 1));
  for (unsigned i = 1; i < gimple_switch_num_labels (sw); i++)
    {
      if (CASE_LABEL (gimple_switch_label (sw, i)) != label)
	return false;
    }
  return true;
}

/* Return true when STMT is if-convertible.

   A statement is if-convertible if:
   - it is an if-convertible GIMPLE_ASSIGN,
   - it is a GIMPLE_LABEL or a GIMPLE_COND,
   - it is a switch equivalent to COND
   - it is builtins call,
   - it is a call to a function with a SIMD clone.  */

static bool
if_convertible_stmt_p (gimple *stmt, vec<data_reference_p> refs)
{
  switch (gimple_code (stmt))
    {
    case GIMPLE_LABEL:
    case GIMPLE_DEBUG:
    case GIMPLE_COND:
      return true;

    case GIMPLE_SWITCH:
      return if_convertible_switch_p (as_a <gswitch *> (stmt));

    case GIMPLE_ASSIGN:
      return if_convertible_gimple_assign_stmt_p (stmt, refs);

    case GIMPLE_CALL:
      {
	tree fndecl = gimple_call_fndecl (stmt);
	if (fndecl)
	  {
	    /* We can vectorize some builtins and functions with SIMD
	       "inbranch" clones.  */
	    struct cgraph_node *node = cgraph_node::get (fndecl);
	    if (node && node->simd_clones != NULL)
	      /* Ensure that at least one clone can be "inbranch".  */
	      for (struct cgraph_node *n = node->simd_clones; n != NULL;
		   n = n->simdclone->next_clone)
		if (n->simdclone->inbranch)
		  {
		    gimple_set_plf (stmt, GF_PLF_2, true);
		    need_to_predicate = true;
		    return true;
		  }
	  }

	/* There are some IFN_s that are used to replace builtins but have the
	   same semantics.  Even if MASK_CALL cannot handle them vectorable_call
	   will insert the proper selection, so do not block conversion.  */
	int flags = gimple_call_flags (stmt);
	if ((flags & ECF_CONST)
	    && !(flags & ECF_LOOPING_CONST_OR_PURE)
	    && gimple_call_combined_fn (stmt) != CFN_LAST)
	  return true;

	return false;
      }

    default:
      /* Don't know what to do with 'em so don't do anything.  */
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "don't know what to do\n");
	  print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);
	}
      return false;
    }
}

/* Assumes that BB has more than 1 predecessors.
   Returns false if at least one successor is not on critical edge
   and true otherwise.  */

static inline bool
all_preds_critical_p (basic_block bb)
{
  edge e;
  edge_iterator ei;

  FOR_EACH_EDGE (e, ei, bb->preds)
    if (EDGE_COUNT (e->src->succs) == 1)
      return false;
  return true;
}

/* Return true when BB is if-convertible.  This routine does not check
   basic block's statements and phis.

   A basic block is not if-convertible if:
   - it is non-empty and it is after the exit block (in BFS order),
   - it is after the exit block but before the latch,
   - its edges are not normal.

   EXIT_BB is the basic block containing the exit of the LOOP.  BB is
   inside LOOP.  */

static bool
if_convertible_bb_p (class loop *loop, basic_block bb, basic_block exit_bb)
{
  edge e;
  edge_iterator ei;

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "----------[%d]-------------\n", bb->index);

  if (EDGE_COUNT (bb->succs) > 2)
    return false;

  if (gcall *call = safe_dyn_cast <gcall *> (*gsi_last_bb (bb)))
    if (gimple_call_ctrl_altering_p (call))
      return false;

  if (exit_bb)
    {
      if (bb != loop->latch)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "basic block after exit bb but before latch\n");
	  return false;
	}
      else if (!empty_block_p (bb))
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "non empty basic block after exit bb\n");
	  return false;
	}
      else if (bb == loop->latch
	       && bb != exit_bb
	       && !dominated_by_p (CDI_DOMINATORS, bb, exit_bb))
	  {
	    if (dump_file && (dump_flags & TDF_DETAILS))
	      fprintf (dump_file, "latch is not dominated by exit_block\n");
	    return false;
	  }
    }

  /* Be less adventurous and handle only normal edges.  */
  FOR_EACH_EDGE (e, ei, bb->succs)
    if (e->flags & (EDGE_EH | EDGE_ABNORMAL | EDGE_IRREDUCIBLE_LOOP))
      {
	if (dump_file && (dump_flags & TDF_DETAILS))
	  fprintf (dump_file, "Difficult to handle edges\n");
	return false;
      }

  return true;
}

/* Return true when all predecessor blocks of BB are visited.  The
   VISITED bitmap keeps track of the visited blocks.  */

static bool
pred_blocks_visited_p (basic_block bb, bitmap *visited)
{
  edge e;
  edge_iterator ei;
  FOR_EACH_EDGE (e, ei, bb->preds)
    if (!bitmap_bit_p (*visited, e->src->index))
      return false;

  return true;
}

/* Get body of a LOOP in suitable order for if-conversion.  It is
   caller's responsibility to deallocate basic block list.
   If-conversion suitable order is, breadth first sort (BFS) order
   with an additional constraint: select a block only if all its
   predecessors are already selected.  */

static basic_block *
get_loop_body_in_if_conv_order (const class loop *loop)
{
  basic_block *blocks, *blocks_in_bfs_order;
  basic_block bb;
  bitmap visited;
  unsigned int index = 0;
  unsigned int visited_count = 0;

  gcc_assert (loop->num_nodes);
  gcc_assert (loop->latch != EXIT_BLOCK_PTR_FOR_FN (cfun));

  blocks = XCNEWVEC (basic_block, loop->num_nodes);
  visited = BITMAP_ALLOC (NULL);

  blocks_in_bfs_order = get_loop_body_in_bfs_order (loop);

  index = 0;
  while (index < loop->num_nodes)
    {
      bb = blocks_in_bfs_order [index];

      if (bb->flags & BB_IRREDUCIBLE_LOOP)
	{
	  free (blocks_in_bfs_order);
	  BITMAP_FREE (visited);
	  free (blocks);
	  return NULL;
	}

      if (!bitmap_bit_p (visited, bb->index))
	{
	  if (pred_blocks_visited_p (bb, &visited)
	      || bb == loop->header)
	    {
	      /* This block is now visited.  */
	      bitmap_set_bit (visited, bb->index);
	      blocks[visited_count++] = bb;
	    }
	}

      index++;

      if (index == loop->num_nodes
	  && visited_count != loop->num_nodes)
	/* Not done yet.  */
	index = 0;
    }
  free (blocks_in_bfs_order);
  BITMAP_FREE (visited);

  /* Go through loop and reject if-conversion or lowering of bitfields if we
     encounter statements we do not believe the vectorizer will be able to
     handle.  If adding a new type of statement here, make sure
     'ifcvt_local_dce' is also able to handle it propertly.  */
  for (index = 0; index < loop->num_nodes; index++)
    {
      basic_block bb = blocks[index];
      gimple_stmt_iterator gsi;

      bool may_have_nonlocal_labels
	= bb_with_exit_edge_p (loop, bb) || bb == loop->latch;
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	switch (gimple_code (gsi_stmt (gsi)))
	  {
	  case GIMPLE_LABEL:
	    if (!may_have_nonlocal_labels)
	      {
		tree label
		  = gimple_label_label (as_a <glabel *> (gsi_stmt (gsi)));
		if (DECL_NONLOCAL (label) || FORCED_LABEL (label))
		  {
		    free (blocks);
		    return NULL;
		  }
	      }
	    /* Fallthru.  */
	  case GIMPLE_ASSIGN:
	  case GIMPLE_CALL:
	  case GIMPLE_DEBUG:
	  case GIMPLE_COND:
	  case GIMPLE_SWITCH:
	    gimple_set_uid (gsi_stmt (gsi), 0);
	    break;
	  default:
	    free (blocks);
	    return NULL;
	  }
    }
  return blocks;
}

/* Returns true when the analysis of the predicates for all the basic
   blocks in LOOP succeeded.

   predicate_bbs first allocates the predicates of the basic blocks.
   These fields are then initialized with the tree expressions
   representing the predicates under which a basic block is executed
   in the LOOP.  As the loop->header is executed at each iteration, it
   has the "true" predicate.  Other statements executed under a
   condition are predicated with that condition, for example

   | if (x)
   |   S1;
   | else
   |   S2;

   S1 will be predicated with "x", and
   S2 will be predicated with "!x".  */

static void
predicate_bbs (loop_p loop)
{
  unsigned int i;

  for (i = 0; i < loop->num_nodes; i++)
    init_bb_predicate (ifc_bbs[i]);

  for (i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];
      tree cond;

      /* The loop latch and loop exit block are always executed and
	 have no extra conditions to be processed: skip them.  */
      if (bb == loop->latch
	  || bb_with_exit_edge_p (loop, bb))
	{
	  reset_bb_predicate (bb);
	  continue;
	}

      cond = bb_predicate (bb);
      if (gcond *stmt = safe_dyn_cast <gcond *> (*gsi_last_bb (bb)))
	{
	  tree c2;
	  edge true_edge, false_edge;
	  location_t loc = gimple_location (stmt);
	  tree c;
	  /* gcc.dg/fold-bopcond-1.c shows that despite all forwprop passes
	     conditions can remain unfolded because of multiple uses so
	     try to re-fold here, especially to get precision changing
	     conversions sorted out.  Do not simply fold the stmt since
	     this is analysis only.  When conditions were embedded in
	     COND_EXPRs those were folded separately before folding the
	     COND_EXPR but as they are now outside we have to make sure
	     to fold them.  Do it here - another opportunity would be to
	     fold predicates as they are inserted.  */
	  gimple_match_op cexpr (gimple_match_cond::UNCOND,
				 gimple_cond_code (stmt),
				 boolean_type_node,
				 gimple_cond_lhs (stmt),
				 gimple_cond_rhs (stmt));
	  if (cexpr.resimplify (NULL, follow_all_ssa_edges)
	      && cexpr.code.is_tree_code ()
	      && TREE_CODE_CLASS ((tree_code)cexpr.code) == tcc_comparison)
	    c = build2_loc (loc, (tree_code)cexpr.code, boolean_type_node,
			    cexpr.ops[0], cexpr.ops[1]);
	  else
	    c = build2_loc (loc, gimple_cond_code (stmt),
			    boolean_type_node,
			    gimple_cond_lhs (stmt),
			    gimple_cond_rhs (stmt));

	  /* Add new condition into destination's predicate list.  */
	  extract_true_false_edges_from_block (gimple_bb (stmt),
					       &true_edge, &false_edge);

	  /* If C is true, then TRUE_EDGE is taken.  */
	  add_to_dst_predicate_list (loop, true_edge, unshare_expr (cond),
				     unshare_expr (c));

	  /* If C is false, then FALSE_EDGE is taken.  */
	  c2 = build1_loc (loc, TRUTH_NOT_EXPR, boolean_type_node,
			   unshare_expr (c));
	  add_to_dst_predicate_list (loop, false_edge,
				     unshare_expr (cond), c2);

	  cond = NULL_TREE;
	}

      /* Assumes the limited COND like switches checked for earlier.  */
      else if (gswitch *sw = safe_dyn_cast <gswitch *> (*gsi_last_bb (bb)))
	{
	  location_t loc = gimple_location (*gsi_last_bb (bb));

	  tree default_label = CASE_LABEL (gimple_switch_default_label (sw));
	  tree cond_label = CASE_LABEL (gimple_switch_label (sw, 1));

	  edge false_edge = find_edge (bb, label_to_block (cfun, default_label));
	  edge true_edge = find_edge (bb, label_to_block (cfun, cond_label));

	  /* Create chain of switch tests for each case.  */
	  tree switch_cond = NULL_TREE;
	  tree index = gimple_switch_index (sw);
	  for (unsigned i = 1; i < gimple_switch_num_labels (sw); i++)
	    {
	      tree label = gimple_switch_label (sw, i);
	      tree case_cond;
	      if (CASE_HIGH (label))
		{
		  tree low = build2_loc (loc, GE_EXPR,
					 boolean_type_node,
					 index, fold_convert_loc (loc, TREE_TYPE (index),
						 CASE_LOW (label)));
		  tree high = build2_loc (loc, LE_EXPR,
					  boolean_type_node,
					  index, fold_convert_loc (loc, TREE_TYPE (index),
						  CASE_HIGH (label)));
		  case_cond = build2_loc (loc, TRUTH_AND_EXPR,
					  boolean_type_node,
					  low, high);
		}
	      else
		case_cond = build2_loc (loc, EQ_EXPR,
					boolean_type_node,
					index,
					fold_convert_loc (loc, TREE_TYPE (index),
							  CASE_LOW (label)));
	      if (i > 1)
		switch_cond = build2_loc (loc, TRUTH_OR_EXPR,
					  boolean_type_node,
					  case_cond, switch_cond);
	      else
		switch_cond = case_cond;
	    }

	  add_to_dst_predicate_list (loop, true_edge, unshare_expr (cond),
				     unshare_expr (switch_cond));
	  switch_cond = build1_loc (loc, TRUTH_NOT_EXPR, boolean_type_node,
				    unshare_expr (switch_cond));
	  add_to_dst_predicate_list (loop, false_edge,
				     unshare_expr (cond), switch_cond);
	  cond = NULL_TREE;
	}

      /* If current bb has only one successor, then consider it as an
	 unconditional goto.  */
      if (single_succ_p (bb))
	{
	  basic_block bb_n = single_succ (bb);

	  /* The successor bb inherits the predicate of its
	     predecessor.  If there is no predicate in the predecessor
	     bb, then consider the successor bb as always executed.  */
	  if (cond == NULL_TREE)
	    cond = boolean_true_node;

	  add_to_predicate_list (loop, bb_n, cond);
	}
    }

  /* The loop header is always executed.  */
  reset_bb_predicate (loop->header);
  gcc_assert (bb_predicate_gimplified_stmts (loop->header) == NULL
	      && bb_predicate_gimplified_stmts (loop->latch) == NULL);
}

/* Build region by adding loop pre-header and post-header blocks.  */

static vec<basic_block>
build_region (class loop *loop)
{
  vec<basic_block> region = vNULL;
  basic_block exit_bb = NULL;

  gcc_assert (ifc_bbs);
  /* The first element is loop pre-header.  */
  region.safe_push (loop_preheader_edge (loop)->src);

  for (unsigned int i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];
      region.safe_push (bb);
      /* Find loop postheader.  */
      edge e;
      edge_iterator ei;
      FOR_EACH_EDGE (e, ei, bb->succs)
	if (loop_exit_edge_p (loop, e))
	  {
	      exit_bb = e->dest;
	      break;
	  }
    }
  /* The last element is loop post-header.  */
  gcc_assert (exit_bb);
  region.safe_push (exit_bb);
  return region;
}

/* Return true when LOOP is if-convertible.  This is a helper function
   for if_convertible_loop_p.  REFS and DDRS are initialized and freed
   in if_convertible_loop_p.  */

static bool
if_convertible_loop_p_1 (class loop *loop, vec<data_reference_p> *refs)
{
  unsigned int i;
  basic_block exit_bb = NULL;
  vec<basic_block> region;

  calculate_dominance_info (CDI_DOMINATORS);

  for (i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];

      if (!if_convertible_bb_p (loop, bb, exit_bb))
	return false;

      if (bb_with_exit_edge_p (loop, bb))
	exit_bb = bb;
    }

  data_reference_p dr;

  innermost_DR_map
	  = new hash_map<innermost_loop_behavior_hash, data_reference_p>;
  baseref_DR_map = new hash_map<tree_operand_hash, data_reference_p>;

  /* Compute post-dominator tree locally.  */
  region = build_region (loop);
  calculate_dominance_info_for_region (CDI_POST_DOMINATORS, region);

  predicate_bbs (loop);

  /* Free post-dominator tree since it is not used after predication.  */
  free_dominance_info_for_region (cfun, CDI_POST_DOMINATORS, region);
  region.release ();

  for (i = 0; refs->iterate (i, &dr); i++)
    {
      tree ref = DR_REF (dr);

      dr->aux = XNEW (struct ifc_dr);
      DR_BASE_W_UNCONDITIONALLY (dr) = false;
      DR_RW_UNCONDITIONALLY (dr) = false;
      DR_W_UNCONDITIONALLY (dr) = false;
      IFC_DR (dr)->rw_predicate = boolean_false_node;
      IFC_DR (dr)->w_predicate = boolean_false_node;
      IFC_DR (dr)->base_w_predicate = boolean_false_node;
      if (gimple_uid (DR_STMT (dr)) == 0)
	gimple_set_uid (DR_STMT (dr), i + 1);

      /* If DR doesn't have innermost loop behavior or it's a compound
         memory reference, we synthesize its innermost loop behavior
         for hashing.  */
      if (TREE_CODE (ref) == COMPONENT_REF
          || TREE_CODE (ref) == IMAGPART_EXPR
          || TREE_CODE (ref) == REALPART_EXPR
          || !(DR_BASE_ADDRESS (dr) || DR_OFFSET (dr)
	       || DR_INIT (dr) || DR_STEP (dr)))
        {
          while (TREE_CODE (ref) == COMPONENT_REF
	         || TREE_CODE (ref) == IMAGPART_EXPR
	         || TREE_CODE (ref) == REALPART_EXPR)
	    ref = TREE_OPERAND (ref, 0);

	  memset (&DR_INNERMOST (dr), 0, sizeof (DR_INNERMOST (dr)));
	  DR_BASE_ADDRESS (dr) = ref;
        }
      hash_memrefs_baserefs_and_store_DRs_read_written_info (dr);
    }

  for (i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];
      gimple_stmt_iterator itr;

      /* Check the if-convertibility of statements in predicated BBs.  */
      if (!dominated_by_p (CDI_DOMINATORS, loop->latch, bb))
	for (itr = gsi_start_bb (bb); !gsi_end_p (itr); gsi_next (&itr))
	  if (!if_convertible_stmt_p (gsi_stmt (itr), *refs))
	    return false;
    }

  /* Checking PHIs needs to be done after stmts, as the fact whether there
     are any masked loads or stores affects the tests.  */
  for (i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];
      gphi_iterator itr;

      for (itr = gsi_start_phis (bb); !gsi_end_p (itr); gsi_next (&itr))
	if (!if_convertible_phi_p (loop, bb, itr.phi ()))
	  return false;
    }

  if (dump_file)
    fprintf (dump_file, "Applying if-conversion\n");

  return true;
}

/* Return true when LOOP is if-convertible.
   LOOP is if-convertible if:
   - it is innermost,
   - it has two or more basic blocks,
   - it has only one exit,
   - loop header is not the exit edge,
   - if its basic blocks and phi nodes are if convertible.  */

static bool
if_convertible_loop_p (class loop *loop, vec<data_reference_p> *refs)
{
  edge e;
  edge_iterator ei;
  bool res = false;

  /* Handle only innermost loop.  */
  if (!loop || loop->inner)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "not innermost loop\n");
      return false;
    }

  /* If only one block, no need for if-conversion.  */
  if (loop->num_nodes <= 2)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "less than 2 basic blocks\n");
      return false;
    }

  /* If one of the loop header's edge is an exit edge then do not
     apply if-conversion.  */
  FOR_EACH_EDGE (e, ei, loop->header->succs)
    if (loop_exit_edge_p (loop, e))
      return false;

  res = if_convertible_loop_p_1 (loop, refs);

  delete innermost_DR_map;
  innermost_DR_map = NULL;

  delete baseref_DR_map;
  baseref_DR_map = NULL;

  return res;
}

/* Return reduc_1 if has_nop.

   if (...)
     tmp1 = (unsigned type) reduc_1;
     tmp2 = tmp1 + rhs2;
     reduc_3 = (signed type) tmp2.  */
static tree
strip_nop_cond_scalar_reduction (bool has_nop, tree op)
{
  if (!has_nop)
    return op;

  if (TREE_CODE (op) != SSA_NAME)
    return NULL_TREE;

  gassign *stmt = safe_dyn_cast <gassign *> (SSA_NAME_DEF_STMT (op));
  if (!stmt
      || !CONVERT_EXPR_CODE_P (gimple_assign_rhs_code (stmt))
      || !tree_nop_conversion_p (TREE_TYPE (op), TREE_TYPE
				 (gimple_assign_rhs1 (stmt))))
    return NULL_TREE;

  return gimple_assign_rhs1 (stmt);
}

/* Returns true if def-stmt for phi argument ARG is simple increment/decrement
   which is in predicated basic block.
   In fact, the following PHI pattern is searching:
      loop-header:
	reduc_1 = PHI <..., reduc_2>
      ...
	if (...)
	  reduc_3 = ...
	reduc_2 = PHI <reduc_1, reduc_3>

   ARG_0 and ARG_1 are correspondent PHI arguments.
   REDUC, OP0 and OP1 contain reduction stmt and its operands.
   EXTENDED is true if PHI has > 2 arguments.  */

static bool
is_cond_scalar_reduction (gimple *phi, gimple **reduc, tree arg_0, tree arg_1,
			  tree *op0, tree *op1, bool extended, bool* has_nop,
			  gimple **nop_reduc)
{
  tree lhs, r_op1, r_op2, r_nop1, r_nop2;
  gimple *stmt;
  gimple *header_phi = NULL;
  enum tree_code reduction_op;
  basic_block bb = gimple_bb (phi);
  class loop *loop = bb->loop_father;
  edge latch_e = loop_latch_edge (loop);
  imm_use_iterator imm_iter;
  use_operand_p use_p;
  edge e;
  edge_iterator ei;
  bool result = *has_nop = false;
  if (TREE_CODE (arg_0) != SSA_NAME || TREE_CODE (arg_1) != SSA_NAME)
    return false;

  if (!extended && gimple_code (SSA_NAME_DEF_STMT (arg_0)) == GIMPLE_PHI)
    {
      lhs = arg_1;
      header_phi = SSA_NAME_DEF_STMT (arg_0);
      stmt = SSA_NAME_DEF_STMT (arg_1);
    }
  else if (gimple_code (SSA_NAME_DEF_STMT (arg_1)) == GIMPLE_PHI)
    {
      lhs = arg_0;
      header_phi = SSA_NAME_DEF_STMT (arg_1);
      stmt = SSA_NAME_DEF_STMT (arg_0);
    }
  else
    return false;
  if (gimple_bb (header_phi) != loop->header)
    return false;

  if (PHI_ARG_DEF_FROM_EDGE (header_phi, latch_e) != PHI_RESULT (phi))
    return false;

  if (gimple_code (stmt) != GIMPLE_ASSIGN
      || gimple_has_volatile_ops (stmt))
    return false;

  if (!flow_bb_inside_loop_p (loop, gimple_bb (stmt)))
    return false;

  if (!is_predicated (gimple_bb (stmt)))
    return false;

  /* Check that stmt-block is predecessor of phi-block.  */
  FOR_EACH_EDGE (e, ei, gimple_bb (stmt)->succs)
    if (e->dest == bb)
      {
	result = true;
	break;
      }
  if (!result)
    return false;

  if (!has_single_use (lhs))
    return false;

  reduction_op = gimple_assign_rhs_code (stmt);

    /* Catch something like below

     loop-header:
     reduc_1 = PHI <..., reduc_2>
     ...
     if (...)
     tmp1 = (unsigned type) reduc_1;
     tmp2 = tmp1 + rhs2;
     reduc_3 = (signed type) tmp2;

     reduc_2 = PHI <reduc_1, reduc_3>

     and convert to

     reduc_2 = PHI <0, reduc_1>
     tmp1 = (unsigned type)reduc_1;
     ifcvt = cond_expr ? rhs2 : 0
     tmp2 = tmp1 +/- ifcvt;
     reduc_1 = (signed type)tmp2;  */

  if (CONVERT_EXPR_CODE_P (reduction_op))
    {
      lhs = gimple_assign_rhs1 (stmt);
      if (TREE_CODE (lhs) != SSA_NAME
	  || !has_single_use (lhs))
	return false;

      *nop_reduc = stmt;
      stmt = SSA_NAME_DEF_STMT (lhs);
      if (gimple_bb (stmt) != gimple_bb (*nop_reduc)
	  || !is_gimple_assign (stmt))
	return false;

      *has_nop = true;
      reduction_op = gimple_assign_rhs_code (stmt);
    }

  if (reduction_op != PLUS_EXPR
      && reduction_op != MINUS_EXPR
      && reduction_op != MULT_EXPR
      && reduction_op != BIT_IOR_EXPR
      && reduction_op != BIT_XOR_EXPR
      && reduction_op != BIT_AND_EXPR)
    return false;
  r_op1 = gimple_assign_rhs1 (stmt);
  r_op2 = gimple_assign_rhs2 (stmt);

  r_nop1 = strip_nop_cond_scalar_reduction (*has_nop, r_op1);
  r_nop2 = strip_nop_cond_scalar_reduction (*has_nop, r_op2);

  /* Make R_OP1 to hold reduction variable.  */
  if (r_nop2 == PHI_RESULT (header_phi)
      && commutative_tree_code (reduction_op))
    {
      std::swap (r_op1, r_op2);
      std::swap (r_nop1, r_nop2);
    }
  else if (r_nop1 != PHI_RESULT (header_phi))
    return false;

  if (*has_nop)
    {
      /* Check that R_NOP1 is used in nop_stmt or in PHI only.  */
      FOR_EACH_IMM_USE_FAST (use_p, imm_iter, r_nop1)
	{
	  gimple *use_stmt = USE_STMT (use_p);
	  if (is_gimple_debug (use_stmt))
	    continue;
	  if (use_stmt == SSA_NAME_DEF_STMT (r_op1))
	    continue;
	  if (use_stmt != phi)
	    return false;
	}
    }

  /* Check that R_OP1 is used in reduction stmt or in PHI only.  */
  FOR_EACH_IMM_USE_FAST (use_p, imm_iter, r_op1)
    {
      gimple *use_stmt = USE_STMT (use_p);
      if (is_gimple_debug (use_stmt))
	continue;
      if (use_stmt == stmt)
	continue;
      if (gimple_code (use_stmt) != GIMPLE_PHI)
	return false;
    }

  *op0 = r_op1; *op1 = r_op2;
  *reduc = stmt;
  return true;
}

/* Converts conditional scalar reduction into unconditional form, e.g.
     bb_4
       if (_5 != 0) goto bb_5 else goto bb_6
     end_bb_4
     bb_5
       res_6 = res_13 + 1;
     end_bb_5
     bb_6
       # res_2 = PHI <res_13(4), res_6(5)>
     end_bb_6

   will be converted into sequence
    _ifc__1 = _5 != 0 ? 1 : 0;
    res_2 = res_13 + _ifc__1;
  Argument SWAP tells that arguments of conditional expression should be
  swapped.
  If LOOP_VERSIONED is true if we assume that we versioned the loop for
  vectorization.  In that case we can create a COND_OP.
  Returns rhs of resulting PHI assignment.  */

static tree
convert_scalar_cond_reduction (gimple *reduc, gimple_stmt_iterator *gsi,
			       tree cond, tree op0, tree op1, bool swap,
			       bool has_nop, gimple* nop_reduc,
			       bool loop_versioned)
{
  gimple_stmt_iterator stmt_it;
  gimple *new_assign;
  tree rhs;
  tree rhs1 = gimple_assign_rhs1 (reduc);
  tree lhs = gimple_assign_lhs (reduc);
  tree tmp = make_temp_ssa_name (TREE_TYPE (rhs1), NULL, "_ifc_");
  tree c;
  enum tree_code reduction_op  = gimple_assign_rhs_code (reduc);
  tree op_nochange = neutral_op_for_reduction (TREE_TYPE (rhs1), reduction_op,
					       NULL, false);
  gimple_seq stmts = NULL;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Found cond scalar reduction.\n");
      print_gimple_stmt (dump_file, reduc, 0, TDF_SLIM);
    }

  /* If possible create a COND_OP instead of a COND_EXPR and an OP_EXPR.
     The COND_OP will have a neutral_op else value.  */
  internal_fn ifn;
  ifn = get_conditional_internal_fn (reduction_op);
  if (loop_versioned && ifn != IFN_LAST
      && vectorized_internal_fn_supported_p (ifn, TREE_TYPE (lhs))
      && !swap)
    {
      gcall *cond_call = gimple_build_call_internal (ifn, 4,
						     unshare_expr (cond),
						     op0, op1, op0);
      gsi_insert_before (gsi, cond_call, GSI_SAME_STMT);
      gimple_call_set_lhs (cond_call, tmp);
      rhs = tmp;
    }
  else
    {
      /* Build cond expression using COND and constant operand
	 of reduction rhs.  */
      c = fold_build_cond_expr (TREE_TYPE (rhs1),
				unshare_expr (cond),
				swap ? op_nochange : op1,
				swap ? op1 : op_nochange);
      /* Create assignment stmt and insert it at GSI.  */
      new_assign = gimple_build_assign (tmp, c);
      gsi_insert_before (gsi, new_assign, GSI_SAME_STMT);
      /* Build rhs for unconditional increment/decrement/logic_operation.  */
      rhs = gimple_build (&stmts, reduction_op,
			  TREE_TYPE (rhs1), op0, tmp);
    }

  if (has_nop)
    {
      rhs = gimple_convert (&stmts,
			    TREE_TYPE (gimple_assign_lhs (nop_reduc)), rhs);
      stmt_it = gsi_for_stmt (nop_reduc);
      gsi_remove (&stmt_it, true);
      release_defs (nop_reduc);
    }
  gsi_insert_seq_before (gsi, stmts, GSI_SAME_STMT);

  /* Delete original reduction stmt.  */
  stmt_it = gsi_for_stmt (reduc);
  gsi_remove (&stmt_it, true);
  release_defs (reduc);
  return rhs;
}

/* Generate a simplified conditional.  */

static tree
gen_simplified_condition (tree cond, scalar_cond_masked_set_type &cond_set)
{
  /* Check if the value is already live in a previous branch.  This resolves
     nested conditionals from diamond PHI reductions.  */
  if (TREE_CODE (cond) == SSA_NAME)
    {
      gimple *stmt = SSA_NAME_DEF_STMT (cond);
      gassign *assign = NULL;
      if ((assign = as_a <gassign *> (stmt))
	   && gimple_assign_rhs_code (assign) == BIT_AND_EXPR)
	{
	  tree arg1 = gimple_assign_rhs1 (assign);
	  tree arg2 = gimple_assign_rhs2 (assign);
	  if (cond_set.contains ({ arg1, 1 }))
	    arg1 = boolean_true_node;
	  else
	    arg1 = gen_simplified_condition (arg1, cond_set);

	  if (cond_set.contains ({ arg2, 1 }))
	    arg2 = boolean_true_node;
	  else
	    arg2 = gen_simplified_condition (arg2, cond_set);

	  cond = fold_build2 (TRUTH_AND_EXPR, boolean_type_node, arg1, arg2);
	}
    }
  return cond;
}

/* Structure used to track meta-data on PHI arguments used to generate
   most efficient comparison sequence to slatten a PHI node.  */

typedef struct ifcvt_arg_entry
{
  /* The PHI node argument value.  */
  tree arg;

  /* The number of compares required to reach this PHI node from start of the
     BB being if-converted.  */
  unsigned num_compares;

  /* The number of times this PHI node argument appears in the current PHI
     node.  */
  unsigned occurs;

  /* The indices at which this PHI arg occurs inside the PHI node.  */
  vec <int> *indexes;
} ifcvt_arg_entry_t;

/* Produce condition for all occurrences of ARG in PHI node.  Set *INVERT
   as to whether the condition is inverted.  */

static tree
gen_phi_arg_condition (gphi *phi, ifcvt_arg_entry_t &arg,
		       gimple_stmt_iterator *gsi,
		       scalar_cond_masked_set_type &cond_set, bool *invert)
{
  int len;
  int i;
  tree cond = NULL_TREE;
  tree c;
  edge e;

  *invert = false;
  len = arg.indexes->length ();
  gcc_assert (len > 0);
  for (i = 0; i < len; i++)
    {
      e = gimple_phi_arg_edge (phi, (*arg.indexes)[i]);
      c = bb_predicate (e->src);
      if (is_true_predicate (c))
	{
	  cond = c;
	  break;
	}
      /* If we have just a single inverted predicate, signal that and
	 instead invert the COND_EXPR arms.  */
      if (len == 1 && TREE_CODE (c) == TRUTH_NOT_EXPR)
	{
	  c = TREE_OPERAND (c, 0);
	  *invert = true;
	}

      c = gen_simplified_condition (c, cond_set);
      c = force_gimple_operand_gsi (gsi, unshare_expr (c),
				    true, NULL_TREE, true, GSI_SAME_STMT);
      if (cond != NULL_TREE)
	{
	  /* Must build OR expression.  */
	  cond = fold_or_predicates (EXPR_LOCATION (c), c, cond);
	  cond = force_gimple_operand_gsi (gsi, unshare_expr (cond), true,
					   NULL_TREE, true, GSI_SAME_STMT);
	}
      else
	cond = c;

      /* Register the new possibly simplified conditional.  When more than 2
	 entries in a phi node we chain entries in the false branch, so the
	 inverted condition is active.  */
      scalar_cond_masked_key pred_cond ({ cond, 1 });
      if (!*invert)
	pred_cond.inverted_p = !pred_cond.inverted_p;
      cond_set.add (pred_cond);
    }
  gcc_assert (cond != NULL_TREE);
  return cond;
}

/* Find the operand which is different between ARG0_OP and ARG1_OP.
   Returns the operand num where the difference is.
   Set NEWARG0 and NEWARG1 from the different argument.
   Returns -1 if none is found.
   If ARG0_OP/ARG1_OP is commutative also try swapping the
   two commutative operands and return the operand number where
   the difference happens in ARG0_OP. */

static int
find_different_opnum (const gimple_match_op &arg0_op,
		      const gimple_match_op &arg1_op,
		      tree *new_arg0, tree *new_arg1)
{
  unsigned opnum = -1;
  unsigned first;
  first = first_commutative_argument (arg1_op.code, arg1_op.type);
  for (unsigned i = 0; i < arg0_op.num_ops; i++)
    {
      if (!operand_equal_for_phi_arg_p (arg0_op.ops[i],
					arg1_op.ops[i]))
	{
	  /* Can handle only one non equal operand. */
	  if (opnum != -1u)
	    {
	      /* Though if opnum is right before i and opnum is equal
		 to the first communtative argument, handle communtative
		 specially. */
	      if (i == opnum + 1 && opnum == first)
		goto commutative;
	      return -1;
	    }
	  opnum = i;
	}
  }
  /* If all operands are equal only do this is there was single
     operand.  */
  if (opnum == -1u)
    {
      if (arg0_op.num_ops != 1)
	return -1;
      opnum = 0;
    }
  *new_arg0 = arg0_op.ops[opnum];
  *new_arg1 = arg1_op.ops[opnum];
  return opnum;

/* Handle commutative operations. */
commutative:
  gcc_assert (first != -1u);

  /* Check the rest of the arguments to make sure they are the same. */
  for (unsigned i = first + 2; i < arg0_op.num_ops; i++)
    if (!operand_equal_for_phi_arg_p (arg0_op.ops[i],
				      arg1_op.ops[i]))
      return -1;

  /* If the arg0[first+1] and arg1[first] are the same
     then the one which is different is arg0[first] and arg1[first+1]
     return first since this is based on arg0.  */
  if (operand_equal_for_phi_arg_p (arg0_op.ops[first + 1],
				   arg1_op.ops[first]))
    {
       *new_arg0 = arg0_op.ops[first];
       *new_arg1 = arg1_op.ops[first + 1];
       return first;
    }
  /* If the arg0[first] and arg1[first+1] are the same
     then the one which is different is arg0[first+1] and arg1[first]
     return first+1 since this is based on arg0.  */
  if (operand_equal_for_phi_arg_p (arg0_op.ops[first],
				   arg1_op.ops[first + 1]))
    {
       *new_arg0 = arg0_op.ops[first + 1];
       *new_arg1 = arg1_op.ops[first];
       return first + 1;
    }
  return -1;
}

/* Factors out an operation from *ARG0 and *ARG1 and
   create the new statement at GSI. *RES is the
   result of that new statement. Update *ARG0 and *ARG1
   and *RES to the new values if the factoring happened.
   Loops until all of the factoring is completed.  */

static void
factor_out_operators (tree *res, gimple_stmt_iterator *gsi,
		      tree *arg0, tree *arg1, gphi *phi)
{
  gimple_match_op arg0_op, arg1_op;
  bool repeated = false;

again:
  if (TREE_CODE (*arg0) != SSA_NAME || TREE_CODE (*arg1) != SSA_NAME)
    return;

  if (operand_equal_p (*arg0, *arg1))
    return;

  /* If either args have > 1 use, then this transformation actually
     increases the number of expressions evaluated at runtime.  */
  if (repeated
      ? (!has_zero_uses (*arg0) || !has_zero_uses (*arg1))
      : (!has_single_use (*arg0) || !has_single_use (*arg1)))
    return;

  gimple *arg0_def_stmt = SSA_NAME_DEF_STMT (*arg0);
  if (!gimple_extract_op (arg0_def_stmt, &arg0_op))
    return;

  /* At this point there should be no ssa names occuring in abnormals.  */
  gcc_assert (!arg0_op.operands_occurs_in_abnormal_phi ());

  gimple *arg1_def_stmt = SSA_NAME_DEF_STMT (*arg1);
  if (!gimple_extract_op (arg1_def_stmt, &arg1_op))
    return;

  /* At this point there should be no ssa names occuring in abnormals.  */
  gcc_assert (!arg1_op.operands_occurs_in_abnormal_phi ());

  /* No factoring can happen if the codes are different
     or the number operands.  */
  if (arg1_op.code != arg0_op.code
      || arg1_op.num_ops != arg0_op.num_ops)
    return;

  tree new_arg0, new_arg1;
  int opnum = find_different_opnum (arg0_op, arg1_op, &new_arg0, &new_arg1);
  if (opnum == -1)
    return;

  if (!types_compatible_p (TREE_TYPE (new_arg0), TREE_TYPE (new_arg1)))
    return;
  tree new_res = make_ssa_name (TREE_TYPE (new_arg0), NULL);

  /* Create the operation stmt if possible and insert it.  */

  gimple_match_op new_op = arg0_op;
  new_op.ops[opnum] = new_res;
  gimple_seq seq = NULL;
  tree result = *res;
  result = maybe_push_res_to_seq (&new_op, &seq, result);

  /* If we can't create the new statement, release the temp name
     and return back.  */
  if (!result)
    {
      release_ssa_name (new_res);
      return;
    }
  gsi_insert_seq_before (gsi, seq, GSI_CONTINUE_LINKING);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "PHI ");
      print_generic_expr (dump_file, gimple_phi_result (phi));
      fprintf (dump_file,
	       " changed to factor operation out from COND_EXPR.\n");
      fprintf (dump_file, "New stmt with OPERATION that defines ");
      print_generic_expr (dump_file, result);
      fprintf (dump_file, ".\n");
    }

  /* Remove the old operation(s) that has single use.  */
  gimple_stmt_iterator gsi_for_def;

  gsi_for_def = gsi_for_stmt (arg0_def_stmt);
  gsi_remove (&gsi_for_def, true);
  release_defs (arg0_def_stmt);
  gsi_for_def = gsi_for_stmt (arg1_def_stmt);
  gsi_remove (&gsi_for_def, true);
  release_defs (arg1_def_stmt);

  /* Update the arguments and try again.  */
  *arg0 = new_arg0;
  *arg1 = new_arg1;
  *res = new_res;

  /* Update the phi node too. */
  gimple_phi_set_result (phi, new_res);
  gimple_phi_arg (phi, 0)->def = new_arg0;
  gimple_phi_arg (phi, 0)->def = new_arg1;
  update_stmt (phi);

  repeated = true;
  goto again;
}

/* Create the smallest nested conditional possible.  On pre-order we record
   which conditionals are live, and on post-order rewrite the chain by removing
   already active conditions.

   As an example we simplify:

  _7 = a_10 < 0;
  _21 = a_10 >= 0;
  _22 = a_10 < e_11(D);
  _23 = _21 & _22;
  _ifc__42 = _23 ? t_13 : 0;
  t_6 = _7 ? 1 : _ifc__42

  into

  _7 = a_10 < 0;
  _22 = a_10 < e_11(D);
  _ifc__42 = _22 ? t_13 : 0;
  t_6 = _7 ? 1 : _ifc__42;

  which produces better code.  */

static tree
gen_phi_nest_statement (gphi *phi, gimple_stmt_iterator *gsi,
			scalar_cond_masked_set_type &cond_set, tree type,
			gimple **res_stmt, tree lhs0,
			vec<struct ifcvt_arg_entry> &args, unsigned idx)
{
  if (idx == args.length ())
    return args[idx - 1].arg;

  bool invert;
  tree cond = gen_phi_arg_condition (phi, args[idx - 1], gsi, cond_set,
				     &invert);
  tree arg1 = gen_phi_nest_statement (phi, gsi, cond_set, type, res_stmt, lhs0,
				      args, idx + 1);

  unsigned prev = idx;
  unsigned curr = prev - 1;
  tree arg0 = args[curr].arg;
  tree rhs, lhs;
  if (idx > 1)
    lhs = make_temp_ssa_name (type, NULL, "_ifc_");
  else
    lhs = lhs0;

  if (invert)
    rhs = fold_build_cond_expr (type, unshare_expr (cond),
				arg1, arg0);
  else
    rhs = fold_build_cond_expr (type, unshare_expr (cond),
				arg0, arg1);
  gassign *new_stmt = gimple_build_assign (lhs, rhs);
  gsi_insert_before (gsi, new_stmt, GSI_SAME_STMT);
  update_stmt (new_stmt);
  *res_stmt = new_stmt;
  return lhs;
}

/* When flattening a PHI node we have a choice of which conditions to test to
   for all the paths from the start of the dominator block of the BB with the
   PHI node.  If the PHI node has X arguments we have to only test X - 1
   conditions as the last one is implicit.  It does matter which conditions we
   test first.  We should test the shortest condition first (distance here is
   measures in the number of logical operators in the condition) and the
   longest one last.  This allows us to skip testing the most expensive
   condition.  To accomplish this we need to sort the conditions.  P1 and P2
   are sorted first based on the number of logical operations (num_compares)
   and then by how often they occur in the PHI node.  */

static int
cmp_arg_entry (const void *p1, const void *p2, void * /* data.  */)
{
  const ifcvt_arg_entry sval1 = *(const ifcvt_arg_entry *)p1;
  const ifcvt_arg_entry sval2 = *(const ifcvt_arg_entry *)p2;

  if (sval1.num_compares < sval2.num_compares)
    return -1;
  else if (sval1.num_compares > sval2.num_compares)
    return 1;

  if (sval1.occurs < sval2.occurs)
    return -1;
  else if (sval1.occurs > sval2.occurs)
    return 1;

  return 0;
}

/* Replace a scalar PHI node with a COND_EXPR using COND as condition.
   This routine can handle PHI nodes with more than two arguments.

   For example,
     S1: A = PHI <x1(1), x2(5)>
   is converted into,
     S2: A = cond ? x1 : x2;

   The generated code is inserted at GSI that points to the top of
   basic block's statement list.
   If PHI node has more than two arguments a chain of conditional
   expression is produced.
   LOOP_VERSIONED should be true if we know that the loop was versioned for
   vectorization. */


static void
predicate_scalar_phi (gphi *phi, gimple_stmt_iterator *gsi, bool loop_versioned)
{
  gimple *new_stmt = NULL, *reduc, *nop_reduc;
  tree rhs, res, arg0, arg1, op0, op1, scev;
  tree cond;
  unsigned int index0;
  edge e;
  basic_block bb;
  unsigned int i;
  bool has_nop;

  res = gimple_phi_result (phi);
  if (virtual_operand_p (res))
    return;

  if ((rhs = degenerate_phi_result (phi))
      || ((scev = analyze_scalar_evolution (gimple_bb (phi)->loop_father,
					    res))
	  && !chrec_contains_undetermined (scev)
	  && scev != res
	  && (rhs = gimple_phi_arg_def (phi, 0))))
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "Degenerate phi!\n");
	  print_gimple_stmt (dump_file, phi, 0, TDF_SLIM);
	}
      new_stmt = gimple_build_assign (res, rhs);
      gsi_insert_before (gsi, new_stmt, GSI_SAME_STMT);
      update_stmt (new_stmt);
      return;
    }

  bb = gimple_bb (phi);
  /* Keep track of conditionals already seen.  */
  scalar_cond_masked_set_type cond_set;
  if (EDGE_COUNT (bb->preds) == 2)
    {
      /* Predicate ordinary PHI node with 2 arguments.  */
      edge first_edge, second_edge;
      basic_block true_bb;
      first_edge = EDGE_PRED (bb, 0);
      second_edge = EDGE_PRED (bb, 1);
      cond = bb_predicate (first_edge->src);
      cond_set.add ({ cond, 1 });
      if (TREE_CODE (cond) == TRUTH_NOT_EXPR)
	std::swap (first_edge, second_edge);
      if (EDGE_COUNT (first_edge->src->succs) > 1)
	{
	  cond = bb_predicate (second_edge->src);
	  if (TREE_CODE (cond) == TRUTH_NOT_EXPR)
	    cond = TREE_OPERAND (cond, 0);
	  else
	    first_edge = second_edge;
	}
      else
	cond = bb_predicate (first_edge->src);

      /* Gimplify the condition to a valid cond-expr conditonal operand.  */
      cond = gen_simplified_condition (cond, cond_set);
      cond = force_gimple_operand_gsi (gsi, unshare_expr (cond), true,
				       NULL_TREE, true, GSI_SAME_STMT);
      true_bb = first_edge->src;
      if (EDGE_PRED (bb, 1)->src == true_bb)
	{
	  arg0 = gimple_phi_arg_def (phi, 1);
	  arg1 = gimple_phi_arg_def (phi, 0);
	}
      else
	{
	  arg0 = gimple_phi_arg_def (phi, 0);
	  arg1 = gimple_phi_arg_def (phi, 1);
	}

      /* Factor out operand if possible. This can only be done easily
	 for PHI with 2 elements.  */
      factor_out_operators (&res, gsi, &arg0, &arg1, phi);

      if (is_cond_scalar_reduction (phi, &reduc, arg0, arg1,
				    &op0, &op1, false, &has_nop,
				    &nop_reduc))
	{
	  /* Convert reduction stmt into vectorizable form.  */
	  rhs = convert_scalar_cond_reduction (reduc, gsi, cond, op0, op1,
					       true_bb != gimple_bb (reduc),
					       has_nop, nop_reduc,
					       loop_versioned);
	  redundant_ssa_names.safe_push (std::make_pair (res, rhs));
	}
      else
	/* Build new RHS using selected condition and arguments.  */
	rhs = fold_build_cond_expr (TREE_TYPE (res), unshare_expr (cond),
				    arg0, arg1);
      new_stmt = gimple_build_assign (res, rhs);
      gsi_insert_before (gsi, new_stmt, GSI_SAME_STMT);
      gimple_stmt_iterator new_gsi = gsi_for_stmt (new_stmt);
      if (fold_stmt (&new_gsi, follow_all_ssa_edges))
	{
	  new_stmt = gsi_stmt (new_gsi);
	  update_stmt (new_stmt);
	}

      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "new phi replacement stmt\n");
	  print_gimple_stmt (dump_file, new_stmt, 0, TDF_SLIM);
	}
      return;
    }

  /* Create hashmap for PHI node which contain vector of argument indexes
     having the same value.  */
  bool swap = false;
  hash_map<tree_operand_hash, auto_vec<int> > phi_arg_map;
  unsigned int num_args = gimple_phi_num_args (phi);
  /* Vector of different PHI argument values.  */
  auto_vec<ifcvt_arg_entry_t> args;

  /* Compute phi_arg_map, determine the list of unique PHI args and the indices
     where they are in the PHI node.  The indices will be used to determine
     the conditions to apply and their complexity.  */
  for (i = 0; i < num_args; i++)
    {
      tree arg;

      arg = gimple_phi_arg_def (phi, i);
      if (!phi_arg_map.get (arg))
	args.safe_push ({ arg, 0, 0, NULL });
      phi_arg_map.get_or_insert (arg).safe_push (i);
    }

  /* Determine element with max number of occurrences and complexity.  Looking
     at only number of occurrences as a measure for complexity isn't enough as
     all usages can be unique but the comparisons to reach the PHI node differ
     per branch.  */
  for (unsigned i = 0; i < args.length (); i++)
    {
      unsigned int len = 0;
      vec<int> *indices = phi_arg_map.get (args[i].arg);
      for (int index : *indices)
	{
	  edge e = gimple_phi_arg_edge (phi, index);
	  len += get_bb_num_predicate_stmts (e->src);
	}

      unsigned occur = indices->length ();
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "Ranking %d as len=%d, idx=%d\n", i, len, occur);
      args[i].num_compares = len;
      args[i].occurs = occur;
      args[i].indexes = indices;
    }

  /* Sort elements based on rankings ARGS.  */
  args.stablesort (cmp_arg_entry, NULL);

  /* Handle one special case when number of arguments with different values
     is equal 2 and one argument has the only occurrence.  Such PHI can be
     handled as if would have only 2 arguments.  */
  if (args.length () == 2
      && args[0].indexes->length () == 1)
    {
      index0 = (*args[0].indexes)[0];
      arg0 = args[0].arg;
      arg1 = args[1].arg;
      e = gimple_phi_arg_edge (phi, index0);
      cond = bb_predicate (e->src);
      if (TREE_CODE (cond) == TRUTH_NOT_EXPR)
	{
	  swap = true;
	  cond = TREE_OPERAND (cond, 0);
	}
      /* Gimplify the condition to a valid cond-expr conditonal operand.  */
      cond = force_gimple_operand_gsi (gsi, unshare_expr (cond), true,
				       NULL_TREE, true, GSI_SAME_STMT);
      if (!(is_cond_scalar_reduction (phi, &reduc, arg0 , arg1,
				      &op0, &op1, true, &has_nop, &nop_reduc)))
	rhs = fold_build_cond_expr (TREE_TYPE (res), unshare_expr (cond),
				    swap ? arg1 : arg0,
				    swap ? arg0 : arg1);
      else
	{
	  /* Convert reduction stmt into vectorizable form.  */
	  rhs = convert_scalar_cond_reduction (reduc, gsi, cond, op0, op1,
					       swap, has_nop, nop_reduc,
					       loop_versioned);
	  redundant_ssa_names.safe_push (std::make_pair (res, rhs));
	}
      new_stmt = gimple_build_assign (res, rhs);
      gsi_insert_before (gsi, new_stmt, GSI_SAME_STMT);
      update_stmt (new_stmt);
    }
  else
    {
      /* Common case.  */
      tree type = TREE_TYPE (gimple_phi_result (phi));
      gen_phi_nest_statement (phi, gsi, cond_set, type, &new_stmt, res,
			      args, 1);
    }

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "new extended phi replacement stmt\n");
      print_gimple_stmt (dump_file, new_stmt, 0, TDF_SLIM);
    }
}

/* Replaces in LOOP all the scalar phi nodes other than those in the
   LOOP->header block with conditional modify expressions.
   LOOP_VERSIONED should be true if we know that the loop was versioned for
   vectorization. */

static void
predicate_all_scalar_phis (class loop *loop, bool loop_versioned)
{
  basic_block bb;
  unsigned int orig_loop_num_nodes = loop->num_nodes;
  unsigned int i;

  for (i = 1; i < orig_loop_num_nodes; i++)
    {
      gphi *phi;
      gimple_stmt_iterator gsi;
      gphi_iterator phi_gsi;
      bb = ifc_bbs[i];

      if (bb == loop->header)
	continue;

      phi_gsi = gsi_start_phis (bb);
      if (gsi_end_p (phi_gsi))
	continue;

      gsi = gsi_after_labels (bb);
      while (!gsi_end_p (phi_gsi))
	{
	  phi = phi_gsi.phi ();
	  if (virtual_operand_p (gimple_phi_result (phi)))
	    gsi_next (&phi_gsi);
	  else
	    {
	      predicate_scalar_phi (phi, &gsi, loop_versioned);
	      remove_phi_node (&phi_gsi, false);
	    }
	}
    }
}

/* Insert in each basic block of LOOP the statements produced by the
   gimplification of the predicates.  */

static void
insert_gimplified_predicates (loop_p loop)
{
  unsigned int i;

  for (i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];
      gimple_seq stmts;
      if (!is_predicated (bb))
	gcc_assert (bb_predicate_gimplified_stmts (bb) == NULL);
      if (!is_predicated (bb))
	{
	  /* Do not insert statements for a basic block that is not
	     predicated.  Also make sure that the predicate of the
	     basic block is set to true.  */
	  reset_bb_predicate (bb);
	  continue;
	}

      stmts = bb_predicate_gimplified_stmts (bb);
      if (stmts)
	{
	  if (need_to_predicate)
	    {
	      /* Insert the predicate of the BB just after the label,
		 as the if-conversion of memory writes will use this
		 predicate.  */
	      gimple_stmt_iterator gsi = gsi_after_labels (bb);
	      gsi_insert_seq_before (&gsi, stmts, GSI_SAME_STMT);
	    }
	  else
	    {
	      /* Insert the predicate of the BB at the end of the BB
		 as this would reduce the register pressure: the only
		 use of this predicate will be in successor BBs.  */
	      gimple_stmt_iterator gsi = gsi_last_bb (bb);

	      if (gsi_end_p (gsi)
		  || stmt_ends_bb_p (gsi_stmt (gsi)))
		gsi_insert_seq_before (&gsi, stmts, GSI_SAME_STMT);
	      else
		gsi_insert_seq_after (&gsi, stmts, GSI_SAME_STMT);
	    }

	  /* Once the sequence is code generated, set it to NULL.  */
	  set_bb_predicate_gimplified_stmts (bb, NULL, true);
	}
    }
}

/* Helper function for predicate_statements. Returns index of existent
   mask if it was created for given SIZE and -1 otherwise.  */

static int
mask_exists (int size, const vec<int> &vec)
{
  unsigned int ix;
  int v;
  FOR_EACH_VEC_ELT (vec, ix, v)
    if (v == size)
      return (int) ix;
  return -1;
}

/* Helper function for predicate_statements.  STMT is a memory read or
   write and it needs to be predicated by MASK.  Return a statement
   that does so.  */

static gimple *
predicate_load_or_store (gimple_stmt_iterator *gsi, gassign *stmt, tree mask)
{
  gcall *new_stmt;

  tree lhs = gimple_assign_lhs (stmt);
  tree rhs = gimple_assign_rhs1 (stmt);
  tree ref = TREE_CODE (lhs) == SSA_NAME ? rhs : lhs;
  mark_addressable (ref);
  tree addr = force_gimple_operand_gsi (gsi, build_fold_addr_expr (ref),
					true, NULL_TREE, true, GSI_SAME_STMT);
  tree ptr = build_int_cst (reference_alias_ptr_type (ref),
			    get_object_alignment (ref));
  /* Copy points-to info if possible.  */
  if (TREE_CODE (addr) == SSA_NAME && !SSA_NAME_PTR_INFO (addr))
    copy_ref_info (build2 (MEM_REF, TREE_TYPE (ref), addr, ptr),
		   ref);
  if (TREE_CODE (lhs) == SSA_NAME)
    {
      /* Get a zero else value.  This might not be what a target actually uses
	 but we cannot be sure about which vector mode the vectorizer will
	 choose.  Therefore, leave the decision whether we need to force the
	 inactive elements to zero to the vectorizer.  */
      tree els = vect_get_mask_load_else (MASK_LOAD_ELSE_ZERO,
					  TREE_TYPE (lhs));

      new_stmt
	= gimple_build_call_internal (IFN_MASK_LOAD, 4, addr,
				      ptr, mask, els);

      gimple_call_set_lhs (new_stmt, lhs);
      gimple_set_vuse (new_stmt, gimple_vuse (stmt));
    }
  else
    {
      new_stmt
	= gimple_build_call_internal (IFN_MASK_STORE, 4, addr, ptr,
				      mask, rhs);
      gimple_move_vops (new_stmt, stmt);
    }
  gimple_call_set_nothrow (new_stmt, true);
  return new_stmt;
}

/* STMT uses OP_LHS.  Check whether it is equivalent to:

     ... = OP_MASK ? OP_LHS : X;

   Return X if so, otherwise return null.  OP_MASK is an SSA_NAME that is
   known to have value OP_COND.  */

static tree
check_redundant_cond_expr (gimple *stmt, tree op_mask, tree op_cond,
			   tree op_lhs)
{
  gassign *assign = dyn_cast <gassign *> (stmt);
  if (!assign || gimple_assign_rhs_code (assign) != COND_EXPR)
    return NULL_TREE;

  tree use_cond = gimple_assign_rhs1 (assign);
  tree if_true = gimple_assign_rhs2 (assign);
  tree if_false = gimple_assign_rhs3 (assign);

  if ((use_cond == op_mask || operand_equal_p (use_cond, op_cond, 0))
      && if_true == op_lhs)
    return if_false;

  if (inverse_conditions_p (use_cond, op_cond) && if_false == op_lhs)
    return if_true;

  return NULL_TREE;
}

/* Return true if VALUE is available for use at STMT.  SSA_NAMES is
   the set of SSA names defined earlier in STMT's block.  */

static bool
value_available_p (gimple *stmt, hash_set<tree_ssa_name_hash> *ssa_names,
		   tree value)
{
  if (is_gimple_min_invariant (value))
    return true;

  if (TREE_CODE (value) == SSA_NAME)
    {
      if (SSA_NAME_IS_DEFAULT_DEF (value))
	return true;

      basic_block def_bb = gimple_bb (SSA_NAME_DEF_STMT (value));
      basic_block use_bb = gimple_bb (stmt);
      return (def_bb == use_bb
	      ? ssa_names->contains (value)
	      : dominated_by_p (CDI_DOMINATORS, use_bb, def_bb));
    }

  return false;
}

/* Helper function for predicate_statements.  STMT is a potentially-trapping
   arithmetic operation that needs to be predicated by MASK, an SSA_NAME that
   has value COND.  Return a statement that does so.  SSA_NAMES is the set of
   SSA names defined earlier in STMT's block.  */

static gimple *
predicate_rhs_code (gassign *stmt, tree mask, tree cond,
		    hash_set<tree_ssa_name_hash> *ssa_names)
{
  tree lhs = gimple_assign_lhs (stmt);
  tree_code code = gimple_assign_rhs_code (stmt);
  unsigned int nops = gimple_num_ops (stmt);
  internal_fn cond_fn = get_conditional_internal_fn (code);

  /* Construct the arguments to the conditional internal function.   */
  auto_vec<tree, 8> args;
  args.safe_grow (nops + 1, true);
  args[0] = mask;
  for (unsigned int i = 1; i < nops; ++i)
    args[i] = gimple_op (stmt, i);
  args[nops] = NULL_TREE;

  /* Look for uses of the result to see whether they are COND_EXPRs that can
     be folded into the conditional call.  */
  imm_use_iterator imm_iter;
  gimple *use_stmt;
  FOR_EACH_IMM_USE_STMT (use_stmt, imm_iter, lhs)
    {
      tree new_else = check_redundant_cond_expr (use_stmt, mask, cond, lhs);
      if (new_else && value_available_p (stmt, ssa_names, new_else))
	{
	  if (!args[nops])
	    args[nops] = new_else;
	  if (operand_equal_p (new_else, args[nops], 0))
	    {
	      /* We have:

		   LHS = IFN_COND (MASK, ..., ELSE);
		   X = MASK ? LHS : ELSE;

		 which makes X equivalent to LHS.  */
	      tree use_lhs = gimple_assign_lhs (use_stmt);
	      redundant_ssa_names.safe_push (std::make_pair (use_lhs, lhs));
	    }
	}
    }
  if (!args[nops])
    args[nops] = targetm.preferred_else_value (cond_fn, TREE_TYPE (lhs),
					       nops - 1, &args[1]);

  /* Create and insert the call.  */
  gcall *new_stmt = gimple_build_call_internal_vec (cond_fn, args);
  gimple_call_set_lhs (new_stmt, lhs);
  gimple_call_set_nothrow (new_stmt, true);

  return new_stmt;
}

/* Predicate each write to memory in LOOP.

   This function transforms control flow constructs containing memory
   writes of the form:

   | for (i = 0; i < N; i++)
   |   if (cond)
   |     A[i] = expr;

   into the following form that does not contain control flow:

   | for (i = 0; i < N; i++)
   |   A[i] = cond ? expr : A[i];

   The original CFG looks like this:

   | bb_0
   |   i = 0
   | end_bb_0
   |
   | bb_1
   |   if (i < N) goto bb_5 else goto bb_2
   | end_bb_1
   |
   | bb_2
   |   cond = some_computation;
   |   if (cond) goto bb_3 else goto bb_4
   | end_bb_2
   |
   | bb_3
   |   A[i] = expr;
   |   goto bb_4
   | end_bb_3
   |
   | bb_4
   |   goto bb_1
   | end_bb_4

   insert_gimplified_predicates inserts the computation of the COND
   expression at the beginning of the destination basic block:

   | bb_0
   |   i = 0
   | end_bb_0
   |
   | bb_1
   |   if (i < N) goto bb_5 else goto bb_2
   | end_bb_1
   |
   | bb_2
   |   cond = some_computation;
   |   if (cond) goto bb_3 else goto bb_4
   | end_bb_2
   |
   | bb_3
   |   cond = some_computation;
   |   A[i] = expr;
   |   goto bb_4
   | end_bb_3
   |
   | bb_4
   |   goto bb_1
   | end_bb_4

   predicate_statements is then predicating the memory write as follows:

   | bb_0
   |   i = 0
   | end_bb_0
   |
   | bb_1
   |   if (i < N) goto bb_5 else goto bb_2
   | end_bb_1
   |
   | bb_2
   |   if (cond) goto bb_3 else goto bb_4
   | end_bb_2
   |
   | bb_3
   |   cond = some_computation;
   |   A[i] = cond ? expr : A[i];
   |   goto bb_4
   | end_bb_3
   |
   | bb_4
   |   goto bb_1
   | end_bb_4

   and finally combine_blocks removes the basic block boundaries making
   the loop vectorizable:

   | bb_0
   |   i = 0
   |   if (i < N) goto bb_5 else goto bb_1
   | end_bb_0
   |
   | bb_1
   |   cond = some_computation;
   |   A[i] = cond ? expr : A[i];
   |   if (i < N) goto bb_5 else goto bb_4
   | end_bb_1
   |
   | bb_4
   |   goto bb_1
   | end_bb_4
*/

static void
predicate_statements (loop_p loop)
{
  unsigned int i, orig_loop_num_nodes = loop->num_nodes;
  auto_vec<int, 1> vect_sizes;
  auto_vec<tree, 1> vect_masks;
  hash_set<tree_ssa_name_hash> ssa_names;

  for (i = 1; i < orig_loop_num_nodes; i++)
    {
      gimple_stmt_iterator gsi;
      basic_block bb = ifc_bbs[i];
      tree cond = bb_predicate (bb);
      bool swap;
      int index;

      if (is_true_predicate (cond))
	continue;

      swap = false;
      if (TREE_CODE (cond) == TRUTH_NOT_EXPR)
	{
	  swap = true;
	  cond = TREE_OPERAND (cond, 0);
	}

      vect_sizes.truncate (0);
      vect_masks.truncate (0);

      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi);)
	{
	  gassign *stmt = dyn_cast <gassign *> (gsi_stmt (gsi));
	  if (!stmt)
	    ;
	  else if (is_false_predicate (cond)
		   && gimple_vdef (stmt))
	    {
	      unlink_stmt_vdef (stmt);
	      gsi_remove (&gsi, true);
	      release_defs (stmt);
	      continue;
	    }
	  else if (gimple_plf (stmt, GF_PLF_2)
		   && is_gimple_assign (stmt))
	    {
	      tree lhs = gimple_assign_lhs (stmt);
	      tree mask;
	      gimple *new_stmt;
	      gimple_seq stmts = NULL;
	      machine_mode mode = TYPE_MODE (TREE_TYPE (lhs));
	      /* We checked before setting GF_PLF_2 that an equivalent
		 integer mode exists.  */
	      int bitsize = GET_MODE_BITSIZE (mode).to_constant ();
	      if (!vect_sizes.is_empty ()
		  && (index = mask_exists (bitsize, vect_sizes)) != -1)
		/* Use created mask.  */
		mask = vect_masks[index];
	      else
		{
		  if (COMPARISON_CLASS_P (cond))
		    mask = gimple_build (&stmts, TREE_CODE (cond),
					 boolean_type_node,
					 TREE_OPERAND (cond, 0),
					 TREE_OPERAND (cond, 1));
		  else
		    mask = cond;

		  if (swap)
		    {
		      tree true_val
			= constant_boolean_node (true, TREE_TYPE (mask));
		      mask = gimple_build (&stmts, BIT_XOR_EXPR,
					   TREE_TYPE (mask), mask, true_val);
		    }
		  gsi_insert_seq_before (&gsi, stmts, GSI_SAME_STMT);

		  /* Save mask and its size for further use.  */
		  vect_sizes.safe_push (bitsize);
		  vect_masks.safe_push (mask);
		}
	      if (gimple_assign_single_p (stmt))
		new_stmt = predicate_load_or_store (&gsi, stmt, mask);
	      else
		new_stmt = predicate_rhs_code (stmt, mask, cond, &ssa_names);

	      gsi_replace (&gsi, new_stmt, true);
	    }
	  else if (gimple_needing_rewrite_undefined (stmt))
	    rewrite_to_defined_unconditional (&gsi);
	  else if (gimple_vdef (stmt))
	    {
	      tree lhs = gimple_assign_lhs (stmt);
	      tree rhs = gimple_assign_rhs1 (stmt);
	      tree type = TREE_TYPE (lhs);

	      lhs = ifc_temp_var (type, unshare_expr (lhs), &gsi);
	      rhs = ifc_temp_var (type, unshare_expr (rhs), &gsi);
	      if (swap)
		std::swap (lhs, rhs);
	      cond = force_gimple_operand_gsi (&gsi, unshare_expr (cond), true,
					       NULL_TREE, true, GSI_SAME_STMT);
	      rhs = fold_build_cond_expr (type, unshare_expr (cond), rhs, lhs);
	      gimple_assign_set_rhs1 (stmt, ifc_temp_var (type, rhs, &gsi));
	      update_stmt (stmt);
	    }

	  if (gimple_plf (gsi_stmt (gsi), GF_PLF_2)
	      && is_gimple_call (gsi_stmt (gsi)))
	    {
	      /* Convert functions that have a SIMD clone to IFN_MASK_CALL.
		 This will cause the vectorizer to match the "in branch"
		 clone variants, and serves to build the mask vector
		 in a natural way.  */
	      tree mask = cond;
	      gcall *call = dyn_cast <gcall *> (gsi_stmt (gsi));
	      tree orig_fn = gimple_call_fn (call);
	      int orig_nargs = gimple_call_num_args (call);
	      auto_vec<tree> args;
	      args.safe_push (orig_fn);
	      for (int i = 0; i < orig_nargs; i++)
		args.safe_push (gimple_call_arg (call, i));
	      /* If `swap', we invert the mask used for the if branch for use
		 when masking the function call.  */
	      if (swap)
		{
		  gimple_seq stmts = NULL;
		  tree true_val
		    = constant_boolean_node (true, TREE_TYPE (mask));
		  mask = gimple_build (&stmts, BIT_XOR_EXPR,
				       TREE_TYPE (mask), mask, true_val);
		  gsi_insert_seq_before (&gsi, stmts, GSI_SAME_STMT);
		}
	      args.safe_push (mask);

	      /* Replace the call with a IFN_MASK_CALL that has the extra
		 condition parameter. */
	      gcall *new_call = gimple_build_call_internal_vec (IFN_MASK_CALL,
								args);
	      gimple_call_set_lhs (new_call, gimple_call_lhs (call));
	      gsi_replace (&gsi, new_call, true);
	    }

	  tree lhs = gimple_get_lhs (gsi_stmt (gsi));
	  if (lhs && TREE_CODE (lhs) == SSA_NAME)
	    ssa_names.add (lhs);
	  gsi_next (&gsi);
	}
      ssa_names.empty ();
    }
}

/* Remove all GIMPLE_CONDs and GIMPLE_LABELs and GIMPLE_SWITCH of all
   the basic blocks other than the exit and latch of the LOOP.  Also
   resets the GIMPLE_DEBUG information.  */

static void
remove_conditions_and_labels (loop_p loop)
{
  gimple_stmt_iterator gsi;
  unsigned int i;

  for (i = 0; i < loop->num_nodes; i++)
    {
      basic_block bb = ifc_bbs[i];

      if (bb_with_exit_edge_p (loop, bb)
	  || bb == loop->latch)
	continue;

      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); )
	switch (gimple_code (gsi_stmt (gsi)))
	  {
	  case GIMPLE_COND:
	  case GIMPLE_LABEL:
	  case GIMPLE_SWITCH:
	    gsi_remove (&gsi, true);
	    break;

	  case GIMPLE_DEBUG:
	    /* ??? Should there be conditional GIMPLE_DEBUG_BINDs?  */
	    if (gimple_debug_bind_p (gsi_stmt (gsi)))
	      {
		gimple_debug_bind_reset_value (gsi_stmt (gsi));
		update_stmt (gsi_stmt (gsi));
	      }
	    gsi_next (&gsi);
	    break;

	  default:
	    gsi_next (&gsi);
	  }
    }
}

/* Combine all the basic blocks from LOOP into one or two super basic
   blocks.  Replace PHI nodes with conditional modify expressions.
   LOOP_VERSIONED should be true if we know that the loop was versioned for
   vectorization. */

static void
combine_blocks (class loop *loop, bool loop_versioned)
{
  basic_block bb, exit_bb, merge_target_bb;
  unsigned int orig_loop_num_nodes = loop->num_nodes;
  unsigned int i;
  edge e;
  edge_iterator ei;

  /* Reset flow-sensitive info before predicating stmts or PHIs we
     might fold.  */
  for (i = 0; i < orig_loop_num_nodes; i++)
    {
      bb = ifc_bbs[i];
      if (is_predicated (bb))
	{
	  for (auto gsi = gsi_start_phis (bb);
	       !gsi_end_p (gsi); gsi_next (&gsi))
	    reset_flow_sensitive_info (gimple_phi_result (*gsi));
	  for (auto gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	    {
	      gimple *stmt = gsi_stmt (gsi);
	      ssa_op_iter i;
	      tree op;
	      FOR_EACH_SSA_TREE_OPERAND (op, stmt, i, SSA_OP_DEF)
		reset_flow_sensitive_info (op);
	    }
	}
    }

  remove_conditions_and_labels (loop);
  insert_gimplified_predicates (loop);
  predicate_all_scalar_phis (loop, loop_versioned);

  if (need_to_predicate || need_to_rewrite_undefined)
    predicate_statements (loop);

  /* Merge basic blocks.  */
  exit_bb = single_exit (loop)->src;
  gcc_assert (exit_bb != loop->latch);
  for (i = 0; i < orig_loop_num_nodes; i++)
    {
      bb = ifc_bbs[i];
      free_bb_predicate (bb);
    }

  merge_target_bb = loop->header;

  /* Get at the virtual def valid for uses starting at the first block
     we merge into the header.  Without a virtual PHI the loop has the
     same virtual use on all stmts.  */
  gphi *vphi = get_virtual_phi (loop->header);
  tree last_vdef = NULL_TREE;
  if (vphi)
    {
      last_vdef = gimple_phi_result (vphi);
      for (gimple_stmt_iterator gsi = gsi_start_bb (loop->header);
	   ! gsi_end_p (gsi); gsi_next (&gsi))
	if (gimple_vdef (gsi_stmt (gsi)))
	  last_vdef = gimple_vdef (gsi_stmt (gsi));
    }
  for (i = 1; i < orig_loop_num_nodes; i++)
    {
      gimple_stmt_iterator gsi;
      gimple_stmt_iterator last;

      bb = ifc_bbs[i];

      if (bb == exit_bb || bb == loop->latch)
	continue;

      /* We release virtual PHIs late because we have to propagate them
         out using the current VUSE.  The def might be the one used
	 after the loop.  */
      vphi = get_virtual_phi (bb);
      if (vphi)
	{
	  /* When there's just loads inside the loop a stray virtual
	     PHI merging the uses can appear, update last_vdef from
	     it.  */
	  if (!last_vdef)
	    last_vdef = gimple_phi_arg_def (vphi, 0);
	  imm_use_iterator iter;
	  use_operand_p use_p;
	  gimple *use_stmt;
	  FOR_EACH_IMM_USE_STMT (use_stmt, iter, gimple_phi_result (vphi))
	    {
	      FOR_EACH_IMM_USE_ON_STMT (use_p, iter)
		SET_USE (use_p, last_vdef);
	    }
	  if (SSA_NAME_OCCURS_IN_ABNORMAL_PHI (gimple_phi_result (vphi)))
	    SSA_NAME_OCCURS_IN_ABNORMAL_PHI (last_vdef) = 1;
	  gsi = gsi_for_stmt (vphi);
	  remove_phi_node (&gsi, true);
	}

      /* Make stmts member of loop->header and clear range info from all stmts
	 in BB which is now no longer executed conditional on a predicate we
	 could have derived it from.  */
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple *stmt = gsi_stmt (gsi);
	  gimple_set_bb (stmt, merge_target_bb);
	  /* Update virtual operands.  */
	  if (last_vdef)
	    {
	      use_operand_p use_p = ssa_vuse_operand (stmt);
	      if (use_p
		  && USE_FROM_PTR (use_p) != last_vdef)
		SET_USE (use_p, last_vdef);
	      if (gimple_vdef (stmt))
		last_vdef = gimple_vdef (stmt);
	    }
	  else
	    /* If this is the first load we arrive at update last_vdef
	       so we handle stray PHIs correctly.  */
	    last_vdef = gimple_vuse (stmt);
	}

      /* Update stmt list.  */
      last = gsi_last_bb (merge_target_bb);
      gsi_insert_seq_after_without_update (&last, bb_seq (bb), GSI_NEW_STMT);
      set_bb_seq (bb, NULL);
    }

  /* Fixup virtual operands in the exit block.  */
  if (exit_bb
      && exit_bb != loop->header)
    {
      /* We release virtual PHIs late because we have to propagate them
         out using the current VUSE.  The def might be the one used
	 after the loop.  */
      vphi = get_virtual_phi (exit_bb);
      if (vphi)
	{
	  /* When there's just loads inside the loop a stray virtual
	     PHI merging the uses can appear, update last_vdef from
	     it.  */
	  if (!last_vdef)
	    last_vdef = gimple_phi_arg_def (vphi, 0);
	  imm_use_iterator iter;
	  use_operand_p use_p;
	  gimple *use_stmt;
	  FOR_EACH_IMM_USE_STMT (use_stmt, iter, gimple_phi_result (vphi))
	    {
	      FOR_EACH_IMM_USE_ON_STMT (use_p, iter)
		SET_USE (use_p, last_vdef);
	    }
	  if (SSA_NAME_OCCURS_IN_ABNORMAL_PHI (gimple_phi_result (vphi)))
	    SSA_NAME_OCCURS_IN_ABNORMAL_PHI (last_vdef) = 1;
	  gimple_stmt_iterator gsi = gsi_for_stmt (vphi);
	  remove_phi_node (&gsi, true);
	}
    }

  /* Now remove all the edges in the loop, except for those from the exit
     block and delete the blocks we elided.  */
  for (i = 1; i < orig_loop_num_nodes; i++)
    {
      bb = ifc_bbs[i];

      for (ei = ei_start (bb->preds); (e = ei_safe_edge (ei));)
	{
	  if (e->src == exit_bb)
	    ei_next (&ei);
	  else
	    remove_edge (e);
	}
    }
  for (i = 1; i < orig_loop_num_nodes; i++)
    {
      bb = ifc_bbs[i];

      if (bb == exit_bb || bb == loop->latch)
	continue;

      delete_basic_block (bb);
    }

  /* Re-connect the exit block.  */
  if (exit_bb != NULL)
    {
      if (exit_bb != loop->header)
	{
	  /* Connect this node to loop header.  */
	  make_single_succ_edge (loop->header, exit_bb, EDGE_FALLTHRU);
	  set_immediate_dominator (CDI_DOMINATORS, exit_bb, loop->header);
	}

      /* Redirect non-exit edges to loop->latch.  */
      FOR_EACH_EDGE (e, ei, exit_bb->succs)
	{
	  if (!loop_exit_edge_p (loop, e))
	    redirect_edge_and_branch (e, loop->latch);
	}
      set_immediate_dominator (CDI_DOMINATORS, loop->latch, exit_bb);
    }
  else
    {
      /* If the loop does not have an exit, reconnect header and latch.  */
      make_edge (loop->header, loop->latch, EDGE_FALLTHRU);
      set_immediate_dominator (CDI_DOMINATORS, loop->latch, loop->header);
    }

  /* If possible, merge loop header to the block with the exit edge.
     This reduces the number of basic blocks to two, to please the
     vectorizer that handles only loops with two nodes.  */
  if (exit_bb
      && exit_bb != loop->header)
    {
      if (can_merge_blocks_p (loop->header, exit_bb))
	merge_blocks (loop->header, exit_bb);
    }

  free (ifc_bbs);
  ifc_bbs = NULL;
}

/* Version LOOP before if-converting it; the original loop
   will be if-converted, the new copy of the loop will not,
   and the LOOP_VECTORIZED internal call will be guarding which
   loop to execute.  The vectorizer pass will fold this
   internal call into either true or false.

   Note that this function intentionally invalidates profile.  Both edges
   out of LOOP_VECTORIZED must have 100% probability so the profile remains
   consistent after the condition is folded in the vectorizer.  */

static class loop *
version_loop_for_if_conversion (class loop *loop, vec<gimple *> *preds)
{
  basic_block cond_bb;
  tree cond = make_ssa_name (boolean_type_node);
  class loop *new_loop;
  gimple *g;
  gimple_stmt_iterator gsi;
  unsigned int save_length = 0;

  g = gimple_build_call_internal (IFN_LOOP_VECTORIZED, 2,
				  build_int_cst (integer_type_node, loop->num),
				  integer_zero_node);
  gimple_call_set_lhs (g, cond);

  void **saved_preds = NULL;
  if (any_complicated_phi || need_to_predicate)
    {
      /* Save BB->aux around loop_version as that uses the same field.  */
      save_length = loop->inner ? loop->inner->num_nodes : loop->num_nodes;
      saved_preds = XALLOCAVEC (void *, save_length);
      for (unsigned i = 0; i < save_length; i++)
	saved_preds[i] = ifc_bbs[i]->aux;
    }

  initialize_original_copy_tables ();
  /* At this point we invalidate porfile confistency until IFN_LOOP_VECTORIZED
     is re-merged in the vectorizer.  */
  new_loop = loop_version (loop, cond, &cond_bb,
			   profile_probability::always (),
			   profile_probability::always (),
			   profile_probability::always (),
			   profile_probability::always (), true);
  free_original_copy_tables ();

  if (any_complicated_phi || need_to_predicate)
    for (unsigned i = 0; i < save_length; i++)
      ifc_bbs[i]->aux = saved_preds[i];

  if (new_loop == NULL)
    return NULL;

  new_loop->dont_vectorize = true;
  new_loop->force_vectorize = false;
  gsi = gsi_last_bb (cond_bb);
  gimple_call_set_arg (g, 1, build_int_cst (integer_type_node, new_loop->num));
  if (preds)
    preds->safe_push (g);
  gsi_insert_before (&gsi, g, GSI_SAME_STMT);
  update_ssa (TODO_update_ssa_no_phi);
  return new_loop;
}

/* Return true when LOOP satisfies the follow conditions that will
   allow it to be recognized by the vectorizer for outer-loop
   vectorization:
    - The loop is not the root node of the loop tree.
    - The loop has exactly one inner loop.
    - The loop has a single exit.
    - The loop header has a single successor, which is the inner
      loop header.
    - Each of the inner and outer loop latches have a single
      predecessor.
    - The loop exit block has a single predecessor, which is the
      inner loop's exit block.  */

static bool
versionable_outer_loop_p (class loop *loop)
{
  if (!loop_outer (loop)
      || loop->dont_vectorize
      || !loop->inner
      || loop->inner->next
      || !single_exit (loop)
      || !single_succ_p (loop->header)
      || single_succ (loop->header) != loop->inner->header
      || !single_pred_p (loop->latch)
      || !single_pred_p (loop->inner->latch))
    return false;

  basic_block outer_exit = single_pred (loop->latch);
  basic_block inner_exit = single_pred (loop->inner->latch);

  if (!single_pred_p (outer_exit) || single_pred (outer_exit) != inner_exit)
    return false;

  if (dump_file)
    fprintf (dump_file, "Found vectorizable outer loop for versioning\n");

  return true;
}

/* Performs splitting of critical edges.  Skip splitting and return false
   if LOOP will not be converted because:

     - LOOP is not well formed.
     - LOOP has PHI with more than MAX_PHI_ARG_NUM arguments.

   Last restriction is valid only if AGGRESSIVE_IF_CONV is false.  */

static bool
ifcvt_split_critical_edges (class loop *loop, bool aggressive_if_conv)
{
  basic_block *body;
  basic_block bb;
  unsigned int num = loop->num_nodes;
  unsigned int i;
  edge e;
  edge_iterator ei;
  auto_vec<edge> critical_edges;

  /* Loop is not well formed.  */
  if (loop->inner)
    return false;

  body = get_loop_body (loop);
  for (i = 0; i < num; i++)
    {
      bb = body[i];
      if (!aggressive_if_conv
	  && phi_nodes (bb)
	  && EDGE_COUNT (bb->preds) > MAX_PHI_ARG_NUM)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file,
		     "BB %d has complicated PHI with more than %u args.\n",
		     bb->index, MAX_PHI_ARG_NUM);

	  free (body);
	  return false;
	}
      if (bb == loop->latch || bb_with_exit_edge_p (loop, bb))
	continue;

      /* Skip basic blocks not ending with conditional branch.  */
      if (!safe_is_a <gcond *> (*gsi_last_bb (bb))
	  && !safe_is_a <gswitch *> (*gsi_last_bb (bb)))
	continue;

      FOR_EACH_EDGE (e, ei, bb->succs)
	if (EDGE_CRITICAL_P (e) && e->dest->loop_father == loop)
	  critical_edges.safe_push (e);
    }
  free (body);

  while (critical_edges.length () > 0)
    {
      e = critical_edges.pop ();
      /* Don't split if bb can be predicated along non-critical edge.  */
      if (EDGE_COUNT (e->dest->preds) > 2 || all_preds_critical_p (e->dest))
	split_edge (e);
    }

  return true;
}

/* Delete redundant statements produced by predication which prevents
   loop vectorization.  */

static void
ifcvt_local_dce (class loop *loop)
{
  gimple *stmt;
  gimple *stmt1;
  gimple *phi;
  gimple_stmt_iterator gsi;
  auto_vec<gimple *> worklist;
  enum gimple_code code;
  use_operand_p use_p;
  imm_use_iterator imm_iter;

  /* The loop has a single BB only.  */
  basic_block bb = loop->header;
  tree latch_vdef = NULL_TREE;

  worklist.create (64);
  /* Consider all phi as live statements.  */
  for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      phi = gsi_stmt (gsi);
      gimple_set_plf (phi, GF_PLF_2, true);
      worklist.safe_push (phi);
      if (virtual_operand_p (gimple_phi_result (phi)))
	latch_vdef = PHI_ARG_DEF_FROM_EDGE (phi, loop_latch_edge (loop));
    }
  /* Consider load/store statements, CALL and COND as live.  */
  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      stmt = gsi_stmt (gsi);
      if (is_gimple_debug (stmt))
	{
	  gimple_set_plf (stmt, GF_PLF_2, true);
	  continue;
	}
      if (gimple_store_p (stmt) || gimple_assign_load_p (stmt))
	{
	  gimple_set_plf (stmt, GF_PLF_2, true);
	  worklist.safe_push (stmt);
	  continue;
	}
      code = gimple_code (stmt);
      if (code == GIMPLE_COND || code == GIMPLE_CALL || code == GIMPLE_SWITCH)
	{
	  gimple_set_plf (stmt, GF_PLF_2, true);
	  worklist.safe_push (stmt);
	  continue;
	}
      gimple_set_plf (stmt, GF_PLF_2, false);

      if (code == GIMPLE_ASSIGN)
	{
	  tree lhs = gimple_assign_lhs (stmt);
	  FOR_EACH_IMM_USE_FAST (use_p, imm_iter, lhs)
	    {
	      stmt1 = USE_STMT (use_p);
	      if (!is_gimple_debug (stmt1) && gimple_bb (stmt1) != bb)
		{
		  gimple_set_plf (stmt, GF_PLF_2, true);
		  worklist.safe_push (stmt);
		  break;
		}
	    }
	}
    }
  /* Propagate liveness through arguments of live stmt.  */
  while (worklist.length () > 0)
    {
      ssa_op_iter iter;
      use_operand_p use_p;
      tree use;

      stmt = worklist.pop ();
      FOR_EACH_PHI_OR_STMT_USE (use_p, stmt, iter, SSA_OP_USE)
	{
	  use = USE_FROM_PTR (use_p);
	  if (TREE_CODE (use) != SSA_NAME)
	    continue;
	  stmt1 = SSA_NAME_DEF_STMT (use);
	  if (gimple_bb (stmt1) != bb || gimple_plf (stmt1, GF_PLF_2))
	    continue;
	  gimple_set_plf (stmt1, GF_PLF_2, true);
	  worklist.safe_push (stmt1);
	}
    }
  /* Delete dead statements.  */
  gsi = gsi_last_bb (bb);
  while (!gsi_end_p (gsi))
    {
      gimple_stmt_iterator gsiprev = gsi;
      gsi_prev (&gsiprev);
      stmt = gsi_stmt (gsi);
      if (!gimple_has_volatile_ops (stmt)
	  && gimple_store_p (stmt)
	  && gimple_vdef (stmt))
	{
	  tree lhs = gimple_get_lhs (stmt);
	  ao_ref write;
	  ao_ref_init (&write, lhs);

	  if (dse_classify_store (&write, stmt, false, NULL, NULL, latch_vdef)
	      == DSE_STORE_DEAD)
	    delete_dead_or_redundant_assignment (&gsi, "dead");
	  gsi = gsiprev;
	  continue;
	}

      if (gimple_plf (stmt, GF_PLF_2))
	{
	  gsi = gsiprev;
	  continue;
	}
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "Delete dead stmt in bb#%d\n", bb->index);
	  print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);
	}
      gsi_remove (&gsi, true);
      release_defs (stmt);
      gsi = gsiprev;
    }
}

/* Return true if VALUE is already available on edge PE.  */

static bool
ifcvt_available_on_edge_p (edge pe, tree value)
{
  if (is_gimple_min_invariant (value))
    return true;

  if (TREE_CODE (value) == SSA_NAME)
    {
      basic_block def_bb = gimple_bb (SSA_NAME_DEF_STMT (value));
      if (!def_bb || dominated_by_p (CDI_DOMINATORS, pe->dest, def_bb))
	return true;
    }

  return false;
}

/* Return true if STMT can be hoisted from if-converted loop LOOP to
   edge PE.  */

static bool
ifcvt_can_hoist (class loop *loop, edge pe, gimple *stmt)
{
  if (auto *call = dyn_cast<gcall *> (stmt))
    {
      if (gimple_call_internal_p (call)
	  && internal_fn_mask_index (gimple_call_internal_fn (call)) >= 0)
	return false;
    }
  else if (auto *assign = dyn_cast<gassign *> (stmt))
    {
      if (gimple_assign_rhs_code (assign) == COND_EXPR)
	return false;
    }
  else
    return false;

  if (gimple_has_side_effects (stmt)
      || gimple_could_trap_p (stmt)
      || stmt_could_throw_p (cfun, stmt)
      || gimple_vdef (stmt)
      || gimple_vuse (stmt))
    return false;

  int num_args = gimple_num_args (stmt);
  if (pe != loop_preheader_edge (loop))
    {
      for (int i = 0; i < num_args; ++i)
	if (!ifcvt_available_on_edge_p (pe, gimple_arg (stmt, i)))
	  return false;
    }
  else
    {
      for (int i = 0; i < num_args; ++i)
	if (!expr_invariant_in_loop_p (loop, gimple_arg (stmt, i)))
	  return false;
    }

  return true;
}

/* Hoist invariant statements from LOOP to edge PE.  */

static void
ifcvt_hoist_invariants (class loop *loop, edge pe)
{
  /* Only hoist from the now unconditionally executed part of the loop.  */
  basic_block bb = loop->header;
  gimple_stmt_iterator hoist_gsi = {};
  for (gimple_stmt_iterator gsi = gsi_start_bb (bb); !gsi_end_p (gsi);)
    {
      gimple *stmt = gsi_stmt (gsi);
      if (ifcvt_can_hoist (loop, pe, stmt))
	{
	  /* Once we've hoisted one statement, insert other statements
	     after it.  */
	  gsi_remove (&gsi, false);
	  if (hoist_gsi.ptr)
	    gsi_insert_after (&hoist_gsi, stmt, GSI_NEW_STMT);
	  else
	    {
	      gsi_insert_on_edge_immediate (pe, stmt);
	      hoist_gsi = gsi_for_stmt (stmt);
	    }
	  continue;
	}
      gsi_next (&gsi);
    }
}

/* Returns the DECL_FIELD_BIT_OFFSET of the bitfield accesse in stmt iff its
   type mode is not BLKmode.  If BITPOS is not NULL it will hold the poly_int64
   value of the DECL_FIELD_BIT_OFFSET of the bitfield access and STRUCT_EXPR,
   if not NULL, will hold the tree representing the base struct of this
   bitfield.  */

static tree
get_bitfield_rep (gassign *stmt, bool write, tree *bitpos,
		  tree *struct_expr)
{
  tree comp_ref = write ? gimple_assign_lhs (stmt)
			: gimple_assign_rhs1 (stmt);

  tree field_decl = TREE_OPERAND (comp_ref, 1);
  tree ref_offset = component_ref_field_offset (comp_ref);
  tree rep_decl = DECL_BIT_FIELD_REPRESENTATIVE (field_decl);

  /* Bail out if the representative is not a suitable type for a scalar
     register variable.  */
  if (!is_gimple_reg_type (TREE_TYPE (rep_decl)))
    return NULL_TREE;

  /* Bail out if the DECL_SIZE of the field_decl isn't the same as the BF's
     precision.  */
  unsigned HOST_WIDE_INT bf_prec
    = TYPE_PRECISION (TREE_TYPE (gimple_assign_lhs (stmt)));
  if (compare_tree_int (DECL_SIZE (field_decl), bf_prec) != 0)
    return NULL_TREE;

  if (TREE_CODE (DECL_FIELD_OFFSET (rep_decl)) != INTEGER_CST
      || TREE_CODE (ref_offset) != INTEGER_CST)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "\t Bitfield NOT OK to lower,"
			    " offset is non-constant.\n");
      return NULL_TREE;
    }

  if (struct_expr)
    *struct_expr = TREE_OPERAND (comp_ref, 0);

  if (bitpos)
    {
      /* To calculate the bitposition of the BITFIELD_REF we have to determine
	 where our bitfield starts in relation to the container REP_DECL. The
	 DECL_FIELD_OFFSET of the original bitfield's member FIELD_DECL tells
	 us how many bytes from the start of the structure there are until the
	 start of the group of bitfield members the FIELD_DECL belongs to,
	 whereas DECL_FIELD_BIT_OFFSET will tell us how many bits from that
	 position our actual bitfield member starts.  For the container
	 REP_DECL adding DECL_FIELD_OFFSET and DECL_FIELD_BIT_OFFSET will tell
	 us the distance between the start of the structure and the start of
	 the container, though the first is in bytes and the later other in
	 bits.  With this in mind we calculate the bit position of our new
	 BITFIELD_REF by subtracting the number of bits between the start of
	 the structure and the container from the number of bits from the start
	 of the structure and the actual bitfield member. */
      tree bf_pos = fold_build2 (MULT_EXPR, bitsizetype,
				 ref_offset,
				 build_int_cst (bitsizetype, BITS_PER_UNIT));
      bf_pos = fold_build2 (PLUS_EXPR, bitsizetype, bf_pos,
			    DECL_FIELD_BIT_OFFSET (field_decl));
      tree rep_pos = fold_build2 (MULT_EXPR, bitsizetype,
				  DECL_FIELD_OFFSET (rep_decl),
				  build_int_cst (bitsizetype, BITS_PER_UNIT));
      rep_pos = fold_build2 (PLUS_EXPR, bitsizetype, rep_pos,
			     DECL_FIELD_BIT_OFFSET (rep_decl));

      *bitpos = fold_build2 (MINUS_EXPR, bitsizetype, bf_pos, rep_pos);
    }

  return rep_decl;

}

/* Lowers the bitfield described by DATA.
   For a write like:

   struct.bf = _1;

   lower to:

   __ifc_1 = struct.<representative>;
   __ifc_2 = BIT_INSERT_EXPR (__ifc_1, _1, bitpos);
   struct.<representative> = __ifc_2;

   For a read:

   _1 = struct.bf;

    lower to:

    __ifc_1 = struct.<representative>;
    _1 =  BIT_FIELD_REF (__ifc_1, bitsize, bitpos);

    where representative is a legal load that contains the bitfield value,
    bitsize is the size of the bitfield and bitpos the offset to the start of
    the bitfield within the representative.  */

static void
lower_bitfield (gassign *stmt, bool write)
{
  tree struct_expr;
  tree bitpos;
  tree rep_decl = get_bitfield_rep (stmt, write, &bitpos, &struct_expr);
  tree rep_type = TREE_TYPE (rep_decl);
  tree bf_type = TREE_TYPE (gimple_assign_lhs (stmt));

  gimple_stmt_iterator gsi = gsi_for_stmt (stmt);
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Lowering:\n");
      print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);
      fprintf (dump_file, "to:\n");
    }

  /* REP_COMP_REF is a COMPONENT_REF for the representative.  NEW_VAL is it's
     defining SSA_NAME.  */
  tree rep_comp_ref = build3 (COMPONENT_REF, rep_type, struct_expr, rep_decl,
			      NULL_TREE);
  tree new_val = ifc_temp_var (rep_type, rep_comp_ref, &gsi);

  if (dump_file && (dump_flags & TDF_DETAILS))
    print_gimple_stmt (dump_file, SSA_NAME_DEF_STMT (new_val), 0, TDF_SLIM);

  if (write)
    {
      new_val = ifc_temp_var (rep_type,
			      build3 (BIT_INSERT_EXPR, rep_type, new_val,
				      unshare_expr (gimple_assign_rhs1 (stmt)),
				      bitpos), &gsi);

      if (dump_file && (dump_flags & TDF_DETAILS))
	print_gimple_stmt (dump_file, SSA_NAME_DEF_STMT (new_val), 0, TDF_SLIM);

      gimple *new_stmt = gimple_build_assign (unshare_expr (rep_comp_ref),
					      new_val);
      gimple_move_vops (new_stmt, stmt);
      gsi_insert_before (&gsi, new_stmt, GSI_SAME_STMT);

      if (dump_file && (dump_flags & TDF_DETAILS))
	print_gimple_stmt (dump_file, new_stmt, 0, TDF_SLIM);
    }
  else
    {
      tree bfr = build3 (BIT_FIELD_REF, bf_type, new_val,
			 build_int_cst (bitsizetype, TYPE_PRECISION (bf_type)),
			 bitpos);
      new_val = ifc_temp_var (bf_type, bfr, &gsi);

      gimple *new_stmt = gimple_build_assign (gimple_assign_lhs (stmt),
					      new_val);
      gimple_move_vops (new_stmt, stmt);
      gsi_insert_before (&gsi, new_stmt, GSI_SAME_STMT);

      if (dump_file && (dump_flags & TDF_DETAILS))
	print_gimple_stmt (dump_file, new_stmt, 0, TDF_SLIM);
    }

  gsi_remove (&gsi, true);
}

/* Return TRUE if there are bitfields to lower in this LOOP.  Fill TO_LOWER
   with data structures representing these bitfields.  */

static bool
bitfields_to_lower_p (class loop *loop,
		      vec <gassign *> &reads_to_lower,
		      vec <gassign *> &writes_to_lower)
{
  gimple_stmt_iterator gsi;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Analyzing loop %d for bitfields:\n", loop->num);
    }

  for (unsigned i = 0; i < loop->num_nodes; ++i)
    {
      basic_block bb = ifc_bbs[i];
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gassign *stmt = dyn_cast<gassign*> (gsi_stmt (gsi));
	  if (!stmt)
	    continue;

	  tree op = gimple_assign_lhs (stmt);
	  bool write = TREE_CODE (op) == COMPONENT_REF;

	  if (!write)
	    op = gimple_assign_rhs1 (stmt);

	  if (TREE_CODE (op) != COMPONENT_REF)
	    continue;

	  if (DECL_BIT_FIELD_TYPE (TREE_OPERAND (op, 1)))
	    {
	      if (dump_file && (dump_flags & TDF_DETAILS))
		print_gimple_stmt (dump_file, stmt, 0, TDF_SLIM);

	      if (TREE_THIS_VOLATILE (op))
		{
		  if (dump_file && (dump_flags & TDF_DETAILS))
		    fprintf (dump_file, "\t Bitfield NO OK to lower,"
					" the access is volatile.\n");
		  return false;
		}

	      if (!INTEGRAL_TYPE_P (TREE_TYPE (op)))
		{
		  if (dump_file && (dump_flags & TDF_DETAILS))
		    fprintf (dump_file, "\t Bitfield NO OK to lower,"
					" field type is not Integral.\n");
		  return false;
		}

	      if (!get_bitfield_rep (stmt, write, NULL, NULL))
		{
		  if (dump_file && (dump_flags & TDF_DETAILS))
		    fprintf (dump_file, "\t Bitfield NOT OK to lower,"
					" representative is BLKmode.\n");
		  return false;
		}

	      if (dump_file && (dump_flags & TDF_DETAILS))
		fprintf (dump_file, "\tBitfield OK to lower.\n");
	      if (write)
		writes_to_lower.safe_push (stmt);
	      else
		reads_to_lower.safe_push (stmt);
	    }
	}
    }
  return !reads_to_lower.is_empty () || !writes_to_lower.is_empty ();
}


/* If-convert LOOP when it is legal.  For the moment this pass has no
   profitability analysis.  Returns non-zero todo flags when something
   changed.  */

unsigned int
tree_if_conversion (class loop *loop, vec<gimple *> *preds)
{
  unsigned int todo = 0;
  bool aggressive_if_conv;
  class loop *rloop;
  auto_vec <gassign *, 4> reads_to_lower;
  auto_vec <gassign *, 4> writes_to_lower;
  bitmap exit_bbs;
  edge pe;
  auto_vec<data_reference_p, 10> refs;
  bool loop_versioned;

 again:
  rloop = NULL;
  ifc_bbs = NULL;
  need_to_lower_bitfields = false;
  need_to_ifcvt = false;
  need_to_predicate = false;
  need_to_rewrite_undefined = false;
  any_complicated_phi = false;
  loop_versioned = false;

  /* Apply more aggressive if-conversion when loop or its outer loop were
     marked with simd pragma.  When that's the case, we try to if-convert
     loop containing PHIs with more than MAX_PHI_ARG_NUM arguments.  */
  aggressive_if_conv = loop->force_vectorize;
  if (!aggressive_if_conv)
    {
      class loop *outer_loop = loop_outer (loop);
      if (outer_loop && outer_loop->force_vectorize)
	aggressive_if_conv = true;
    }

  /* If there are more than two BBs in the loop then there is at least one if
     to convert.  */
  if (loop->num_nodes > 2
      && !ifcvt_split_critical_edges (loop, aggressive_if_conv))
    goto cleanup;

  ifc_bbs = get_loop_body_in_if_conv_order (loop);
  if (!ifc_bbs)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "Irreducible loop\n");
      goto cleanup;
    }

  if (find_data_references_in_loop (loop, &refs) == chrec_dont_know)
    goto cleanup;

  if (loop->num_nodes > 2)
    {
      /* More than one loop exit is too much to handle.  */
      if (!single_exit (loop))
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "Can not ifcvt due to multiple exits\n");
	}
      else
	{
	  need_to_ifcvt = true;

	  if (!if_convertible_loop_p (loop, &refs)
	      || !dbg_cnt (if_conversion_tree))
	    goto cleanup;

	  if ((need_to_predicate || any_complicated_phi)
	      && ((!flag_tree_loop_vectorize && !loop->force_vectorize)
		  || loop->dont_vectorize))
	    goto cleanup;
	}
    }

  if ((flag_tree_loop_vectorize || loop->force_vectorize)
      && !loop->dont_vectorize)
    need_to_lower_bitfields = bitfields_to_lower_p (loop, reads_to_lower,
						    writes_to_lower);

  if (!need_to_ifcvt && !need_to_lower_bitfields)
    goto cleanup;

  /* The edge to insert invariant stmts on.  */
  pe = loop_preheader_edge (loop);

  /* Since we have no cost model, always version loops unless the user
     specified -ftree-loop-if-convert or unless versioning is required.
     Either version this loop, or if the pattern is right for outer-loop
     vectorization, version the outer loop.  In the latter case we will
     still if-convert the original inner loop.  */
  if (need_to_lower_bitfields
      || need_to_predicate
      || any_complicated_phi
      || flag_tree_loop_if_convert != 1)
    {
      class loop *vloop
	= (versionable_outer_loop_p (loop_outer (loop))
	   ? loop_outer (loop) : loop);
      class loop *nloop = version_loop_for_if_conversion (vloop, preds);
      if (nloop == NULL)
	goto cleanup;
      if (vloop != loop)
	{
	  /* If versionable_outer_loop_p decided to version the
	     outer loop, version also the inner loop of the non-vectorized
	     loop copy.  So we transform:
	      loop1
		loop2
	     into:
	      if (LOOP_VECTORIZED (1, 3))
		{
		  loop1
		    loop2
		}
	      else
		loop3 (copy of loop1)
		  if (LOOP_VECTORIZED (4, 5))
		    loop4 (copy of loop2)
		  else
		    loop5 (copy of loop4)  */
	  gcc_assert (nloop->inner && nloop->inner->next == NULL);
	  rloop = nloop->inner;
	}
      else
	/* If we versioned loop then make sure to insert invariant
	   stmts before the .LOOP_VECTORIZED check since the vectorizer
	   will re-use that for things like runtime alias versioning
	   whose condition can end up using those invariants.  */
	pe = single_pred_edge (gimple_bb (preds->last ()));

      loop_versioned = true;
    }

  if (need_to_lower_bitfields)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "-------------------------\n");
	  fprintf (dump_file, "Start lowering bitfields\n");
	}
      while (!reads_to_lower.is_empty ())
	lower_bitfield (reads_to_lower.pop (), false);
      while (!writes_to_lower.is_empty ())
	lower_bitfield (writes_to_lower.pop (), true);

      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "Done lowering bitfields\n");
	  fprintf (dump_file, "-------------------------\n");
	}
    }
  if (need_to_ifcvt)
    {
      /* Before we rewrite edges we'll record their original position in the
	 edge map such that we can map the edges between the ifcvt and the
	 non-ifcvt loop during peeling.  */
      uintptr_t idx = 0;
      for (edge exit : get_loop_exit_edges (loop))
	exit->aux = (void*)idx++;

      /* Now all statements are if-convertible.  Combine all the basic
	 blocks into one huge basic block doing the if-conversion
	 on-the-fly.  */
      combine_blocks (loop, loop_versioned);
    }

  std::pair <tree, tree> *name_pair;
  unsigned ssa_names_idx;
  FOR_EACH_VEC_ELT (redundant_ssa_names, ssa_names_idx, name_pair)
    replace_uses_by (name_pair->first, name_pair->second);
  redundant_ssa_names.release ();

  /* Perform local CSE, this esp. helps the vectorizer analysis if loads
     and stores are involved.  CSE only the loop body, not the entry
     PHIs, those are to be kept in sync with the non-if-converted copy.
     ???  We'll still keep dead stores though.  */
  exit_bbs = BITMAP_ALLOC (NULL);
  for (edge exit : get_loop_exit_edges (loop))
    bitmap_set_bit (exit_bbs, exit->dest->index);
  todo |= do_rpo_vn (cfun, loop_preheader_edge (loop), exit_bbs,
		     false, true, true);

  /* Delete dead predicate computations.  */
  ifcvt_local_dce (loop);
  BITMAP_FREE (exit_bbs);

  ifcvt_hoist_invariants (loop, pe);

  todo |= TODO_cleanup_cfg;

 cleanup:
  data_reference_p dr;
  unsigned int i;
  for (i = 0; refs.iterate (i, &dr); i++)
    {
      free (dr->aux);
      free_data_ref (dr);
    }
  refs.truncate (0);

  if (ifc_bbs)
    {
      unsigned int i;

      for (i = 0; i < loop->num_nodes; i++)
	free_bb_predicate (ifc_bbs[i]);

      free (ifc_bbs);
      ifc_bbs = NULL;
    }
  if (rloop != NULL)
    {
      loop = rloop;
      reads_to_lower.truncate (0);
      writes_to_lower.truncate (0);
      goto again;
    }

  return todo;
}

/* Tree if-conversion pass management.  */

namespace {

const pass_data pass_data_if_conversion =
{
  GIMPLE_PASS, /* type */
  "ifcvt", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_TREE_LOOP_IFCVT, /* tv_id */
  ( PROP_cfg | PROP_ssa ), /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_if_conversion : public gimple_opt_pass
{
public:
  pass_if_conversion (gcc::context *ctxt)
    : gimple_opt_pass (pass_data_if_conversion, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override;
  unsigned int execute (function *) final override;

}; // class pass_if_conversion

bool
pass_if_conversion::gate (function *fun)
{
  return (((flag_tree_loop_vectorize || fun->has_force_vectorize_loops)
	   && flag_tree_loop_if_convert != 0)
	  || flag_tree_loop_if_convert == 1);
}

unsigned int
pass_if_conversion::execute (function *fun)
{
  unsigned todo = 0;

  if (number_of_loops (fun) <= 1)
    return 0;

  auto_vec<gimple *> preds;
  for (auto loop : loops_list (cfun, 0))
    if (flag_tree_loop_if_convert == 1
	|| ((flag_tree_loop_vectorize || loop->force_vectorize)
	    && !loop->dont_vectorize))
      todo |= tree_if_conversion (loop, &preds);

  if (todo)
    {
      free_numbers_of_iterations_estimates (fun);
      scev_reset ();
    }

  if (flag_checking)
    {
      basic_block bb;
      FOR_EACH_BB_FN (bb, fun)
	gcc_assert (!bb->aux);
    }

  /* Perform IL update now, it might elide some loops.  */
  if (todo & TODO_cleanup_cfg)
    {
      cleanup_tree_cfg ();
      if (need_ssa_update_p (fun))
	todo |= TODO_update_ssa;
    }
  if (todo & TODO_update_ssa_any)
    update_ssa (todo & TODO_update_ssa_any);

  /* If if-conversion elided the loop fall back to the original one.  Likewise
     if the loops are not nested in the same outer loop.  */
  for (unsigned i = 0; i < preds.length (); ++i)
    {
      gimple *g = preds[i];
      if (!gimple_bb (g))
	continue;
      auto ifcvt_loop = get_loop (fun, tree_to_uhwi (gimple_call_arg (g, 0)));
      auto orig_loop = get_loop (fun, tree_to_uhwi (gimple_call_arg (g, 1)));
      if (!ifcvt_loop || !orig_loop)
	{
	  if (dump_file)
	    fprintf (dump_file, "If-converted loop vanished\n");
	  fold_loop_internal_call (g, boolean_false_node);
	}
      else if (loop_outer (ifcvt_loop) != loop_outer (orig_loop))
	{
	  if (dump_file)
	    fprintf (dump_file, "If-converted loop in different outer loop\n");
	  fold_loop_internal_call (g, boolean_false_node);
	}
    }

  return 0;
}

} // anon namespace

gimple_opt_pass *
make_pass_if_conversion (gcc::context *ctxt)
{
  return new pass_if_conversion (ctxt);
}
