/* Matching subroutines in all sizes, shapes and colors.
   Copyright (C) 2000-2025 Free Software Foundation, Inc.
   Contributed by Andy Vaught

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

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "options.h"
#include "gfortran.h"
#include "match.h"
#include "parse.h"

int gfc_matching_ptr_assignment = 0;
int gfc_matching_procptr_assignment = 0;
bool gfc_matching_prefix = false;

/* Stack of SELECT TYPE statements.  */
gfc_select_type_stack *select_type_stack = NULL;

/* List of type parameter expressions.  */
gfc_actual_arglist *type_param_spec_list;

/* For debugging and diagnostic purposes.  Return the textual representation
   of the intrinsic operator OP.  */
const char *
gfc_op2string (gfc_intrinsic_op op)
{
  switch (op)
    {
    case INTRINSIC_UPLUS:
    case INTRINSIC_PLUS:
      return "+";

    case INTRINSIC_UMINUS:
    case INTRINSIC_MINUS:
      return "-";

    case INTRINSIC_POWER:
      return "**";
    case INTRINSIC_CONCAT:
      return "//";
    case INTRINSIC_TIMES:
      return "*";
    case INTRINSIC_DIVIDE:
      return "/";

    case INTRINSIC_AND:
      return ".and.";
    case INTRINSIC_OR:
      return ".or.";
    case INTRINSIC_EQV:
      return ".eqv.";
    case INTRINSIC_NEQV:
      return ".neqv.";

    case INTRINSIC_EQ_OS:
      return ".eq.";
    case INTRINSIC_EQ:
      return "==";
    case INTRINSIC_NE_OS:
      return ".ne.";
    case INTRINSIC_NE:
      return "/=";
    case INTRINSIC_GE_OS:
      return ".ge.";
    case INTRINSIC_GE:
      return ">=";
    case INTRINSIC_LE_OS:
      return ".le.";
    case INTRINSIC_LE:
      return "<=";
    case INTRINSIC_LT_OS:
      return ".lt.";
    case INTRINSIC_LT:
      return "<";
    case INTRINSIC_GT_OS:
      return ".gt.";
    case INTRINSIC_GT:
      return ">";
    case INTRINSIC_NOT:
      return ".not.";

    case INTRINSIC_ASSIGN:
      return "=";

    case INTRINSIC_PARENTHESES:
      return "parens";

    case INTRINSIC_NONE:
      return "none";

    /* DTIO  */
    case INTRINSIC_FORMATTED:
      return "formatted";
    case INTRINSIC_UNFORMATTED:
      return "unformatted";

    default:
      break;
    }

  gfc_internal_error ("gfc_op2string(): Bad code");
  /* Not reached.  */
}


/******************** Generic matching subroutines ************************/

/* Matches a member separator. With standard FORTRAN this is '%', but with
   DEC structures we must carefully match dot ('.').
   Because operators are spelled ".op.", a dotted string such as "x.y.z..."
   can be either a component reference chain or a combination of binary
   operations.
   There is no real way to win because the string may be grammatically
   ambiguous. The following rules help avoid ambiguities - they match
   some behavior of other (older) compilers. If the rules here are changed
   the test cases should be updated. If the user has problems with these rules
   they probably deserve the consequences. Consider "x.y.z":
     (1) If any user defined operator ".y." exists, this is always y(x,z)
         (even if ".y." is the wrong type and/or x has a member y).
     (2) Otherwise if x has a member y, and y is itself a derived type,
         this is (x->y)->z, even if an intrinsic operator exists which
         can handle (x,z).
     (3) If x has no member y or (x->y) is not a derived type but ".y."
         is an intrinsic operator (such as ".eq."), this is y(x,z).
     (4) Lastly if there is no operator ".y." and x has no member "y", it is an
         error.
   It is worth noting that the logic here does not support mixed use of member
   accessors within a single string. That is, even if x has component y and y
   has component z, the following are all syntax errors:
         "x%y.z"  "x.y%z" "(x.y).z"  "(x%y)%z"
 */

match
gfc_match_member_sep(gfc_symbol *sym)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  locus dot_loc, start_loc;
  gfc_intrinsic_op iop;
  match m;
  gfc_symbol *tsym;
  gfc_component *c = NULL;

  /* What a relief: '%' is an unambiguous member separator.  */
  if (gfc_match_char ('%') == MATCH_YES)
    return MATCH_YES;

  /* Beware ye who enter here.  */
  if (!flag_dec_structure || !sym)
    return MATCH_NO;

  tsym = NULL;

  /* We may be given either a derived type variable or the derived type
    declaration itself (which actually contains the components);
    we need the latter to search for components.  */
  if (gfc_fl_struct (sym->attr.flavor))
    tsym = sym;
  else if (gfc_bt_struct (sym->ts.type))
    tsym = sym->ts.u.derived;

  iop = INTRINSIC_NONE;
  name[0] = '\0';
  m = MATCH_NO;

  /* If we have to reject come back here later.  */
  start_loc = gfc_current_locus;

  /* Look for a component access next.  */
  if (gfc_match_char ('.') != MATCH_YES)
    return MATCH_NO;

  /* If we accept, come back here.  */
  dot_loc = gfc_current_locus;

  /* Try to match a symbol name following the dot.  */
  if (gfc_match_name (name) != MATCH_YES)
    {
      gfc_error ("Expected structure component or operator name "
		 "after %<.%> at %C");
      goto error;
    }

  /* If no dot follows we have "x.y" which should be a component access.  */
  if (gfc_match_char ('.') != MATCH_YES)
    goto yes;

  /* Now we have a string "x.y.z" which could be a nested member access
    (x->y)->z or a binary operation y on x and z.  */

  /* First use any user-defined operators ".y."  */
  if (gfc_find_uop (name, sym->ns) != NULL)
    goto no;

  /* Match accesses to existing derived-type components for
    derived-type vars: "x.y.z" = (x->y)->z  */
  c = gfc_find_component(tsym, name, false, true, NULL);
  if (c && (gfc_bt_struct (c->ts.type) || c->ts.type == BT_CLASS))
    goto yes;

  /* If y is not a component or has no members, try intrinsic operators.  */
  gfc_current_locus = start_loc;
  if (gfc_match_intrinsic_op (&iop) != MATCH_YES)
    {
      /* If ".y." is not an intrinsic operator but y was a valid non-
        structure component, match and leave the trailing dot to be
        dealt with later.  */
      if (c)
        goto yes;

      gfc_error ("%qs is neither a defined operator nor a "
                 "structure component in dotted string at %C", name);
      goto error;
    }

  /* .y. is an intrinsic operator, overriding any possible member access.  */
  goto no;

  /* Return keeping the current locus consistent with the match result.  */
error:
  m = MATCH_ERROR;
no:
  gfc_current_locus = start_loc;
  return m;
yes:
  gfc_current_locus = dot_loc;
  return MATCH_YES;
}


/* This function scans the current statement counting the opened and closed
   parenthesis to make sure they are balanced.  */

match
gfc_match_parens (void)
{
  locus old_loc, where;
  int count;
  gfc_instring instring;
  gfc_char_t c, quote;

  old_loc = gfc_current_locus;
  count = 0;
  instring = NONSTRING;
  quote = ' ';

  for (;;)
    {
      if (count > 0)
	where = gfc_current_locus;
      c = gfc_next_char_literal (instring);
      if (c == '\n')
	break;
      if (quote == ' ' && ((c == '\'') || (c == '"')))
	{
	  quote = c;
	  instring = INSTRING_WARN;
	  continue;
	}
      if (quote != ' ' && c == quote)
	{
	  quote = ' ';
	  instring = NONSTRING;
	  continue;
	}

      if (c == '(' && quote == ' ')
	{
	  count++;
	}
      if (c == ')' && quote == ' ')
	{
	  count--;
	  where = gfc_current_locus;
	}
    }

  gfc_current_locus = old_loc;

  if (count != 0)
    {
      gfc_error ("Missing %qs in statement at or before %L",
		 count > 0? ")":"(", &where);
      return MATCH_ERROR;
    }

  return MATCH_YES;
}


/* See if the next character is a special character that has
   escaped by a \ via the -fbackslash option.  */

match
gfc_match_special_char (gfc_char_t *res)
{
  int len, i;
  gfc_char_t c, n;
  match m;

  m = MATCH_YES;

  switch ((c = gfc_next_char_literal (INSTRING_WARN)))
    {
    case 'a':
      *res = '\a';
      break;
    case 'b':
      *res = '\b';
      break;
    case 't':
      *res = '\t';
      break;
    case 'f':
      *res = '\f';
      break;
    case 'n':
      *res = '\n';
      break;
    case 'r':
      *res = '\r';
      break;
    case 'v':
      *res = '\v';
      break;
    case '\\':
      *res = '\\';
      break;
    case '0':
      *res = '\0';
      break;

    case 'x':
    case 'u':
    case 'U':
      /* Hexadecimal form of wide characters.  */
      len = (c == 'x' ? 2 : (c == 'u' ? 4 : 8));
      n = 0;
      for (i = 0; i < len; i++)
	{
	  char buf[2] = { '\0', '\0' };

	  c = gfc_next_char_literal (INSTRING_WARN);
	  if (!gfc_wide_fits_in_byte (c)
	      || !gfc_check_digit ((unsigned char) c, 16))
	    return MATCH_NO;

	  buf[0] = (unsigned char) c;
	  n = n << 4;
	  n += strtol (buf, NULL, 16);
	}
      *res = n;
      break;

    default:
      /* Unknown backslash codes are simply not expanded.  */
      m = MATCH_NO;
      break;
    }

  return m;
}


/* In free form, match at least one space.  Always matches in fixed
   form.  */

match
gfc_match_space (void)
{
  locus old_loc;
  char c;

  if (gfc_current_form == FORM_FIXED)
    return MATCH_YES;

  old_loc = gfc_current_locus;

  c = gfc_next_ascii_char ();
  if (!gfc_is_whitespace (c))
    {
      gfc_current_locus = old_loc;
      return MATCH_NO;
    }

  gfc_gobble_whitespace ();

  return MATCH_YES;
}


/* Match an end of statement.  End of statement is optional
   whitespace, followed by a ';' or '\n' or comment '!'.  If a
   semicolon is found, we continue to eat whitespace and semicolons.  */

match
gfc_match_eos (void)
{
  locus old_loc;
  int flag;
  char c;

  flag = 0;

  for (;;)
    {
      old_loc = gfc_current_locus;
      gfc_gobble_whitespace ();

      c = gfc_next_ascii_char ();
      switch (c)
	{
	case '!':
	  do
	    {
	      c = gfc_next_ascii_char ();
	    }
	  while (c != '\n');

	  /* Fall through.  */

	case '\n':
	  return MATCH_YES;

	case ';':
	  flag = 1;
	  continue;
	}

      break;
    }

  gfc_current_locus = old_loc;
  return (flag) ? MATCH_YES : MATCH_NO;
}


/* Match a literal integer on the input, setting the value on
   MATCH_YES.  Literal ints occur in kind-parameters as well as
   old-style character length specifications.  If cnt is non-NULL it
   will be set to the number of digits.
   When gobble_ws is false, do not skip over leading blanks.  */

match
gfc_match_small_literal_int (int *value, int *cnt, bool gobble_ws)
{
  locus old_loc;
  char c;
  int i, j;

  old_loc = gfc_current_locus;

  *value = -1;
  if (gobble_ws)
    gfc_gobble_whitespace ();
  c = gfc_next_ascii_char ();
  if (cnt)
    *cnt = 0;

  if (!ISDIGIT (c))
    {
      gfc_current_locus = old_loc;
      return MATCH_NO;
    }

  i = c - '0';
  j = 1;

  for (;;)
    {
      old_loc = gfc_current_locus;
      c = gfc_next_ascii_char ();

      if (!ISDIGIT (c))
	break;

      i = 10 * i + c - '0';
      j++;

      if (i > 99999999)
	{
	  gfc_error ("Integer too large at %C");
	  return MATCH_ERROR;
	}
    }

  gfc_current_locus = old_loc;

  *value = i;
  if (cnt)
    *cnt = j;
  return MATCH_YES;
}


/* Match a small, constant integer expression, like in a kind
   statement.  On MATCH_YES, 'value' is set.  */

match
gfc_match_small_int (int *value)
{
  gfc_expr *expr;
  match m;
  int i;

  m = gfc_match_expr (&expr);
  if (m != MATCH_YES)
    return m;

  if (gfc_extract_int (expr, &i, 1))
    m = MATCH_ERROR;
  gfc_free_expr (expr);

  *value = i;
  return m;
}


/* Matches a statement label.  Uses gfc_match_small_literal_int() to
   do most of the work.  */

match
gfc_match_st_label (gfc_st_label **label)
{
  locus old_loc;
  match m;
  int i, cnt;

  old_loc = gfc_current_locus;

  m = gfc_match_small_literal_int (&i, &cnt);
  if (m != MATCH_YES)
    return m;

  if (cnt > 5)
    {
      gfc_error ("Too many digits in statement label at %C");
      goto cleanup;
    }

  if (i == 0)
    {
      gfc_error ("Statement label at %C is zero");
      goto cleanup;
    }

  *label = gfc_get_st_label (i);
  return MATCH_YES;

cleanup:

  gfc_current_locus = old_loc;
  return MATCH_ERROR;
}


/* Match and validate a label associated with a named IF, DO or SELECT
   statement.  If the symbol does not have the label attribute, we add
   it.  We also make sure the symbol does not refer to another
   (active) block.  A matched label is pointed to by gfc_new_block.  */

static match
gfc_match_label (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  match m;

  gfc_new_block = NULL;

  m = gfc_match (" %n :", name);
  if (m != MATCH_YES)
    return m;

  if (gfc_get_symbol (name, NULL, &gfc_new_block))
    {
      gfc_error ("Label name %qs at %C is ambiguous", name);
      return MATCH_ERROR;
    }

  if (gfc_new_block->attr.flavor == FL_LABEL)
    {
      gfc_error ("Duplicate construct label %qs at %C", name);
      return MATCH_ERROR;
    }

  if (!gfc_add_flavor (&gfc_new_block->attr, FL_LABEL,
		       gfc_new_block->name, NULL))
    return MATCH_ERROR;

  return MATCH_YES;
}


/* See if the current input looks like a name of some sort.  Modifies
   the passed buffer which must be GFC_MAX_SYMBOL_LEN+1 bytes long.
   Note that options.cc restricts max_identifier_length to not more
   than GFC_MAX_SYMBOL_LEN.
   When gobble_ws is false, do not skip over leading blanks.  */

match
gfc_match_name (char *buffer, bool gobble_ws)
{
  locus old_loc;
  int i;
  char c;

  old_loc = gfc_current_locus;
  if (gobble_ws)
    gfc_gobble_whitespace ();

  c = gfc_next_ascii_char ();
  if (!(ISALPHA (c) || (c == '_' && flag_allow_leading_underscore)))
    {
      /* Special cases for unary minus and plus, which allows for a sensible
	 error message for code of the form 'c = exp(-a*b) )' where an
	 extra ')' appears at the end of statement.  */
      if (!gfc_error_flag_test () && c != '(' && c != '-' && c != '+')
	gfc_error ("Invalid character in name at %C");
      gfc_current_locus = old_loc;
      return MATCH_NO;
    }

  i = 0;

  do
    {
      buffer[i++] = c;

      if (i > gfc_option.max_identifier_length)
	{
	  gfc_error ("Name at %C is too long");
	  return MATCH_ERROR;
	}

      old_loc = gfc_current_locus;
      c = gfc_next_ascii_char ();
    }
  while (ISALNUM (c) || c == '_' || (flag_dollar_ok && c == '$'));

  if (c == '$' && !flag_dollar_ok)
    {
      gfc_fatal_error ("Invalid character %<$%> at %L. Use %<-fdollar-ok%> to "
		       "allow it as an extension", &old_loc);
      return MATCH_ERROR;
    }

  buffer[i] = '\0';
  gfc_current_locus = old_loc;

  return MATCH_YES;
}


/* Match a symbol on the input.  Modifies the pointer to the symbol
   pointer if successful.  */

match
gfc_match_sym_tree (gfc_symtree **matched_symbol, int host_assoc)
{
  char buffer[GFC_MAX_SYMBOL_LEN + 1];
  match m;
  int ret;

  locus loc = gfc_current_locus;
  m = gfc_match_name (buffer);
  if (m != MATCH_YES)
    return m;
  loc = gfc_get_location_range (NULL, 0, &loc, 1, &gfc_current_locus);
  if (host_assoc)
    {
      ret = gfc_get_ha_sym_tree (buffer, matched_symbol, &loc);
      return ret ? MATCH_ERROR : MATCH_YES;
    }

  ret = gfc_get_sym_tree (buffer, NULL, matched_symbol, false, &loc);
  if (ret)
    return MATCH_ERROR;

  return MATCH_YES;
}


match
gfc_match_symbol (gfc_symbol **matched_symbol, int host_assoc)
{
  gfc_symtree *st;
  match m;

  m = gfc_match_sym_tree (&st, host_assoc);

  if (m == MATCH_YES)
    {
      if (st)
	*matched_symbol = st->n.sym;
      else
	*matched_symbol = NULL;
    }
  else
    *matched_symbol = NULL;
  return m;
}


/* Match an intrinsic operator.  Returns an INTRINSIC enum. While matching,
   we always find INTRINSIC_PLUS before INTRINSIC_UPLUS. We work around this
   in matchexp.cc.  */

match
gfc_match_intrinsic_op (gfc_intrinsic_op *result)
{
  locus orig_loc = gfc_current_locus;
  char ch;

  gfc_gobble_whitespace ();
  ch = gfc_next_ascii_char ();
  switch (ch)
    {
    case '+':
      /* Matched "+".  */
      *result = INTRINSIC_PLUS;
      return MATCH_YES;

    case '-':
      /* Matched "-".  */
      *result = INTRINSIC_MINUS;
      return MATCH_YES;

    case '=':
      if (gfc_next_ascii_char () == '=')
	{
	  /* Matched "==".  */
	  *result = INTRINSIC_EQ;
	  return MATCH_YES;
	}
      break;

    case '<':
      if (gfc_peek_ascii_char () == '=')
	{
	  /* Matched "<=".  */
	  gfc_next_ascii_char ();
	  *result = INTRINSIC_LE;
	  return MATCH_YES;
	}
      /* Matched "<".  */
      *result = INTRINSIC_LT;
      return MATCH_YES;

    case '>':
      if (gfc_peek_ascii_char () == '=')
	{
	  /* Matched ">=".  */
	  gfc_next_ascii_char ();
	  *result = INTRINSIC_GE;
	  return MATCH_YES;
	}
      /* Matched ">".  */
      *result = INTRINSIC_GT;
      return MATCH_YES;

    case '*':
      if (gfc_peek_ascii_char () == '*')
	{
	  /* Matched "**".  */
	  gfc_next_ascii_char ();
	  *result = INTRINSIC_POWER;
	  return MATCH_YES;
	}
      /* Matched "*".  */
      *result = INTRINSIC_TIMES;
      return MATCH_YES;

    case '/':
      ch = gfc_peek_ascii_char ();
      if (ch == '=')
	{
	  /* Matched "/=".  */
	  gfc_next_ascii_char ();
	  *result = INTRINSIC_NE;
	  return MATCH_YES;
	}
      else if (ch == '/')
	{
	  /* Matched "//".  */
	  gfc_next_ascii_char ();
	  *result = INTRINSIC_CONCAT;
	  return MATCH_YES;
	}
      /* Matched "/".  */
      *result = INTRINSIC_DIVIDE;
      return MATCH_YES;

    case '.':
      ch = gfc_next_ascii_char ();
      switch (ch)
	{
	case 'a':
	  if (gfc_next_ascii_char () == 'n'
	      && gfc_next_ascii_char () == 'd'
	      && gfc_next_ascii_char () == '.')
	    {
	      /* Matched ".and.".  */
	      *result = INTRINSIC_AND;
	      return MATCH_YES;
	    }
	  break;

	case 'e':
	  if (gfc_next_ascii_char () == 'q')
	    {
	      ch = gfc_next_ascii_char ();
	      if (ch == '.')
		{
		  /* Matched ".eq.".  */
		  *result = INTRINSIC_EQ_OS;
		  return MATCH_YES;
		}
	      else if (ch == 'v')
		{
		  if (gfc_next_ascii_char () == '.')
		    {
		      /* Matched ".eqv.".  */
		      *result = INTRINSIC_EQV;
		      return MATCH_YES;
		    }
		}
	    }
	  break;

	case 'g':
	  ch = gfc_next_ascii_char ();
	  if (ch == 'e')
	    {
	      if (gfc_next_ascii_char () == '.')
		{
		  /* Matched ".ge.".  */
		  *result = INTRINSIC_GE_OS;
		  return MATCH_YES;
		}
	    }
	  else if (ch == 't')
	    {
	      if (gfc_next_ascii_char () == '.')
		{
		  /* Matched ".gt.".  */
		  *result = INTRINSIC_GT_OS;
		  return MATCH_YES;
		}
	    }
	  break;

	case 'l':
	  ch = gfc_next_ascii_char ();
	  if (ch == 'e')
	    {
	      if (gfc_next_ascii_char () == '.')
		{
		  /* Matched ".le.".  */
		  *result = INTRINSIC_LE_OS;
		  return MATCH_YES;
		}
	    }
	  else if (ch == 't')
	    {
	      if (gfc_next_ascii_char () == '.')
		{
		  /* Matched ".lt.".  */
		  *result = INTRINSIC_LT_OS;
		  return MATCH_YES;
		}
	    }
	  break;

	case 'n':
	  ch = gfc_next_ascii_char ();
	  if (ch == 'e')
	    {
	      ch = gfc_next_ascii_char ();
	      if (ch == '.')
		{
		  /* Matched ".ne.".  */
		  *result = INTRINSIC_NE_OS;
		  return MATCH_YES;
		}
	      else if (ch == 'q')
		{
		  if (gfc_next_ascii_char () == 'v'
		      && gfc_next_ascii_char () == '.')
		    {
		      /* Matched ".neqv.".  */
		      *result = INTRINSIC_NEQV;
		      return MATCH_YES;
		    }
		}
	    }
	  else if (ch == 'o')
	    {
	      if (gfc_next_ascii_char () == 't'
		  && gfc_next_ascii_char () == '.')
		{
		  /* Matched ".not.".  */
		  *result = INTRINSIC_NOT;
		  return MATCH_YES;
		}
	    }
	  break;

	case 'o':
	  if (gfc_next_ascii_char () == 'r'
	      && gfc_next_ascii_char () == '.')
	    {
	      /* Matched ".or.".  */
	      *result = INTRINSIC_OR;
	      return MATCH_YES;
	    }
	  break;

	case 'x':
	  if (gfc_next_ascii_char () == 'o'
	      && gfc_next_ascii_char () == 'r'
	      && gfc_next_ascii_char () == '.')
	    {
              if (!gfc_notify_std (GFC_STD_LEGACY, ".XOR. operator at %C"))
                return MATCH_ERROR;
	      /* Matched ".xor." - equivalent to ".neqv.".  */
	      *result = INTRINSIC_NEQV;
	      return MATCH_YES;
	    }
	  break;

	default:
	  break;
	}
      break;

    default:
      break;
    }

  gfc_current_locus = orig_loc;
  return MATCH_NO;
}


/* Match a loop control phrase:

    <LVALUE> = <EXPR>, <EXPR> [, <EXPR> ]

   If the final integer expression is not present, a constant unity
   expression is returned.  We don't return MATCH_ERROR until after
   the equals sign is seen.  */

match
gfc_match_iterator (gfc_iterator *iter, int init_flag)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_expr *var, *e1, *e2, *e3;
  locus start;
  match m;

  e1 = e2 = e3 = NULL;

  /* Match the start of an iterator without affecting the symbol table.  */

  start = gfc_current_locus;
  m = gfc_match (" %n =", name);
  gfc_current_locus = start;

  if (m != MATCH_YES)
    return MATCH_NO;

  m = gfc_match_variable (&var, 0);
  if (m != MATCH_YES)
    return MATCH_NO;

  if (var->symtree->n.sym->attr.dimension)
    {
      gfc_error ("Loop variable at %C cannot be an array");
      goto cleanup;
    }

  /* F2008, C617 & C565.  */
  if (var->symtree->n.sym->attr.codimension)
    {
      gfc_error ("Loop variable at %C cannot be a coarray");
      goto cleanup;
    }

  if (var->ref != NULL)
    {
      gfc_error ("Loop variable at %C cannot be a sub-component");
      goto cleanup;
    }

  gfc_match_char ('=');

  var->symtree->n.sym->attr.implied_index = 1;

  m = init_flag ? gfc_match_init_expr (&e1) : gfc_match_expr (&e1);
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (gfc_match_char (',') != MATCH_YES)
    goto syntax;

  m = init_flag ? gfc_match_init_expr (&e2) : gfc_match_expr (&e2);
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (gfc_match_char (',') != MATCH_YES)
    {
      e3 = gfc_get_int_expr (gfc_default_integer_kind, NULL, 1);
      goto done;
    }

  m = init_flag ? gfc_match_init_expr (&e3) : gfc_match_expr (&e3);
  if (m == MATCH_ERROR)
    goto cleanup;
  if (m == MATCH_NO)
    {
      gfc_error ("Expected a step value in iterator at %C");
      goto cleanup;
    }

done:
  iter->var = var;
  iter->start = e1;
  iter->end = e2;
  iter->step = e3;
  return MATCH_YES;

syntax:
  gfc_error ("Syntax error in iterator at %C");

cleanup:
  gfc_free_expr (e1);
  gfc_free_expr (e2);
  gfc_free_expr (e3);

  return MATCH_ERROR;
}


/* Tries to match the next non-whitespace character on the input.
   This subroutine does not return MATCH_ERROR.
   When gobble_ws is false, do not skip over leading blanks.  */

match
gfc_match_char (char c, bool gobble_ws)
{
  locus where;

  where = gfc_current_locus;
  if (gobble_ws)
    gfc_gobble_whitespace ();

  if (gfc_next_ascii_char () == c)
    return MATCH_YES;

  gfc_current_locus = where;
  return MATCH_NO;
}


/* General purpose matching subroutine.  The target string is a
   scanf-like format string in which spaces correspond to arbitrary
   whitespace (including no whitespace), characters correspond to
   themselves.  The %-codes are:

   %%  Literal percent sign
   %e  Expression, pointer to a pointer is set
   %s  Symbol, pointer to the symbol is set (host_assoc = 0)
   %S  Symbol, pointer to the symbol is set (host_assoc = 1)
   %n  Name, character buffer is set to name
   %t  Matches end of statement.
   %o  Matches an intrinsic operator, returned as an INTRINSIC enum.
   %l  Matches a statement label
   %v  Matches a variable expression (an lvalue, except function references
   having a data pointer result)
   %   Matches a required space (in free form) and optional spaces.  */

match
gfc_match (const char *target, ...)
{
  gfc_st_label **label;
  int matches, *ip;
  locus old_loc;
  va_list argp;
  char c, *np;
  match m, n;
  void **vp;
  const char *p;

  old_loc = gfc_current_locus;
  va_start (argp, target);
  m = MATCH_NO;
  matches = 0;
  p = target;

loop:
  c = *p++;
  switch (c)
    {
    case ' ':
      gfc_gobble_whitespace ();
      goto loop;
    case '\0':
      m = MATCH_YES;
      break;

    case '%':
      c = *p++;
      switch (c)
	{
	case 'e':
	  vp = va_arg (argp, void **);
	  n = gfc_match_expr ((gfc_expr **) vp);
	  if (n != MATCH_YES)
	    {
	      m = n;
	      goto not_yes;
	    }

	  matches++;
	  goto loop;

	case 'v':
	  vp = va_arg (argp, void **);
	  n = gfc_match_variable ((gfc_expr **) vp, 0);
	  if (n != MATCH_YES)
	    {
	      m = n;
	      goto not_yes;
	    }

	  matches++;
	  goto loop;

	case 's':
	case 'S':
	  vp = va_arg (argp, void **);
	  n = gfc_match_symbol ((gfc_symbol **) vp, c == 'S');
	  if (n != MATCH_YES)
	    {
	      m = n;
	      goto not_yes;
	    }

	  matches++;
	  goto loop;

	case 'n':
	  np = va_arg (argp, char *);
	  n = gfc_match_name (np);
	  if (n != MATCH_YES)
	    {
	      m = n;
	      goto not_yes;
	    }

	  matches++;
	  goto loop;

	case 'l':
	  label = va_arg (argp, gfc_st_label **);
	  n = gfc_match_st_label (label);
	  if (n != MATCH_YES)
	    {
	      m = n;
	      goto not_yes;
	    }

	  matches++;
	  goto loop;

	case 'o':
	  ip = va_arg (argp, int *);
	  n = gfc_match_intrinsic_op ((gfc_intrinsic_op *) ip);
	  if (n != MATCH_YES)
	    {
	      m = n;
	      goto not_yes;
	    }

	  matches++;
	  goto loop;

	case 't':
	  if (gfc_match_eos () != MATCH_YES)
	    {
	      m = MATCH_NO;
	      goto not_yes;
	    }
	  goto loop;

	case ' ':
	  if (gfc_match_space () == MATCH_YES)
	    goto loop;
	  m = MATCH_NO;
	  goto not_yes;

	case '%':
	  break;	/* Fall through to character matcher.  */

	default:
	  gfc_internal_error ("gfc_match(): Bad match code %c", c);
	}
      /* FALLTHRU */

    default:

      /* gfc_next_ascii_char converts characters to lower-case, so we shouldn't
	 expect an upper case character here!  */
      gcc_assert (TOLOWER (c) == c);

      if (c == gfc_next_ascii_char ())
	goto loop;
      break;
    }

not_yes:
  va_end (argp);

  if (m != MATCH_YES)
    {
      /* Clean up after a failed match.  */
      gfc_current_locus = old_loc;
      va_start (argp, target);

      p = target;
      for (; matches > 0; matches--)
	{
	  while (*p++ != '%');

	  switch (*p++)
	    {
	    case '%':
	      matches++;
	      break;		/* Skip.  */

	    /* Matches that don't have to be undone */
	    case 'o':
	    case 'l':
	    case 'n':
	    case 's':
	      (void) va_arg (argp, void **);
	      break;

	    case 'e':
	    case 'v':
	      vp = va_arg (argp, void **);
	      gfc_free_expr ((struct gfc_expr *)*vp);
	      *vp = NULL;
	      break;
	    }
	}

      va_end (argp);
    }

  return m;
}


/*********************** Statement level matching **********************/

/* Matches the start of a program unit, which is the program keyword
   followed by an obligatory symbol.  */

match
gfc_match_program (void)
{
  gfc_symbol *sym;
  match m;

  m = gfc_match ("% %s%t", &sym);

  if (m == MATCH_NO)
    {
      gfc_error ("Invalid form of PROGRAM statement at %C");
      m = MATCH_ERROR;
    }

  if (m == MATCH_ERROR)
    return m;

  if (!gfc_add_flavor (&sym->attr, FL_PROGRAM, sym->name, NULL))
    return MATCH_ERROR;

  gfc_new_block = sym;

  return MATCH_YES;
}


/* Match a simple assignment statement.  */

match
gfc_match_assignment (void)
{
  gfc_expr *lvalue, *rvalue;
  locus old_loc;
  match m;

  old_loc = gfc_current_locus;

  lvalue = NULL;
  m = gfc_match (" %v =", &lvalue);
  if (m != MATCH_YES)
    {
      gfc_current_locus = old_loc;
      gfc_free_expr (lvalue);
      return MATCH_NO;
    }

  rvalue = NULL;
  m = gfc_match (" %e%t", &rvalue);

  if (m == MATCH_YES
      && rvalue->ts.type == BT_BOZ
      && lvalue->ts.type == BT_CLASS)
    {
      m = MATCH_ERROR;
      gfc_error ("BOZ literal constant at %L is neither a DATA statement "
		 "value nor an actual argument of INT/REAL/DBLE/CMPLX "
		 "intrinsic subprogram", &rvalue->where);
    }

  if (lvalue->expr_type == EXPR_CONSTANT)
    {
      /* This clobbers %len and %kind.  */
      m = MATCH_ERROR;
      gfc_error ("Assignment to a constant expression at %C");
    }

  if (m != MATCH_YES)
    {
      gfc_current_locus = old_loc;
      gfc_free_expr (lvalue);
      gfc_free_expr (rvalue);
      return m;
    }

  if (!lvalue->symtree)
    {
      gfc_free_expr (lvalue);
      gfc_free_expr (rvalue);
      return MATCH_ERROR;
    }


  gfc_set_sym_referenced (lvalue->symtree->n.sym);

  new_st.op = EXEC_ASSIGN;
  new_st.expr1 = lvalue;
  new_st.expr2 = rvalue;

  gfc_check_do_variable (lvalue->symtree);

  return MATCH_YES;
}


/* Match a pointer assignment statement.  */

match
gfc_match_pointer_assignment (void)
{
  gfc_expr *lvalue, *rvalue;
  locus old_loc;
  match m;

  old_loc = gfc_current_locus;

  lvalue = rvalue = NULL;
  gfc_matching_ptr_assignment = 0;
  gfc_matching_procptr_assignment = 0;

  m = gfc_match (" %v =>", &lvalue);
  if (m != MATCH_YES || !lvalue->symtree)
    {
      m = MATCH_NO;
      goto cleanup;
    }

  if (lvalue->symtree->n.sym->attr.proc_pointer
      || gfc_is_proc_ptr_comp (lvalue))
    gfc_matching_procptr_assignment = 1;
  else
    gfc_matching_ptr_assignment = 1;

  m = gfc_match (" %e%t", &rvalue);
  gfc_matching_ptr_assignment = 0;
  gfc_matching_procptr_assignment = 0;
  if (m != MATCH_YES)
    goto cleanup;

  new_st.op = EXEC_POINTER_ASSIGN;
  new_st.expr1 = lvalue;
  new_st.expr2 = rvalue;

  return MATCH_YES;

cleanup:
  gfc_current_locus = old_loc;
  gfc_free_expr (lvalue);
  gfc_free_expr (rvalue);
  return m;
}


/* We try to match an easy arithmetic IF statement. This only happens
   when just after having encountered a simple IF statement. This code
   is really duplicate with parts of the gfc_match_if code, but this is
   *much* easier.  */

static match
match_arithmetic_if (void)
{
  gfc_st_label *l1, *l2, *l3;
  gfc_expr *expr;
  match m;

  m = gfc_match (" ( %e ) %l , %l , %l%t", &expr, &l1, &l2, &l3);
  if (m != MATCH_YES)
    return m;

  if (!gfc_reference_st_label (l1, ST_LABEL_TARGET)
      || !gfc_reference_st_label (l2, ST_LABEL_TARGET)
      || !gfc_reference_st_label (l3, ST_LABEL_TARGET))
    {
      gfc_free_expr (expr);
      return MATCH_ERROR;
    }

  if (!gfc_notify_std (GFC_STD_F95_OBS | GFC_STD_F2018_DEL,
		       "Arithmetic IF statement at %C"))
    return MATCH_ERROR;

  new_st.op = EXEC_ARITHMETIC_IF;
  new_st.expr1 = expr;
  new_st.label1 = l1;
  new_st.label2 = l2;
  new_st.label3 = l3;

  return MATCH_YES;
}


/* The IF statement is a bit of a pain.  First of all, there are three
   forms of it, the simple IF, the IF that starts a block and the
   arithmetic IF.

   There is a problem with the simple IF and that is the fact that we
   only have a single level of undo information on symbols.  What this
   means is for a simple IF, we must re-match the whole IF statement
   multiple times in order to guarantee that the symbol table ends up
   in the proper state.  */

static match match_simple_forall (void);
static match match_simple_where (void);

match
gfc_match_if (gfc_statement *if_type)
{
  gfc_expr *expr;
  gfc_st_label *l1, *l2, *l3;
  locus old_loc, old_loc2;
  gfc_code *p;
  match m, n;

  n = gfc_match_label ();
  if (n == MATCH_ERROR)
    return n;

  old_loc = gfc_current_locus;

  m = gfc_match (" if ", &expr);
  if (m != MATCH_YES)
    return m;

  if (gfc_match_char ('(') != MATCH_YES)
    {
      gfc_error ("Missing %<(%> in IF-expression at %C");
      return MATCH_ERROR;
    }

  m = gfc_match ("%e", &expr);
  if (m != MATCH_YES)
    return m;

  old_loc2 = gfc_current_locus;
  gfc_current_locus = old_loc;

  if (gfc_match_parens () == MATCH_ERROR)
    return MATCH_ERROR;

  gfc_current_locus = old_loc2;

  if (gfc_match_char (')') != MATCH_YES)
    {
      gfc_error ("Syntax error in IF-expression at %C");
      gfc_free_expr (expr);
      return MATCH_ERROR;
    }

  m = gfc_match (" %l , %l , %l%t", &l1, &l2, &l3);

  if (m == MATCH_YES)
    {
      if (n == MATCH_YES)
	{
	  gfc_error ("Block label not appropriate for arithmetic IF "
		     "statement at %C");
	  gfc_free_expr (expr);
	  return MATCH_ERROR;
	}

      if (!gfc_reference_st_label (l1, ST_LABEL_TARGET)
	  || !gfc_reference_st_label (l2, ST_LABEL_TARGET)
	  || !gfc_reference_st_label (l3, ST_LABEL_TARGET))
	{
	  gfc_free_expr (expr);
	  return MATCH_ERROR;
	}

      if (!gfc_notify_std (GFC_STD_F95_OBS | GFC_STD_F2018_DEL,
			   "Arithmetic IF statement at %C"))
	return MATCH_ERROR;

      new_st.op = EXEC_ARITHMETIC_IF;
      new_st.expr1 = expr;
      new_st.label1 = l1;
      new_st.label2 = l2;
      new_st.label3 = l3;

      *if_type = ST_ARITHMETIC_IF;
      return MATCH_YES;
    }

  if (gfc_match (" then%t") == MATCH_YES)
    {
      new_st.op = EXEC_IF;
      new_st.expr1 = expr;
      *if_type = ST_IF_BLOCK;
      return MATCH_YES;
    }

  if (n == MATCH_YES)
    {
      gfc_error ("Block label is not appropriate for IF statement at %C");
      gfc_free_expr (expr);
      return MATCH_ERROR;
    }

  /* At this point the only thing left is a simple IF statement.  At
     this point, n has to be MATCH_NO, so we don't have to worry about
     re-matching a block label.  From what we've got so far, try
     matching an assignment.  */

  *if_type = ST_SIMPLE_IF;

  m = gfc_match_assignment ();
  if (m == MATCH_YES)
    goto got_match;

  gfc_free_expr (expr);
  gfc_undo_symbols ();
  gfc_current_locus = old_loc;

  /* m can be MATCH_NO or MATCH_ERROR, here.  For MATCH_ERROR, a mangled
     assignment was found.  For MATCH_NO, continue to call the various
     matchers.  */
  if (m == MATCH_ERROR)
    return MATCH_ERROR;

  gfc_match (" if ( %e ) ", &expr);	/* Guaranteed to match.  */

  m = gfc_match_pointer_assignment ();
  if (m == MATCH_YES)
    goto got_match;

  gfc_free_expr (expr);
  gfc_undo_symbols ();
  gfc_current_locus = old_loc;

  gfc_match (" if ( %e ) ", &expr);	/* Guaranteed to match.  */

  /* Look at the next keyword to see which matcher to call.  Matching
     the keyword doesn't affect the symbol table, so we don't have to
     restore between tries.  */

#define match(string, subr, statement) \
  if (gfc_match (string) == MATCH_YES) { m = subr(); goto got_match; }

  gfc_clear_error ();

  match ("allocate", gfc_match_allocate, ST_ALLOCATE)
  match ("assign", gfc_match_assign, ST_LABEL_ASSIGNMENT)
  match ("backspace", gfc_match_backspace, ST_BACKSPACE)
  match ("call", gfc_match_call, ST_CALL)
  match ("change% team", gfc_match_change_team, ST_CHANGE_TEAM)
  match ("close", gfc_match_close, ST_CLOSE)
  match ("continue", gfc_match_continue, ST_CONTINUE)
  match ("cycle", gfc_match_cycle, ST_CYCLE)
  match ("deallocate", gfc_match_deallocate, ST_DEALLOCATE)
  match ("end file", gfc_match_endfile, ST_END_FILE)
  match ("end team", gfc_match_end_team, ST_END_TEAM)
  match ("error% stop", gfc_match_error_stop, ST_ERROR_STOP)
  match ("event% post", gfc_match_event_post, ST_EVENT_POST)
  match ("event% wait", gfc_match_event_wait, ST_EVENT_WAIT)
  match ("exit", gfc_match_exit, ST_EXIT)
  match ("fail% image", gfc_match_fail_image, ST_FAIL_IMAGE)
  match ("flush", gfc_match_flush, ST_FLUSH)
  match ("forall", match_simple_forall, ST_FORALL)
  match ("form% team", gfc_match_form_team, ST_FORM_TEAM)
  match ("go to", gfc_match_goto, ST_GOTO)
  match ("if", match_arithmetic_if, ST_ARITHMETIC_IF)
  match ("inquire", gfc_match_inquire, ST_INQUIRE)
  match ("lock", gfc_match_lock, ST_LOCK)
  match ("nullify", gfc_match_nullify, ST_NULLIFY)
  match ("open", gfc_match_open, ST_OPEN)
  match ("pause", gfc_match_pause, ST_NONE)
  match ("print", gfc_match_print, ST_WRITE)
  match ("read", gfc_match_read, ST_READ)
  match ("return", gfc_match_return, ST_RETURN)
  match ("rewind", gfc_match_rewind, ST_REWIND)
  match ("stop", gfc_match_stop, ST_STOP)
  match ("wait", gfc_match_wait, ST_WAIT)
  match ("sync% all", gfc_match_sync_all, ST_SYNC_CALL);
  match ("sync% images", gfc_match_sync_images, ST_SYNC_IMAGES);
  match ("sync% memory", gfc_match_sync_memory, ST_SYNC_MEMORY);
  match ("sync% team", gfc_match_sync_team, ST_SYNC_TEAM)
  match ("unlock", gfc_match_unlock, ST_UNLOCK)
  match ("where", match_simple_where, ST_WHERE)
  match ("write", gfc_match_write, ST_WRITE)

  if (flag_dec)
    match ("type", gfc_match_print, ST_WRITE)

  /* All else has failed, so give up.  See if any of the matchers has
     stored an error message of some sort.  */
  if (!gfc_error_check ())
    gfc_error ("Syntax error in IF-clause after %C");

  gfc_free_expr (expr);
  return MATCH_ERROR;

got_match:
  if (m == MATCH_NO)
    gfc_error ("Syntax error in IF-clause after %C");
  if (m != MATCH_YES)
    {
      gfc_free_expr (expr);
      return MATCH_ERROR;
    }

  /* At this point, we've matched the single IF and the action clause
     is in new_st.  Rearrange things so that the IF statement appears
     in new_st.  */

  p = gfc_get_code (EXEC_IF);
  p->next = XCNEW (gfc_code);
  *p->next = new_st;
  p->next->loc = gfc_current_locus;

  p->expr1 = expr;

  gfc_clear_new_st ();

  new_st.op = EXEC_IF;
  new_st.block = p;

  return MATCH_YES;
}

#undef match


/* Match an ELSE statement.  */

match
gfc_match_else (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];

  if (gfc_match_eos () == MATCH_YES)
    return MATCH_YES;

  if (gfc_match_name (name) != MATCH_YES
      || gfc_current_block () == NULL
      || gfc_match_eos () != MATCH_YES)
    {
      gfc_error ("Invalid character(s) in ELSE statement after %C");
      return MATCH_ERROR;
    }

  if (strcmp (name, gfc_current_block ()->name) != 0)
    {
      gfc_error ("Label %qs at %C doesn't match IF label %qs",
		 name, gfc_current_block ()->name);
      return MATCH_ERROR;
    }

  return MATCH_YES;
}


/* Match an ELSE IF statement.  */

match
gfc_match_elseif (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_expr *expr, *then;
  locus where;
  match m;

  if (gfc_match_char ('(') != MATCH_YES)
    {
      gfc_error ("Missing %<(%> in ELSE IF expression at %C");
      return MATCH_ERROR;
    }

  m = gfc_match (" %e ", &expr);
  if (m != MATCH_YES)
    return m;

  if (gfc_match_char (')') != MATCH_YES)
    {
      gfc_error ("Missing %<)%> in ELSE IF expression at %C");
      goto cleanup;
    }

  m = gfc_match (" then ", &then);

  where = gfc_current_locus;

  if (m == MATCH_YES && (gfc_match_eos () == MATCH_YES
			 || (gfc_current_block ()
			     && gfc_match_name (name) == MATCH_YES)))
    goto done;

  if (gfc_match_eos () == MATCH_YES)
    {
      gfc_error ("Missing THEN in ELSE IF statement after %L", &where);
      goto cleanup;
    }

  if (gfc_match_name (name) != MATCH_YES
      || gfc_current_block () == NULL
      || gfc_match_eos () != MATCH_YES)
    {
      gfc_error ("Syntax error in ELSE IF statement after %L", &where);
      goto cleanup;
    }

  if (strcmp (name, gfc_current_block ()->name) != 0)
    {
      gfc_error ("Label %qs after %L doesn't match IF label %qs",
		 name, &where, gfc_current_block ()->name);
      goto cleanup;
    }

  if (m != MATCH_YES)
    return m;

done:
  new_st.op = EXEC_IF;
  new_st.expr1 = expr;
  return MATCH_YES;

cleanup:
  gfc_free_expr (expr);
  return MATCH_ERROR;
}


/* Free a gfc_iterator structure.  */

void
gfc_free_iterator (gfc_iterator *iter, int flag)
{

  if (iter == NULL)
    return;

  gfc_free_expr (iter->var);
  gfc_free_expr (iter->start);
  gfc_free_expr (iter->end);
  gfc_free_expr (iter->step);

  if (flag)
    free (iter);
}

static match
match_named_arg (const char *pat, const char *name, gfc_expr **e,
		 gfc_statement st_code)
{
  match m;
  gfc_expr *tmp;

  m = gfc_match (pat, &tmp);
  if (m == MATCH_ERROR)
    {
      gfc_syntax_error (st_code);
      return m;
    }
  if (m == MATCH_YES)
    {
      if (*e)
	{
	  gfc_error ("Duplicate %s attribute in %C", name);
	  gfc_free_expr (tmp);
	  return MATCH_ERROR;
	}
      *e = tmp;

      return MATCH_YES;
    }
  return MATCH_NO;
}

static match
match_stat_errmsg (struct sync_stat *sync_stat, gfc_statement st_code)
{
  match m;

  m = match_named_arg (" stat = %v", "STAT", &sync_stat->stat, st_code);
  if (m != MATCH_NO)
    return m;

  m = match_named_arg (" errmsg = %v", "ERRMSG", &sync_stat->errmsg, st_code);
  return m;
}

/* Match a CRITICAL statement.  */
match
gfc_match_critical (void)
{
  gfc_st_label *label = NULL;
  match m;

  if (gfc_match_label () == MATCH_ERROR)
    return MATCH_ERROR;

  if (gfc_match (" critical") != MATCH_YES)
    return MATCH_NO;

  if (gfc_match_st_label (&label) == MATCH_ERROR)
    return MATCH_ERROR;

  if (gfc_match_eos () == MATCH_YES)
    goto done;

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  for (;;)
    {
      m = match_stat_errmsg (&new_st.ext.sync_stat, ST_CRITICAL);
      if (m == MATCH_ERROR)
	goto cleanup;

      if (gfc_match_char (',') == MATCH_YES)
	continue;

      break;
    }

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:

  if (gfc_pure (NULL))
    {
      gfc_error ("Image control statement CRITICAL at %C in PURE procedure");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_DO_CONCURRENT))
    {
      gfc_error ("Image control statement CRITICAL at %C in DO CONCURRENT "
		 "block");
      return MATCH_ERROR;
    }

  gfc_unset_implicit_pure (NULL);

  if (!gfc_notify_std (GFC_STD_F2008, "CRITICAL statement at %C"))
    return MATCH_ERROR;

  if (flag_coarray == GFC_FCOARRAY_NONE)
    {
      gfc_fatal_error ("Coarrays disabled at %C, use %<-fcoarray=%> to "
		       "enable");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_CRITICAL))
    {
      gfc_error ("Nested CRITICAL block at %C");
      return MATCH_ERROR;
    }

  new_st.op = EXEC_CRITICAL;

  if (label != NULL && !gfc_reference_st_label (label, ST_LABEL_TARGET))
    goto cleanup;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_CRITICAL);

cleanup:
  gfc_free_expr (new_st.ext.sync_stat.stat);
  gfc_free_expr (new_st.ext.sync_stat.errmsg);
  new_st.ext.sync_stat = {NULL, NULL};

  return MATCH_ERROR;
}

/* Match a BLOCK statement.  */

match
gfc_match_block (void)
{
  match m;

  if (gfc_match_label () == MATCH_ERROR)
    return MATCH_ERROR;

  if (gfc_match (" block") != MATCH_YES)
    return MATCH_NO;

  /* For this to be a correct BLOCK statement, the line must end now.  */
  m = gfc_match_eos ();
  if (m == MATCH_ERROR)
    return MATCH_ERROR;
  if (m == MATCH_NO)
    return MATCH_NO;

  return MATCH_YES;
}

bool
check_coarray_assoc (const char *name, gfc_association_list *assoc)
{
  if (assoc->target->expr_type == EXPR_VARIABLE
      && !strcmp (assoc->target->symtree->name, name))
    {
      gfc_error ("Codimension decl name %qs in association at %L "
		 "must not be the same as a selector",
		 name, &assoc->where);
      return false;
    }
  return true;
}

match
match_association_list (bool for_change_team = false)
{
  new_st.ext.block.assoc = NULL;
  while (true)
    {
      gfc_association_list *newAssoc = gfc_get_association_list ();
      gfc_association_list *a;
      locus pre_name = gfc_current_locus;

      /* Match the next association.  */
      if (gfc_match (" %n ", newAssoc->name) != MATCH_YES)
	{
	  gfc_error ("Expected associate name at %C");
	  goto assocListError;
	}

      /* Required for an assumed rank target.  */
      if (!for_change_team && gfc_peek_char () == '(')
	{
	  newAssoc->ar = gfc_get_array_ref ();
	  if (gfc_match_array_ref (newAssoc->ar, NULL, 0, 0) != MATCH_YES)
	    {
	      gfc_error ("Bad bounds remapping list at %C");
	      goto assocListError;
	    }
	}

      if (newAssoc->ar && !(gfc_option.allow_std & GFC_STD_F202Y))
	gfc_error_now ("The bounds remapping list at %C is an experimental "
		       "F202y feature. Use std=f202y to enable");

      if (for_change_team && gfc_peek_char () == '[')
	{
	  if (!newAssoc->ar)
	    newAssoc->ar = gfc_get_array_ref ();
	  if (gfc_match_array_spec (&newAssoc->ar->as, false, true)
	      == MATCH_ERROR)
	    goto assocListError;
	}

      /* Match the next association.  */
      if (gfc_match (" =>", newAssoc->name) != MATCH_YES)
	{
	  if (for_change_team)
	    gfc_current_locus = pre_name;

	  free (newAssoc);
	  return MATCH_NO;
	}

      if (!for_change_team)
	{
	  if (gfc_match (" %e", &newAssoc->target) != MATCH_YES)
	    {
	      /* Have another go, allowing for procedure pointer selectors.  */
	      gfc_matching_procptr_assignment = 1;
	      if (gfc_match (" %e", &newAssoc->target) != MATCH_YES)
		{
		  gfc_matching_procptr_assignment = 0;
		  gfc_error ("Invalid association target at %C");
		  goto assocListError;
		}
	      gfc_matching_procptr_assignment = 0;
	    }
	  newAssoc->where = gfc_current_locus;
	}
      else
	{
	  newAssoc->where = gfc_current_locus;
	  /* F2018, C1116: A selector in a coarray-association shall be a named
	     coarray.  */
	  if (gfc_match (" %v", &newAssoc->target) != MATCH_YES)
	    {
	      gfc_error ("Selector in coarray association as %C shall be a "
			 "named coarray");
	      goto assocListError;
	    }
	}

      /* Check that the current name is not yet in the list.  */
      for (a = new_st.ext.block.assoc; a; a = a->next)
	if (!strcmp (a->name, newAssoc->name))
	  {
	    gfc_error ("Duplicate name %qs in association at %C",
		       newAssoc->name);
	    goto assocListError;
	  }

      if (for_change_team)
	{
	  /* F2018, C1113: In a change-team-stmt, a coarray-name in a
	     codimension-decl shall not be the same as a selector, or another
	     coarray-name, in that statement.
	     The latter is already checked for above.  So check only the
	     former.
	   */
	  if (!check_coarray_assoc (newAssoc->name, newAssoc))
	    goto assocListError;

	  for (a = new_st.ext.block.assoc; a; a = a->next)
	    {
	      if (!check_coarray_assoc (newAssoc->name, a)
		  || !check_coarray_assoc (a->name, newAssoc))
		goto assocListError;

	      /* F2018, C1115: No selector shall appear more than once in a
	       * given change-team-stmt.  */
	      if (!strcmp (newAssoc->target->symtree->name,
			   a->target->symtree->name))
		{
		  gfc_error ("Selector at %L duplicates selector at %L",
			     &newAssoc->target->where, &a->target->where);
		  goto assocListError;
		}
	    }
	}

      /* The target expression must not be coindexed.  */
      if (gfc_is_coindexed (newAssoc->target))
	{
	  gfc_error ("Association target at %C must not be coindexed");
	  goto assocListError;
	}

      /* The target expression cannot be a BOZ literal constant.  */
      if (newAssoc->target->ts.type == BT_BOZ)
	{
	  gfc_error ("Association target at %L cannot be a BOZ literal "
		     "constant", &newAssoc->target->where);
	  goto assocListError;
	}

      if (newAssoc->target->expr_type == EXPR_VARIABLE
	  && newAssoc->target->symtree->n.sym->as
	  && newAssoc->target->symtree->n.sym->as->type == AS_ASSUMED_RANK)
	{
	  bool bounds_remapping_list = true;
	  if (!newAssoc->ar)
	    bounds_remapping_list = false;
	  else
	    for (int dim = 0; dim < newAssoc->ar->dimen; dim++)
	      if (!newAssoc->ar->start[dim] || !newAssoc->ar->end[dim]
		  || newAssoc->ar->stride[dim] != NULL)
		bounds_remapping_list = false;

	  if (!bounds_remapping_list)
	    {
	      gfc_error ("The associate name %s with an assumed rank "
			 "target at %L must have a bounds remapping list "
			 "(list of lbound:ubound for each dimension)",
			 newAssoc->name, &newAssoc->target->where);
	      goto assocListError;
	    }

	  if (!newAssoc->target->symtree->n.sym->attr.contiguous)
	    {
	      gfc_error ("The assumed rank target at %C must be contiguous");
	      goto assocListError;
	    }
	}

      /* The `variable' field is left blank for now; because the target is not
	 yet resolved, we can't use gfc_has_vector_subscript to determine it
	 for now.  This is set during resolution.  */

      /* Put it into the list.  */
      newAssoc->next = new_st.ext.block.assoc;
      new_st.ext.block.assoc = newAssoc;

      /* Try next one or end if closing parenthesis is found.  */
      gfc_gobble_whitespace ();
      if (gfc_peek_char () == ')')
	break;
      if (gfc_match_char (',') != MATCH_YES)
	{
	  gfc_error ("Expected %<)%> or %<,%> at %C");
	  return MATCH_ERROR;
	}

      continue;

assocListError:
      free (newAssoc);
      return MATCH_ERROR;
    }

  return MATCH_YES;
}

/* Match an ASSOCIATE statement.  */

match
gfc_match_associate (void)
{
  match m;
  if (gfc_match_label () == MATCH_ERROR)
    return MATCH_ERROR;

  if (gfc_match (" associate") != MATCH_YES)
    return MATCH_NO;

  /* Match the association list.  */
  if (gfc_match_char ('(') != MATCH_YES)
    {
      gfc_error ("Expected association list at %C");
      return MATCH_ERROR;
    }

  m = match_association_list ();
  if (m == MATCH_ERROR)
    goto error;
  else if (m == MATCH_NO)
    {
      gfc_error ("Expected association at %C");
      goto error;
    }

  if (gfc_match_char (')') != MATCH_YES)
    {
      /* This should never happen as we peek above.  */
      gcc_unreachable ();
    }

  if (gfc_match_eos () != MATCH_YES)
    {
      gfc_error ("Junk after ASSOCIATE statement at %C");
      goto error;
    }

  return MATCH_YES;

error:
  gfc_free_association_list (new_st.ext.block.assoc);
  return MATCH_ERROR;
}


/* Match a Fortran 2003 derived-type-spec (F03:R455), which is just the name of
   an accessible derived type.  */

static match
match_derived_type_spec (gfc_typespec *ts)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  locus old_locus;
  gfc_symbol *derived, *der_type;
  match m = MATCH_YES;
  gfc_actual_arglist *decl_type_param_list = NULL;
  bool is_pdt_template = false;

  old_locus = gfc_current_locus;

  if (gfc_match ("%n", name) != MATCH_YES)
    {
       gfc_current_locus = old_locus;
       return MATCH_NO;
    }

  gfc_find_symbol (name, NULL, 1, &derived);

  /* Match the PDT spec list, if there.  */
  if (derived && derived->attr.flavor == FL_PROCEDURE)
    {
      gfc_find_symbol (gfc_dt_upper_string (name), NULL, 1, &der_type);
      is_pdt_template = der_type
			&& der_type->attr.flavor == FL_DERIVED
			&& der_type->attr.pdt_template;
    }

  if (is_pdt_template)
    m = gfc_match_actual_arglist (1, &decl_type_param_list, true);

  if (m == MATCH_ERROR)
    {
      gfc_free_actual_arglist (decl_type_param_list);
      return m;
    }

  if (derived && derived->attr.flavor == FL_PROCEDURE && derived->attr.generic)
    derived = gfc_find_dt_in_generic (derived);

  /* If this is a PDT, find the specific instance.  */
  if (m == MATCH_YES && is_pdt_template)
    {
      gfc_namespace *old_ns;

      old_ns = gfc_current_ns;
      while (gfc_current_ns && gfc_current_ns->parent)
	gfc_current_ns = gfc_current_ns->parent;

      if (type_param_spec_list)
	gfc_free_actual_arglist (type_param_spec_list);
      m = gfc_get_pdt_instance (decl_type_param_list, &der_type,
				&type_param_spec_list);
      gfc_free_actual_arglist (decl_type_param_list);

      if (m != MATCH_YES)
	return m;
      derived = der_type;
      gcc_assert (!derived->attr.pdt_template && derived->attr.pdt_type);
      gfc_set_sym_referenced (derived);

      gfc_current_ns = old_ns;
    }

  if (derived && derived->attr.flavor == FL_DERIVED)
    {
      ts->type = BT_DERIVED;
      ts->u.derived = derived;
      return MATCH_YES;
    }

  gfc_current_locus = old_locus;
  return MATCH_NO;
}


/* Match a Fortran 2003 type-spec (F03:R401).  This is similar to
   gfc_match_decl_type_spec() from decl.cc, with the following exceptions:
   It only includes the intrinsic types from the Fortran 2003 standard
   (thus, neither BYTE nor forms like REAL*4 are allowed). Additionally,
   the implicit_flag is not needed, so it was removed. Derived types are
   identified by their name alone.  */

match
gfc_match_type_spec (gfc_typespec *ts)
{
  match m;
  locus old_locus;
  char c, name[GFC_MAX_SYMBOL_LEN + 1];

  gfc_clear_ts (ts);
  gfc_gobble_whitespace ();
  old_locus = gfc_current_locus;

  /* If c isn't [a-z], then return immediately.  */
  c = gfc_peek_ascii_char ();
  if (!ISALPHA(c))
    return MATCH_NO;

  type_param_spec_list = NULL;

  if (match_derived_type_spec (ts) == MATCH_YES)
    {
      /* Enforce F03:C401.  */
      if (ts->u.derived->attr.abstract)
	{
	  gfc_error ("Derived type %qs at %L may not be ABSTRACT",
		     ts->u.derived->name, &old_locus);
	  return MATCH_ERROR;
	}
      return MATCH_YES;
    }

  if (gfc_match ("integer") == MATCH_YES)
    {
      ts->type = BT_INTEGER;
      ts->kind = gfc_default_integer_kind;
      goto kind_selector;
    }

  if (flag_unsigned && gfc_match ("unsigned") == MATCH_YES)
    {
      ts->type = BT_UNSIGNED;
      ts->kind = gfc_default_integer_kind;
      goto kind_selector;
    }

  if (gfc_match ("double precision") == MATCH_YES)
    {
      ts->type = BT_REAL;
      ts->kind = gfc_default_double_kind;
      return MATCH_YES;
    }

  if (gfc_match ("complex") == MATCH_YES)
    {
      ts->type = BT_COMPLEX;
      ts->kind = gfc_default_complex_kind;
      goto kind_selector;
    }

  if (gfc_match ("character") == MATCH_YES)
    {
      ts->type = BT_CHARACTER;

      m = gfc_match_char_spec (ts);

      if (m == MATCH_NO)
	m = MATCH_YES;

      return m;
    }

  /* REAL is a real pain because it can be a type, intrinsic subprogram,
     or list item in a type-list of an OpenMP reduction clause.  Need to
     differentiate REAL([KIND]=scalar-int-initialization-expr) from
     REAL(A,[KIND]) and REAL(KIND,A).  Logically, when this code was
     written the use of LOGICAL as a type-spec or intrinsic subprogram
     was overlooked.  */

  m = gfc_match (" %n", name);
  if (m == MATCH_YES
      && (strcmp (name, "real") == 0 || strcmp (name, "logical") == 0))
    {
      char c;
      gfc_expr *e;
      locus where;

      if (*name == 'r')
	{
	  ts->type = BT_REAL;
	  ts->kind = gfc_default_real_kind;
	}
      else
	{
	  ts->type = BT_LOGICAL;
	  ts->kind = gfc_default_logical_kind;
	}

      gfc_gobble_whitespace ();

      /* Prevent REAL*4, etc.  */
      c = gfc_peek_ascii_char ();
      if (c == '*')
	{
	  gfc_error ("Invalid type-spec at %C");
	  return MATCH_ERROR;
	}

      /* Found leading colon in REAL::, a trailing ')' in for example
	 TYPE IS (REAL), or REAL, for an OpenMP list-item.  */
      if (c == ':' || c == ')' || (flag_openmp && c == ','))
	return MATCH_YES;

      /* Found something other than the opening '(' in REAL(...  */
      if (c != '(')
	return MATCH_NO;
      else
	gfc_next_char (); /* Burn the '('. */

      /* Look for the optional KIND=. */
      where = gfc_current_locus;
      m = gfc_match ("%n", name);
      if (m == MATCH_YES)
	{
	  gfc_gobble_whitespace ();
	  c = gfc_next_char ();
	  if (c == '=')
	    {
	      if (strcmp(name, "a") == 0 || strcmp(name, "l") == 0)
		return MATCH_NO;
	      else if (strcmp(name, "kind") == 0)
		goto found;
	      else
		return MATCH_ERROR;
	    }
	  else
	    gfc_current_locus = where;
	}
      else
	gfc_current_locus = where;

found:

      m = gfc_match_expr (&e);
      if (m == MATCH_NO || m == MATCH_ERROR)
	return m;

      /* If a comma appears, it is an intrinsic subprogram. */
      gfc_gobble_whitespace ();
      c = gfc_peek_ascii_char ();
      if (c == ',')
	{
	  gfc_free_expr (e);
	  return MATCH_NO;
	}

      /* If ')' appears, we have REAL(initialization-expr), here check for
	 a scalar integer initialization-expr and valid kind parameter. */
      if (c == ')')
	{
	  bool ok = true;
	  if (e->expr_type != EXPR_CONSTANT && e->expr_type != EXPR_VARIABLE)
	    ok = gfc_reduce_init_expr (e);
	  if (!ok || e->ts.type != BT_INTEGER || e->rank > 0)
	    {
	      gfc_free_expr (e);
	      return MATCH_NO;
	    }

	  if (e->expr_type != EXPR_CONSTANT)
	    goto ohno;

	  gfc_next_char (); /* Burn the ')'. */
	  ts->kind = (int) mpz_get_si (e->value.integer);
	  if (gfc_validate_kind (ts->type, ts->kind , true) == -1)
	    {
	      gfc_error ("Invalid type-spec at %C");
	      return MATCH_ERROR;
	    }

	  gfc_free_expr (e);

	  return MATCH_YES;
	}
    }

ohno:

  /* If a type is not matched, simply return MATCH_NO.  */
  gfc_current_locus = old_locus;
  return MATCH_NO;

kind_selector:

  gfc_gobble_whitespace ();

  /* This prevents INTEGER*4, etc.  */
  if (gfc_peek_ascii_char () == '*')
    {
      gfc_error ("Invalid type-spec at %C");
      return MATCH_ERROR;
    }

  m = gfc_match_kind_spec (ts, false);

  /* No kind specifier found.  */
  if (m == MATCH_NO)
    m = MATCH_YES;

  return m;
}


/******************** FORALL subroutines ********************/

/* Free a list of FORALL iterators.  */

void
gfc_free_forall_iterator (gfc_forall_iterator *iter)
{
  gfc_forall_iterator *next;

  while (iter)
    {
      next = iter->next;
      gfc_free_expr (iter->var);
      gfc_free_expr (iter->start);
      gfc_free_expr (iter->end);
      gfc_free_expr (iter->stride);
      free (iter);
      iter = next;
    }
}


/* Match an iterator as part of a FORALL statement.  The format is:

     <var> = <start>:<end>[:<stride>]

   On MATCH_NO, the caller tests for the possibility that there is a
   scalar mask expression.  */

static match
match_forall_iterator (gfc_forall_iterator **result)
{
  gfc_forall_iterator *iter;
  locus where;
  match m;

  where = gfc_current_locus;
  iter = XCNEW (gfc_forall_iterator);

  m = gfc_match_expr (&iter->var);
  if (m != MATCH_YES)
    goto cleanup;

  if (gfc_match_char ('=') != MATCH_YES
      || iter->var->expr_type != EXPR_VARIABLE)
    {
      m = MATCH_NO;
      goto cleanup;
    }

  m = gfc_match_expr (&iter->start);
  if (m != MATCH_YES)
    goto cleanup;

  if (gfc_match_char (':') != MATCH_YES)
    goto syntax;

  m = gfc_match_expr (&iter->end);
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (gfc_match_char (':') == MATCH_NO)
    iter->stride = gfc_get_int_expr (gfc_default_integer_kind, NULL, 1);
  else
    {
      m = gfc_match_expr (&iter->stride);
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;
    }

  /* Mark the iteration variable's symbol as used as a FORALL index.  */
  iter->var->symtree->n.sym->forall_index = true;

  *result = iter;
  return MATCH_YES;

syntax:
  gfc_error ("Syntax error in FORALL iterator at %C");
  m = MATCH_ERROR;

cleanup:

  gfc_current_locus = where;
  gfc_free_forall_iterator (iter);
  return m;
}


/* Match the header of a FORALL statement.  */

static match
match_forall_header (gfc_forall_iterator **phead, gfc_expr **mask)
{
  gfc_forall_iterator *head, *tail, *new_iter;
  gfc_expr *msk;
  match m;

  gfc_gobble_whitespace ();

  head = tail = NULL;
  msk = NULL;

  if (gfc_match_char ('(') != MATCH_YES)
    return MATCH_NO;

  m = match_forall_iterator (&new_iter);
  if (m == MATCH_ERROR)
    goto cleanup;
  if (m == MATCH_NO)
    goto syntax;

  head = tail = new_iter;

  for (;;)
    {
      if (gfc_match_char (',') != MATCH_YES)
	break;

      m = match_forall_iterator (&new_iter);
      if (m == MATCH_ERROR)
	goto cleanup;

      if (m == MATCH_YES)
	{
	  tail->next = new_iter;
	  tail = new_iter;
	  continue;
	}

      /* Have to have a mask expression.  */

      m = gfc_match_expr (&msk);
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      break;
    }

  if (gfc_match_char (')') == MATCH_NO)
    goto syntax;

  *phead = head;
  *mask = msk;
  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_FORALL);

cleanup:
  gfc_free_expr (msk);
  gfc_free_forall_iterator (head);

  return MATCH_ERROR;
}

/* Match the rest of a simple FORALL statement that follows an
   IF statement.  */

static match
match_simple_forall (void)
{
  gfc_forall_iterator *head;
  gfc_expr *mask;
  gfc_code *c;
  match m;

  mask = NULL;
  head = NULL;
  c = NULL;

  m = match_forall_header (&head, &mask);

  if (m == MATCH_NO)
    goto syntax;
  if (m != MATCH_YES)
    goto cleanup;

  m = gfc_match_assignment ();

  if (m == MATCH_ERROR)
    goto cleanup;
  if (m == MATCH_NO)
    {
      m = gfc_match_pointer_assignment ();
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_NO)
	goto syntax;
    }

  c = XCNEW (gfc_code);
  *c = new_st;
  c->loc = gfc_current_locus;

  if (gfc_match_eos () != MATCH_YES)
    goto syntax;

  gfc_clear_new_st ();
  new_st.op = EXEC_FORALL;
  new_st.expr1 = mask;
  new_st.ext.concur.forall_iterator = head;
  new_st.block = gfc_get_code (EXEC_FORALL);
  new_st.block->next = c;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_FORALL);

cleanup:
  gfc_free_forall_iterator (head);
  gfc_free_expr (mask);

  return MATCH_ERROR;
}


/* Match a FORALL statement.  */

match
gfc_match_forall (gfc_statement *st)
{
  gfc_forall_iterator *head;
  gfc_expr *mask;
  gfc_code *c;
  match m0, m;

  head = NULL;
  mask = NULL;
  c = NULL;

  m0 = gfc_match_label ();
  if (m0 == MATCH_ERROR)
    return MATCH_ERROR;

  m = gfc_match (" forall");
  if (m != MATCH_YES)
    return m;

  m = match_forall_header (&head, &mask);
  if (m == MATCH_ERROR)
    goto cleanup;
  if (m == MATCH_NO)
    goto syntax;

  if (gfc_match_eos () == MATCH_YES)
    {
      *st = ST_FORALL_BLOCK;
      new_st.op = EXEC_FORALL;
      new_st.expr1 = mask;
      new_st.ext.concur.forall_iterator = head;
      return MATCH_YES;
    }

  m = gfc_match_assignment ();
  if (m == MATCH_ERROR)
    goto cleanup;
  if (m == MATCH_NO)
    {
      m = gfc_match_pointer_assignment ();
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_NO)
	goto syntax;
    }

  c = XCNEW (gfc_code);
  *c = new_st;
  c->loc = gfc_current_locus;

  gfc_clear_new_st ();
  new_st.op = EXEC_FORALL;
  new_st.expr1 = mask;
  new_st.ext.concur.forall_iterator = head;
  new_st.block = gfc_get_code (EXEC_FORALL);
  new_st.block->next = c;

  *st = ST_FORALL;
  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_FORALL);

cleanup:
  gfc_free_forall_iterator (head);
  gfc_free_expr (mask);
  gfc_free_statements (c);
  return MATCH_NO;
}


/* Match a DO statement.  */

match
gfc_match_do (void)
{
  gfc_iterator iter, *ip;
  locus old_loc;
  gfc_st_label *label;
  match m;

  old_loc = gfc_current_locus;

  memset (&iter, '\0', sizeof (gfc_iterator));
  label = NULL;

  m = gfc_match_label ();
  if (m == MATCH_ERROR)
    return m;

  if (gfc_match (" do") != MATCH_YES)
    return MATCH_NO;

  m = gfc_match_st_label (&label);
  if (m == MATCH_ERROR)
    goto cleanup;

  /* Match an infinite DO, make it like a DO WHILE(.TRUE.).  */

  if (gfc_match_eos () == MATCH_YES)
    {
      iter.end = gfc_get_logical_expr (gfc_default_logical_kind, NULL, true);
      new_st.op = EXEC_DO_WHILE;
      goto done;
    }

  /* Match an optional comma, if no comma is found, a space is obligatory.  */
  if (gfc_match_char (',') != MATCH_YES && gfc_match ("% ") != MATCH_YES)
    return MATCH_NO;

  /* Check for balanced parens.  */

  if (gfc_match_parens () == MATCH_ERROR)
    return MATCH_ERROR;

  /* Handle DO CONCURRENT construct.  */

  if (gfc_match (" concurrent") == MATCH_YES)
    {
      gfc_forall_iterator *head = NULL;
      gfc_expr_list *local = NULL;
      gfc_expr_list *local_tail = NULL;
      gfc_expr_list *local_init = NULL;
      gfc_expr_list *local_init_tail = NULL;
      gfc_expr_list *shared = NULL;
      gfc_expr_list *shared_tail = NULL;
      gfc_expr_list *reduce = NULL;
      gfc_expr_list *reduce_tail = NULL;
      bool default_none = false;
      gfc_expr *mask;

      if (!gfc_notify_std (GFC_STD_F2008, "DO CONCURRENT construct at %C"))
	return MATCH_ERROR;


      mask = NULL;
      head = NULL;
      m = match_forall_header (&head, &mask);

      if (m == MATCH_NO)
	goto match_do_loop;
      if (m == MATCH_ERROR)
	goto concurr_cleanup;

      while (true)
	{
	  gfc_gobble_whitespace ();
	  locus where = gfc_current_locus;

	  if (gfc_match_eos () == MATCH_YES)
	    goto concurr_ok;

	  else if (gfc_match ("local ( ") == MATCH_YES)
	    {
	      gfc_expr *e;
	      while (true)
		{
		  if (gfc_match_variable (&e, 0) != MATCH_YES)
		    goto concurr_cleanup;

		  if (local == NULL)
		    local = local_tail = gfc_get_expr_list ();

		  else
		    {
		      local_tail->next = gfc_get_expr_list ();
		      local_tail = local_tail->next;
		    }
		  local_tail->expr = e;

		  if (gfc_match_char (',') == MATCH_YES)
		    continue;
		  if (gfc_match_char (')') == MATCH_YES)
		    break;
		  goto concurr_cleanup;
		}
	    }

	    else if (gfc_match ("local_init ( ") == MATCH_YES)
	      {
		gfc_expr *e;

		while (true)
		  {
		    if (gfc_match_variable (&e, 0) != MATCH_YES)
		      goto concurr_cleanup;

		    if (local_init == NULL)
		      local_init = local_init_tail = gfc_get_expr_list ();

		    else
		      {
			local_init_tail->next = gfc_get_expr_list ();
			local_init_tail = local_init_tail->next;
		      }
		    local_init_tail->expr = e;

		    if (gfc_match_char (',') == MATCH_YES)
		      continue;
		    if (gfc_match_char (')') == MATCH_YES)
		      break;
		    goto concurr_cleanup;
		  }
	      }

	    else if (gfc_match ("shared ( ") == MATCH_YES)
	      {
		gfc_expr *e;
		while (true)
		  {
		    if (gfc_match_variable (&e, 0) != MATCH_YES)
		      goto concurr_cleanup;

		    if (shared == NULL)
		      shared = shared_tail = gfc_get_expr_list ();

		    else
		      {
			shared_tail->next = gfc_get_expr_list ();
			shared_tail = shared_tail->next;
		      }
		    shared_tail->expr = e;

		    if (gfc_match_char (',') == MATCH_YES)
		      continue;
		    if (gfc_match_char (')') == MATCH_YES)
		      break;
		    goto concurr_cleanup;
		  }
	      }

	    else if (gfc_match ("default (none)") == MATCH_YES)
	      {
		if (default_none)
		  {
		    gfc_error ("DEFAULT (NONE) specified more than once in DO "
			       "CONCURRENT at %C");
		    goto concurr_cleanup;
		  }
		default_none = true;
	      }

	    else if (gfc_match ("reduce ( ") == MATCH_YES)
	      {
		gfc_expr *reduction_expr;
		where = gfc_current_locus;

		if (gfc_match_char ('+') == MATCH_YES)
		  reduction_expr = gfc_get_operator_expr (&where,
							  INTRINSIC_PLUS,
							  NULL, NULL);

		else if (gfc_match_char ('*') == MATCH_YES)
		  reduction_expr = gfc_get_operator_expr (&where,
							  INTRINSIC_TIMES,
							  NULL, NULL);

		else if (gfc_match (".and.") == MATCH_YES)
		  reduction_expr = gfc_get_operator_expr (&where,
							  INTRINSIC_AND,
							  NULL, NULL);

		else if (gfc_match (".or.") == MATCH_YES)
		  reduction_expr = gfc_get_operator_expr (&where,
							  INTRINSIC_OR,
							  NULL, NULL);

		else if (gfc_match (".eqv.") == MATCH_YES)
		  reduction_expr = gfc_get_operator_expr (&where,
							  INTRINSIC_EQV,
							  NULL, NULL);

		else if (gfc_match (".neqv.") == MATCH_YES)
		  reduction_expr = gfc_get_operator_expr (&where,
							  INTRINSIC_NEQV,
							  NULL, NULL);

		else if (gfc_match ("min") == MATCH_YES)
		  {
		    reduction_expr = gfc_get_expr ();
		    reduction_expr->expr_type = EXPR_FUNCTION;
		    reduction_expr->value.function.isym
				= gfc_intrinsic_function_by_id (GFC_ISYM_MIN);
		    reduction_expr->where = where;
		  }

		else if (gfc_match ("max") == MATCH_YES)
		  {
		    reduction_expr = gfc_get_expr ();
		    reduction_expr->expr_type = EXPR_FUNCTION;
		    reduction_expr->value.function.isym
				= gfc_intrinsic_function_by_id (GFC_ISYM_MAX);
		    reduction_expr->where = where;
		  }

		else if (gfc_match ("iand") == MATCH_YES)
		  {
		    reduction_expr = gfc_get_expr ();
		    reduction_expr->expr_type = EXPR_FUNCTION;
		    reduction_expr->value.function.isym
				= gfc_intrinsic_function_by_id (GFC_ISYM_IAND);
		    reduction_expr->where = where;
		  }

		else if (gfc_match ("ior") == MATCH_YES)
		  {
		    reduction_expr = gfc_get_expr ();
		    reduction_expr->expr_type = EXPR_FUNCTION;
		    reduction_expr->value.function.isym
				= gfc_intrinsic_function_by_id (GFC_ISYM_IOR);
		    reduction_expr->where = where;
		  }

		else if (gfc_match ("ieor") == MATCH_YES)
		  {
		    reduction_expr = gfc_get_expr ();
		    reduction_expr->expr_type = EXPR_FUNCTION;
		    reduction_expr->value.function.isym
				= gfc_intrinsic_function_by_id (GFC_ISYM_IEOR);
		    reduction_expr->where = where;
		  }

		else
		  {
		    gfc_error ("Expected reduction operator or function name "
			       "at %C");
		    goto concurr_cleanup;
		  }

		if (!reduce)
		  {
		    reduce = reduce_tail = gfc_get_expr_list ();
		  }
		else
		  {
		    reduce_tail->next = gfc_get_expr_list ();
		    reduce_tail = reduce_tail->next;
		  }
		reduce_tail->expr = reduction_expr;

		gfc_gobble_whitespace ();

		if (gfc_match_char (':') != MATCH_YES)
		  {
		    gfc_error ("Expected %<:%> at %C");
		    goto concurr_cleanup;
		  }

		while (true)
		  {
		    gfc_expr *reduction_expr;

		    if (gfc_match_variable (&reduction_expr, 0) != MATCH_YES)
		      {
			gfc_error ("Expected variable name in reduction list "
				   "at %C");
			goto concurr_cleanup;
		      }

		    if (reduce == NULL)
		      reduce = reduce_tail = gfc_get_expr_list ();
		    else
		      {
			reduce_tail = reduce_tail->next = gfc_get_expr_list ();
			reduce_tail->expr = reduction_expr;
		      }

		    if (gfc_match_char (',') == MATCH_YES)
		      continue;
		    else if (gfc_match_char (')') == MATCH_YES)
		      break;
		    else
		      {
			gfc_error ("Expected ',' or ')' in reduction list "
				   "at %C");
			goto concurr_cleanup;
		      }
		  }

		if (!gfc_notify_std (GFC_STD_F2023, "REDUCE locality spec at "
				     "%L", &where))
		  goto concurr_cleanup;
	      }
	    else
	      goto concurr_cleanup;

	    if (!gfc_notify_std (GFC_STD_F2018, "Locality spec at %L",
				 &gfc_current_locus))
	      goto concurr_cleanup;
	}

      if (m == MATCH_NO)
	return m;
      if (m == MATCH_ERROR)
	goto concurr_cleanup;

      if (gfc_match_eos () != MATCH_YES)
	goto concurr_cleanup;

concurr_ok:
      if (label != NULL
	   && !gfc_reference_st_label (label, ST_LABEL_DO_TARGET))
	goto concurr_cleanup;

      new_st.label1 = label;
      new_st.op = EXEC_DO_CONCURRENT;
      new_st.expr1 = mask;
      new_st.ext.concur.forall_iterator = head;
      new_st.ext.concur.locality[LOCALITY_LOCAL] = local;
      new_st.ext.concur.locality[LOCALITY_LOCAL_INIT] = local_init;
      new_st.ext.concur.locality[LOCALITY_SHARED] = shared;
      new_st.ext.concur.locality[LOCALITY_REDUCE] = reduce;
      new_st.ext.concur.default_none = default_none;

      return MATCH_YES;

concurr_cleanup:
      gfc_free_expr (mask);
      gfc_free_forall_iterator (head);
      gfc_free_expr_list (local);
      gfc_free_expr_list (local_init);
      gfc_free_expr_list (shared);
      gfc_free_expr_list (reduce);

      if (!gfc_error_check ())
	gfc_syntax_error (ST_DO);

      return MATCH_ERROR;
    }

  /* See if we have a DO WHILE.  */
  if (gfc_match (" while ( %e )%t", &iter.end) == MATCH_YES)
    {
      new_st.op = EXEC_DO_WHILE;
      goto done;
    }

match_do_loop:
  /* The abortive DO WHILE may have done something to the symbol
     table, so we start over.  */
  gfc_undo_symbols ();
  gfc_current_locus = old_loc;

  gfc_match_label ();		/* This won't error.  */
  gfc_match (" do ");		/* This will work.  */

  gfc_match_st_label (&label);	/* Can't error out.  */
  gfc_match_char (',');		/* Optional comma.  */

  m = gfc_match_iterator (&iter, 0);
  if (m == MATCH_NO)
    return MATCH_NO;
  if (m == MATCH_ERROR)
    goto cleanup;

  iter.var->symtree->n.sym->attr.implied_index = 0;
  gfc_check_do_variable (iter.var->symtree);

  if (gfc_match_eos () != MATCH_YES)
    {
      gfc_syntax_error (ST_DO);
      goto cleanup;
    }

  new_st.op = EXEC_DO;

done:
  if (label != NULL
      && !gfc_reference_st_label (label, ST_LABEL_DO_TARGET))
    goto cleanup;

  new_st.label1 = label;

  if (new_st.op == EXEC_DO_WHILE)
    new_st.expr1 = iter.end;
  else
    {
      new_st.ext.iterator = ip = gfc_get_iterator ();
      *ip = iter;
    }

  return MATCH_YES;

cleanup:
  gfc_free_iterator (&iter, 0);

  return MATCH_ERROR;
}


/* Match an EXIT or CYCLE statement.  */

static match
match_exit_cycle (gfc_statement st, gfc_exec_op op)
{
  gfc_state_data *p, *o;
  gfc_symbol *sym;
  match m;
  int cnt;

  if (gfc_match_eos () == MATCH_YES)
    sym = NULL;
  else
    {
      char name[GFC_MAX_SYMBOL_LEN + 1];
      gfc_symtree* stree;

      m = gfc_match ("% %n%t", name);
      if (m == MATCH_ERROR)
	return MATCH_ERROR;
      if (m == MATCH_NO)
	{
	  gfc_syntax_error (st);
	  return MATCH_ERROR;
	}

      /* Find the corresponding symbol.  If there's a BLOCK statement
	 between here and the label, it is not in gfc_current_ns but a parent
	 namespace!  */
      stree = gfc_find_symtree_in_proc (name, gfc_current_ns);
      if (!stree)
	{
	  gfc_error ("Name %qs in %s statement at %C is unknown",
		     name, gfc_ascii_statement (st));
	  return MATCH_ERROR;
	}

      sym = stree->n.sym;
      if (sym->attr.flavor != FL_LABEL)
	{
	  gfc_error ("Name %qs in %s statement at %C is not a construct name",
		     name, gfc_ascii_statement (st));
	  return MATCH_ERROR;
	}
    }

  /* Find the loop specified by the label (or lack of a label).  */
  for (o = NULL, p = gfc_state_stack; p; p = p->previous)
    if (o == NULL && p->state == COMP_OMP_STRUCTURED_BLOCK)
      o = p;
    else if (p->state == COMP_CRITICAL)
      {
	gfc_error("%s statement at %C leaves CRITICAL construct",
		  gfc_ascii_statement (st));
	return MATCH_ERROR;
      }
    else if (p->state == COMP_DO_CONCURRENT
	     && (op == EXEC_EXIT || (sym && sym != p->sym)))
      {
	/* F2008, C821 & C845.  */
	gfc_error("%s statement at %C leaves DO CONCURRENT construct",
		  gfc_ascii_statement (st));
	return MATCH_ERROR;
      }
    else if ((sym && sym == p->sym)
	     || (!sym && (p->state == COMP_DO
			  || p->state == COMP_DO_CONCURRENT)))
      break;

  if (p == NULL)
    {
      if (sym == NULL)
	gfc_error ("%s statement at %C is not within a construct",
		   gfc_ascii_statement (st));
      else
	gfc_error ("%s statement at %C is not within construct %qs",
		   gfc_ascii_statement (st), sym->name);

      return MATCH_ERROR;
    }

  /* Special checks for EXIT from non-loop constructs.  */
  switch (p->state)
    {
    case COMP_DO:
    case COMP_DO_CONCURRENT:
      break;

    case COMP_CRITICAL:
      /* This is already handled above.  */
      gcc_unreachable ();

    case COMP_ASSOCIATE:
    case COMP_BLOCK:
    case COMP_CHANGE_TEAM:
    case COMP_IF:
    case COMP_SELECT:
    case COMP_SELECT_TYPE:
    case COMP_SELECT_RANK:
      gcc_assert (sym);
      if (op == EXEC_CYCLE)
	{
	  gfc_error ("CYCLE statement at %C is not applicable to non-loop"
		     " construct %qs", sym->name);
	  return MATCH_ERROR;
	}
      gcc_assert (op == EXEC_EXIT);
      if (!gfc_notify_std (GFC_STD_F2008, "EXIT statement with no"
			   " do-construct-name at %C"))
	return MATCH_ERROR;
      break;

    default:
      gfc_error ("%s statement at %C is not applicable to construct %qs",
		 gfc_ascii_statement (st), sym->name);
      return MATCH_ERROR;
    }

  if (o != NULL)
    {
      gfc_error (is_oacc (p)
		 ? G_("%s statement at %C leaving OpenACC structured block")
		 : G_("%s statement at %C leaving OpenMP structured block"),
		 gfc_ascii_statement (st));
      return MATCH_ERROR;
    }

  for (o = p, cnt = 0; o->state == COMP_DO && o->previous != NULL; cnt++)
    o = o->previous;

  int count = 1;
  if (cnt > 0
      && o != NULL
      && o->state == COMP_OMP_STRUCTURED_BLOCK)
    switch (o->head->op)
      {
      case EXEC_OACC_LOOP:
      case EXEC_OACC_KERNELS_LOOP:
      case EXEC_OACC_PARALLEL_LOOP:
      case EXEC_OACC_SERIAL_LOOP:
	gcc_assert (o->head->next != NULL
		    && (o->head->next->op == EXEC_DO
			|| o->head->next->op == EXEC_DO_WHILE)
		    && o->previous != NULL
		    && o->previous->tail->op == o->head->op);
	if (o->previous->tail->ext.omp_clauses != NULL)
	  {
	    /* Both collapsed and tiled loops are lowered the same way, but are
	       not compatible.  In gfc_trans_omp_do, the tile is prioritized. */
	    if (o->previous->tail->ext.omp_clauses->tile_list)
	      {
		count = 0;
		gfc_expr_list *el
		  = o->previous->tail->ext.omp_clauses->tile_list;
		for ( ; el; el = el->next)
		  ++count;
	      }
	    else if (o->previous->tail->ext.omp_clauses->collapse > 1)
	      count = o->previous->tail->ext.omp_clauses->collapse;
	  }
	if (st == ST_EXIT && cnt <= count)
	  {
	    gfc_error ("EXIT statement at %C terminating !$ACC LOOP loop");
	    return MATCH_ERROR;
	  }
	if (st == ST_CYCLE && cnt < count)
	  {
	    gfc_error (o->previous->tail->ext.omp_clauses->tile_list
		       ? G_("CYCLE statement at %C to non-innermost tiled "
			    "!$ACC LOOP loop")
		       : G_("CYCLE statement at %C to non-innermost collapsed "
			    "!$ACC LOOP loop"));
	    return MATCH_ERROR;
	  }
	break;
      case EXEC_OMP_TARGET_TEAMS_DISTRIBUTE_PARALLEL_DO_SIMD:
      case EXEC_OMP_TARGET_PARALLEL_DO_SIMD:
      case EXEC_OMP_TARGET_SIMD:
      case EXEC_OMP_TASKLOOP_SIMD:
      case EXEC_OMP_PARALLEL_MASTER_TASKLOOP_SIMD:
      case EXEC_OMP_MASTER_TASKLOOP_SIMD:
      case EXEC_OMP_PARALLEL_MASKED_TASKLOOP_SIMD:
      case EXEC_OMP_MASKED_TASKLOOP_SIMD:
      case EXEC_OMP_PARALLEL_DO_SIMD:
      case EXEC_OMP_DISTRIBUTE_SIMD:
      case EXEC_OMP_DISTRIBUTE_PARALLEL_DO_SIMD:
      case EXEC_OMP_TEAMS_DISTRIBUTE_SIMD:
      case EXEC_OMP_TARGET_TEAMS_DISTRIBUTE_SIMD:
      case EXEC_OMP_LOOP:
      case EXEC_OMP_PARALLEL_LOOP:
      case EXEC_OMP_TEAMS_LOOP:
      case EXEC_OMP_TARGET_PARALLEL_LOOP:
      case EXEC_OMP_TARGET_TEAMS_LOOP:
      case EXEC_OMP_DO:
      case EXEC_OMP_PARALLEL_DO:
      case EXEC_OMP_SIMD:
      case EXEC_OMP_DO_SIMD:
      case EXEC_OMP_DISTRIBUTE_PARALLEL_DO:
      case EXEC_OMP_TEAMS_DISTRIBUTE_PARALLEL_DO:
      case EXEC_OMP_TARGET_TEAMS_DISTRIBUTE_PARALLEL_DO:
      case EXEC_OMP_TARGET_PARALLEL_DO:
      case EXEC_OMP_TEAMS_DISTRIBUTE_PARALLEL_DO_SIMD:

	gcc_assert (o->head->next != NULL
		  && (o->head->next->op == EXEC_DO
		      || o->head->next->op == EXEC_DO_WHILE)
		  && o->previous != NULL
		  && o->previous->tail->op == o->head->op);
	if (o->previous->tail->ext.omp_clauses != NULL)
	  {
	    if (o->previous->tail->ext.omp_clauses->collapse > 1)
	      count = o->previous->tail->ext.omp_clauses->collapse;
	    if (o->previous->tail->ext.omp_clauses->orderedc)
	      count = o->previous->tail->ext.omp_clauses->orderedc;
	  }
	if (st == ST_EXIT && cnt <= count)
	  {
	    gfc_error ("EXIT statement at %C terminating !$OMP DO loop");
	    return MATCH_ERROR;
	  }
	if (st == ST_CYCLE && cnt < count)
	  {
	    gfc_error ("CYCLE statement at %C to non-innermost collapsed "
		       "!$OMP DO loop");
	    return MATCH_ERROR;
	  }
	break;
      default:
	break;
      }

  /* Save the first statement in the construct - needed by the backend.  */
  new_st.ext.which_construct = p->construct;

  new_st.op = op;

  return MATCH_YES;
}


/* Match the EXIT statement.  */

match
gfc_match_exit (void)
{
  return match_exit_cycle (ST_EXIT, EXEC_EXIT);
}


/* Match the CYCLE statement.  */

match
gfc_match_cycle (void)
{
  return match_exit_cycle (ST_CYCLE, EXEC_CYCLE);
}


/* Match a stop-code after an (ERROR) STOP or PAUSE statement.  The
   requirements for a stop-code differ in the standards.

Fortran 95 has

   R840 stop-stmt  is STOP [ stop-code ]
   R841 stop-code  is scalar-char-constant
                   or digit [ digit [ digit [ digit [ digit ] ] ] ]

Fortran 2003 matches Fortran 95 except R840 and R841 are now R849 and R850.
Fortran 2008 has

   R855 stop-stmt     is STOP [ stop-code ]
   R856 allstop-stmt  is ALL STOP [ stop-code ]
   R857 stop-code     is scalar-default-char-constant-expr
                      or scalar-int-constant-expr
Fortran 2018 has

   R1160 stop-stmt       is STOP [ stop-code ] [ , QUIET = scalar-logical-expr]
   R1161 error-stop-stmt is
                      ERROR STOP [ stop-code ] [ , QUIET = scalar-logical-expr]
   R1162 stop-code       is scalar-default-char-expr
                         or scalar-int-expr

For free-form source code, all standards contain a statement of the form:

   A blank shall be used to separate names, constants, or labels from
   adjacent keywords, names, constants, or labels.

A stop-code is not a name, constant, or label.  So, under Fortran 95 and 2003,

  STOP123

is valid, but it is invalid Fortran 2008.  */

static match
gfc_match_stopcode (gfc_statement st)
{
  gfc_expr *e = NULL;
  gfc_expr *quiet = NULL;
  match m;
  bool f95, f03, f08;
  char c;

  /* Set f95 for -std=f95.  */
  f95 = (gfc_option.allow_std == GFC_STD_OPT_F95);

  /* Set f03 for -std=f2003.  */
  f03 = (gfc_option.allow_std == GFC_STD_OPT_F03);

  /* Set f08 for -std=f2008.  */
  f08 = (gfc_option.allow_std == GFC_STD_OPT_F08);

  /* Plain STOP statement?  */
  if (gfc_match_eos () == MATCH_YES)
    goto checks;

  /* Look for a blank between STOP and the stop-code for F2008 or later.
     But allow for F2018's ,QUIET= specifier.  */
  c = gfc_peek_ascii_char ();

  if (gfc_current_form != FORM_FIXED && !(f95 || f03) && c != ',')
    {
      /* Look for end-of-statement.  There is no stop-code.  */
      if (c == '\n' || c == '!' || c == ';')
        goto done;

      if (c != ' ')
	{
	  gfc_error ("Blank required in %s statement near %C",
		     gfc_ascii_statement (st));
	  return MATCH_ERROR;
	}
    }

  if (c == ' ')
    {
      gfc_gobble_whitespace ();
      c = gfc_peek_ascii_char ();
    }
  if (c != ',')
    {
      int stopcode;
      locus old_locus;

      /* First look for the F95 or F2003 digit [...] construct.  */
      old_locus = gfc_current_locus;
      m = gfc_match_small_int (&stopcode);
      if (m == MATCH_YES && (f95 || f03))
	{
	  if (stopcode < 0)
	    {
	      gfc_error ("STOP code at %C cannot be negative");
	      return MATCH_ERROR;
	    }

	  if (stopcode > 99999)
	    {
	      gfc_error ("STOP code at %C contains too many digits");
	      return MATCH_ERROR;
	    }
	}

      /* Reset the locus and now load gfc_expr.  */
      gfc_current_locus = old_locus;
      m = gfc_match_expr (&e);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_NO)
	goto syntax;
    }

  if (gfc_match (" , quiet = %e", &quiet) == MATCH_YES)
    {
      if (!gfc_notify_std (GFC_STD_F2018, "QUIET= specifier for %s at %L",
			   gfc_ascii_statement (st), &quiet->where))
	goto cleanup;
    }

  if (gfc_match_eos () != MATCH_YES)
    goto syntax;

checks:

  if (gfc_pure (NULL))
    {
      if (st == ST_ERROR_STOP)
	{
	  if (!gfc_notify_std (GFC_STD_F2018, "%s statement at %C in PURE "
			       "procedure", gfc_ascii_statement (st)))
	    goto cleanup;
	}
      else
	{
	  gfc_error ("%s statement not allowed in PURE procedure at %C",
		     gfc_ascii_statement (st));
	  goto cleanup;
	}
    }

  gfc_unset_implicit_pure (NULL);

  if (st == ST_STOP && gfc_find_state (COMP_CRITICAL))
    {
      gfc_error ("Image control statement STOP at %C in CRITICAL block");
      goto cleanup;
    }
  if (st == ST_STOP && gfc_find_state (COMP_DO_CONCURRENT))
    {
      gfc_error ("Image control statement STOP at %C in DO CONCURRENT block");
      goto cleanup;
    }

  if (e != NULL)
    {
      if (!gfc_simplify_expr (e, 0))
	goto cleanup;

      /* Test for F95 and F2003 style STOP stop-code.  */
      if (e->expr_type != EXPR_CONSTANT && (f95 || f03))
	{
	  gfc_error ("STOP code at %L must be a scalar CHARACTER constant "
		     "or digit[digit[digit[digit[digit]]]]", &e->where);
	  goto cleanup;
	}

      /* Use the machinery for an initialization expression to reduce the
	 stop-code to a constant.  */
      gfc_reduce_init_expr (e);

      /* Test for F2008 style STOP stop-code.  */
      if (e->expr_type != EXPR_CONSTANT && f08)
	{
	  gfc_error ("STOP code at %L must be a scalar default CHARACTER or "
		     "INTEGER constant expression", &e->where);
	  goto cleanup;
	}

      if (!(e->ts.type == BT_CHARACTER || e->ts.type == BT_INTEGER))
	{
	  gfc_error ("STOP code at %L must be either INTEGER or CHARACTER type",
		     &e->where);
	  goto cleanup;
	}

      if (e->rank != 0)
	{
	  gfc_error ("STOP code at %L must be scalar", &e->where);
	  goto cleanup;
	}

      if (e->ts.type == BT_CHARACTER
	  && e->ts.kind != gfc_default_character_kind)
	{
	  gfc_error ("STOP code at %L must be default character KIND=%d",
		     &e->where, (int) gfc_default_character_kind);
	  goto cleanup;
	}

      if (e->ts.type == BT_INTEGER && e->ts.kind != gfc_default_integer_kind
	  && !gfc_notify_std (GFC_STD_F2018,
			      "STOP code at %L must be default integer KIND=%d",
			      &e->where, (int) gfc_default_integer_kind))
	goto cleanup;
    }

  if (quiet != NULL)
    {
      if (!gfc_simplify_expr (quiet, 0))
	goto cleanup;

      if (quiet->rank != 0)
	{
	  gfc_error ("QUIET specifier at %L must be a scalar LOGICAL",
		     &quiet->where);
	  goto cleanup;
	}
    }

done:

  switch (st)
    {
    case ST_STOP:
      new_st.op = EXEC_STOP;
      break;
    case ST_ERROR_STOP:
      new_st.op = EXEC_ERROR_STOP;
      break;
    case ST_PAUSE:
      new_st.op = EXEC_PAUSE;
      break;
    default:
      gcc_unreachable ();
    }

  new_st.expr1 = e;
  new_st.expr2 = quiet;
  new_st.ext.stop_code = -1;

  return MATCH_YES;

syntax:
  gfc_syntax_error (st);

cleanup:

  gfc_free_expr (e);
  gfc_free_expr (quiet);
  return MATCH_ERROR;
}


/* Match the (deprecated) PAUSE statement.  */

match
gfc_match_pause (void)
{
  match m;

  m = gfc_match_stopcode (ST_PAUSE);
  if (m == MATCH_YES)
    {
      if (!gfc_notify_std (GFC_STD_F95_DEL, "PAUSE statement at %C"))
	m = MATCH_ERROR;
    }
  return m;
}


/* Match the STOP statement.  */

match
gfc_match_stop (void)
{
  return gfc_match_stopcode (ST_STOP);
}


/* Match the ERROR STOP statement.  */

match
gfc_match_error_stop (void)
{
  if (!gfc_notify_std (GFC_STD_F2008, "ERROR STOP statement at %C"))
    return MATCH_ERROR;

  return gfc_match_stopcode (ST_ERROR_STOP);
}

/* Match EVENT POST/WAIT statement. Syntax:
     EVENT POST ( event-variable [, sync-stat-list] )
     EVENT WAIT ( event-variable [, wait-spec-list] )
   with
      wait-spec-list  is  sync-stat-list  or until-spec
      until-spec  is  UNTIL_COUNT = scalar-int-expr
      sync-stat  is  STAT= or ERRMSG=.  */

static match
event_statement (gfc_statement st)
{
  match m;
  gfc_expr *tmp, *eventvar, *until_count, *stat, *errmsg;
  bool saw_until_count, saw_stat, saw_errmsg;

  tmp = eventvar = until_count = stat = errmsg = NULL;
  saw_until_count = saw_stat = saw_errmsg = false;

  if (gfc_pure (NULL))
    {
      gfc_error ("Image control statement EVENT %s at %C in PURE procedure",
		 st == ST_EVENT_POST ? "POST" : "WAIT");
      return MATCH_ERROR;
    }

  gfc_unset_implicit_pure (NULL);

  if (flag_coarray == GFC_FCOARRAY_NONE)
    {
       gfc_fatal_error ("Coarrays disabled at %C, use %<-fcoarray=%> to enable");
       return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_CRITICAL))
    {
      gfc_error ("Image control statement EVENT %s at %C in CRITICAL block",
		 st == ST_EVENT_POST ? "POST" : "WAIT");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_DO_CONCURRENT))
    {
      gfc_error ("Image control statement EVENT %s at %C in DO CONCURRENT "
		 "block", st == ST_EVENT_POST ? "POST" : "WAIT");
      return MATCH_ERROR;
    }

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  if (gfc_match ("%e", &eventvar) != MATCH_YES)
    goto syntax;
  m = gfc_match_char (',');
  if (m == MATCH_ERROR)
    goto syntax;
  if (m == MATCH_NO)
    {
      m = gfc_match_char (')');
      if (m == MATCH_YES)
	goto done;
      goto syntax;
    }

  for (;;)
    {
      m = gfc_match (" stat = %v", &tmp);
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_stat)
	    {
	      gfc_error ("Redundant STAT tag found at %L", &tmp->where);
	      goto cleanup;
	    }
	  stat = tmp;
	  saw_stat = true;

	  m = gfc_match_char (',');
	  if (m == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      m = gfc_match (" errmsg = %v", &tmp);
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_errmsg)
	    {
	      gfc_error ("Redundant ERRMSG tag found at %L", &tmp->where);
	      goto cleanup;
	    }
	  errmsg = tmp;
	  saw_errmsg = true;

	  m = gfc_match_char (',');
	  if (m == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      m = gfc_match (" until_count = %e", &tmp);
      if (m == MATCH_ERROR || st == ST_EVENT_POST)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_until_count)
	    {
	      gfc_error ("Redundant UNTIL_COUNT tag found at %L",
			 &tmp->where);
	      goto cleanup;
	    }
	  until_count = tmp;
	  saw_until_count = true;

	  m = gfc_match_char (',');
	  if (m == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      break;
    }

  if (m == MATCH_ERROR)
    goto syntax;

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:
  switch (st)
    {
    case ST_EVENT_POST:
      new_st.op = EXEC_EVENT_POST;
      break;
    case ST_EVENT_WAIT:
      new_st.op = EXEC_EVENT_WAIT;
      break;
    default:
      gcc_unreachable ();
    }

  new_st.expr1 = eventvar;
  new_st.expr2 = stat;
  new_st.expr3 = errmsg;
  new_st.expr4 = until_count;

  return MATCH_YES;

syntax:
  gfc_syntax_error (st);

cleanup:
  if (until_count != tmp)
    gfc_free_expr (until_count);
  if (errmsg != tmp)
    gfc_free_expr (errmsg);
  if (stat != tmp)
    gfc_free_expr (stat);

  gfc_free_expr (tmp);
  gfc_free_expr (eventvar);

  return MATCH_ERROR;

}


match
gfc_match_event_post (void)
{
  if (!gfc_notify_std (GFC_STD_F2018, "EVENT POST statement at %C"))
    return MATCH_ERROR;

  return event_statement (ST_EVENT_POST);
}


match
gfc_match_event_wait (void)
{
  if (!gfc_notify_std (GFC_STD_F2018, "EVENT WAIT statement at %C"))
    return MATCH_ERROR;

  return event_statement (ST_EVENT_WAIT);
}


/* Match a FAIL IMAGE statement.  */

match
gfc_match_fail_image (void)
{
  if (!gfc_notify_std (GFC_STD_F2018, "FAIL IMAGE statement at %C"))
    return MATCH_ERROR;

  if (gfc_match_char ('(') == MATCH_YES)
    goto syntax;

  new_st.op = EXEC_FAIL_IMAGE;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_FAIL_IMAGE);

  return MATCH_ERROR;
}

/* Match a FORM TEAM statement.  */

match
gfc_match_form_team (void)
{
  match m;
  gfc_expr *teamid, *team, *new_index;

  teamid = team = new_index = NULL;

  if (!gfc_notify_std (GFC_STD_F2018, "FORM TEAM statement at %C"))
    return MATCH_ERROR;

  if (gfc_match_char ('(') == MATCH_NO)
    goto syntax;

  new_st.op = EXEC_FORM_TEAM;

  if (gfc_match ("%e", &teamid) != MATCH_YES)
    goto syntax;
  m = gfc_match_char (',');
  if (m == MATCH_ERROR)
    goto syntax;
  if (gfc_match ("%e", &team) != MATCH_YES)
    goto syntax;

  m = gfc_match_char (',');
  if (m == MATCH_ERROR)
    goto syntax;
  if (m == MATCH_NO)
    {
      m = gfc_match_char (')');
      if (m == MATCH_YES)
	goto done;
      goto syntax;
    }

  for (;;)
    {
      m = match_stat_errmsg (&new_st.ext.sync_stat, ST_FORM_TEAM);
      if (m == MATCH_ERROR)
	goto cleanup;

      m = match_named_arg (" new_index = %e", "NEW_INDEX", &new_index,
			   ST_FORM_TEAM);
      if (m == MATCH_ERROR)
	goto cleanup;

      m = gfc_match_char (',');
      if (m == MATCH_YES)
	continue;

      break;
    }

  if (m == MATCH_ERROR)
    goto syntax;

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:

  new_st.expr1 = teamid;
  new_st.expr2 = team;
  new_st.expr3 = new_index;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_FORM_TEAM);

cleanup:
  gfc_free_expr (new_index);
  gfc_free_expr (new_st.ext.sync_stat.stat);
  gfc_free_expr (new_st.ext.sync_stat.errmsg);
  new_st.ext.sync_stat = {NULL, NULL};

  gfc_free_expr (team);
  gfc_free_expr (teamid);

  return MATCH_ERROR;
}

/* Match a CHANGE TEAM statement.  */

match
gfc_match_change_team (void)
{
  match m;
  gfc_expr *team = NULL;

  if (gfc_match_label () == MATCH_ERROR)
    return MATCH_ERROR;

  if (gfc_match (" change% team") != MATCH_YES)
    return MATCH_NO;

  if (!gfc_notify_std (GFC_STD_F2018, "CHANGE TEAM statement at %C"))
    return MATCH_ERROR;

  if (gfc_match_char ('(') == MATCH_NO)
    goto syntax;

  if (gfc_match ("%e", &team) != MATCH_YES)
    goto syntax;

  m = gfc_match_char (',');
  if (m == MATCH_ERROR)
    goto syntax;
  if (m == MATCH_NO)
    {
      m = gfc_match_char (')');
      if (m == MATCH_YES)
	goto done;
      goto syntax;
    }

  m = match_association_list (true);
  if (m == MATCH_ERROR)
    goto cleanup;
  else if (m == MATCH_NO)
    for (;;)
      {
	m = match_stat_errmsg (&new_st.ext.block.sync_stat, ST_CHANGE_TEAM);
	if (m == MATCH_ERROR)
	  goto cleanup;

	if (gfc_match_char (',') == MATCH_YES)
	  continue;

	break;
      }

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:

  new_st.expr1 = team;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_CHANGE_TEAM);

cleanup:
  gfc_free_expr (new_st.ext.block.sync_stat.stat);
  gfc_free_expr (new_st.ext.block.sync_stat.errmsg);
  new_st.ext.block.sync_stat = {NULL, NULL};
  gfc_free_association_list (new_st.ext.block.assoc);
  new_st.ext.block.assoc = NULL;
  gfc_free_expr (team);

  return MATCH_ERROR;
}

/* Match an END TEAM statement.  */

match
gfc_match_end_team (void)
{
  if (gfc_match_eos () == MATCH_YES)
    goto done;

  if (gfc_match_char ('(') != MATCH_YES)
    {
      /* There could be a team-construct-name following.  Let caller decide
	 about error.  */
      new_st.op = EXEC_END_TEAM;
      return MATCH_NO;
    }

  for (;;)
    {
      if (match_stat_errmsg (&new_st.ext.sync_stat, ST_END_TEAM) == MATCH_ERROR)
	goto cleanup;

      if (gfc_match_char (',') == MATCH_YES)
	continue;

      break;
    }

  if (gfc_match_char (')') != MATCH_YES)
    goto syntax;

done:

  new_st.op = EXEC_END_TEAM;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_END_TEAM);

cleanup:
  gfc_free_expr (new_st.ext.sync_stat.stat);
  gfc_free_expr (new_st.ext.sync_stat.errmsg);
  new_st.ext.sync_stat = {NULL, NULL};

  /* Try to match the closing bracket to allow error recovery.  */
  gfc_match_char (')');

  return MATCH_ERROR;
}

/* Match a SYNC TEAM statement.  */

match
gfc_match_sync_team (void)
{
  match m;
  gfc_expr *team = NULL;

  if (!gfc_notify_std (GFC_STD_F2018, "SYNC TEAM statement at %C"))
    return MATCH_ERROR;

  if (gfc_match_char ('(') == MATCH_NO)
    goto syntax;

  new_st.op = EXEC_SYNC_TEAM;

  if (gfc_match ("%e", &team) != MATCH_YES)
    goto syntax;

  m = gfc_match_char (',');
  if (m == MATCH_ERROR)
    goto syntax;
  if (m == MATCH_NO)
    {
      m = gfc_match_char (')');
      if (m == MATCH_YES)
	goto done;
      goto syntax;
    }

  for (;;)
    {
      m = match_stat_errmsg (&new_st.ext.sync_stat, ST_SYNC_TEAM);
      if (m == MATCH_ERROR)
	goto cleanup;

      if (gfc_match_char (',') == MATCH_YES)
	continue;

      break;
    }

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:

  new_st.expr1 = team;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_SYNC_TEAM);

cleanup:
  gfc_free_expr (new_st.ext.sync_stat.stat);
  gfc_free_expr (new_st.ext.sync_stat.errmsg);
  new_st.ext.sync_stat = {NULL, NULL};

  gfc_free_expr (team);

  return MATCH_ERROR;
}

/* Match LOCK/UNLOCK statement. Syntax:
     LOCK ( lock-variable [ , lock-stat-list ] )
     UNLOCK ( lock-variable [ , sync-stat-list ] )
   where lock-stat is ACQUIRED_LOCK or sync-stat
   and sync-stat is STAT= or ERRMSG=.  */

static match
lock_unlock_statement (gfc_statement st)
{
  match m;
  gfc_expr *tmp, *lockvar, *acq_lock, *stat, *errmsg;
  bool saw_acq_lock, saw_stat, saw_errmsg;

  tmp = lockvar = acq_lock = stat = errmsg = NULL;
  saw_acq_lock = saw_stat = saw_errmsg = false;

  if (gfc_pure (NULL))
    {
      gfc_error ("Image control statement %s at %C in PURE procedure",
		 st == ST_LOCK ? "LOCK" : "UNLOCK");
      return MATCH_ERROR;
    }

  gfc_unset_implicit_pure (NULL);

  if (flag_coarray == GFC_FCOARRAY_NONE)
    {
       gfc_fatal_error ("Coarrays disabled at %C, use %<-fcoarray=%> to enable");
       return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_CRITICAL))
    {
      gfc_error ("Image control statement %s at %C in CRITICAL block",
		 st == ST_LOCK ? "LOCK" : "UNLOCK");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_DO_CONCURRENT))
    {
      gfc_error ("Image control statement %s at %C in DO CONCURRENT block",
		 st == ST_LOCK ? "LOCK" : "UNLOCK");
      return MATCH_ERROR;
    }

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  if (gfc_match ("%e", &lockvar) != MATCH_YES)
    goto syntax;
  m = gfc_match_char (',');
  if (m == MATCH_ERROR)
    goto syntax;
  if (m == MATCH_NO)
    {
      m = gfc_match_char (')');
      if (m == MATCH_YES)
	goto done;
      goto syntax;
    }

  for (;;)
    {
      m = gfc_match (" stat = %v", &tmp);
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_stat)
	    {
	      gfc_error ("Redundant STAT tag found at %L", &tmp->where);
	      goto cleanup;
	    }
	  stat = tmp;
	  saw_stat = true;

	  m = gfc_match_char (',');
	  if (m == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      m = gfc_match (" errmsg = %v", &tmp);
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_errmsg)
	    {
	      gfc_error ("Redundant ERRMSG tag found at %L", &tmp->where);
	      goto cleanup;
	    }
	  errmsg = tmp;
	  saw_errmsg = true;

	  m = gfc_match_char (',');
	  if (m == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      m = gfc_match (" acquired_lock = %v", &tmp);
      if (m == MATCH_ERROR || st == ST_UNLOCK)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_acq_lock)
	    {
	      gfc_error ("Redundant ACQUIRED_LOCK tag found at %L",
			 &tmp->where);
	      goto cleanup;
	    }
	  acq_lock = tmp;
	  saw_acq_lock = true;

	  m = gfc_match_char (',');
	  if (m == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      break;
    }

  if (m == MATCH_ERROR)
    goto syntax;

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:
  switch (st)
    {
    case ST_LOCK:
      new_st.op = EXEC_LOCK;
      break;
    case ST_UNLOCK:
      new_st.op = EXEC_UNLOCK;
      break;
    default:
      gcc_unreachable ();
    }

  new_st.expr1 = lockvar;
  new_st.expr2 = stat;
  new_st.expr3 = errmsg;
  new_st.expr4 = acq_lock;

  return MATCH_YES;

syntax:
  gfc_syntax_error (st);

cleanup:
  if (acq_lock != tmp)
    gfc_free_expr (acq_lock);
  if (errmsg != tmp)
    gfc_free_expr (errmsg);
  if (stat != tmp)
    gfc_free_expr (stat);

  gfc_free_expr (tmp);
  gfc_free_expr (lockvar);

  return MATCH_ERROR;
}


match
gfc_match_lock (void)
{
  if (!gfc_notify_std (GFC_STD_F2008, "LOCK statement at %C"))
    return MATCH_ERROR;

  return lock_unlock_statement (ST_LOCK);
}


match
gfc_match_unlock (void)
{
  if (!gfc_notify_std (GFC_STD_F2008, "UNLOCK statement at %C"))
    return MATCH_ERROR;

  return lock_unlock_statement (ST_UNLOCK);
}


/* Match SYNC ALL/IMAGES/MEMORY statement. Syntax:
     SYNC ALL [(sync-stat-list)]
     SYNC MEMORY [(sync-stat-list)]
     SYNC IMAGES (image-set [, sync-stat-list] )
   with sync-stat is int-expr or *.  */

static match
sync_statement (gfc_statement st)
{
  match m;
  gfc_expr *tmp, *imageset, *stat, *errmsg;
  bool saw_stat, saw_errmsg;

  tmp = imageset = stat = errmsg = NULL;
  saw_stat = saw_errmsg = false;

  if (gfc_pure (NULL))
    {
      gfc_error ("Image control statement SYNC at %C in PURE procedure");
      return MATCH_ERROR;
    }

  gfc_unset_implicit_pure (NULL);

  if (!gfc_notify_std (GFC_STD_F2008, "SYNC statement at %C"))
    return MATCH_ERROR;

  if (flag_coarray == GFC_FCOARRAY_NONE)
    {
       gfc_fatal_error ("Coarrays disabled at %C, use %<-fcoarray=%> to "
			"enable");
       return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_CRITICAL))
    {
      gfc_error ("Image control statement SYNC at %C in CRITICAL block");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_DO_CONCURRENT))
    {
      gfc_error ("Image control statement SYNC at %C in DO CONCURRENT block");
      return MATCH_ERROR;
    }

  if (gfc_match_eos () == MATCH_YES)
    {
      if (st == ST_SYNC_IMAGES)
	goto syntax;
      goto done;
    }

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  if (st == ST_SYNC_IMAGES)
    {
      /* Denote '*' as imageset == NULL.  */
      m = gfc_match_char ('*');
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_NO)
	{
	  if (gfc_match ("%e", &imageset) != MATCH_YES)
	    goto syntax;
	}
      m = gfc_match_char (',');
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_NO)
	{
	  m = gfc_match_char (')');
	  if (m == MATCH_YES)
	    goto done;
	  goto syntax;
	}
    }

  for (;;)
    {
      m = gfc_match (" stat = %e", &tmp);
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_stat)
	    {
	      gfc_error ("Redundant STAT tag found at %L", &tmp->where);
	      goto cleanup;
	    }
	  stat = tmp;
	  saw_stat = true;

	  if (gfc_match_char (',') == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

      m = gfc_match (" errmsg = %e", &tmp);
      if (m == MATCH_ERROR)
	goto syntax;
      if (m == MATCH_YES)
	{
	  if (saw_errmsg)
	    {
	      gfc_error ("Redundant ERRMSG tag found at %L", &tmp->where);
	      goto cleanup;
	    }
	  errmsg = tmp;
	  saw_errmsg = true;

	  if (gfc_match_char (',') == MATCH_YES)
	    continue;

	  tmp = NULL;
	  break;
	}

	break;
    }

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

done:
  switch (st)
    {
    case ST_SYNC_ALL:
      new_st.op = EXEC_SYNC_ALL;
      break;
    case ST_SYNC_IMAGES:
      new_st.op = EXEC_SYNC_IMAGES;
      break;
    case ST_SYNC_MEMORY:
      new_st.op = EXEC_SYNC_MEMORY;
      break;
    default:
      gcc_unreachable ();
    }

  new_st.expr1 = imageset;
  new_st.expr2 = stat;
  new_st.expr3 = errmsg;

  return MATCH_YES;

syntax:
  gfc_syntax_error (st);

cleanup:
  if (stat != tmp)
    gfc_free_expr (stat);
  if (errmsg != tmp)
    gfc_free_expr (errmsg);

  gfc_free_expr (tmp);
  gfc_free_expr (imageset);

  return MATCH_ERROR;
}


/* Match SYNC ALL statement.  */

match
gfc_match_sync_all (void)
{
  return sync_statement (ST_SYNC_ALL);
}


/* Match SYNC IMAGES statement.  */

match
gfc_match_sync_images (void)
{
  return sync_statement (ST_SYNC_IMAGES);
}


/* Match SYNC MEMORY statement.  */

match
gfc_match_sync_memory (void)
{
  return sync_statement (ST_SYNC_MEMORY);
}


/* Match a CONTINUE statement.  */

match
gfc_match_continue (void)
{
  if (gfc_match_eos () != MATCH_YES)
    {
      gfc_syntax_error (ST_CONTINUE);
      return MATCH_ERROR;
    }

  new_st.op = EXEC_CONTINUE;
  return MATCH_YES;
}


/* Match the (deprecated) ASSIGN statement.  */

match
gfc_match_assign (void)
{
  gfc_expr *expr;
  gfc_st_label *label;

  if (gfc_match (" %l", &label) == MATCH_YES)
    {
      if (!gfc_reference_st_label (label, ST_LABEL_UNKNOWN))
	return MATCH_ERROR;
      if (gfc_match (" to %v%t", &expr) == MATCH_YES)
	{
	  if (!gfc_notify_std (GFC_STD_F95_DEL, "ASSIGN statement at %C"))
	    return MATCH_ERROR;

	  expr->symtree->n.sym->attr.assign = 1;

	  new_st.op = EXEC_LABEL_ASSIGN;
	  new_st.label1 = label;
	  new_st.expr1 = expr;
	  return MATCH_YES;
	}
    }
  return MATCH_NO;
}


/* Match the GO TO statement.  As a computed GOTO statement is
   matched, it is transformed into an equivalent SELECT block.  No
   tree is necessary, and the resulting jumps-to-jumps are
   specifically optimized away by the back end.  */

match
gfc_match_goto (void)
{
  gfc_code *head, *tail;
  gfc_expr *expr;
  gfc_case *cp;
  gfc_st_label *label;
  int i;
  match m;

  if (gfc_match (" %l%t", &label) == MATCH_YES)
    {
      if (!gfc_reference_st_label (label, ST_LABEL_TARGET))
	return MATCH_ERROR;

      new_st.op = EXEC_GOTO;
      new_st.label1 = label;
      return MATCH_YES;
    }

  /* The assigned GO TO statement.  */

  if (gfc_match_variable (&expr, 0) == MATCH_YES)
    {
      if (!gfc_notify_std (GFC_STD_F95_DEL, "Assigned GOTO statement at %C"))
	return MATCH_ERROR;

      new_st.op = EXEC_GOTO;
      new_st.expr1 = expr;

      if (gfc_match_eos () == MATCH_YES)
	return MATCH_YES;

      /* Match label list.  */
      gfc_match_char (',');
      if (gfc_match_char ('(') != MATCH_YES)
	{
	  gfc_syntax_error (ST_GOTO);
	  return MATCH_ERROR;
	}
      head = tail = NULL;

      do
	{
	  m = gfc_match_st_label (&label);
	  if (m != MATCH_YES)
	    goto syntax;

	  if (!gfc_reference_st_label (label, ST_LABEL_TARGET))
	    goto cleanup;

	  if (head == NULL)
	    head = tail = gfc_get_code (EXEC_GOTO);
	  else
	    {
	      tail->block = gfc_get_code (EXEC_GOTO);
	      tail = tail->block;
	    }

	  tail->label1 = label;
	}
      while (gfc_match_char (',') == MATCH_YES);

      if (gfc_match (" )%t") != MATCH_YES)
	goto syntax;

      if (head == NULL)
	{
	   gfc_error ("Statement label list in GOTO at %C cannot be empty");
	   goto syntax;
	}
      new_st.block = head;

      return MATCH_YES;
    }

  /* Last chance is a computed GO TO statement.  */
  if (gfc_match_char ('(') != MATCH_YES)
    {
      gfc_syntax_error (ST_GOTO);
      return MATCH_ERROR;
    }

  head = tail = NULL;
  i = 1;

  do
    {
      m = gfc_match_st_label (&label);
      if (m != MATCH_YES)
	goto syntax;

      if (!gfc_reference_st_label (label, ST_LABEL_TARGET))
	goto cleanup;

      if (head == NULL)
	head = tail = gfc_get_code (EXEC_SELECT);
      else
	{
	  tail->block = gfc_get_code (EXEC_SELECT);
	  tail = tail->block;
	}

      cp = gfc_get_case ();
      cp->low = cp->high = gfc_get_int_expr (gfc_default_integer_kind,
					     NULL, i++);

      tail->ext.block.case_list = cp;

      tail->next = gfc_get_code (EXEC_GOTO);
      tail->next->label1 = label;
    }
  while (gfc_match_char (',') == MATCH_YES);

  if (gfc_match_char (')') != MATCH_YES)
    goto syntax;

  if (head == NULL)
    {
      gfc_error ("Statement label list in GOTO at %C cannot be empty");
      goto syntax;
    }

  /* Get the rest of the statement.  */
  gfc_match_char (',');

  if (gfc_match (" %e%t", &expr) != MATCH_YES)
    goto syntax;

  if (!gfc_notify_std (GFC_STD_F95_OBS, "Computed GOTO at %C"))
    return MATCH_ERROR;

  /* At this point, a computed GOTO has been fully matched and an
     equivalent SELECT statement constructed.  */

  new_st.op = EXEC_SELECT;
  new_st.expr1 = NULL;

  /* Hack: For a "real" SELECT, the expression is in expr. We put
     it in expr2 so we can distinguish then and produce the correct
     diagnostics.  */
  new_st.expr2 = expr;
  new_st.block = head;
  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_GOTO);
cleanup:
  gfc_free_statements (head);
  return MATCH_ERROR;
}


/* Frees a list of gfc_alloc structures.  */

void
gfc_free_alloc_list (gfc_alloc *p)
{
  gfc_alloc *q;

  for (; p; p = q)
    {
      q = p->next;
      gfc_free_expr (p->expr);
      free (p);
    }
}


/* Match an ALLOCATE statement.  */

match
gfc_match_allocate (void)
{
  gfc_alloc *head, *tail;
  gfc_expr *stat, *errmsg, *tmp, *source, *mold;
  gfc_typespec ts;
  gfc_symbol *sym;
  match m;
  locus old_locus, deferred_locus, assumed_locus;
  bool saw_stat, saw_errmsg, saw_source, saw_mold, saw_deferred, b1, b2, b3;
  bool saw_unlimited = false, saw_assumed = false;

  head = tail = NULL;
  stat = errmsg = source = mold = tmp = NULL;
  saw_stat = saw_errmsg = saw_source = saw_mold = saw_deferred = false;

  if (gfc_match_char ('(') != MATCH_YES)
    {
      gfc_syntax_error (ST_ALLOCATE);
      return MATCH_ERROR;
    }

  /* Match an optional type-spec.  */
  old_locus = gfc_current_locus;
  m = gfc_match_type_spec (&ts);
  if (m == MATCH_ERROR)
    goto cleanup;
  else if (m == MATCH_NO)
    {
      char name[GFC_MAX_SYMBOL_LEN + 3];

      if (gfc_match ("%n :: ", name) == MATCH_YES)
	{
	  gfc_error ("Error in type-spec at %L", &old_locus);
	  goto cleanup;
	}

      ts.type = BT_UNKNOWN;
    }
  else
    {
      /* Needed for the F2008:C631 check below. */
      assumed_locus = gfc_current_locus;

      if (gfc_match (" :: ") == MATCH_YES)
	{
	  if (!gfc_notify_std (GFC_STD_F2003, "typespec in ALLOCATE at %L",
			       &old_locus))
	    goto cleanup;

	  if (ts.deferred)
	    {
	      gfc_error ("Type-spec at %L cannot contain a deferred "
			 "type parameter", &old_locus);
	      goto cleanup;
	    }

	  if (ts.type == BT_CHARACTER)
	    {
	      if (!ts.u.cl->length)
		saw_assumed = true;
	      else
		ts.u.cl->length_from_typespec = true;
	    }

	  if (type_param_spec_list
	      && gfc_spec_list_type (type_param_spec_list, NULL)
		 == SPEC_DEFERRED)
	    {
	      gfc_error ("The type parameter spec list in the type-spec at "
			 "%L cannot contain DEFERRED parameters", &old_locus);
	      goto cleanup;
	    }
	}
      else
	{
	  ts.type = BT_UNKNOWN;
	  gfc_current_locus = old_locus;
	}
    }

  for (;;)
    {
      if (head == NULL)
	head = tail = gfc_get_alloc ();
      else
	{
	  tail->next = gfc_get_alloc ();
	  tail = tail->next;
	}

      m = gfc_match_variable (&tail->expr, 0);
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      if (tail->expr->expr_type == EXPR_CONSTANT)
	{
	  gfc_error ("Unexpected constant at %C");
	  goto cleanup;
	}

      if (gfc_check_do_variable (tail->expr->symtree))
	goto cleanup;

      bool impure = gfc_impure_variable (tail->expr->symtree->n.sym);
      if (impure && gfc_pure (NULL))
	{
	  gfc_error ("Bad allocate-object at %C for a PURE procedure");
	  goto cleanup;
	}

      if (impure)
	gfc_unset_implicit_pure (NULL);

      /* F2008:C631 (R626) A type-param-value in a type-spec shall be an
	 asterisk if and only if each allocate-object is a dummy argument
	 for which the corresponding type parameter is assumed.  */
      if (saw_assumed
	  && (tail->expr->ts.deferred
	      || (tail->expr->ts.u.cl && tail->expr->ts.u.cl->length)
	      || tail->expr->symtree->n.sym->attr.dummy == 0))
	{
	  gfc_error ("Incompatible allocate-object at %C for CHARACTER "
		     "type-spec at %L", &assumed_locus);
	  goto cleanup;
	}

      if (tail->expr->ts.deferred)
	{
	  saw_deferred = true;
	  deferred_locus = tail->expr->where;
	}

      if (gfc_find_state (COMP_DO_CONCURRENT)
	  || gfc_find_state (COMP_CRITICAL))
	{
	  gfc_ref *ref;
	  bool coarray = tail->expr->symtree->n.sym->attr.codimension;
	  for (ref = tail->expr->ref; ref; ref = ref->next)
	    if (ref->type == REF_COMPONENT)
	      coarray = ref->u.c.component->attr.codimension;

	  if (coarray && gfc_find_state (COMP_DO_CONCURRENT))
	    {
	      gfc_error ("ALLOCATE of coarray at %C in DO CONCURRENT block");
	      goto cleanup;
	    }
	  if (coarray && gfc_find_state (COMP_CRITICAL))
	    {
	      gfc_error ("ALLOCATE of coarray at %C in CRITICAL block");
	      goto cleanup;
	    }
	}

      /* Check for F08:C628.  */
      sym = tail->expr->symtree->n.sym;
      b1 = !(tail->expr->ref
	     && (tail->expr->ref->type == REF_COMPONENT
		 || tail->expr->ref->type == REF_ARRAY));
      if (sym && sym->ts.type == BT_CLASS && sym->attr.class_ok)
	b2 = !(CLASS_DATA (sym)->attr.allocatable
	       || CLASS_DATA (sym)->attr.class_pointer);
      else
	b2 = sym && !(sym->attr.allocatable || sym->attr.pointer
		      || sym->attr.proc_pointer);
      b3 = sym && sym->ns && sym->ns->proc_name
	   && (sym->ns->proc_name->attr.allocatable
	       || sym->ns->proc_name->attr.pointer
	       || sym->ns->proc_name->attr.proc_pointer);
      if (b1 && b2 && !b3)
	{
	  gfc_error ("Allocate-object at %L is neither a data pointer "
		     "nor an allocatable variable", &tail->expr->where);
	  goto cleanup;
	}

      /* The ALLOCATE statement had an optional typespec.  Check the
	 constraints.  */
      if (ts.type != BT_UNKNOWN)
	{
	  /* Enforce F03:C624.  */
	  if (!gfc_type_compatible (&tail->expr->ts, &ts))
	    {
	      gfc_error ("Type of entity at %L is type incompatible with "
			 "typespec", &tail->expr->where);
	      goto cleanup;
	    }

	  /* Enforce F03:C627.  */
	  if (ts.kind != tail->expr->ts.kind && !UNLIMITED_POLY (tail->expr))
	    {
	      gfc_error ("Kind type parameter for entity at %L differs from "
			 "the kind type parameter of the typespec",
			 &tail->expr->where);
	      goto cleanup;
	    }
	}

      if (tail->expr->ts.type == BT_DERIVED)
	tail->expr->ts.u.derived = gfc_use_derived (tail->expr->ts.u.derived);

      if (type_param_spec_list)
	tail->expr->param_list = gfc_copy_actual_arglist (type_param_spec_list);

      saw_unlimited = saw_unlimited | UNLIMITED_POLY (tail->expr);

      if (gfc_peek_ascii_char () == '(' && !sym->attr.dimension)
	{
	  gfc_error ("Shape specification for allocatable scalar at %C");
	  goto cleanup;
	}

      if (gfc_match_char (',') != MATCH_YES)
	break;

alloc_opt_list:

      m = gfc_match (" stat = %e", &tmp);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_YES)
	{
	  /* Enforce C630.  */
	  if (saw_stat)
	    {
	      gfc_error ("Redundant STAT tag found at %L", &tmp->where);
	      goto cleanup;
	    }

	  stat = tmp;
	  tmp = NULL;
	  saw_stat = true;

	  if (stat->expr_type == EXPR_CONSTANT)
	    {
	      gfc_error ("STAT tag at %L cannot be a constant", &stat->where);
	      goto cleanup;
	    }

	  if (gfc_check_do_variable (stat->symtree))
	    goto cleanup;

	  if (gfc_match_char (',') == MATCH_YES)
	    goto alloc_opt_list;
	}

      m = gfc_match (" errmsg = %e", &tmp);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_YES)
	{
	  if (!gfc_notify_std (GFC_STD_F2003, "ERRMSG tag at %L", &tmp->where))
	    goto cleanup;

	  /* Enforce C630.  */
	  if (saw_errmsg)
	    {
	      gfc_error ("Redundant ERRMSG tag found at %L", &tmp->where);
	      goto cleanup;
	    }

	  errmsg = tmp;
	  tmp = NULL;
	  saw_errmsg = true;

	  if (gfc_match_char (',') == MATCH_YES)
	    goto alloc_opt_list;
	}

      m = gfc_match (" source = %e", &tmp);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_YES)
	{
	  if (!gfc_notify_std (GFC_STD_F2003, "SOURCE tag at %L", &tmp->where))
	    goto cleanup;

	  /* Enforce C630.  */
	  if (saw_source)
	    {
	      gfc_error ("Redundant SOURCE tag found at %L", &tmp->where);
	      goto cleanup;
	    }

	  /* The next 2 conditionals check C631.  */
	  if (ts.type != BT_UNKNOWN)
	    {
	      gfc_error ("SOURCE tag at %L conflicts with the typespec at %L",
			 &tmp->where, &old_locus);
	      goto cleanup;
	    }

	  if (head->next
	      && !gfc_notify_std (GFC_STD_F2008, "SOURCE tag at %L"
				  " with more than a single allocate object",
				  &tmp->where))
	    goto cleanup;

	  source = tmp;
	  tmp = NULL;
	  saw_source = true;

	  if (gfc_match_char (',') == MATCH_YES)
	    goto alloc_opt_list;
	}

      m = gfc_match (" mold = %e", &tmp);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_YES)
	{
	  if (!gfc_notify_std (GFC_STD_F2008, "MOLD tag at %L", &tmp->where))
	    goto cleanup;

	  /* Check F08:C636.  */
	  if (saw_mold)
	    {
	      gfc_error ("Redundant MOLD tag found at %L", &tmp->where);
	      goto cleanup;
	    }

	  /* Check F08:C637.  */
	  if (ts.type != BT_UNKNOWN)
	    {
	      gfc_error ("MOLD tag at %L conflicts with the typespec at %L",
			 &tmp->where, &old_locus);
	      goto cleanup;
	    }

	  mold = tmp;
	  tmp = NULL;
	  saw_mold = true;
	  mold->mold = 1;

	  if (gfc_match_char (',') == MATCH_YES)
	    goto alloc_opt_list;
	}

	gfc_gobble_whitespace ();

	if (gfc_peek_char () == ')')
	  break;
    }

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

  /* Check F08:C637.  */
  if (source && mold)
    {
      gfc_error ("MOLD tag at %L conflicts with SOURCE tag at %L",
		 &mold->where, &source->where);
      goto cleanup;
    }

  /* Check F03:C623,  */
  if (saw_deferred && ts.type == BT_UNKNOWN && !source && !mold)
    {
      gfc_error ("Allocate-object at %L with a deferred type parameter "
		 "requires either a type-spec or SOURCE tag or a MOLD tag",
		 &deferred_locus);
      goto cleanup;
    }

  /* Check F03:C625,  */
  if (saw_unlimited && ts.type == BT_UNKNOWN && !source && !mold)
    {
      for (tail = head; tail; tail = tail->next)
	{
	  if (UNLIMITED_POLY (tail->expr))
	    gfc_error ("Unlimited polymorphic allocate-object at %L "
		       "requires either a type-spec or SOURCE tag "
		       "or a MOLD tag", &tail->expr->where);
	}
      goto cleanup;
    }

  new_st.op = EXEC_ALLOCATE;
  new_st.expr1 = stat;
  new_st.expr2 = errmsg;
  if (source)
    new_st.expr3 = source;
  else
    new_st.expr3 = mold;
  new_st.ext.alloc.list = head;
  new_st.ext.alloc.ts = ts;

  if (type_param_spec_list)
    gfc_free_actual_arglist (type_param_spec_list);

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_ALLOCATE);

cleanup:
  gfc_free_expr (errmsg);
  gfc_free_expr (source);
  gfc_free_expr (stat);
  gfc_free_expr (mold);
  if (tmp && tmp->expr_type) gfc_free_expr (tmp);
  gfc_free_alloc_list (head);
  if (type_param_spec_list)
    gfc_free_actual_arglist (type_param_spec_list);
  return MATCH_ERROR;
}


/* Match a NULLIFY statement. A NULLIFY statement is transformed into
   a set of pointer assignments to intrinsic NULL().  */

match
gfc_match_nullify (void)
{
  gfc_code *tail;
  gfc_expr *e, *p = NULL;
  match m;

  tail = NULL;

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  for (;;)
    {
      m = gfc_match_variable (&p, 0);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_NO)
	goto syntax;

      if (gfc_check_do_variable (p->symtree))
	goto cleanup;

      /* F2008, C1242.  */
      if (gfc_is_coindexed (p))
	{
	  gfc_error ("Pointer object at %C shall not be coindexed");
	  goto cleanup;
	}

      /* Check for valid array pointer object.  Bounds remapping is not
	 allowed with NULLIFY.  */
      if (p->ref)
	{
	  gfc_ref *remap = p->ref;
	  for (; remap; remap = remap->next)
	    if (!remap->next && remap->type == REF_ARRAY
		&& remap->u.ar.type != AR_FULL)
	      break;
	  if (remap)
	    {
	      gfc_error ("NULLIFY does not allow bounds remapping for "
			 "pointer object at %C");
	      goto cleanup;
	    }
	}

      /* build ' => NULL() '.  */
      e = gfc_get_null_expr (&gfc_current_locus);

      /* Chain to list.  */
      if (tail == NULL)
	{
	  tail = &new_st;
	  tail->op = EXEC_POINTER_ASSIGN;
	}
      else
	{
	  tail->next = gfc_get_code (EXEC_POINTER_ASSIGN);
	  tail = tail->next;
	}

      tail->expr1 = p;
      tail->expr2 = e;

      if (gfc_match (" )%t") == MATCH_YES)
	break;
      if (gfc_match_char (',') != MATCH_YES)
	goto syntax;
    }

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_NULLIFY);

cleanup:
  gfc_free_statements (new_st.next);
  new_st.next = NULL;
  gfc_free_expr (new_st.expr1);
  new_st.expr1 = NULL;
  gfc_free_expr (new_st.expr2);
  new_st.expr2 = NULL;
  gfc_free_expr (p);
  return MATCH_ERROR;
}


/* Match a DEALLOCATE statement.  */

match
gfc_match_deallocate (void)
{
  gfc_alloc *head, *tail;
  gfc_expr *stat, *errmsg, *tmp;
  gfc_symbol *sym;
  match m;
  bool saw_stat, saw_errmsg, b1, b2;

  head = tail = NULL;
  stat = errmsg = tmp = NULL;
  saw_stat = saw_errmsg = false;

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  for (;;)
    {
      if (head == NULL)
	head = tail = gfc_get_alloc ();
      else
	{
	  tail->next = gfc_get_alloc ();
	  tail = tail->next;
	}

      m = gfc_match_variable (&tail->expr, 0);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_NO)
	goto syntax;

      if (tail->expr->expr_type == EXPR_CONSTANT)
	{
	  gfc_error ("Unexpected constant at %C");
	  goto cleanup;
	}

      if (gfc_check_do_variable (tail->expr->symtree))
	goto cleanup;

      sym = tail->expr->symtree->n.sym;

      bool impure = gfc_impure_variable (sym);
      if (impure && gfc_pure (NULL))
	{
	  gfc_error ("Illegal allocate-object at %C for a PURE procedure");
	  goto cleanup;
	}

      if (impure)
	gfc_unset_implicit_pure (NULL);

      if (gfc_is_coarray (tail->expr)
	  && gfc_find_state (COMP_DO_CONCURRENT))
	{
	  gfc_error ("DEALLOCATE of coarray at %C in DO CONCURRENT block");
	  goto cleanup;
	}

      if (gfc_is_coarray (tail->expr)
	  && gfc_find_state (COMP_CRITICAL))
	{
	  gfc_error ("DEALLOCATE of coarray at %C in CRITICAL block");
	  goto cleanup;
	}

      /* FIXME: disable the checking on derived types.  */
      b1 = !(tail->expr->ref
	   && (tail->expr->ref->type == REF_COMPONENT
	       || tail->expr->ref->type == REF_ARRAY));
      if (sym && sym->ts.type == BT_CLASS)
	b2 = !(CLASS_DATA (sym) && (CLASS_DATA (sym)->attr.allocatable
	       || CLASS_DATA (sym)->attr.class_pointer));
      else
	b2 = sym && !(sym->attr.allocatable || sym->attr.pointer
		      || sym->attr.proc_pointer);
      if (b1 && b2)
	{
	  gfc_error ("Allocate-object at %C is not a nonprocedure pointer "
		     "nor an allocatable variable");
	  goto cleanup;
	}

      if (gfc_match_char (',') != MATCH_YES)
	break;

dealloc_opt_list:

      m = gfc_match (" stat = %e", &tmp);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_YES)
	{
	  if (saw_stat)
	    {
	      gfc_error ("Redundant STAT tag found at %L", &tmp->where);
	      gfc_free_expr (tmp);
	      goto cleanup;
	    }

	  stat = tmp;
	  saw_stat = true;

	  if (gfc_check_do_variable (stat->symtree))
	    goto cleanup;

	  if (gfc_match_char (',') == MATCH_YES)
	    goto dealloc_opt_list;
	}

      m = gfc_match (" errmsg = %e", &tmp);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_YES)
	{
	  if (!gfc_notify_std (GFC_STD_F2003, "ERRMSG at %L", &tmp->where))
	    goto cleanup;

	  if (saw_errmsg)
	    {
	      gfc_error ("Redundant ERRMSG tag found at %L", &tmp->where);
	      gfc_free_expr (tmp);
	      goto cleanup;
	    }

	  errmsg = tmp;
	  saw_errmsg = true;

	  if (gfc_match_char (',') == MATCH_YES)
	    goto dealloc_opt_list;
	}

	gfc_gobble_whitespace ();

	if (gfc_peek_char () == ')')
	  break;
    }

  if (gfc_match (" )%t") != MATCH_YES)
    goto syntax;

  new_st.op = EXEC_DEALLOCATE;
  new_st.expr1 = stat;
  new_st.expr2 = errmsg;
  new_st.ext.alloc.list = head;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_DEALLOCATE);

cleanup:
  gfc_free_expr (errmsg);
  gfc_free_expr (stat);
  gfc_free_alloc_list (head);
  return MATCH_ERROR;
}


/* Match a RETURN statement.  */

match
gfc_match_return (void)
{
  gfc_expr *e;
  match m;
  gfc_compile_state s;

  e = NULL;

  if (gfc_find_state (COMP_CRITICAL))
    {
      gfc_error ("Image control statement RETURN at %C in CRITICAL block");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_DO_CONCURRENT))
    {
      gfc_error ("Image control statement RETURN at %C in DO CONCURRENT block");
      return MATCH_ERROR;
    }

  if (gfc_find_state (COMP_CHANGE_TEAM))
    {
      /* F2018, C1111: A RETURN statement shall not appear within a CHANGE TEAM
	 construct.  */
      gfc_error (
	"Image control statement RETURN at %C in CHANGE TEAM-END TEAM block");
      return MATCH_ERROR;
    }

  if (gfc_match_eos () == MATCH_YES)
    goto done;

  if (!gfc_find_state (COMP_SUBROUTINE))
    {
      gfc_error ("Alternate RETURN statement at %C is only allowed within "
		 "a SUBROUTINE");
      goto cleanup;
    }

  if (gfc_current_form == FORM_FREE)
    {
      /* The following are valid, so we can't require a blank after the
	RETURN keyword:
	  return+1
	  return(1)  */
      char c = gfc_peek_ascii_char ();
      if (ISALPHA (c) || ISDIGIT (c))
	return MATCH_NO;
    }

  m = gfc_match (" %e%t", &e);
  if (m == MATCH_YES)
    goto done;
  if (m == MATCH_ERROR)
    goto cleanup;

  gfc_syntax_error (ST_RETURN);

cleanup:
  gfc_free_expr (e);
  return MATCH_ERROR;

done:
  gfc_enclosing_unit (&s);
  if (s == COMP_PROGRAM
      && !gfc_notify_std (GFC_STD_GNU, "RETURN statement in "
			  "main program at %C"))
      return MATCH_ERROR;

  new_st.op = EXEC_RETURN;
  new_st.expr1 = e;

  return MATCH_YES;
}


/* Match the call of a type-bound procedure, if CALL%var has already been
   matched and var found to be a derived-type variable.  */

static match
match_typebound_call (gfc_symtree* varst)
{
  gfc_expr* base;
  match m;

  base = gfc_get_expr ();
  base->expr_type = EXPR_VARIABLE;
  base->symtree = varst;
  base->where = gfc_current_locus;
  gfc_set_sym_referenced (varst->n.sym);

  m = gfc_match_varspec (base, 0, true, true);
  if (m == MATCH_NO)
    gfc_error ("Expected component reference at %C");
  if (m != MATCH_YES)
    {
      gfc_free_expr (base);
      return MATCH_ERROR;
    }

  if (gfc_match_eos () != MATCH_YES)
    {
      gfc_error ("Junk after CALL at %C");
      gfc_free_expr (base);
      return MATCH_ERROR;
    }

  if (base->expr_type == EXPR_COMPCALL)
    new_st.op = EXEC_COMPCALL;
  else if (base->expr_type == EXPR_PPC)
    new_st.op = EXEC_CALL_PPC;
  else
    {
      gfc_error ("Expected type-bound procedure or procedure pointer component "
		 "at %C");
      gfc_free_expr (base);
      return MATCH_ERROR;
    }
  new_st.expr1 = base;

  return MATCH_YES;
}


/* Match a CALL statement.  The tricky part here are possible
   alternate return specifiers.  We handle these by having all
   "subroutines" actually return an integer via a register that gives
   the return number.  If the call specifies alternate returns, we
   generate code for a SELECT statement whose case clauses contain
   GOTOs to the various labels.  */

match
gfc_match_call (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_actual_arglist *a, *arglist;
  gfc_case *new_case;
  gfc_symbol *sym;
  gfc_symtree *st;
  gfc_code *c;
  match m;
  int i;

  arglist = NULL;

  m = gfc_match ("% %n", name);
  if (m == MATCH_NO)
    goto syntax;
  if (m != MATCH_YES)
    return m;

  if (gfc_get_ha_sym_tree (name, &st))
    return MATCH_ERROR;

  sym = st->n.sym;

  /* If this is a variable of derived-type, it probably starts a type-bound
     procedure call. Associate variable targets have to be resolved for the
     target type.  */
  if (((sym->attr.flavor != FL_PROCEDURE
	|| gfc_is_function_return_value (sym, gfc_current_ns))
       && (sym->ts.type == BT_DERIVED || sym->ts.type == BT_CLASS))
		||
      (sym->assoc && sym->assoc->target
       && gfc_resolve_expr (sym->assoc->target)
       && (sym->assoc->target->ts.type == BT_DERIVED
	   || sym->assoc->target->ts.type == BT_CLASS)))
    return match_typebound_call (st);

  /* If it does not seem to be callable (include functions so that the
     right association is made.  They are thrown out in resolution.)
     ...  */
  if (!sym->attr.generic
	&& !sym->attr.proc_pointer
	&& !sym->attr.subroutine
	&& !sym->attr.function)
    {
      if (!(sym->attr.external && !sym->attr.referenced))
	{
	  /* ...create a symbol in this scope...  */
	  if (sym->ns != gfc_current_ns
	        && gfc_get_sym_tree (name, NULL, &st, false) == 1)
            return MATCH_ERROR;

	  if (sym != st->n.sym)
	    sym = st->n.sym;
	}

      /* ...and then to try to make the symbol into a subroutine.  */
      if (!gfc_add_subroutine (&sym->attr, sym->name, NULL))
	return MATCH_ERROR;
    }

  gfc_set_sym_referenced (sym);

  if (gfc_match_eos () != MATCH_YES)
    {
      m = gfc_match_actual_arglist (1, &arglist);
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      if (gfc_match_eos () != MATCH_YES)
	goto syntax;
    }

  /* Walk the argument list looking for invalid BOZ.  */
  for (a = arglist; a; a = a->next)
    if (a->expr && a->expr->ts.type == BT_BOZ)
      {
	gfc_error ("A BOZ literal constant at %L cannot appear as an actual "
		   "argument in a subroutine reference", &a->expr->where);
	goto cleanup;
      }


  /* If any alternate return labels were found, construct a SELECT
     statement that will jump to the right place.  */

  i = 0;
  for (a = arglist; a; a = a->next)
    if (a->expr == NULL)
      {
	i = 1;
	break;
      }

  if (i)
    {
      gfc_symtree *select_st;
      gfc_symbol *select_sym;
      char name[GFC_MAX_SYMBOL_LEN + 1];

      new_st.next = c = gfc_get_code (EXEC_SELECT);
      sprintf (name, "_result_%s", sym->name);
      gfc_get_ha_sym_tree (name, &select_st);   /* Can't fail.  */

      select_sym = select_st->n.sym;
      select_sym->ts.type = BT_INTEGER;
      select_sym->ts.kind = gfc_default_integer_kind;
      gfc_set_sym_referenced (select_sym);
      c->expr1 = gfc_get_expr ();
      c->expr1->expr_type = EXPR_VARIABLE;
      c->expr1->symtree = select_st;
      c->expr1->ts = select_sym->ts;
      c->expr1->where = gfc_current_locus;

      i = 0;
      for (a = arglist; a; a = a->next)
	{
	  if (a->expr != NULL)
	    continue;

	  if (!gfc_reference_st_label (a->label, ST_LABEL_TARGET))
	    continue;

	  i++;

	  c->block = gfc_get_code (EXEC_SELECT);
	  c = c->block;

	  new_case = gfc_get_case ();
	  new_case->high = gfc_get_int_expr (gfc_default_integer_kind, NULL, i);
	  new_case->low = new_case->high;
	  c->ext.block.case_list = new_case;

	  c->next = gfc_get_code (EXEC_GOTO);
	  c->next->label1 = a->label;
	}
    }

  new_st.op = EXEC_CALL;
  new_st.symtree = st;
  new_st.ext.actual = arglist;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_CALL);

cleanup:
  gfc_free_actual_arglist (arglist);
  return MATCH_ERROR;
}


/* Given a name, return a pointer to the common head structure,
   creating it if it does not exist. If FROM_MODULE is nonzero, we
   mangle the name so that it doesn't interfere with commons defined
   in the using namespace.
   TODO: Add to global symbol tree.  */

gfc_common_head *
gfc_get_common (const char *name, int from_module)
{
  gfc_symtree *st;
  static int serial = 0;
  char mangled_name[GFC_MAX_SYMBOL_LEN + 1];

  if (from_module)
    {
      /* A use associated common block is only needed to correctly layout
	 the variables it contains.  */
      snprintf (mangled_name, GFC_MAX_SYMBOL_LEN, "_%d_%s", serial++, name);
      st = gfc_new_symtree (&gfc_current_ns->common_root, mangled_name);
    }
  else
    {
      st = gfc_find_symtree (gfc_current_ns->common_root, name);

      if (st == NULL)
	st = gfc_new_symtree (&gfc_current_ns->common_root, name);
    }

  if (st->n.common == NULL)
    {
      st->n.common = gfc_get_common_head ();
      st->n.common->where = gfc_current_locus;
      strcpy (st->n.common->name, name);
    }

  return st->n.common;
}


/* Match a common block name.  */

match
gfc_match_common_name (char *name)
{
  match m;

  if (gfc_match_char ('/') == MATCH_NO)
    {
      name[0] = '\0';
      return MATCH_YES;
    }

  if (gfc_match_char ('/') == MATCH_YES)
    {
      name[0] = '\0';
      return MATCH_YES;
    }

  m = gfc_match_name (name);

  if (m == MATCH_ERROR)
    return MATCH_ERROR;
  if (m == MATCH_YES && gfc_match_char ('/') == MATCH_YES)
    return MATCH_YES;

  gfc_error ("Syntax error in common block name at %C");
  return MATCH_ERROR;
}


/* Match a COMMON statement.  */

match
gfc_match_common (void)
{
  gfc_symbol *sym, **head, *tail, *other;
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_common_head *t;
  gfc_array_spec *as;
  gfc_equiv *e1, *e2;
  match m;
  char c;

  /* COMMON has been matched.  In free form source code, the next character
     needs to be whitespace or '/'.  Check that here.   Fixed form source
     code needs to be checked below.  */
  c = gfc_peek_ascii_char ();
  if (gfc_current_form == FORM_FREE && !gfc_is_whitespace (c) && c != '/')
    return MATCH_NO;

  as = NULL;

  for (;;)
    {
      m = gfc_match_common_name (name);
      if (m == MATCH_ERROR)
	goto cleanup;

      if (name[0] == '\0')
	{
	  t = &gfc_current_ns->blank_common;
	  if (t->head == NULL)
	    t->where = gfc_current_locus;
	}
      else
	{
	  t = gfc_get_common (name, 0);
	}
      head = &t->head;

      if (*head == NULL)
	tail = NULL;
      else
	{
	  tail = *head;
	  while (tail->common_next)
	    tail = tail->common_next;
	}

      /* Grab the list of symbols.  */
      for (;;)
	{
	  m = gfc_match_symbol (&sym, 0);
	  if (m == MATCH_ERROR)
	    goto cleanup;
	  if (m == MATCH_NO)
	    goto syntax;

          /* See if we know the current common block is bind(c), and if
             so, then see if we can check if the symbol is (which it'll
             need to be).  This can happen if the bind(c) attr stmt was
             applied to the common block, and the variable(s) already
             defined, before declaring the common block.  */
          if (t->is_bind_c == 1)
            {
              if (sym->ts.type != BT_UNKNOWN && sym->ts.is_c_interop != 1)
                {
                  /* If we find an error, just print it and continue,
                     cause it's just semantic, and we can see if there
                     are more errors.  */
                  gfc_error_now ("Variable %qs at %L in common block %qs "
				 "at %C must be declared with a C "
				 "interoperable kind since common block "
				 "%qs is bind(c)",
				 sym->name, &(sym->declared_at), t->name,
				 t->name);
                }

              if (sym->attr.is_bind_c == 1)
                gfc_error_now ("Variable %qs in common block %qs at %C cannot "
                               "be bind(c) since it is not global", sym->name,
			       t->name);
            }

	  if (sym->attr.in_common)
	    {
	      gfc_error ("Symbol %qs at %C is already in a COMMON block",
			 sym->name);
	      goto cleanup;
	    }

	  if (((sym->value != NULL && sym->value->expr_type != EXPR_NULL)
	       || sym->attr.data) && gfc_current_state () != COMP_BLOCK_DATA)
	    {
	      if (!gfc_notify_std (GFC_STD_GNU, "Initialized symbol %qs at "
				   "%C can only be COMMON in BLOCK DATA",
				   sym->name))
		goto cleanup;
	    }

	  /* F2018:R874:  common-block-object is variable-name [ (array-spec) ]
	     F2018:C8121: A variable-name shall not be a name made accessible
	     by use association.  */
	  if (sym->attr.use_assoc)
	    {
	      gfc_error ("Symbol %qs at %C is USE associated from module %qs "
			 "and cannot occur in COMMON", sym->name, sym->module);
	      goto cleanup;
	    }

	  /* Deal with an optional array specification after the
	     symbol name.  */
	  m = gfc_match_array_spec (&as, true, true);
	  if (m == MATCH_ERROR)
	    goto cleanup;

	  if (m == MATCH_YES)
	    {
	      if (as->type != AS_EXPLICIT)
		{
		  gfc_error ("Array specification for symbol %qs in COMMON "
			     "at %C must be explicit", sym->name);
		  goto cleanup;
		}

	      if (as->corank)
		{
		  gfc_error ("Symbol %qs in COMMON at %C cannot be a "
			     "coarray", sym->name);
		  goto cleanup;
		}

	      if (!gfc_add_dimension (&sym->attr, sym->name, NULL))
		goto cleanup;

	      if (sym->attr.pointer)
		{
		  gfc_error ("Symbol %qs in COMMON at %C cannot be a "
			     "POINTER array", sym->name);
		  goto cleanup;
		}

	      sym->as = as;
	      as = NULL;

	    }

	  /* Add the in_common attribute, but ignore the reported errors
	     if any, and continue matching.  */
	  gfc_add_in_common (&sym->attr, sym->name, NULL);

	  sym->common_block = t;
	  sym->common_block->refs++;

	  if (tail != NULL)
	    tail->common_next = sym;
	  else
	    *head = sym;

	  tail = sym;

	  sym->common_head = t;

	  /* Check to see if the symbol is already in an equivalence group.
	     If it is, set the other members as being in common.  */
	  if (sym->attr.in_equivalence)
	    {
	      for (e1 = gfc_current_ns->equiv; e1; e1 = e1->next)
		{
		  for (e2 = e1; e2; e2 = e2->eq)
		    if (e2->expr->symtree->n.sym == sym)
		      goto equiv_found;

		  continue;

	  equiv_found:

		  for (e2 = e1; e2; e2 = e2->eq)
		    {
		      other = e2->expr->symtree->n.sym;
		      if (other->common_head
			  && other->common_head != sym->common_head)
			{
			  gfc_error ("Symbol %qs, in COMMON block %qs at "
				     "%C is being indirectly equivalenced to "
				     "another COMMON block %qs",
				     sym->name, sym->common_head->name,
				     other->common_head->name);
			    goto cleanup;
			}
		      other->attr.in_common = 1;
		      other->common_head = t;
		    }
		}
	    }


	  gfc_gobble_whitespace ();
	  if (gfc_match_eos () == MATCH_YES)
	    goto done;
	  c = gfc_peek_ascii_char ();
	  if (c == '/')
	    break;
	  if (c != ',')
	    {
	      /* In Fixed form source code, gfortran can end up here for an
		 expression of the form COMMONI = RHS.  This may not be an
		 error, so return MATCH_NO.  */
	      if (gfc_current_form == FORM_FIXED && c == '=')
		{
		  gfc_free_array_spec (as);
		  return MATCH_NO;
		}
	      goto syntax;
	    }
	  else
	    gfc_match_char (',');

	  gfc_gobble_whitespace ();
	  if (gfc_peek_ascii_char () == '/')
	    break;
	}
    }

done:
  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_COMMON);

cleanup:
  gfc_free_array_spec (as);
  return MATCH_ERROR;
}


/* Match a BLOCK DATA program unit.  */

match
gfc_match_block_data (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_symbol *sym;
  match m;

  if (!gfc_notify_std (GFC_STD_F2018_OBS, "BLOCK DATA construct at %L",
      &gfc_current_locus))
    return MATCH_ERROR;

  if (gfc_match_eos () == MATCH_YES)
    {
      gfc_new_block = NULL;
      return MATCH_YES;
    }

  m = gfc_match ("% %n%t", name);
  if (m != MATCH_YES)
    return MATCH_ERROR;

  if (gfc_get_symbol (name, NULL, &sym))
    return MATCH_ERROR;

  if (!gfc_add_flavor (&sym->attr, FL_BLOCK_DATA, sym->name, NULL))
    return MATCH_ERROR;

  gfc_new_block = sym;

  return MATCH_YES;
}


/* Free a namelist structure.  */

void
gfc_free_namelist (gfc_namelist *name)
{
  gfc_namelist *n;

  for (; name; name = n)
    {
      n = name->next;
      free (name);
    }
}


/* Free an OpenMP namelist structure.  */

void
gfc_free_omp_namelist (gfc_omp_namelist *name, bool free_ns,
		       bool free_align_allocator,
		       bool free_mem_traits_space, bool free_init)
{
  gfc_omp_namelist *n;
  gfc_expr *last_allocator = NULL;
  char *last_init_interop = NULL;

  for (; name; name = n)
    {
      gfc_free_expr (name->expr);
      if (free_align_allocator)
	gfc_free_expr (name->u.align);
      else if (free_mem_traits_space)
	{ }  /* name->u.memspace_sym: shall not call gfc_free_symbol here. */

      if (free_ns)
	gfc_free_namespace (name->u2.ns);
      else if (free_align_allocator)
	{
	  if (last_allocator != name->u2.allocator)
	    {
	      last_allocator = name->u2.allocator;
	      gfc_free_expr (name->u2.allocator);
	    }
	}
      else if (free_mem_traits_space)
	{ }  /* name->u2.traits_sym: shall not call gfc_free_symbol here. */
      else if (free_init)
	{
	  if (name->u2.init_interop != last_init_interop)
	    {
	      last_init_interop = name->u2.init_interop;
	      free (name->u2.init_interop);
	    }
	}
      else if (name->u2.udr)
	{
	  if (name->u2.udr->combiner)
	    gfc_free_statement (name->u2.udr->combiner);
	  if (name->u2.udr->initializer)
	    gfc_free_statement (name->u2.udr->initializer);
	  free (name->u2.udr);
	}
      n = name->next;
      free (name);
    }
}


/* Match a NAMELIST statement.  */

match
gfc_match_namelist (void)
{
  gfc_symbol *group_name, *sym;
  gfc_namelist *nl;
  match m, m2;

  m = gfc_match (" / %s /", &group_name);
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto error;

  for (;;)
    {
      if (group_name->ts.type != BT_UNKNOWN)
	{
	  gfc_error ("Namelist group name %qs at %C already has a basic "
		     "type of %s", group_name->name,
		     gfc_typename (&group_name->ts));
	  return MATCH_ERROR;
	}

      /* A use associated name shall not be used as a namelist group name
	 (e.g. F2003:C581).  It is only supported as a legacy extension.  */
      if (group_name->attr.flavor == FL_NAMELIST
	  && group_name->attr.use_assoc
	  && !gfc_notify_std (GFC_STD_LEGACY, "Namelist group name %qs "
			      "at %C already is USE associated and can"
			      "not be respecified.", group_name->name))
	return MATCH_ERROR;

      if (group_name->attr.flavor != FL_NAMELIST
	  && !gfc_add_flavor (&group_name->attr, FL_NAMELIST,
			      group_name->name, NULL))
	return MATCH_ERROR;

      for (;;)
	{
	  m = gfc_match_symbol (&sym, 1);
	  if (m == MATCH_NO)
	    goto syntax;
	  if (m == MATCH_ERROR)
	    goto error;

	  if (sym->ts.type == BT_UNKNOWN)
	    {
	      if (gfc_current_ns->seen_implicit_none)
		{
		  /* It is required that members of a namelist be declared
		     before the namelist.  We check this by checking if the
		     symbol has a defined type for IMPLICIT NONE.  */
		  gfc_error ("Symbol %qs in namelist %qs at %C must be "
			     "declared before the namelist is declared.",
			     sym->name, group_name->name);
		  gfc_error_check ();
		}
	      else
		{
		  /* Before the symbol is given an implicit type, check to
		     see if the symbol is already available in the namespace,
		     possibly through host association.  Importantly, the
		     symbol may be a user defined type.  */

		  gfc_symbol *tmp;

		  gfc_find_symbol (sym->name, NULL, 1, &tmp);
		  if (tmp && tmp->attr.generic
		      && (tmp = gfc_find_dt_in_generic (tmp)))
		    {
		      if (tmp->attr.flavor == FL_DERIVED)
			{
			  gfc_error ("Derived type %qs at %L conflicts with "
				     "namelist object %qs at %C",
				     tmp->name, &tmp->declared_at, sym->name);
			  goto error;
			}
		    }

		  /* Set type of the symbol to its implicit default type.  It is
		     not allowed to set it later to any other type.  */
		  gfc_set_default_type (sym, 0, gfc_current_ns);
		}
	    }
	  if (sym->attr.in_namelist == 0
	      && !gfc_add_in_namelist (&sym->attr, sym->name, NULL))
	    goto error;

	  /* Use gfc_error_check here, rather than goto error, so that
	     these are the only errors for the next two lines.  */
	  if (sym->as && sym->as->type == AS_ASSUMED_SIZE)
	    {
	      gfc_error ("Assumed size array %qs in namelist %qs at "
			 "%C is not allowed", sym->name, group_name->name);
	      gfc_error_check ();
	    }

	  nl = gfc_get_namelist ();
	  nl->sym = sym;
	  sym->refs++;

	  if (group_name->namelist == NULL)
	    group_name->namelist = group_name->namelist_tail = nl;
	  else
	    {
	      group_name->namelist_tail->next = nl;
	      group_name->namelist_tail = nl;
	    }

	  if (gfc_match_eos () == MATCH_YES)
	    goto done;

	  m = gfc_match_char (',');

	  if (gfc_match_char ('/') == MATCH_YES)
	    {
	      m2 = gfc_match (" %s /", &group_name);
	      if (m2 == MATCH_YES)
		break;
	      if (m2 == MATCH_ERROR)
		goto error;
	      goto syntax;
	    }

	  if (m != MATCH_YES)
	    goto syntax;
	}
    }

done:
  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_NAMELIST);

error:
  return MATCH_ERROR;
}


/* Match a MODULE statement.  */

match
gfc_match_module (void)
{
  match m;

  m = gfc_match (" %s%t", &gfc_new_block);
  if (m != MATCH_YES)
    return m;

  if (!gfc_add_flavor (&gfc_new_block->attr, FL_MODULE,
		       gfc_new_block->name, NULL))
    return MATCH_ERROR;

  return MATCH_YES;
}


/* Free equivalence sets and lists.  Recursively is the easiest way to
   do this.  */

void
gfc_free_equiv_until (gfc_equiv *eq, gfc_equiv *stop)
{
  if (eq == stop)
    return;

  gfc_free_equiv (eq->eq);
  gfc_free_equiv_until (eq->next, stop);
  gfc_free_expr (eq->expr);
  free (eq);
}


void
gfc_free_equiv (gfc_equiv *eq)
{
  gfc_free_equiv_until (eq, NULL);
}


/* Match an EQUIVALENCE statement.  */

match
gfc_match_equivalence (void)
{
  gfc_equiv *eq, *set, *tail;
  gfc_ref *ref;
  gfc_symbol *sym;
  match m;
  gfc_common_head *common_head = NULL;
  bool common_flag;
  int cnt;
  char c;

  /* EQUIVALENCE has been matched.  After gobbling any possible whitespace,
     the next character needs to be '('.  Check that here, and return
     MATCH_NO for a variable of the form equivalence.  */
  gfc_gobble_whitespace ();
  c = gfc_peek_ascii_char ();
  if (c != '(')
    return MATCH_NO;

  tail = NULL;

  for (;;)
    {
      eq = gfc_get_equiv ();
      if (tail == NULL)
	tail = eq;

      eq->next = gfc_current_ns->equiv;
      gfc_current_ns->equiv = eq;

      if (gfc_match_char ('(') != MATCH_YES)
	goto syntax;

      set = eq;
      common_flag = false;
      cnt = 0;

      for (;;)
	{
	  m = gfc_match_equiv_variable (&set->expr);
	  if (m == MATCH_ERROR)
	    goto cleanup;
	  if (m == MATCH_NO)
	    goto syntax;

	  /*  count the number of objects.  */
	  cnt++;

	  if (gfc_match_char ('%') == MATCH_YES)
	    {
	      gfc_error ("Derived type component %C is not a "
			 "permitted EQUIVALENCE member");
	      goto cleanup;
	    }

	  for (ref = set->expr->ref; ref; ref = ref->next)
	    if (ref->type == REF_ARRAY && ref->u.ar.type == AR_SECTION)
	      {
		gfc_error ("Array reference in EQUIVALENCE at %C cannot "
			   "be an array section");
		goto cleanup;
	      }

	  sym = set->expr->symtree->n.sym;

	  if (!gfc_add_in_equivalence (&sym->attr, sym->name, NULL))
	    goto cleanup;
	  if (sym->ts.type == BT_CLASS
	      && CLASS_DATA (sym)
	      && !gfc_add_in_equivalence (&CLASS_DATA (sym)->attr,
					  sym->name, NULL))
	    goto cleanup;

	  if (sym->attr.in_common)
	    {
	      common_flag = true;
	      common_head = sym->common_head;
	    }

	  if (gfc_match_char (')') == MATCH_YES)
	    break;

	  if (gfc_match_char (',') != MATCH_YES)
	    goto syntax;

	  set->eq = gfc_get_equiv ();
	  set = set->eq;
	}

      if (cnt < 2)
	{
	  gfc_error ("EQUIVALENCE at %C requires two or more objects");
	  goto cleanup;
	}

      /* If one of the members of an equivalence is in common, then
	 mark them all as being in common.  Before doing this, check
	 that members of the equivalence group are not in different
	 common blocks.  */
      if (common_flag)
	for (set = eq; set; set = set->eq)
	  {
	    sym = set->expr->symtree->n.sym;
	    if (sym->common_head && sym->common_head != common_head)
	      {
		gfc_error ("Attempt to indirectly overlap COMMON "
			   "blocks %s and %s by EQUIVALENCE at %C",
			   sym->common_head->name, common_head->name);
		goto cleanup;
	      }
	    sym->attr.in_common = 1;
	    sym->common_head = common_head;
	  }

      if (gfc_match_eos () == MATCH_YES)
	break;
      if (gfc_match_char (',') != MATCH_YES)
	{
	  gfc_error ("Expecting a comma in EQUIVALENCE at %C");
	  goto cleanup;
	}
    }

  if (!gfc_notify_std (GFC_STD_F2018_OBS, "EQUIVALENCE statement at %C"))
    return MATCH_ERROR;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_EQUIVALENCE);

cleanup:
  eq = tail->next;
  tail->next = NULL;

  gfc_free_equiv (gfc_current_ns->equiv);
  gfc_current_ns->equiv = eq;

  return MATCH_ERROR;
}


/* Check that a statement function is not recursive. This is done by looking
   for the statement function symbol(sym) by looking recursively through its
   expression(e).  If a reference to sym is found, true is returned.
   12.5.4 requires that any variable of function that is implicitly typed
   shall have that type confirmed by any subsequent type declaration.  The
   implicit typing is conveniently done here.  */
static bool
recursive_stmt_fcn (gfc_expr *, gfc_symbol *);

static bool
check_stmt_fcn (gfc_expr *e, gfc_symbol *sym, int *f ATTRIBUTE_UNUSED)
{

  if (e == NULL)
    return false;

  switch (e->expr_type)
    {
    case EXPR_FUNCTION:
      if (e->symtree == NULL)
	return false;

      /* Check the name before testing for nested recursion!  */
      if (sym->name == e->symtree->n.sym->name)
	return true;

      /* Catch recursion via other statement functions.  */
      if (e->symtree->n.sym->attr.proc == PROC_ST_FUNCTION
	  && e->symtree->n.sym->value
	  && recursive_stmt_fcn (e->symtree->n.sym->value, sym))
	return true;

      if (e->symtree->n.sym->ts.type == BT_UNKNOWN)
	gfc_set_default_type (e->symtree->n.sym, 0, NULL);

      break;

    case EXPR_VARIABLE:
      if (e->symtree && sym->name == e->symtree->n.sym->name)
	return true;

      if (e->symtree->n.sym->ts.type == BT_UNKNOWN)
	gfc_set_default_type (e->symtree->n.sym, 0, NULL);
      break;

    default:
      break;
    }

  return false;
}


static bool
recursive_stmt_fcn (gfc_expr *e, gfc_symbol *sym)
{
  return gfc_traverse_expr (e, sym, check_stmt_fcn, 0);
}


/* Check for invalid uses of statement function dummy arguments in body.  */

static bool
chk_stmt_fcn_body (gfc_expr *e, gfc_symbol *sym, int *f ATTRIBUTE_UNUSED)
{
  gfc_formal_arglist *formal;

  if (e == NULL || e->symtree == NULL || e->expr_type != EXPR_FUNCTION)
    return false;

  for (formal = sym->formal; formal; formal = formal->next)
    {
      if (formal->sym == e->symtree->n.sym)
	{
	  gfc_error ("Invalid use of statement function argument at %L",
		     &e->where);
	  return true;
	}
    }

  return false;
}


/* Match a statement function declaration.  It is so easy to match
   non-statement function statements with a MATCH_ERROR as opposed to
   MATCH_NO that we suppress error message in most cases.  */

match
gfc_match_st_function (void)
{
  gfc_error_buffer old_error;
  gfc_symbol *sym;
  gfc_expr *expr;
  match m;
  char name[GFC_MAX_SYMBOL_LEN + 1];
  locus old_locus;
  bool fcn;
  gfc_formal_arglist *ptr;

  /* Read the possible statement function name, and then check to see if
     a symbol is already present in the namespace.  Record if it is a
     function and whether it has been referenced.  */
  fcn = false;
  ptr = NULL;
  old_locus = gfc_current_locus;
  m = gfc_match_name (name);
  if (m == MATCH_YES)
    {
      gfc_find_symbol (name, NULL, 1, &sym);
      if (sym && sym->attr.function && !sym->attr.referenced)
	{
	  fcn = true;
	  ptr = sym->formal;
	}
    }

  gfc_current_locus = old_locus;
  m = gfc_match_symbol (&sym, 0);
  if (m != MATCH_YES)
    return m;

  gfc_push_error (&old_error);

  if (!gfc_add_procedure (&sym->attr, PROC_ST_FUNCTION, sym->name, NULL))
    goto undo_error;

  if (gfc_match_formal_arglist (sym, 1, 0) != MATCH_YES)
    goto undo_error;

  m = gfc_match (" = %e%t", &expr);
  if (m == MATCH_NO)
    goto undo_error;

  gfc_free_error (&old_error);

  if (m == MATCH_ERROR)
    return m;

  if (recursive_stmt_fcn (expr, sym))
    {
      gfc_error ("Statement function at %L is recursive", &expr->where);
      return MATCH_ERROR;
    }

  if (fcn && ptr != sym->formal)
    {
      gfc_error ("Statement function %qs at %L conflicts with function name",
		 sym->name, &expr->where);
      return MATCH_ERROR;
    }

  if (gfc_traverse_expr (expr, sym, chk_stmt_fcn_body, 0))
    return MATCH_ERROR;

  sym->value = expr;

  if ((gfc_current_state () == COMP_FUNCTION
       || gfc_current_state () == COMP_SUBROUTINE)
      && gfc_state_stack->previous->state == COMP_INTERFACE)
    {
      gfc_error ("Statement function at %L cannot appear within an INTERFACE",
		 &expr->where);
      return MATCH_ERROR;
    }

  if (!gfc_notify_std (GFC_STD_F95_OBS, "Statement function at %C"))
    return MATCH_ERROR;

  return MATCH_YES;

undo_error:
  gfc_pop_error (&old_error);
  return MATCH_NO;
}


/* Match an assignment to a pointer function (F2008). This could, in
   general be ambiguous with a statement function. In this implementation
   it remains so if it is the first statement after the specification
   block.  */

match
gfc_match_ptr_fcn_assign (void)
{
  gfc_error_buffer old_error;
  locus old_loc;
  gfc_symbol *sym;
  gfc_expr *expr;
  match m;
  char name[GFC_MAX_SYMBOL_LEN + 1];

  old_loc = gfc_current_locus;
  m = gfc_match_name (name);
  if (m != MATCH_YES)
    return m;

  gfc_find_symbol (name, NULL, 1, &sym);
  if (sym && sym->attr.flavor != FL_PROCEDURE)
    return MATCH_NO;

  gfc_push_error (&old_error);

  if (sym && sym->attr.function)
    goto match_actual_arglist;

  gfc_current_locus = old_loc;
  m = gfc_match_symbol (&sym, 0);
  if (m != MATCH_YES)
    return m;

  if (!gfc_add_procedure (&sym->attr, PROC_UNKNOWN, sym->name, NULL))
    goto undo_error;

match_actual_arglist:
  gfc_current_locus = old_loc;
  m = gfc_match (" %e", &expr);
  if (m != MATCH_YES)
    goto undo_error;

  new_st.op = EXEC_ASSIGN;
  new_st.expr1 = expr;
  expr = NULL;

  m = gfc_match (" = %e%t", &expr);
  if (m != MATCH_YES)
    goto undo_error;

  new_st.expr2 = expr;
  return MATCH_YES;

undo_error:
  gfc_pop_error (&old_error);
  return MATCH_NO;
}


/***************** SELECT CASE subroutines ******************/

/* Free a single case structure.  */

static void
free_case (gfc_case *p)
{
  if (p->low == p->high)
    p->high = NULL;
  gfc_free_expr (p->low);
  gfc_free_expr (p->high);
  free (p);
}


/* Free a list of case structures.  */

void
gfc_free_case_list (gfc_case *p)
{
  gfc_case *q;

  for (; p; p = q)
    {
      q = p->next;
      free_case (p);
    }
}


/* Match a single case selector.  Combining the requirements of F08:C830
   and F08:C832 (R838) means that the case-value must have either CHARACTER,
   INTEGER, or LOGICAL type.  */

static match
match_case_selector (gfc_case **cp)
{
  gfc_case *c;
  match m;

  c = gfc_get_case ();
  c->where = gfc_current_locus;

  if (gfc_match_char (':') == MATCH_YES)
    {
      m = gfc_match_init_expr (&c->high);
      if (m == MATCH_NO)
	goto need_expr;
      if (m == MATCH_ERROR)
	goto cleanup;

      if (c->high->ts.type != BT_LOGICAL && c->high->ts.type != BT_INTEGER
	  && c->high->ts.type != BT_CHARACTER
	  && (!flag_unsigned
	      || (flag_unsigned && c->high->ts.type != BT_UNSIGNED)))
	{
	  gfc_error ("Expression in CASE selector at %L cannot be %s",
		     &c->high->where, gfc_typename (&c->high->ts));
	  goto cleanup;
	}
    }
  else
    {
      m = gfc_match_init_expr (&c->low);
      if (m == MATCH_ERROR)
	goto cleanup;
      if (m == MATCH_NO)
	goto need_expr;

      if (c->low->ts.type != BT_LOGICAL && c->low->ts.type != BT_INTEGER
	  && c->low->ts.type != BT_CHARACTER
	  && (!flag_unsigned
	      || (flag_unsigned && c->low->ts.type != BT_UNSIGNED)))
	{
	  gfc_error ("Expression in CASE selector at %L cannot be %s",
		     &c->low->where, gfc_typename (&c->low->ts));
	  goto cleanup;
	}

      /* If we're not looking at a ':' now, make a range out of a single
	 target.  Else get the upper bound for the case range.  */
      if (gfc_match_char (':') != MATCH_YES)
	c->high = c->low;
      else
	{
	  m = gfc_match_init_expr (&c->high);
	  if (m == MATCH_ERROR)
	    goto cleanup;
	  if (m == MATCH_YES
	      && c->high->ts.type != BT_LOGICAL
	      && c->high->ts.type != BT_INTEGER
	      && c->high->ts.type != BT_CHARACTER
	      && (!flag_unsigned
		  || (flag_unsigned && c->high->ts.type != BT_UNSIGNED)))
	    {
	      gfc_error ("Expression in CASE selector at %L cannot be %s",
			 &c->high->where, gfc_typename (c->high));
	      goto cleanup;
	    }
	  /* MATCH_NO is fine.  It's OK if nothing is there!  */
	}
    }

  if (c->low && c->low->rank != 0)
    {
      gfc_error ("Expression in CASE selector at %L must be scalar",
		 &c->low->where);
      goto cleanup;
    }
  if (c->high && c->high->rank != 0)
    {
      gfc_error ("Expression in CASE selector at %L must be scalar",
		 &c->high->where);
      goto cleanup;
    }

  *cp = c;
  return MATCH_YES;

need_expr:
  gfc_error ("Expected initialization expression in CASE at %C");

cleanup:
  free_case (c);
  return MATCH_ERROR;
}


/* Match the end of a case statement.  */

static match
match_case_eos (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  match m;

  if (gfc_match_eos () == MATCH_YES)
    return MATCH_YES;

  /* If the case construct doesn't have a case-construct-name, we
     should have matched the EOS.  */
  if (!gfc_current_block ())
    return MATCH_NO;

  gfc_gobble_whitespace ();

  m = gfc_match_name (name);
  if (m != MATCH_YES)
    return m;

  if (strcmp (name, gfc_current_block ()->name) != 0)
    {
      gfc_error ("Expected block name %qs of SELECT construct at %C",
		 gfc_current_block ()->name);
      return MATCH_ERROR;
    }

  return gfc_match_eos ();
}


/* Match a SELECT statement.  */

match
gfc_match_select (void)
{
  gfc_expr *expr;
  match m;

  m = gfc_match_label ();
  if (m == MATCH_ERROR)
    return m;

  m = gfc_match (" select case ( %e )%t", &expr);
  if (m != MATCH_YES)
    return m;

  new_st.op = EXEC_SELECT;
  new_st.expr1 = expr;

  return MATCH_YES;
}


/* Transfer the selector typespec to the associate name.  */

static void
copy_ts_from_selector_to_associate (gfc_expr *associate, gfc_expr *selector,
				    bool select_type = false)
{
  gfc_ref *ref;
  gfc_symbol *assoc_sym;
  int rank = 0, corank = 0;

  assoc_sym = associate->symtree->n.sym;

  /* At this stage the expression rank and arrayspec dimensions have
     not been completely sorted out. We must get the expr2->rank
     right here, so that the correct class container is obtained.  */
  ref = selector->ref;
  while (ref && ref->next)
    ref = ref->next;

  if (selector->ts.type == BT_CLASS
      && CLASS_DATA (selector)
      && CLASS_DATA (selector)->as
      && CLASS_DATA (selector)->as->type == AS_ASSUMED_RANK)
    {
      assoc_sym->attr.dimension = 1;
      assoc_sym->as = gfc_copy_array_spec (CLASS_DATA (selector)->as);
      corank = assoc_sym->as->corank;
      goto build_class_sym;
    }
  else if (selector->ts.type == BT_CLASS
	   && CLASS_DATA (selector)
	   && CLASS_DATA (selector)->as
	   && ((ref && ref->type == REF_ARRAY)
	       || selector->expr_type == EXPR_OP))
    {
      /* Ensure that the array reference type is set.  We cannot use
	 gfc_resolve_expr at this point, so the usable parts of
	 resolve.cc(resolve_array_ref) are employed to do it.  */
      if (ref && ref->u.ar.type == AR_UNKNOWN)
	{
	  ref->u.ar.type = AR_ELEMENT;
	  for (int i = 0; i < ref->u.ar.dimen + ref->u.ar.codimen; i++)
	    if (ref->u.ar.dimen_type[i] == DIMEN_RANGE
		|| ref->u.ar.dimen_type[i] == DIMEN_VECTOR
		|| (ref->u.ar.dimen_type[i] == DIMEN_UNKNOWN
		    && ref->u.ar.start[i] && ref->u.ar.start[i]->rank))
	      {
		ref->u.ar.type = AR_SECTION;
		break;
	      }
	}

      if (!ref || ref->u.ar.type == AR_FULL)
	{
	  selector->rank = CLASS_DATA (selector)->as->rank;
	  selector->corank = CLASS_DATA (selector)->as->corank;
	}
      else if (ref->u.ar.type == AR_SECTION)
	{
	  selector->rank = ref->u.ar.dimen;
	  selector->corank = ref->u.ar.codimen;
	}
      else
	selector->rank = 0;

      rank = selector->rank;
      corank = selector->corank;
    }

  if (rank)
    {
      if (ref)
	{
	  for (int i = 0; i < ref->u.ar.dimen + ref->u.ar.codimen; i++)
	    if (ref->u.ar.dimen_type[i] == DIMEN_ELEMENT
	      || (ref->u.ar.dimen_type[i] == DIMEN_UNKNOWN
		  && ref->u.ar.end[i] == NULL
		  && ref->u.ar.stride[i] == NULL))
	      rank--;
	}

      if (rank)
	{
	  assoc_sym->attr.dimension = 1;
	  assoc_sym->as = gfc_get_array_spec ();
	  assoc_sym->as->rank = rank;
	  assoc_sym->as->type = AS_DEFERRED;
	}
    }

  if (corank != 0 && rank == 0)
    {
      if (!assoc_sym->as)
	assoc_sym->as = gfc_get_array_spec ();
      assoc_sym->as->corank = corank;
      assoc_sym->attr.codimension = 1;
    }
  else if (corank == 0 && rank == 0 && assoc_sym->as)
    {
      free (assoc_sym->as);
      assoc_sym->as = NULL;
    }
build_class_sym:
  /* Deal with the very specific case of a SELECT_TYPE selector being an
     associate_name whose type has been identified by component references.
     It must be assumed that it will be identified as a CLASS expression,
     so convert it now.  */
  if (select_type
      && IS_INFERRED_TYPE (selector)
      && selector->ts.type == BT_DERIVED)
    {
      gfc_find_derived_vtab (selector->ts.u.derived);
      /* The correct class container has to be available.  */
      assoc_sym->ts.u.derived = selector->ts.u.derived;
      assoc_sym->ts.type = BT_CLASS;
      assoc_sym->attr.pointer = 1;
      if (!selector->ts.u.derived->attr.is_class)
	gfc_build_class_symbol (&assoc_sym->ts, &assoc_sym->attr, &assoc_sym->as);
      associate->ts = assoc_sym->ts;
    }
  else if (selector->ts.type == BT_CLASS)
    {
      /* The correct class container has to be available.  */
      assoc_sym->ts.type = BT_CLASS;
      assoc_sym->ts.u.derived = CLASS_DATA (selector)
				? CLASS_DATA (selector)->ts.u.derived
				: selector->ts.u.derived;
      assoc_sym->attr.pointer = 1;
      gfc_build_class_symbol (&assoc_sym->ts, &assoc_sym->attr, &assoc_sym->as);
    }
}


/* Build the associate name  */
static int
build_associate_name (const char *name, gfc_expr **e1, gfc_expr **e2)
{
  gfc_expr *expr1 = *e1;
  gfc_expr *expr2 = *e2;
  gfc_symbol *sym;

  /* For the case where the associate name is already an associate name.  */
  if (!expr2)
    expr2 = expr1;
  expr1 = gfc_get_expr ();
  expr1->expr_type = EXPR_VARIABLE;
  expr1->where = expr2->where;
  if (gfc_get_sym_tree (name, NULL, &expr1->symtree, false))
    return 1;

  sym = expr1->symtree->n.sym;
  if (expr2->ts.type == BT_UNKNOWN)
    sym->attr.untyped = 1;
  else
    copy_ts_from_selector_to_associate (expr1, expr2, true);

  sym->attr.flavor = FL_VARIABLE;
  sym->attr.referenced = 1;
  sym->attr.class_ok = 1;

  *e1 = expr1;
  *e2 = expr2;
  return 0;
}


/* Push the current selector onto the SELECT TYPE stack.  */

static void
select_type_push (gfc_symbol *sel)
{
  gfc_select_type_stack *top = gfc_get_select_type_stack ();
  top->selector = sel;
  top->tmp = NULL;
  top->prev = select_type_stack;

  select_type_stack = top;
}


/* Set the temporary for the current intrinsic SELECT TYPE selector.  */

static gfc_symtree *
select_intrinsic_set_tmp (gfc_typespec *ts, const char *var_name)
{
  /* Keep size in sync with the buffer size in resolve_select_type as it
     determines the final name through truncation.  */
  char name[GFC_MAX_SYMBOL_LEN + 12 + 1];
  gfc_symtree *tmp;
  HOST_WIDE_INT charlen = 0;
  gfc_symbol *selector = select_type_stack->selector;
  gfc_symbol *sym;

  if (ts->type == BT_CLASS || ts->type == BT_DERIVED)
    return NULL;

  if (selector->ts.type == BT_CLASS && !selector->attr.class_ok)
    return NULL;

  /* Case value == NULL corresponds to SELECT TYPE cases otherwise
     the values correspond to SELECT rank cases.  */
  if (ts->type == BT_CHARACTER && ts->u.cl && ts->u.cl->length
      && ts->u.cl->length->expr_type == EXPR_CONSTANT)
    charlen = gfc_mpz_get_hwi (ts->u.cl->length->value.integer);

  if (ts->type != BT_CHARACTER)
    snprintf (name, sizeof (name), "__tmp_%s_%d_%s",
	      gfc_basic_typename (ts->type), ts->kind, var_name);
  else
    snprintf (name, sizeof (name),
	      "__tmp_%s_" HOST_WIDE_INT_PRINT_DEC "_%d_%s",
	      gfc_basic_typename (ts->type), charlen, ts->kind, var_name);

  gfc_get_sym_tree (name, gfc_current_ns, &tmp, false);
  sym = tmp->n.sym;
  gfc_add_type (sym, ts, NULL);

  /* Copy across the array spec to the selector.  */
  if (selector->ts.type == BT_CLASS
      && (CLASS_DATA (selector)->attr.dimension
	  || CLASS_DATA (selector)->attr.codimension))
    {
      sym->attr.pointer = 1;
      sym->attr.dimension = CLASS_DATA (selector)->attr.dimension;
      sym->attr.codimension = CLASS_DATA (selector)->attr.codimension;
      sym->as = gfc_copy_array_spec (CLASS_DATA (selector)->as);
    }

  gfc_set_sym_referenced (sym);
  gfc_add_flavor (&sym->attr, FL_VARIABLE, name, NULL);
  sym->attr.select_type_temporary = 1;

  return tmp;
}


/* Set up a temporary for the current TYPE IS / CLASS IS branch .  */

static void
select_type_set_tmp (gfc_typespec *ts)
{
  char name[GFC_MAX_SYMBOL_LEN + 12 + 1];
  gfc_symtree *tmp = NULL;
  gfc_symbol *selector = select_type_stack->selector;
  gfc_symbol *sym;
  gfc_expr *expr2;

  if (!ts)
    {
      select_type_stack->tmp = NULL;
      return;
    }

  gfc_expr *select_type_expr = gfc_state_stack->construct->expr1;
  const char *var_name = gfc_var_name_for_select_type_temp (select_type_expr);
  tmp = select_intrinsic_set_tmp (ts, var_name);

  if (tmp == NULL)
    {
      if (!ts->u.derived)
	return;

      if (ts->type == BT_CLASS)
	snprintf (name, sizeof (name), "__tmp_class_%s_%s", ts->u.derived->name,
		  var_name);
      else
	snprintf (name, sizeof (name), "__tmp_type_%s_%s", ts->u.derived->name,
		  var_name);

      gfc_get_sym_tree (name, gfc_current_ns, &tmp, false);
      sym = tmp->n.sym;
      gfc_add_type (sym, ts, NULL);

      /* If the SELECT TYPE selector is a function we might be able to obtain
	 a typespec from the result. Since the function might not have been
	 parsed yet we have to check that there is indeed a result symbol.  */
      if (selector->ts.type == BT_UNKNOWN
	  && gfc_state_stack->construct

	  && (expr2 = gfc_state_stack->construct->expr2)
	  && expr2->expr_type == EXPR_FUNCTION
	  && expr2->symtree
	  && expr2->symtree->n.sym && expr2->symtree->n.sym->result)
	selector->ts = expr2->symtree->n.sym->result->ts;

      if (selector->ts.type == BT_CLASS
	  && selector->attr.class_ok
	  && selector->ts.u.derived && CLASS_DATA (selector))
	{
	  sym->attr.pointer
		= CLASS_DATA (selector)->attr.class_pointer;

	  /* Copy across the array spec to the selector.  */
	  if (CLASS_DATA (selector)->attr.dimension
	      || CLASS_DATA (selector)->attr.codimension)
	    {
	      sym->attr.dimension
		    = CLASS_DATA (selector)->attr.dimension;
	      sym->attr.codimension
		    = CLASS_DATA (selector)->attr.codimension;
	      if (CLASS_DATA (selector)->as->type != AS_EXPLICIT)
		sym->as = gfc_copy_array_spec (CLASS_DATA (selector)->as);
	      else
		{
		  sym->as = gfc_get_array_spec();
		  sym->as->rank = CLASS_DATA (selector)->as->rank;
		  sym->as->type = AS_DEFERRED;
		}
	    }
	}

      gfc_set_sym_referenced (sym);
      gfc_add_flavor (&sym->attr, FL_VARIABLE, name, NULL);
      sym->attr.select_type_temporary = 1;

      if (ts->type == BT_CLASS)
	gfc_build_class_symbol (&sym->ts, &sym->attr, &sym->as);
    }
  else
    sym = tmp->n.sym;


  /* Add an association for it, so the rest of the parser knows it is
     an associate-name.  The target will be set during resolution.  */
  sym->assoc = gfc_get_association_list ();
  sym->assoc->dangling = 1;
  sym->assoc->st = tmp;

  select_type_stack->tmp = tmp;
}


/* Match a SELECT TYPE statement.  */

match
gfc_match_select_type (void)
{
  gfc_expr *expr1, *expr2 = NULL;
  match m;
  char name[GFC_MAX_SYMBOL_LEN + 1];
  bool class_array;
  gfc_namespace *ns = gfc_current_ns;

  m = gfc_match_label ();
  if (m == MATCH_ERROR)
    return m;

  m = gfc_match (" select type ( ");
  if (m != MATCH_YES)
    return m;

  if (gfc_current_state() == COMP_MODULE
      || gfc_current_state() == COMP_SUBMODULE)
    {
      gfc_error ("SELECT TYPE at %C cannot appear in this scope");
      return MATCH_ERROR;
    }

  gfc_current_ns = gfc_build_block_ns (ns);
  m = gfc_match (" %n => %e", name, &expr2);
  if (m == MATCH_YES)
    {
      if (build_associate_name (name, &expr1, &expr2))
	{
	  m = MATCH_ERROR;
	  goto cleanup;
	}
    }
  else
    {
      m = gfc_match (" %e ", &expr1);
      if (m != MATCH_YES)
	{
	  std::swap (ns, gfc_current_ns);
	  gfc_free_namespace (ns);
	  return m;
	}
    }

  m = gfc_match (" )%t");
  if (m != MATCH_YES)
    {
      gfc_error ("parse error in SELECT TYPE statement at %C");
      goto cleanup;
    }

  /* This ghastly expression seems to be needed to distinguish a CLASS
     array, which can have a reference, from other expressions that
     have references, such as derived type components, and are not
     allowed by the standard.
     TODO: see if it is sufficient to exclude component and substring
     references.  */
  class_array = (expr1->expr_type == EXPR_VARIABLE
		 && expr1->ts.type == BT_CLASS
		 && CLASS_DATA (expr1)
		 && (strcmp (CLASS_DATA (expr1)->name, "_data") == 0)
		 && (CLASS_DATA (expr1)->attr.dimension
		     || CLASS_DATA (expr1)->attr.codimension)
		 && expr1->ref
		 && expr1->ref->type == REF_ARRAY
		 && expr1->ref->u.ar.type == AR_FULL
		 && expr1->ref->next == NULL);

  /* Check for F03:C811 (F08:C835).  */
  if (!expr2 && (expr1->expr_type != EXPR_VARIABLE
		 || (!class_array && expr1->ref != NULL)))
    {
      gfc_error ("Selector in SELECT TYPE at %C is not a named variable; "
		 "use associate-name=>");
      m = MATCH_ERROR;
      goto cleanup;
    }

  /* Prevent an existing associate name from reuse here by pushing expr1 to
     expr2 and building a new associate name.  */
  if (!expr2 && expr1->symtree->n.sym->assoc
      && !expr1->symtree->n.sym->attr.select_type_temporary
      && !expr1->symtree->n.sym->attr.select_rank_temporary
      && build_associate_name (expr1->symtree->n.sym->name, &expr1, &expr2))
    {
      m = MATCH_ERROR;
      goto cleanup;
    }

  /* Select type namespaces are not filled until resolution. Therefore, the
     namespace must be marked as having an inferred type associate name if
     either expr1 is an inferred type variable or expr2 is. In the latter
     case, as well as the symbol being marked as inferred type, it might be
     that it has not been detected to be so. In this case the target has
     unknown type. Once the namespace is marked, the fixups in resolution can
     be triggered.  */
  if (!expr2
      && expr1->symtree->n.sym->assoc
      && expr1->symtree->n.sym->assoc->inferred_type)
    gfc_current_ns->assoc_name_inferred = 1;
  else if (expr2 && expr2->expr_type == EXPR_VARIABLE
	   && expr2->symtree->n.sym->assoc)
    {
      if (expr2->symtree->n.sym->assoc->inferred_type)
	gfc_current_ns->assoc_name_inferred = 1;
      else if (expr2->symtree->n.sym->assoc->target
	       && expr2->symtree->n.sym->assoc->target->ts.type == BT_UNKNOWN)
	gfc_current_ns->assoc_name_inferred = 1;
    }

  new_st.op = EXEC_SELECT_TYPE;
  new_st.expr1 = expr1;
  new_st.expr2 = expr2;
  new_st.ext.block.ns = gfc_current_ns;

  select_type_push (expr1->symtree->n.sym);
  gfc_current_ns = ns;

  return MATCH_YES;

cleanup:
  gfc_free_expr (expr1);
  gfc_free_expr (expr2);
  gfc_undo_symbols ();
  std::swap (ns, gfc_current_ns);
  gfc_free_namespace (ns);
  return m;
}


/* Set the temporary for the current intrinsic SELECT RANK selector.  */

static void
select_rank_set_tmp (gfc_typespec *ts, int *case_value)
{
  char name[2 * GFC_MAX_SYMBOL_LEN];
  char tname[GFC_MAX_SYMBOL_LEN + 7];
  gfc_symtree *tmp;
  gfc_symbol *selector = select_type_stack->selector;
  gfc_symbol *sym;
  gfc_symtree *st;
  HOST_WIDE_INT charlen = 0;

  if (case_value == NULL)
    return;

  if (ts->type == BT_CHARACTER && ts->u.cl && ts->u.cl->length
      && ts->u.cl->length->expr_type == EXPR_CONSTANT)
    charlen = gfc_mpz_get_hwi (ts->u.cl->length->value.integer);

  if (ts->type == BT_CLASS)
    sprintf (tname, "class_%s", ts->u.derived->name);
  else if (ts->type == BT_DERIVED)
    sprintf (tname, "type_%s", ts->u.derived->name);
  else if (ts->type != BT_CHARACTER)
    sprintf (tname, "%s_%d", gfc_basic_typename (ts->type), ts->kind);
  else
    sprintf (tname, "%s_" HOST_WIDE_INT_PRINT_DEC "_%d",
	     gfc_basic_typename (ts->type), charlen, ts->kind);

  /* Case value == NULL corresponds to SELECT TYPE cases otherwise
     the values correspond to SELECT rank cases.  */
  if (*case_value >=0)
    sprintf (name, "__tmp_%s_rank_%d", tname, *case_value);
  else
    sprintf (name, "__tmp_%s_rank_m%d", tname, -*case_value);

  gfc_find_sym_tree (name, gfc_current_ns, 0, &st);
  if (st)
    return;

  gfc_get_sym_tree (name, gfc_current_ns, &tmp, false);
  sym = tmp->n.sym;
  gfc_add_type (sym, ts, NULL);

  /* Copy across the array spec to the selector.  */
  if (selector->ts.type == BT_CLASS)
    {
      sym->ts.u.derived = CLASS_DATA (selector)->ts.u.derived;
      sym->attr.pointer = CLASS_DATA (selector)->attr.pointer;
      sym->attr.allocatable = CLASS_DATA (selector)->attr.allocatable;
      sym->attr.target = CLASS_DATA (selector)->attr.target;
      sym->attr.class_ok = 0;
      if (case_value && *case_value != 0)
	{
	  sym->attr.dimension = 1;
	  sym->as = gfc_copy_array_spec (CLASS_DATA (selector)->as);
	  if (*case_value > 0)
	    {
	      sym->as->type = AS_DEFERRED;
	      sym->as->rank = *case_value;
	    }
	  else if (*case_value == -1)
	    {
	      sym->as->type = AS_ASSUMED_SIZE;
	      sym->as->rank = 1;
	    }
	}
    }
  else
    {
      sym->attr.pointer = selector->attr.pointer;
      sym->attr.allocatable = selector->attr.allocatable;
      sym->attr.target = selector->attr.target;
      if (case_value && *case_value != 0)
	{
	  sym->attr.dimension = 1;
	  sym->as = gfc_copy_array_spec (selector->as);
	  if (*case_value > 0)
	    {
	      sym->as->type = AS_DEFERRED;
	      sym->as->rank = *case_value;
	    }
	  else if (*case_value == -1)
	    {
	      sym->as->type = AS_ASSUMED_SIZE;
	      sym->as->rank = 1;
	    }
	}
    }

  gfc_set_sym_referenced (sym);
  gfc_add_flavor (&sym->attr, FL_VARIABLE, name, NULL);
  sym->attr.select_type_temporary = 1;
  if (case_value)
    sym->attr.select_rank_temporary = 1;

  if (ts->type == BT_CLASS)
    gfc_build_class_symbol (&sym->ts, &sym->attr, &sym->as);

  /* Add an association for it, so the rest of the parser knows it is
     an associate-name.  The target will be set during resolution.  */
  sym->assoc = gfc_get_association_list ();
  sym->assoc->dangling = 1;
  sym->assoc->st = tmp;

  select_type_stack->tmp = tmp;
}


/* Match a SELECT RANK statement.  */

match
gfc_match_select_rank (void)
{
  gfc_expr *expr1, *expr2 = NULL;
  match m;
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_symbol *sym, *sym2;
  gfc_namespace *ns = gfc_current_ns;
  gfc_array_spec *as = NULL;

  m = gfc_match_label ();
  if (m == MATCH_ERROR)
    return m;

  m = gfc_match (" select% rank ( ");
  if (m != MATCH_YES)
    return m;

  if (!gfc_notify_std (GFC_STD_F2018, "SELECT RANK statement at %C"))
    return MATCH_NO;

  gfc_current_ns = gfc_build_block_ns (ns);
  m = gfc_match (" %n => %e", name, &expr2);

  if (m == MATCH_YES)
    {
      /* If expr2 corresponds to an implicitly typed variable, then the
	 actual type of the variable may not have been set.  Set it here.  */
      if (!gfc_current_ns->seen_implicit_none
	  && expr2->expr_type == EXPR_VARIABLE
	  && expr2->ts.type == BT_UNKNOWN
	  && expr2->symtree && expr2->symtree->n.sym)
	{
	  gfc_set_default_type (expr2->symtree->n.sym, 0, gfc_current_ns);
	  expr2->ts.type = expr2->symtree->n.sym->ts.type;
	}

      expr1 = gfc_get_expr ();
      expr1->expr_type = EXPR_VARIABLE;
      expr1->where = expr2->where;
      expr1->ref = gfc_copy_ref (expr2->ref);
      if (gfc_get_sym_tree (name, NULL, &expr1->symtree, false))
	{
	  m = MATCH_ERROR;
	  goto cleanup;
	}

      sym = expr1->symtree->n.sym;

      if (expr2->symtree)
	{
	  sym2 = expr2->symtree->n.sym;
	  as = (sym2->ts.type == BT_CLASS
		&& CLASS_DATA (sym2)) ? CLASS_DATA (sym2)->as : sym2->as;
	}

      if (expr2->expr_type != EXPR_VARIABLE
	  || !(as && as->type == AS_ASSUMED_RANK))
	{
	  gfc_error ("The SELECT RANK selector at %C must be an assumed "
		     "rank variable");
	  m = MATCH_ERROR;
	  goto cleanup;
	}

      if (expr2->ts.type == BT_CLASS && CLASS_DATA (sym2))
	{
	  copy_ts_from_selector_to_associate (expr1, expr2);

	  sym->attr.flavor = FL_VARIABLE;
	  sym->attr.referenced = 1;
	  sym->attr.class_ok = 1;
	  CLASS_DATA (sym)->attr.allocatable = CLASS_DATA (sym2)->attr.allocatable;
	  CLASS_DATA (sym)->attr.pointer = CLASS_DATA (sym2)->attr.pointer;
	  CLASS_DATA (sym)->attr.target = CLASS_DATA (sym2)->attr.target;
	  sym->attr.pointer = 1;
	}
      else
	{
	  sym->ts = sym2->ts;
	  sym->as = gfc_copy_array_spec (sym2->as);
	  sym->attr.dimension = 1;

	  sym->attr.flavor = FL_VARIABLE;
	  sym->attr.referenced = 1;
	  sym->attr.class_ok = sym2->attr.class_ok;
	  sym->attr.allocatable = sym2->attr.allocatable;
	  sym->attr.pointer = sym2->attr.pointer;
	  sym->attr.target = sym2->attr.target;
	}
    }
  else
    {
      m = gfc_match (" %e ", &expr1);

      if (m != MATCH_YES)
	{
	  gfc_undo_symbols ();
	  std::swap (ns, gfc_current_ns);
	  gfc_free_namespace (ns);
	  return m;
	}

      if (expr1->symtree)
	{
	  sym = expr1->symtree->n.sym;
	  as = (sym->ts.type == BT_CLASS
		&& CLASS_DATA (sym)) ? CLASS_DATA (sym)->as : sym->as;
	}

      if (expr1->expr_type != EXPR_VARIABLE
	  || !(as && as->type == AS_ASSUMED_RANK))
	{
	  gfc_error("The SELECT RANK selector at %C must be an assumed "
		    "rank variable");
	  m = MATCH_ERROR;
	  goto cleanup;
	}
    }

  m = gfc_match (" )%t");
  if (m != MATCH_YES)
    {
      gfc_error ("parse error in SELECT RANK statement at %C");
      goto cleanup;
    }

  new_st.op = EXEC_SELECT_RANK;
  new_st.expr1 = expr1;
  new_st.expr2 = expr2;
  new_st.ext.block.ns = gfc_current_ns;

  select_type_push (expr1->symtree->n.sym);
  gfc_current_ns = ns;

  return MATCH_YES;

cleanup:
  gfc_free_expr (expr1);
  gfc_free_expr (expr2);
  gfc_undo_symbols ();
  std::swap (ns, gfc_current_ns);
  gfc_free_namespace (ns);
  return m;
}


/* Match a CASE statement.  */

match
gfc_match_case (void)
{
  gfc_case *c, *head, *tail;
  match m;

  head = tail = NULL;

  if (gfc_current_state () != COMP_SELECT)
    {
      gfc_error ("Unexpected CASE statement at %C");
      return MATCH_ERROR;
    }

  if (gfc_match ("% default") == MATCH_YES)
    {
      m = match_case_eos ();
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      new_st.op = EXEC_SELECT;
      c = gfc_get_case ();
      c->where = gfc_current_locus;
      new_st.ext.block.case_list = c;
      return MATCH_YES;
    }

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  for (;;)
    {
      if (match_case_selector (&c) == MATCH_ERROR)
	goto cleanup;

      if (head == NULL)
	head = c;
      else
	tail->next = c;

      tail = c;

      if (gfc_match_char (')') == MATCH_YES)
	break;
      if (gfc_match_char (',') != MATCH_YES)
	goto syntax;
    }

  m = match_case_eos ();
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  new_st.op = EXEC_SELECT;
  new_st.ext.block.case_list = head;

  return MATCH_YES;

syntax:
  gfc_error ("Syntax error in CASE specification at %C");

cleanup:
  gfc_free_case_list (head);  /* new_st is cleaned up in parse.cc.  */
  return MATCH_ERROR;
}


/* Match a TYPE IS statement.  */

match
gfc_match_type_is (void)
{
  gfc_case *c = NULL;
  match m;

  if (gfc_current_state () != COMP_SELECT_TYPE)
    {
      gfc_error ("Unexpected TYPE IS statement at %C");
      return MATCH_ERROR;
    }

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  c = gfc_get_case ();
  c->where = gfc_current_locus;

  m = gfc_match_type_spec (&c->ts);
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (gfc_match_char (')') != MATCH_YES)
    goto syntax;

  m = match_case_eos ();
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  new_st.op = EXEC_SELECT_TYPE;
  new_st.ext.block.case_list = c;

  if (c->ts.type == BT_DERIVED && c->ts.u.derived
      && (c->ts.u.derived->attr.sequence
	  || c->ts.u.derived->attr.is_bind_c))
    {
      gfc_error ("The type-spec shall not specify a sequence derived "
		 "type or a type with the BIND attribute in SELECT "
		 "TYPE at %C [F2003:C815]");
      return MATCH_ERROR;
    }

  if (c->ts.type == BT_DERIVED
      && c->ts.u.derived && c->ts.u.derived->attr.pdt_type
      && gfc_spec_list_type (type_param_spec_list, c->ts.u.derived)
							!= SPEC_ASSUMED)
    {
      gfc_error ("All the LEN type parameters in the TYPE IS statement "
		 "at %C must be ASSUMED");
      return MATCH_ERROR;
    }

  /* Create temporary variable.  */
  select_type_set_tmp (&c->ts);

  return MATCH_YES;

syntax:
  gfc_error ("Syntax error in TYPE IS specification at %C");

cleanup:
  if (c != NULL)
    gfc_free_case_list (c);  /* new_st is cleaned up in parse.cc.  */
  return MATCH_ERROR;
}


/* Match a CLASS IS or CLASS DEFAULT statement.  */

match
gfc_match_class_is (void)
{
  gfc_case *c = NULL;
  match m;

  if (gfc_current_state () != COMP_SELECT_TYPE)
    return MATCH_NO;

  if (gfc_match ("% default") == MATCH_YES)
    {
      m = match_case_eos ();
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      new_st.op = EXEC_SELECT_TYPE;
      c = gfc_get_case ();
      c->where = gfc_current_locus;
      c->ts.type = BT_UNKNOWN;
      new_st.ext.block.case_list = c;
      select_type_set_tmp (NULL);
      return MATCH_YES;
    }

  m = gfc_match ("% is");
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  c = gfc_get_case ();
  c->where = gfc_current_locus;

  m = match_derived_type_spec (&c->ts);
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (c->ts.type == BT_DERIVED)
    c->ts.type = BT_CLASS;

  if (gfc_match_char (')') != MATCH_YES)
    goto syntax;

  m = match_case_eos ();
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  new_st.op = EXEC_SELECT_TYPE;
  new_st.ext.block.case_list = c;

  /* Create temporary variable.  */
  select_type_set_tmp (&c->ts);

  return MATCH_YES;

syntax:
  gfc_error ("Syntax error in CLASS IS specification at %C");

cleanup:
  if (c != NULL)
    gfc_free_case_list (c);  /* new_st is cleaned up in parse.cc.  */
  return MATCH_ERROR;
}


/* Match a RANK statement.  */

match
gfc_match_rank_is (void)
{
  gfc_case *c = NULL;
  match m;
  int case_value;

  if (gfc_current_state () != COMP_SELECT_RANK)
    {
      gfc_error ("Unexpected RANK statement at %C");
      return MATCH_ERROR;
    }

  if (gfc_match ("% default") == MATCH_YES)
    {
      m = match_case_eos ();
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      new_st.op = EXEC_SELECT_RANK;
      c = gfc_get_case ();
      c->ts.type = BT_UNKNOWN;
      c->where = gfc_current_locus;
      new_st.ext.block.case_list = c;
      select_type_stack->tmp = NULL;
      return MATCH_YES;
    }

  if (gfc_match_char ('(') != MATCH_YES)
    goto syntax;

  c = gfc_get_case ();
  c->where = gfc_current_locus;
  c->ts = select_type_stack->selector->ts;

  m = gfc_match_expr (&c->low);
  if (m == MATCH_NO)
    {
      if (gfc_match_char ('*') == MATCH_YES)
	c->low = gfc_get_int_expr (gfc_default_integer_kind,
				   NULL, -1);
      else
	goto syntax;

      case_value = -1;
    }
  else if (m == MATCH_YES)
    {
      /* F2018: R1150  */
      if (c->low->expr_type != EXPR_CONSTANT
	  || c->low->ts.type != BT_INTEGER
	  || c->low->rank)
	{
	  gfc_error ("The SELECT RANK CASE expression at %C must be a "
		     "scalar, integer constant");
	  goto cleanup;
	}

      case_value = (int) mpz_get_si (c->low->value.integer);
      /* F2018: C1151  */
      if ((case_value < 0) || (case_value > GFC_MAX_DIMENSIONS))
	{
	  gfc_error ("The value of the SELECT RANK CASE expression at "
		     "%C must not be less than zero or greater than %d",
		     GFC_MAX_DIMENSIONS);
	  goto cleanup;
	}
    }
  else
    goto cleanup;

  if (gfc_match_char (')') != MATCH_YES)
    goto syntax;

  m = match_case_eos ();
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  new_st.op = EXEC_SELECT_RANK;
  new_st.ext.block.case_list = c;

  /* Create temporary variable. Recycle the select type code.  */
  select_rank_set_tmp (&c->ts, &case_value);

  return MATCH_YES;

syntax:
  gfc_error ("Syntax error in RANK specification at %C");

cleanup:
  if (c != NULL)
    gfc_free_case_list (c);  /* new_st is cleaned up in parse.cc.  */
  return MATCH_ERROR;
}

/********************* WHERE subroutines ********************/

/* Match the rest of a simple WHERE statement that follows an IF statement.
 */

static match
match_simple_where (void)
{
  gfc_expr *expr;
  gfc_code *c;
  match m;

  m = gfc_match (" ( %e )", &expr);
  if (m != MATCH_YES)
    return m;

  m = gfc_match_assignment ();
  if (m == MATCH_NO)
    goto syntax;
  if (m == MATCH_ERROR)
    goto cleanup;

  if (gfc_match_eos () != MATCH_YES)
    goto syntax;

  c = gfc_get_code (EXEC_WHERE);
  c->expr1 = expr;

  c->next = XCNEW (gfc_code);
  *c->next = new_st;
  c->next->loc = gfc_current_locus;
  gfc_clear_new_st ();

  new_st.op = EXEC_WHERE;
  new_st.block = c;

  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_WHERE);

cleanup:
  gfc_free_expr (expr);
  return MATCH_ERROR;
}


/* Match a WHERE statement.  */

match
gfc_match_where (gfc_statement *st)
{
  gfc_expr *expr;
  match m0, m;
  gfc_code *c;

  m0 = gfc_match_label ();
  if (m0 == MATCH_ERROR)
    return m0;

  m = gfc_match (" where ( %e )", &expr);
  if (m != MATCH_YES)
    return m;

  if (gfc_match_eos () == MATCH_YES)
    {
      *st = ST_WHERE_BLOCK;
      new_st.op = EXEC_WHERE;
      new_st.expr1 = expr;
      return MATCH_YES;
    }

  m = gfc_match_assignment ();
  if (m == MATCH_NO)
    gfc_syntax_error (ST_WHERE);

  if (m != MATCH_YES)
    {
      gfc_free_expr (expr);
      return MATCH_ERROR;
    }

  /* We've got a simple WHERE statement.  */
  *st = ST_WHERE;
  c = gfc_get_code (EXEC_WHERE);
  c->expr1 = expr;

  /* Put in the assignment.  It will not be processed by add_statement, so we
     need to copy the location here. */

  c->next = XCNEW (gfc_code);
  *c->next = new_st;
  c->next->loc = gfc_current_locus;
  gfc_clear_new_st ();

  new_st.op = EXEC_WHERE;
  new_st.block = c;

  return MATCH_YES;
}


/* Match an ELSEWHERE statement.  We leave behind a WHERE node in
   new_st if successful.  */

match
gfc_match_elsewhere (void)
{
  char name[GFC_MAX_SYMBOL_LEN + 1];
  gfc_expr *expr;
  match m;

  if (gfc_current_state () != COMP_WHERE)
    {
      gfc_error ("ELSEWHERE statement at %C not enclosed in WHERE block");
      return MATCH_ERROR;
    }

  expr = NULL;

  if (gfc_match_char ('(') == MATCH_YES)
    {
      m = gfc_match_expr (&expr);
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	return MATCH_ERROR;

      if (gfc_match_char (')') != MATCH_YES)
	goto syntax;
    }

  if (gfc_match_eos () != MATCH_YES)
    {
      /* Only makes sense if we have a where-construct-name.  */
      if (!gfc_current_block ())
	{
	  m = MATCH_ERROR;
	  goto cleanup;
	}
      /* Better be a name at this point.  */
      m = gfc_match_name (name);
      if (m == MATCH_NO)
	goto syntax;
      if (m == MATCH_ERROR)
	goto cleanup;

      if (gfc_match_eos () != MATCH_YES)
	goto syntax;

      if (strcmp (name, gfc_current_block ()->name) != 0)
	{
	  gfc_error ("Label %qs at %C doesn't match WHERE label %qs",
		     name, gfc_current_block ()->name);
	  goto cleanup;
	}
    }

  new_st.op = EXEC_WHERE;
  new_st.expr1 = expr;
  return MATCH_YES;

syntax:
  gfc_syntax_error (ST_ELSEWHERE);

cleanup:
  gfc_free_expr (expr);
  return MATCH_ERROR;
}
