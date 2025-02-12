/*
MIT License

Copyright(c) 2022 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/print.h"
#include "nodes/relation.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

#include "access/k2/k2_plan.h"
#include "access/k2/pg_gate_api.h"
#include "access/k2/k2pg_aux.h"

/*
 * Theoretically, any expression that evaluates to a constant before K2PG
 * execution.
 * Currently restrict this to a subset of expressions.
 * Note: As enhance pushdown support in K2 PG (e.g. expr evaluation) this
 * can be expanded.
 */
bool K2PgIsSupportedSingleRowModifyWhereExpr(Expr *expr)
{
	switch (nodeTag(expr))
	{
		case T_Const:
			return true;
		case T_Param:
		{
			/* Bind variables. */
			Param *param = castNode(Param, expr);
			return param->paramkind == PARAM_EXTERN;
		}
		case T_RelabelType:
		{
			/*
			 * RelabelType is a "dummy" type coercion between two binary-
			 * compatible datatypes so we just recurse into its argument.
			 */
			RelabelType *rt = castNode(RelabelType, expr);
			return K2PgIsSupportedSingleRowModifyWhereExpr(rt->arg);
		}
		case T_FuncExpr:
		case T_OpExpr:
		{
			List         *args = NULL;
			ListCell     *lc = NULL;
			Oid          funcid = InvalidOid;
			HeapTuple    tuple = NULL;

			/* Get the function info. */
			if (IsA(expr, FuncExpr))
			{
				FuncExpr *func_expr = castNode(FuncExpr, expr);
				args = func_expr->args;
				funcid = func_expr->funcid;
			}
			else if (IsA(expr, OpExpr))
			{
				OpExpr *op_expr = castNode(OpExpr, expr);
				args = op_expr->args;
				funcid = op_expr->opfuncid;
			}

			/*
			 * Only allow immutable functions as they cannot modify the
			 * database or do lookups.
			 */
			tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);
			char provolatile = ((Form_pg_proc) GETSTRUCT(tuple))->provolatile;
			ReleaseSysCache(tuple);
			if (provolatile != PROVOLATILE_IMMUTABLE)
			{
				return false;
			}

			/* Checking all arguments are valid (stable). */
			foreach (lc, args) {
				Expr* expr = (Expr *) lfirst(lc);
				if (!K2PgIsSupportedSingleRowModifyWhereExpr(expr)) {
					return false;
				}
			}
			return true;
		}
		default:
			break;
	}

	return false;
}

static void K2PgExprInstantiateParamsInternal(Expr* expr,
                                             ParamListInfo paramLI,
                                             Expr** parent_var)
{
	switch (nodeTag(expr))
	{
		case T_Const:
			return;
		case T_Var:
			return;
		case T_Param:
		{
			/* Bind variables. */
			Param *param = castNode(Param, expr);

			// TODO: check if this logic is correct
			ParamExternData *prm = &paramLI->params[param->paramid - 1];
			/* give hook a chance in case parameter is dynamic */
			if (!OidIsValid(prm->ptype) && paramLI->paramFetch != NULL) {
				(*paramLI->paramFetch)(paramLI, param->paramid);
			}

			if (!OidIsValid(prm->ptype) ||
				prm->ptype != param->paramtype ||
				!(prm->pflags & PARAM_FLAG_CONST))
			{
				/* Planner should ensure this does not happen */
				elog(ERROR, "Invalid parameter: %s", nodeToString(param));
			}
			int16		typLen = 0;
			bool		typByVal = false;
			Datum		pval = 0;

			get_typlenbyval(param->paramtype,
							&typLen, &typByVal);
			if (prm->isnull || typByVal)
				pval = prm->value;
			else
				pval = datumCopy(prm->value, typByVal, typLen);

			Expr *const_expr = (Expr *) makeConst(param->paramtype,
										param->paramtypmod,
										param->paramcollid,
										(int) typLen,
										pval,
										prm->isnull,
										typByVal);

			*parent_var = const_expr;
			return;
		}
		case T_RelabelType:
		{
			/*
			 * RelabelType is a "dummy" type coercion between two binary-
			 * compatible datatypes so we just recurse into its argument.
			 */
			RelabelType *rt = castNode(RelabelType, expr);
			K2PgExprInstantiateParamsInternal(rt->arg, paramLI, &rt->arg);
			return;
		}
		case T_FuncExpr:
		{
			FuncExpr *func_expr = castNode(FuncExpr, expr);
			ListCell *lc = NULL;
			foreach(lc, func_expr->args)
			{
				Expr *arg = (Expr *) lfirst(lc);
				K2PgExprInstantiateParamsInternal(arg,
				                                 paramLI,
				                                 (Expr **)&lc->data.ptr_value);
			}
			return;
		}
		case T_OpExpr:
		{
			OpExpr   *op_expr = castNode(OpExpr, expr);
			ListCell *lc = NULL;
			foreach(lc, op_expr->args)
			{
				Expr *arg = (Expr *) lfirst(lc);
				K2PgExprInstantiateParamsInternal(arg,
				                                 paramLI,
				                                 (Expr **)&lc->data.ptr_value);
			}
			return;
		}
		default:
			break;
	}

	/* Planner should ensure this does not happen */
	elog(ERROR, "Invalid expression: %s", nodeToString(expr));
}


void K2PgExprInstantiateParams(Expr* expr, ParamListInfo paramLI)
{
	/* Fast-path if there are no params. */
	if (paramLI == NULL)
		return;

	K2PgExprInstantiateParamsInternal(expr, paramLI, NULL);
}

/*
 * Check if the function/procedure can be executed by K2 Platform (i.e. if we can
 * pushdown its execution).
 * This is for future function support in K2 platform
 */
static bool IsSupportedK2FunctionId(Oid funcid, Form_pg_proc pg_proc) {
	// Will add logic to enable the support to push down functions once
	// K2 platform implements the function support.
	//
	// See
	//   https://github.com/futurewei-cloud/chogori-platform/issues/137
	//
	return false;
}

static bool K2PgAnalyzeExpression(Expr *expr, AttrNumber target_attnum, bool *has_vars, bool *has_k2_unsupported_funcs) {
	switch (nodeTag(expr))
	{
		case T_Const:
			return true;
		case T_Var:
		{
			/* References to table attrs (to be read) */
			Var *var = castNode(Var, expr);
			*has_vars = true;
			return var->varattno == target_attnum;
		}
		case T_Param:
		{
			/* Bind variables. */
			Param *param = castNode(Param, expr);
			return param->paramkind == PARAM_EXTERN;
		}
		case T_RelabelType:
		{
			/*
			 * RelabelType is a "dummy" type coercion between two binary-
			 * compatible datatypes so we just recurse into its argument.
			 */
			RelabelType *rt = castNode(RelabelType, expr);
			return K2PgAnalyzeExpression(rt->arg, target_attnum, has_vars, has_k2_unsupported_funcs);
		}
		case T_FuncExpr:
		case T_OpExpr:
		{
			List         *args = NULL;
			ListCell     *lc = NULL;
			Oid          funcid = InvalidOid;
			HeapTuple    tuple = NULL;

			/* Get the function info. */
			if (IsA(expr, FuncExpr))
			{
				FuncExpr *func_expr = castNode(FuncExpr, expr);
				args = func_expr->args;
				funcid = func_expr->funcid;
			}
			else if (IsA(expr, OpExpr))
			{
				OpExpr *op_expr = castNode(OpExpr, expr);
				args = op_expr->args;
				funcid = op_expr->opfuncid;
			}

			/*
			 * Only allow immutable functions as they cannot modify the
			 * database or do lookups.
			 */
			tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);
			Form_pg_proc pg_proc = ((Form_pg_proc) GETSTRUCT(tuple));

			if (pg_proc->provolatile != PROVOLATILE_IMMUTABLE)
			{
				ReleaseSysCache(tuple);
				return false;
			}

			if (!IsSupportedK2FunctionId(funcid, pg_proc)) {
				*has_k2_unsupported_funcs = true;
			}
			ReleaseSysCache(tuple);

			/* Checking all arguments are valid (stable). */
			foreach (lc, args) {
				Expr* expr = (Expr *) lfirst(lc);
				if (!K2PgAnalyzeExpression(expr, target_attnum, has_vars, has_k2_unsupported_funcs)) {
				    return false;
				}
			}
			return true;
		}
		default:
			break;
	}

	return false;
}

/*
 * Can expression be evaluated in K2 Platform.
 * Eventually any immutable expression whose only variables are column references.
 * Currently, limit to the case where the only referenced column is the target column.
 */
bool K2PgIsSupportedSingleRowModifyAssignExpr(Expr *expr, AttrNumber target_attnum, bool *needs_pushdown) {
	bool has_vars = false;
	bool has_k2_unsupported_funcs = false;
	bool is_basic_expr = K2PgAnalyzeExpression(expr, target_attnum, &has_vars, &has_k2_unsupported_funcs);

	/* default, will set to true below if needed. */
	*needs_pushdown = false;

	/* Immediately bail for complex expressions */
	if (!is_basic_expr)
		return false;

	/* If there are no variables, we can just evaluate it to a const. */
	if (!has_vars)
	{
		return true;
	}

	/* We can push down expression evaluation to K2 platform. */
	if (has_vars && !has_k2_unsupported_funcs)
	{
		*needs_pushdown = true;
		return true;
	}

	/* Variables plus k2 platform-unsupported funcs -> query layer must evaluate. */
	return false;
}

/*
 * Returns true if the following are all true:
 *  - is insert, update, or delete command.
 *  - only one target table.
 *  - there are no ON CONFLICT or WITH clauses.
 *  - source data is a VALUES clause with one value set.
 *  - all values are either constants or bind markers.
 *
 *  Additionally, during execution we will also check:
 *  - not in transaction block.
 *  - is a single-plan execution.
 *  - target table has no triggers.
 *  - target table has no indexes.
 *  And if all are true we will execute this op as a single-row transaction
 *  rather than a distributed transaction.
 */
static bool ModifyTableIsSingleRowWrite(ModifyTable *modifyTable)
{

	/* Support INSERT, UPDATE, and DELETE. */
	if (modifyTable->operation != CMD_INSERT &&
		modifyTable->operation != CMD_UPDATE &&
		modifyTable->operation != CMD_DELETE)
		return false;

	/* Multi-relation implies multi-shard. */
	if (list_length(modifyTable->resultRelations) != 1)
		return false;

	/* ON CONFLICT clause is not supported here yet. */
//	if (modifyTable->onConflictAction != ONCONFLICT_NONE)
//		return false;

	/* WITH clause is not supported here yet. */
	if (modifyTable->plan.initPlan != NIL)
		return false;

	/* Check the data source, only allow a values clause right now */
	if (list_length(modifyTable->plans) != 1)
		return false;

	switch nodeTag(linitial(modifyTable->plans))
	{
        // TODO: need to consider if we need to change the logic since T_Result node type isn't available here
        //
		// case T_Result:
		// {
		// 	/* Simple values clause: one valueset (single row) */
		// 	Result *values = (Result *)linitial(modifyTable->plans);
		// 	ListCell *lc;
		// 	foreach(lc, values->plan.targetlist)
		// 	{
		// 		TargetEntry *target = (TargetEntry *) lfirst(lc);
		// 		bool needs_pushdown = false;
		// 		if (!K2PgIsSupportedSingleRowModifyAssignExpr(target->expr,
		// 		                                             target->resno,
		// 		                                             &needs_pushdown))
		// 		{
		// 			return false;
		// 		}
		// 	}
		// 	break;
		// }
		case T_ValuesScan:
		{
			/*
			 * Simple values clause: multiple valueset (multi-row).
			 * TODO Eventually we could inspect hash key values to check
			 * if single shard and optimize that.
			 */
			return false;

		}
		default:
			/* Do not support any other data sources. */
			return false;
	}

	/* If all our checks passed return true */
	return true;
}

bool K2PgIsSingleRowModify(PlannedStmt *pstmt)
{
	if (pstmt->planTree && IsA(pstmt->planTree, ModifyTable))
	{
		ModifyTable *node = castNode(ModifyTable, pstmt->planTree);
		return ModifyTableIsSingleRowWrite(node);
	}

	return false;
}

/*
 * Returns true if the following are all true:
 *  - is update or delete command.
 *  - source data is a Result node (meaning we are skipping scan and thus
 *    are single row).
 */
bool K2PgIsSingleRowUpdateOrDelete(ModifyTable *modifyTable)
{
	/* Support UPDATE and DELETE. */
	if (modifyTable->operation != CMD_UPDATE &&
		modifyTable->operation != CMD_DELETE)
		return false;

	/* Should only have one data source. */
	if (list_length(modifyTable->plans) != 1)
		return false;

	/* Verify the single data source is a Result node. */
	// if (!IsA(linitial(modifyTable->plans), Result))
	// 	return false;

	return true;
}

/*
 * Returns true if provided Bitmapset of attribute numbers
 * matches the primary key attribute numbers of the relation.
 */
bool K2PgAllPrimaryKeysProvided(Oid relid, Bitmapset *attrs)
{
	if (bms_is_empty(attrs))
	{
		/*
		 * If we don't explicitly check for empty attributes it is possible
		 * for this function to improperly return true. This is because in the
		 * case where a table does not have any primary key attributes we will
		 * use a hidden RowId column which is not exposed to the PG side, so
		 * both the K2PG primary key attributes and the input attributes would
		 * appear empty and would be equal, even though this is incorrect as
		 * the K2PG table has the hidden RowId primary key column.
		 */
		return false;
	}

	Relation        rel                = RelationIdGetRelation(relid);
	Oid             dboid              = K2PgGetDatabaseOid(rel);
	AttrNumber      natts              = RelationGetNumberOfAttributes(rel);
	K2PgTableDesc  k2pg_tabledesc      = NULL;
	Bitmapset      *primary_key_attrs  = NULL;

	/* Get primary key columns from K2PG table desc. */
	HandleK2PgStatus(PgGate_GetTableDesc(dboid, relid, &k2pg_tabledesc));
	for (AttrNumber attnum = 1; attnum <= natts; attnum++)
	{
		bool is_primary = false;
		HandleK2PgTableDescStatus(PgGate_GetColumnInfo(k2pg_tabledesc,
		                                           attnum,
		                                           &is_primary), k2pg_tabledesc);

		if (is_primary)
		{
			primary_key_attrs = bms_add_member(primary_key_attrs, attnum);
		}
	}

	RelationClose(rel);

	/* Verify the sets are the same. */
	return bms_equal(attrs, primary_key_attrs);
}
