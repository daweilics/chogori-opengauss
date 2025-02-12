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
#include <string.h>
#include "postgres.h"

#include "access/k2/k2catam.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "commands/dbcommands.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/relation.h"
#include "utils/rel.h"
#include "optimizer/cost.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "access/nbtree.h"

#include "access/k2/k2pg_aux.h"

typedef struct CamScanPlanData
{
	/* The relation where to read data from */
	Relation target_relation;

	/* Primary and hash key columns of the referenced table/relation. */
	Bitmapset *primary_key;
	Bitmapset *nonprimary_key;

	/* Set of key columns whose values will be used for scanning. */
	Bitmapset *sk_cols;

	/* Description and attnums of the columns to bind */
	TupleDesc bind_desc;
	AttrNumber bind_key_attnums[K2PG_MAX_SCAN_KEYS];
} CamScanPlanData;

typedef CamScanPlanData *CamScanPlan;

static void camAddAttributeColumn(CamScanPlan scan_plan, AttrNumber attnum)
{
  const int idx = K2PgAttnumToBmsIndex(scan_plan->target_relation, attnum);

  if (bms_is_member(idx, scan_plan->primary_key))
    scan_plan->sk_cols = bms_add_member(scan_plan->sk_cols, idx);
}

/*
 * Checks if an attribute is a hash or primary key column and note it in
 * the scan plan.
 */
static void camCheckPrimaryKeyAttribute(CamScanPlan      scan_plan,
										K2PgTableDesc  k2pg_table_desc,
										AttrNumber      attnum)
{
	bool is_primary = false;

	/*
	 * We shouldn't need to upload the table descriptor here because the structure
	 * Postgres::Relation already has all information.
	 * - Primary key indicator: IndexRelation->rd_index->indisprimary
	 * - Number of key columns: IndexRelation->rd_index->indnkeyatts
	 * - Number of all columns: IndexRelation->rd_index->indnatts
	 * - Range, etc: IndexRelation->rd_indoption (Bits INDOPTION_HASH, RANGE, etc)
	 */
	HandleK2PgTableDescStatus(PgGate_GetColumnInfo(k2pg_table_desc,
											   attnum,
											   &is_primary), k2pg_table_desc);

	int idx = K2PgAttnumToBmsIndex(scan_plan->target_relation, attnum);

	if (is_primary)
	{
		scan_plan->primary_key = bms_add_member(scan_plan->primary_key, idx);
	}
}

/*
 * Get K2PG-specific table metadata and load it into the scan_plan.
 * Currently only the hash and primary key info.
 */
static void camLoadTableInfo(Relation relation, CamScanPlan scan_plan)
{
	Oid            dboid          = K2PgGetDatabaseOid(relation);
	Oid            relid          = RelationGetRelid(relation);
	K2PgTableDesc k2pg_table_desc = NULL;

	HandleK2PgStatus(PgGate_GetTableDesc(dboid, relid, &k2pg_table_desc));

	for (AttrNumber attnum = 1; attnum <= relation->rd_att->natts; attnum++)
	{
		camCheckPrimaryKeyAttribute(scan_plan, k2pg_table_desc, attnum);
	}
	if (relation->rd_rel->relhasoids)
	{
		camCheckPrimaryKeyAttribute(scan_plan, k2pg_table_desc, ObjectIdAttributeNumber);
	}
}

static void cam_get_att_type_info(TupleDesc bind_desc, AttrNumber attnum, Oid& atttypid, int& att_size, bool& att_byval)
{
	if (attnum > 0)
	{
		/* Get the type from the description */
        auto tup_desc = TupleDescAttr(bind_desc, attnum - 1);
		atttypid = tup_desc->atttypid;
		att_size = tup_desc->attlen;
		att_byval = tup_desc->attbyval;
	}
	else
	{
		/* This must be an OID column. */
		atttypid = OIDOID;
        att_size = 4;
        att_byval = true;
	}
}

/*
* Bind a scan key.
*/
static void camBindColumn(CamScanDesc camScan, TupleDesc bind_desc, AttrNumber attnum, Datum value, bool is_null)
{
    Oid     atttypid;
    int att_size;
    bool att_byval;
    cam_get_att_type_info(bind_desc, attnum, atttypid, att_size, att_byval);
    K2PgConstant constant {
        .type_id = atttypid,
        .attr_size = att_size,
        .attr_byvalue = att_byval,
        .datum = value,
        .is_null = is_null
    };

    K2PgConstraintDef constraint {
        .attr_num = attnum,
        .constraint = K2PG_CONSTRAINT_EQ,
        .constants = {constant}
    };

    camScan->constraints.push_back(std::move(constraint));
}

// bind a column for an inequal condition for example, < or >
static void camBindColumnCondInequal(CamScanDesc camScan, TupleDesc bind_desc, AttrNumber attnum, Datum value, bool is_null, K2PgConstraintType type)
{
    Oid     atttypid;
    int att_size;
    bool att_byval;
    cam_get_att_type_info(bind_desc, attnum, atttypid, att_size, att_byval);
    K2PgConstant constant {
        .type_id = atttypid,
        .attr_size = att_size,
        .attr_byvalue = att_byval,
        .datum = value,
        .is_null = is_null
    };

    K2PgConstraintDef constraint {
        .attr_num = attnum,
        .constraint = type,
        .constants = {constant}
    };

    camScan->constraints.push_back(std::move(constraint));
}

static void camBindColumnCondBetween(CamScanDesc camScan, TupleDesc bind_desc, AttrNumber attnum,
                                     bool start_valid, Datum value, bool end_valid, Datum value_end)
{
    Oid     atttypid;
    int att_size;
    bool att_byval;
    cam_get_att_type_info(bind_desc, attnum, atttypid, att_size, att_byval);

    K2PgConstant start {
        .type_id = atttypid,
        .attr_size = att_size,
        .attr_byvalue = att_byval,
        .datum = value,
        .is_null = !start_valid
    };
    K2PgConstant end {
        .type_id = atttypid,
        .attr_size = att_size,
        .attr_byvalue = att_byval,
        .datum = value_end,
        .is_null = !end_valid
    };

    K2PgConstraintDef constraint {
        .attr_num = attnum,
        .constraint = K2PG_CONSTRAINT_BETWEEN,
        .constants = {start, end}
    };

    camScan->constraints.push_back(std::move(constraint));
}

/*
 * Bind an array of scan keys for a column.
 */
static void camBindColumnCondIn(CamScanDesc camScan, TupleDesc bind_desc, AttrNumber attnum,
                                int nvalues, Datum *values)
{
    std::vector<K2PgConstant> constants;

	Oid	atttypid;
    int att_size;
    bool att_byval;
    cam_get_att_type_info(bind_desc, attnum, atttypid, att_size, att_byval);

	for (int i = 0; i < nvalues; i++) {
		/*
		 * For IN we are removing all null values in camBindScanKeys before
		 * getting here (relying on btree/lsm operators being strict).
		 * So we can safely set is_null to false for all options left here.
		 */
        K2PgConstant constant {
            .type_id = atttypid,
            .attr_size = att_size,
            .attr_byvalue = att_byval,
            .datum = values[i],
            .is_null = false
        };
        constants.push_back(constant);
	}

    K2PgConstraintDef constraint {
        .attr_num = attnum,
        .constraint = K2PG_CONSTRAINT_IN,
        .constants = std::move(constants)
    };

    camScan->constraints.push_back(std::move(constraint));
}

/*
 * Add a target column.
 */
static void camAddTargetColumn(CamScanDesc camScan, AttrNumber attnum)
{
	/* Regular (non-system) attribute. */
	if (attnum > 0)
	{
		Form_pg_attribute attr = TupleDescAttr(camScan->target_desc, attnum - 1);
		/* Ignore dropped attributes */
		if (attr->attisdropped)
			return;
	}

    camScan->targets_attrnum.push_back(attnum);
}

static HeapTuple camFetchNextHeapTuple(CamScanDesc camScan, bool is_forward_scan)
{
	HeapTuple tuple    = NULL;
	bool      has_data = false;
	TupleDesc tupdesc  = camScan->target_desc;

	Datum *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool *nulls  = (bool *) palloc0(tupdesc->natts * sizeof(bool));
	K2PgSysColumns syscols{};

	/* Execute the select statement. */
	if (!camScan->is_exec_done)
	{
		HandleK2PgStatus(PgGate_ExecSelect(camScan->handle, camScan->constraints, camScan->targets_attrnum, is_forward_scan, camScan->exec_params));
		camScan->is_exec_done = true;
	}

	/* Fetch one row. */
    HandleK2PgStatus(PgGate_DmlFetch(camScan->handle,
	                                 tupdesc->natts,
									 (uint64_t *) values,
									 nulls,
									 &syscols,
									 &has_data));
	if (has_data)
	{
		tuple = heap_form_tuple(tupdesc, values, nulls);

		if (syscols.oid != InvalidOid)
		{
			HeapTupleSetOid(tuple, syscols.oid);
		}
		if (syscols.k2pgctid != NULL)
		{
			tuple->t_k2pgctid = PointerGetDatum(syscols.k2pgctid);
		}
		if (camScan->tableOid != InvalidOid)
		{
			tuple->t_tableOid = camScan->tableOid;
		}
	}
	pfree(values);
	pfree(nulls);

	return tuple;
}

static IndexTuple camFetchNextIndexTuple(CamScanDesc camScan, Relation index, bool is_forward_scan)
{
	IndexTuple tuple    = NULL;
	bool       has_data = false;
	TupleDesc  tupdesc  = camScan->target_desc;

	Datum           *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool            *nulls = (bool *) palloc0(tupdesc->natts * sizeof(bool));
	K2PgSysColumns syscols{};

	/* Execute the select statement. */
	if (!camScan->is_exec_done)
	{
		HandleK2PgStatus(PgGate_ExecSelect(camScan->handle, camScan->constraints, camScan->targets_attrnum, is_forward_scan, camScan->exec_params));
		camScan->is_exec_done = true;
	}

	/* Fetch one row. */
	HandleK2PgStatus(PgGate_DmlFetch(camScan->handle,
	                                          tupdesc->natts,
	                                          (uint64_t *) values,
	                                          nulls,
	                                          &syscols,
	                                          &has_data));
	if (has_data)
	{
		/*
		 * Return the IndexTuple. If this is a primary key, reorder the values first as expected
		 * in the index's column order first.
		 */
		if (index->rd_index->indisprimary)
		{
			Assert(index->rd_index->indnatts <= INDEX_MAX_KEYS);

			Datum ivalues[INDEX_MAX_KEYS];
			bool  inulls[INDEX_MAX_KEYS];

			for (int i = 0; i < index->rd_index->indnatts; i++)
			{
				AttrNumber attno = index->rd_index->indkey.values[i];
				ivalues[i] = values[attno - 1];
				inulls[i]  = nulls[attno - 1];
			}

			tuple = index_form_tuple(RelationGetDescr(index), ivalues, inulls);
			if (syscols.k2pgctid != NULL)
			{
				tuple->t_k2pgctid = PointerGetDatum(syscols.k2pgctid);
			}
		}
		else
		{
			tuple = index_form_tuple(tupdesc, values, nulls);
			if (syscols.k2pgbasectid != NULL)
			{
				tuple->t_k2pgctid = PointerGetDatum(syscols.k2pgbasectid);
			}
		}

	}
	pfree(values);
	pfree(nulls);

	return tuple;
}

/*
 * Set up scan plan.
 * This function sets up target and bind columns for each type of scans.
 *    SELECT <Target_columns> FROM <Table> WHERE <Key_columns> op <Binds>
 *
 * 1. SequentialScan(Table) and PrimaryIndexScan(Table): index = 0
 *    - Table can be systable or usertable.
 *    - K2PG doesn't have a separate PrimaryIndexTable. It's a special case.
 *    - Both target and bind descriptors are specified by the <Table>
 *
 * 2. IndexScan(SysTable, Index).
 *    - Target descriptor is specifed by the SysTable.
 *    - Bind descriptor is specified by the IndexTable.
 *    - For this scan, K2PG returns a heap-tuple, which has all user's requested data.
 *
 * 3. IndexScan(UserTable, Index)
 *    - Both target and bind descriptors are specifed by the IndexTable.
 *    - For this scan, K2PG returns an index-tuple, which has a k2pgctid (ROWID) to be used for
 *      querying data from the UserTable.
 * 4. IndexOnlyScan(Table, Index)
 *    - Table can be systable or usertable.
 *    - Both target and bind descriptors are specifed by the IndexTable.
 *    - For this scan, K2PG ALWAYS return index-tuple, which is expected by Postgres layer.
 */
static void camSetupScanPlan(Relation relation, Relation index, bool xs_want_itup,
				 CamScanDesc camScan, CamScanPlan scan_plan)
{
	int i;
	memset(scan_plan, 0, sizeof(*scan_plan));

	/*
	 * Setup control-parameters for K2PG preparing statements for different
	 * types of scan.
	 * - "index_oid, index_only_scan, use_secondary_index": Different index
	 *   scans.
	 * NOTE: Primary index is a special case as there isn't a primary index
	 * table in K2PG.
	 */
	camScan->index = index;

	if (index)
	{
		camScan->prepare_params.index_oid = RelationGetRelid(index);
		camScan->prepare_params.index_only_scan = xs_want_itup;
		camScan->prepare_params.use_secondary_index = !index->rd_index->indisprimary;
	}

	/* Setup descriptors for target and bind. */
	if (!index || index->rd_index->indisprimary)
	{
		/*
		 * SequentialScan or PrimaryIndexScan
		 * - K2PG does not have a separate table for PrimaryIndex.
		 * - The target table descriptor, where data is read and returned, is the main table.
		 * - The binding table descriptor, whose column is bound to values, is also the main table.
		 */
		scan_plan->target_relation = relation;
		camLoadTableInfo(relation, scan_plan);
		camScan->target_desc = RelationGetDescr(relation);
		scan_plan->bind_desc = RelationGetDescr(relation);
	}
	else
	{
		/*
		 * Index-Scan: SELECT data FROM UserTable WHERE rowid IN (SELECT k2pgctid FROM indexTable)
		 *
		 */

		if (camScan->prepare_params.index_only_scan)
		{
			/*
			 * IndexOnlyScan
			 * - This special case is optimized where data is read from index table.
			 * - The target table descriptor, where data is read and returned, is the index table.
			 * - The binding table descriptor, whose column is bound to values, is also the index table.
			 */
			scan_plan->target_relation = index;
			camScan->target_desc = RelationGetDescr(index);
		}
		else
		{
			/*
			 * IndexScan ( SysTable / UserTable)
			 * - K2PG will use the binds to query base-k2pgctid in the index table, which is then used
			 *   to query data from the main table.
			 * - The target table descriptor, where data is read and returned, is the main table.
			 * - The binding table descriptor, whose column is bound to values, is the index table.
			 */
			scan_plan->target_relation = relation;
			camScan->target_desc = RelationGetDescr(relation);
		}

		camLoadTableInfo(index, scan_plan);
		scan_plan->bind_desc = RelationGetDescr(index);
	}

	/*
	 * Setup bind and target attnum of ScanKey.
	 * - The target-attnum comes from the table that is being read by the scan
	 * - The bind-attnum comes from the table that is being scan by the scan.
	 *
	 * Examples:
	 * - For IndexScan(SysTable, Index), SysTable is used for targets, but Index is for binds.
	 * - For IndexOnlyScan(Table, Index), only Index is used to setup both target and bind.
	 */
	for (i = 0; i < camScan->nkeys; i++)
	{
		if (!index)
		{
			/* Sequential scan */
			camScan->target_key_attnums[i] = camScan->key[i].sk_attno;
			scan_plan->bind_key_attnums[i] = camScan->key[i].sk_attno;
		}
		else if (index->rd_index->indisprimary)
		{
			/*
			 * PrimaryIndex scan: This is a special case in K2PG. There is no PrimaryIndexTable.
			 * The table itself will be scanned.
			 */
			camScan->target_key_attnums[i] =	scan_plan->bind_key_attnums[i] =
				index->rd_index->indkey.values[camScan->key[i].sk_attno - 1];
		}
		else if (camScan->prepare_params.index_only_scan)
		{
			/*
			 * IndexOnlyScan(Table, Index) returns IndexTuple.
			 * Use the index attnum for both targets and binds.
			 */
			scan_plan->bind_key_attnums[i] = camScan->key[i].sk_attno;
			camScan->target_key_attnums[i] =	camScan->key[i].sk_attno;
		}
		else
		{
			/*
			 * IndexScan(SysTable or UserTable, Index) returns HeapTuple.
			 * Use SysTable attnum for targets. Use its index attnum for binds.
			 */
			scan_plan->bind_key_attnums[i] = camScan->key[i].sk_attno;
			camScan->target_key_attnums[i] =	index->rd_index->indkey.values[camScan->key[i].sk_attno - 1];
		}
	}
}

static bool cam_should_pushdown_op(CamScanPlan scan_plan, AttrNumber attnum, int op_strategy)
{
	switch (op_strategy)
	{
		case BTEqualStrategyNumber:
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return true;
		default:
			return false;
	}
}

/*
 * Is this a basic (c =/</<=/>=/> value) (in)equality condition.
 * TODO: The null value case (SK_ISNULL) should always evaluate to false
 *       per SQL semantics but in K2 PG it will be true. So this case
 *       will require PG filtering (for null values only).
 */
static bool IsBasicOpSearch(int sk_flags) {
	return sk_flags == 0 || sk_flags == SK_ISNULL;
}

/*
 * Is this a null search (c IS NULL) -- same as equality cond for K2 PG.
 */
static bool IsSearchNull(int sk_flags) {
	return sk_flags == (SK_ISNULL | SK_SEARCHNULL);
}

/*
 * Is this an array search (c = ANY(..) or c IN ..).
 */
static bool IsSearchArray(int sk_flags) {
	return sk_flags == SK_SEARCHARRAY;
}

static bool ShouldPushdownScanKey(Relation relation, CamScanPlan scan_plan, AttrNumber attnum,
                      ScanKey key, bool is_primary_key) {
	if (IsSystemRelation(relation))
	{
		/*
		 * Only support eq operators for system tables.
		 * TODO: we can probably allow ineq conditions for system tables now.
		 */
		return IsBasicOpSearch(key->sk_flags) &&
			key->sk_strategy == BTEqualStrategyNumber;
	}
	else
	{
		if (IsBasicOpSearch(key->sk_flags))
		{
			/* Eq strategy for hash key, eq + ineq for range key. */
			return cam_should_pushdown_op(scan_plan, attnum, key->sk_strategy);
		}

		if (IsSearchNull(key->sk_flags))
		{
			/* Always expect InvalidStrategy for NULL search. */
			Assert(key->sk_strategy == InvalidStrategy);
			return true;
		}

		if (IsSearchArray(key->sk_flags))
		{
			/*
			 * Expect equal strategy here (i.e. IN .. or = ANY(..) conditions,
			 * NOT IN will generate <> which is not a supported LSM/BTREE
			 * operator, so it should not get to this point.
			 */
			Assert(key->sk_strategy == BTEqualStrategyNumber);
			return true;
		}
		/* No other operators are supported. */
		return false;
	}
}

/* int comparator for qsort() */
static int int_compar_cb(const void *v1, const void *v2)
{
  int k1 = (int)(*(const int *)v1);
  int k2 = (int)(*(const int *)v2);

  if (k1 < k2)
    return -1;

  if (k1 > k2)
    return 1;

  return 0;
}

/* Use the scan-descriptor and scan-plan to setup scan key for filtering */
static void	camSetupScanKeys(Relation relation,
							 Relation index,
							 CamScanDesc camScan,
							 CamScanPlan scan_plan)
{
	/*
	 * Find the scan keys that are the primary key.
	 */
	for (int i = 0; i < camScan->nkeys; i++)
	{
		if (scan_plan->bind_key_attnums[i] == InvalidOid)
			break;

		int idx = K2PgAttnumToBmsIndex(scan_plan->target_relation, scan_plan->bind_key_attnums[i]);
		bool is_primary_key = bms_is_member(idx, scan_plan->primary_key);

		if (!ShouldPushdownScanKey(relation, scan_plan, scan_plan->bind_key_attnums[i],
		                           &camScan->key[i], is_primary_key)) {
			if (!camScan->exec_params.limit_use_default) {
				// do not set limit count if we don't pushdown all conditions and we don't use default prefetch limit
				camScan->exec_params.limit_count = -1;
			}
			continue;
		}

		scan_plan->sk_cols = bms_add_member(scan_plan->sk_cols, idx);
	}
}

/* Use the scan-descriptor and scan-plan to setup binds for the queryplan */
static void camBindScanKeys(Relation relation,
							Relation index,
							CamScanDesc camScan,
							CamScanPlan scan_plan) {
	Oid		dboid    = K2PgGetDatabaseOid(relation);
	Oid		relid    = RelationGetRelid(relation);

	HandleK2PgStatus(PgGate_NewSelect(dboid, relid, camScan->prepare_params, &camScan->handle));

	if (IsSystemRelation(relation))
	{
		/* Bind the scan keys */
		for (int i = 0; i < camScan->nkeys; i++)
		{
			int idx = K2PgAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);
			if (bms_is_member(idx, scan_plan->sk_cols))
			{
				bool is_null = (camScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL;

				camBindColumn(camScan, scan_plan->bind_desc, scan_plan->bind_key_attnums[i],
							  camScan->key[i].sk_argument, is_null);
			}
		}
	}
	else
	{
		/* Find max number of cols in schema in use in query */
		int max_idx = 0;
		for (int i = 0; i < camScan->nkeys; i++)
		{
			int idx = K2PgAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);
			if (!bms_is_member(idx, scan_plan->sk_cols))
				continue;

			if (max_idx < idx)
				max_idx = idx;
		}
		max_idx++;

		/* Find intervals for columns */

		bool is_column_bound[max_idx]; /* VLA - scratch space */
		memset(is_column_bound, 0, sizeof(bool) * max_idx);

		/*
		 * find an order of relevant keys such that for the same column, an EQUAL
		 * condition is encountered before IN or BETWEEN. is_column_bound is then used
		 * to establish priority order EQUAL > IN > BETWEEN.
		 */
		int noffsets = 0;
		int offsets[camScan->nkeys + 1]; /* VLA - scratch space: +1 to avoid zero elements */

		for (int i = 0; i < camScan->nkeys; i++)
		{
			/* Check if this is primary columns */
			int idx = K2PgAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);
			if (!bms_is_member(idx, scan_plan->sk_cols))
				continue;

			/* Assign key offsets */
			switch (camScan->key[i].sk_strategy)
			{
				case InvalidStrategy:
					/* Should be ensured during planning. */
					Assert(IsSearchNull(camScan->key[i].sk_flags));
					/* fallthrough  -- treating IS NULL as (K2PG) = (null) */
				case BTEqualStrategyNumber:
					if (IsBasicOpSearch(camScan->key[i].sk_flags) ||
						IsSearchNull(camScan->key[i].sk_flags))
					{
						/* Use a -ve value so that qsort places EQUAL before others */
						offsets[noffsets++] = -i;
					}
					else if (IsSearchArray(camScan->key[i].sk_flags))
					{
						offsets[noffsets++] = i;
					}
					break;
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					offsets[noffsets++] = i;

				default:
					break; /* unreachable */
			}
		}

		qsort(offsets, noffsets, sizeof(int), int_compar_cb);
		/* restore -ve offsets to +ve */
		for (int i = 0; i < noffsets; i++)
		if (offsets[i] < 0)
			offsets[i] = -offsets[i];
		else
			break;

		/* Bind keys for EQUALS and IN */
		for (int k = 0; k < noffsets; k++)
		{
			int i = offsets[k];
			int idx = K2PgAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);

			/* Do not bind more than one condition to a column */
			if (is_column_bound[idx])
				continue;

			bool is_primary_key = bms_is_member(idx, scan_plan->primary_key); // Includes hash key

			switch (camScan->key[i].sk_strategy)
			{
				case InvalidStrategy: /* fallthrough, c IS NULL -> c = NULL (checked above) */
				case BTEqualStrategyNumber:
					/* Bind the scan keys */
					if (IsBasicOpSearch(camScan->key[i].sk_flags) ||
						IsSearchNull(camScan->key[i].sk_flags))
					{
						/* Either c = NULL or c IS NULL. */
						bool is_null = (camScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL;
						camBindColumn(camScan, scan_plan->bind_desc,
									  scan_plan->bind_key_attnums[i],
									  camScan->key[i].sk_argument, is_null);
						is_column_bound[idx] = true;
					}
					else if (IsSearchArray(camScan->key[i].sk_flags) && is_primary_key)
					{
						/* based on _bt_preprocess_array_keys() */
						ArrayType  *arrayval;
						int16		elmlen;
						bool		elmbyval;
						char		elmalign;
						int			num_elems;
						Datum	   *elem_values;
						bool	   *elem_nulls;
						int			num_nonnulls;
						int			j;

						ScanKey cur = &camScan->key[i];

						/*
						 * First, deconstruct the array into elements.  Anything allocated
						 * here (including a possibly detoasted array value) is in the
						 * workspace context.
						 */
						arrayval = DatumGetArrayTypeP(cur->sk_argument);
						/* We could cache this data, but not clear it's worth it */
						get_typlenbyvalalign(ARR_ELEMTYPE(arrayval), &elmlen,
						                     &elmbyval, &elmalign);
						deconstruct_array(arrayval,
						                  ARR_ELEMTYPE(arrayval),
						                  elmlen, elmbyval, elmalign,
						                  &elem_values, &elem_nulls, &num_elems);

						/*
						 * Compress out any null elements.  We can ignore them since we assume
						 * all btree operators are strict.
						 */
						num_nonnulls = 0;
						for (j = 0; j < num_elems; j++)
						{
							if (!elem_nulls[j])
							elem_values[num_nonnulls++] = elem_values[j];
						}

						/* We could pfree(elem_nulls) now, but not worth the cycles */

						/*
						 * If there's no non-nulls, the scan qual is unsatisfiable
						 */
						if (num_nonnulls == 0)
							break;

						/* Build temporary vars */
						IndexScanDescData tmp_scan_desc{};
						tmp_scan_desc.indexRelation = index;

						/*
						 * Sort the non-null elements and eliminate any duplicates.  We must
						 * sort in the same ordering used by the index column, so that the
						 * successive primitive indexscans produce data in index order.
						 */
						num_elems = _bt_sort_array_elements(&tmp_scan_desc, cur,
						                                    false /* reverse */,
						                                    elem_values, num_nonnulls);

						/*
						 * And set up the BTArrayKeyInfo data.
						 */
						camBindColumnCondIn(camScan, scan_plan->bind_desc, scan_plan->bind_key_attnums[i],
						                    num_elems, elem_values);
						is_column_bound[idx] = true;
					} else {
						/* unreachable */
					}
					break;

				case BTGreaterEqualStrategyNumber:
					camBindColumnCondInequal(camScan,
									scan_plan->bind_desc,
									scan_plan->bind_key_attnums[i],
									camScan->key[i].sk_argument,
									(camScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL,
									K2PgConstraintType::K2PG_CONSTRAINT_GTE);
					is_column_bound[idx] = true;
					break;
				case BTGreaterStrategyNumber:
					camBindColumnCondInequal(camScan,
									scan_plan->bind_desc,
									scan_plan->bind_key_attnums[i],
									camScan->key[i].sk_argument,
									(camScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL,
									K2PgConstraintType::K2PG_CONSTRAINT_GT);
					is_column_bound[idx] = true;
					break;
				case BTLessStrategyNumber:
					camBindColumnCondInequal(camScan,
									scan_plan->bind_desc,
									scan_plan->bind_key_attnums[i],
									camScan->key[i].sk_argument,
									(camScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL,
									K2PgConstraintType::K2PG_CONSTRAINT_LT);
					is_column_bound[idx] = true;
					break;
				case BTLessEqualStrategyNumber:
					camBindColumnCondInequal(camScan,
									scan_plan->bind_desc,
									scan_plan->bind_key_attnums[i],
									camScan->key[i].sk_argument,
									(camScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL,
									K2PgConstraintType::K2PG_CONSTRAINT_LTE);
					is_column_bound[idx] = true;
					break;
				default:
					break;
			}
		}
	}
}

/* Setup the targets */
static void camSetupTargets(Relation relation,
														Relation index,
														CamScanDesc camScan,
														CamScanPlan scan_plan) {
	if (scan_plan->target_relation->rd_rel->relhasoids)
		camAddTargetColumn(camScan, ObjectIdAttributeNumber);

	if (camScan->prepare_params.index_only_scan && index->rd_index->indisprimary)
		/*
		 * Special case: For Primary-Key-ONLY-Scan, we select ONLY the primary key from the target
		 * table instead of the whole target table.
		 */
		for (int i = 0; i < index->rd_index->indnatts; i++)
			camAddTargetColumn(camScan, index->rd_index->indkey.values[i]);
	else
		for (AttrNumber attnum = 1; attnum <= camScan->target_desc->natts; attnum++)
			camAddTargetColumn(camScan, attnum);

	if (scan_plan->target_relation->rd_index)
		/*
		 * IndexOnlyScan:
		 *   SELECT [ data, ] k2pgbasectid (ROWID of UserTable, relation) FROM secondary-index-table
		 * In this case, Postgres requests base_ctid and maybe also data from IndexTable and then uses
		 * them for further processing.
		 */
		camAddTargetColumn(camScan, K2PgIdxBaseTupleIdAttributeNumber);
	else
	{
		/* Two cases:
		 * - Primary Scan (Key or sequential)
		 *     SELECT data, k2pgctid FROM table [ WHERE primary-key-condition ]
		 * - Secondary IndexScan
		 *     SELECT data, k2pgctid FROM table WHERE k2pgctid IN ( SELECT base_k2pgctid FROM IndexTable )
		 */
		camAddTargetColumn(camScan, K2PgTupleIdAttributeNumber);
		if (index && !index->rd_index->indisprimary)
		{
			/*
			 * IndexScan: Postgres layer sends both actual-query and index-scan to PgGate, who will
			 * select and immediately use base_ctid to query data before responding.
			 */
			camAddTargetColumn(camScan, K2PgIdxBaseTupleIdAttributeNumber);
		}
	}
}

/*
 * Begin a scan for
 *   SELECT <Targets> FROM <Relation relation> USING <Relation index>
 * NOTES:
 * - "relation" is the table being SELECTed.
 * - "index" identify the INDEX that will be used for scaning.
 * - "nkeys" and "key" identify which key columns are provided in the SELECT WHERE clause.
 *   nkeys = Number of key.
 *   keys[].sk_attno = the columns' attnum in the IndexTable or "index"
 *                     (This is not the attnum in UserTable or "relation")
 *
 * - If "xs_want_itup" is true, Postgres layer is expecting an IndexTuple that has k2pgctid to
 *   identify the desired row.
 */
CamScanDesc camBeginScan(Relation relation, Relation index, bool xs_want_itup, int nkeys, ScanKey key)
{
	if (nkeys > K2PG_MAX_SCAN_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d predicates in a table or index scan",
						K2PG_MAX_SCAN_KEYS)));

	/* Set up K2PG scan description */
	CamScanDesc camScan = (CamScanDesc) palloc0(sizeof(CamScanDescData));
	// copy the keys to avoid the current keys go out of scope for subsequent scan operations
	size_t size = sizeof(ScanKeyData) * nkeys;
	ScanKeyData *s_keys = (ScanKeyData *)palloc0(size);
	memcpy(s_keys, key, size);
	camScan->key = s_keys;
	camScan->nkeys = nkeys;
	camScan->tableOid = RelationGetRelid(relation);

	/* Setup the scan plan */
	CamScanPlanData	scan_plan;
	camSetupScanPlan(relation, index, xs_want_itup, camScan, &scan_plan);

	/* Setup binds for the scan-key */
	camSetupScanKeys(relation, index, camScan, &scan_plan);
	camBindScanKeys(relation, index, camScan, &scan_plan);

	/*
	 * Set up the scan targets. If the table is indexed and only the indexed columns should be
	 * returned, fetch just those columns. Otherwise, fetch all "real" columns.
	 */
	camSetupTargets(relation, index, camScan, &scan_plan);

	/*
	 * Set the current syscatalog version (will check that we are up to date).
	 * Avoid it for syscatalog tables so that we can still use this for
	 * refreshing the caches when we are behind.
	 * Note: This works because we do not allow modifying schemas (alter/drop)
	 * for system catalog tables.
	 */
	// TODO Add this back when we consolidate PGStatement and K2PGScanHandle
	// if (!IsSystemRelation(relation))
	// {
	// 	HandleK2PgStatus(PgGate_SetCatalogCacheVersion((K2PgStatement)camScan->handle, k2pg_catalog_cache_version));
	// }

	bms_free(scan_plan.nonprimary_key);
	bms_free(scan_plan.primary_key);
	bms_free(scan_plan.sk_cols);

	return camScan;
}

void camEndScan(CamScanDesc camScan)
{
	pfree(camScan);
}

static bool heaptuple_matches_key(HeapTuple tup,
					  TupleDesc tupdesc,
					  int nkeys,
					  ScanKey key,
					  AttrNumber sk_attno[],
					  bool *recheck)
{
	*recheck = false;

	for (int i = 0; i < nkeys; i++)
	{
		if (sk_attno[i] == InvalidOid)
			break;

		bool  is_null = false;
		Datum res_datum = heap_getattr(tup, sk_attno[i], tupdesc, &is_null);

		if (key[i].sk_flags & SK_SEARCHNULL)
		{
			if (is_null)
				continue;
			else
				return false;
		}

		if (key[i].sk_flags & SK_SEARCHNOTNULL)
		{
			if (!is_null)
				continue;
			else
				return false;
		}

		/*
		 * TODO: support the different search options like SK_SEARCHARRAY.
		 */
		if (key[i].sk_flags != 0)
		{
			*recheck = true;
			continue;
		}

		if (is_null)
			return false;

		bool matches = DatumGetBool(FunctionCall2Coll(&key[i].sk_func,
		                                              key[i].sk_collation,
		                                              res_datum,
		                                              key[i].sk_argument));
		if (!matches)
			return false;
	}

	return true;
}

static bool indextuple_matches_key(IndexTuple tup,
					   TupleDesc tupdesc,
					   int nkeys,
					   ScanKey key,
					   AttrNumber sk_attno[],
					   bool *recheck)
{
	*recheck = false;

	for (int i = 0; i < nkeys; i++)
	{
		if (sk_attno[i] == InvalidOid)
			break;

		bool  is_null = false;
		Datum res_datum = index_getattr(tup, sk_attno[i], tupdesc, &is_null);

		if (key[i].sk_flags & SK_SEARCHNULL)
		{
			if (is_null)
				continue;
			else
				return false;
		}

		if (key[i].sk_flags & SK_SEARCHNOTNULL)
		{
			if (!is_null)
				continue;
			else
				return false;
		}

		/*
		 * TODO: support the different search options like SK_SEARCHARRAY.
		 */
		if (key[i].sk_flags != 0)
		{
			*recheck = true;
			continue;
		}

		if (is_null)
			return false;

		bool matches = DatumGetBool(FunctionCall2Coll(&key[i].sk_func,
		                                              key[i].sk_collation,
		                                              res_datum,
		                                              key[i].sk_argument));
		if (!matches)
			return false;
	}

	return true;
}

HeapTuple cam_getnext_heaptuple(CamScanDesc camScan, bool is_forward_scan, bool *recheck)
{
	int         nkeys    = camScan->nkeys;
	ScanKey     key      = camScan->key;
	AttrNumber *sk_attno = camScan->target_key_attnums;
	HeapTuple   tup      = NULL;

	/*
	 * K2PG Scan may not be able to push down the scan key condition so we may
	 * need additional filtering here.
	 */
	while (HeapTupleIsValid(tup = camFetchNextHeapTuple(camScan, is_forward_scan)))
	{
		if (heaptuple_matches_key(tup, camScan->target_desc, nkeys, key, sk_attno, recheck))
			return tup;

		heap_freetuple(tup);
	}

	return NULL;
}

IndexTuple cam_getnext_indextuple(CamScanDesc camScan, bool is_forward_scan, bool *recheck)
{
	int         nkeys    = camScan->nkeys;
	ScanKey     key      = camScan->key;
	AttrNumber *sk_attno = camScan->target_key_attnums;
	Relation    index    = camScan->index;
	IndexTuple  tup      = NULL;

	/*
	 * K2PG Scan may not be able to push down the scan key condition so we may
	 * need additional filtering here.
	 */
	while (PointerIsValid(tup = camFetchNextIndexTuple(camScan, index, is_forward_scan)))
	{
		if (indextuple_matches_key(tup, RelationGetDescr(index), nkeys, key, sk_attno, recheck))
			return tup;

		pfree(tup);
	}

	return NULL;
}

SysScanDesc cam_systable_beginscan(Relation relation,
                                   Oid indexId,
                                   bool indexOK,
                                   Snapshot snapshot,
                                   int nkeys,
                                   ScanKey key)
{
	Relation index = NULL;

	/*
	 * Look up the index to scan with if we can. If the index is the primary key which is part
	 * of the table in K2PG, we should scan the table directly.
	 */
	if (indexOK && !u_sess->attr.attr_common.IgnoreSystemIndexes && !ReindexIsProcessingIndex(indexId))
	{
		index = RelationIdGetRelation(indexId);
		if (index->rd_index->indisprimary)
		{
			RelationClose(index);
			index = NULL;
		}

		if (index) {
			/*
			 * Change attribute numbers to be index column numbers.
			 * - This conversion is the same as function systable_beginscan() in file "genam.c". If we
			 *   ever reuse Postgres index code, this conversion is a must because the key entries must
			 *   match what Postgres code expects.
			 *
			 * - When selecting using INDEX, the key values are bound to the IndexTable, so index attnum
			 *   must be used for bindings.
			 */
			int i, j;
			for (i = 0; i < nkeys; i++)
			{
				for (j = 0; j < IndexRelationGetNumberOfAttributes(index); j++)
				{
					if (key[i].sk_attno == index->rd_index->indkey.values[j])
					{
						key[i].sk_attno = j + 1;
						break;
					}
				}
				if (j == IndexRelationGetNumberOfAttributes(index))
					elog(ERROR, "column is not in index");
			}
		}
	}

	CamScanDesc camScan = camBeginScan(relation, index, false /* xs_want_itup */, nkeys, key);

	/* Set up Postgres sys table scan description */
	SysScanDesc scan_desc = (SysScanDesc) palloc0(sizeof(SysScanDescData));
	scan_desc->heap_rel   = relation;
	scan_desc->snapshot   = snapshot;
	scan_desc->k2scan     = camScan;

	if (index)
	{
		RelationClose(index);
	}

	return scan_desc;
}

HeapTuple cam_systable_getnext(SysScanDesc scan_desc)
{
	bool recheck = false;

	Assert(PointerIsValid(scan_desc->k2scan));

	HeapTuple tuple = cam_getnext_heaptuple(scan_desc->k2scan, true /* is_forward_scan */,
											&recheck);

	Assert(!recheck);

	return tuple;
}


void cam_systable_endscan(SysScanDesc scan_desc)
{
	Assert(PointerIsValid(scan_desc->k2scan));
	camEndScan(scan_desc->k2scan);
	pfree(scan_desc);
}

HeapScanDesc cam_heap_beginscan(Relation relation,
                                Snapshot snapshot,
                                int nkeys,
                                ScanKey key,
                                bool temp_snap)
{
	/* Restart should not be prevented if operation caused by system read of system table. */
	CamScanDesc camScan = camBeginScan(relation, NULL /* index */, false /* xs_want_itup */,
	                                 nkeys, key);

	/* Set up Postgres sys table scan description */
	HeapScanDesc scan_desc = (HeapScanDesc) palloc0(sizeof(HeapScanDescData));
	scan_desc->rs_base.rs_rd        = relation;
	scan_desc->rs_base.rs_snapshot  = snapshot;
	scan_desc->rs_base.rs_cblock    = InvalidBlockNumber;
	scan_desc->rs_temp_snap = temp_snap;
	scan_desc->k2scan       = camScan;

	return scan_desc;
}

HeapTuple cam_heap_getnext(HeapScanDesc scan_desc)
{
	bool recheck = false;

	Assert(PointerIsValid(scan_desc->k2scan));

	HeapTuple tuple = cam_getnext_heaptuple(scan_desc->k2scan, true /* is_forward_scan */,
											&recheck);

	Assert(!recheck);

	return tuple;
}

void cam_heap_endscan(HeapScanDesc scan_desc)
{
	Assert(PointerIsValid(scan_desc->k2scan));
	camEndScan(scan_desc->k2scan);
	if (scan_desc->rs_temp_snap)
		UnregisterSnapshot(scan_desc->rs_base.rs_snapshot);
	pfree(scan_desc);
}

/* --------------------------------------------------------------------------------------------- */

void camCostEstimate(RelOptInfo *baserel, Selectivity selectivity,
					 bool is_backwards_scan, bool is_uncovered_idx_scan,
					 Cost *startup_cost, Cost *total_cost)
{
	/*
	 * K2PG-specific per-tuple cost considerations:
	 *   - 10x the regular CPU cost to account for network/RPC + K2 PG Gate overhead.
	 *   - backwards scan scale factor as it will need that many more fetches
	 *     to get all rows/tuples.
	 *   - uncovered index scan is more costly than index-only or seq scan because
	 *     it requires extra request to the main table.
	 */
	Cost k2pg_per_tuple_cost_factor = 10;
	if (is_backwards_scan)
	{
		k2pg_per_tuple_cost_factor *= K2PG_BACKWARDS_SCAN_COST_FACTOR;
	}
	if (is_uncovered_idx_scan)
	{
		k2pg_per_tuple_cost_factor *= K2PG_UNCOVERED_INDEX_COST_FACTOR;
	}

	Cost cost_per_tuple = u_sess->attr.attr_sql.cpu_tuple_cost * k2pg_per_tuple_cost_factor +
	                      baserel->baserestrictcost.per_tuple;

	*startup_cost = baserel->baserestrictcost.startup;

	*total_cost   = *startup_cost + cost_per_tuple * baserel->tuples * selectivity;
}

/*
 * Evaluate the selectivity for some qualified cols given the hash and primary key cols.
 * TODO this should look into the actual operators and distinguish, for instance
 * equality and inequality conditions (for ASC/DESC columns) better.
 */
static double camIndexEvalClauseSelectivity(Bitmapset *qual_cols,
                                            bool is_unique_idx,
                                            Bitmapset *nonprimary_key,
                                            Bitmapset *primary_key)
{
	/*
	 * If there is no search condition, or not all of the non-primary columns have
	 * search conditions, it will be a full-table scan.
	 */
	if (bms_is_empty(qual_cols) || !bms_is_subset(nonprimary_key, qual_cols))
	{
		return K2PG_FULL_SCAN_SELECTIVITY;
	}

	/*
	 * Otherwise, it will be either a primary key lookup or range scan
	 * on a hash key.
	 */
	if (bms_is_subset(primary_key, qual_cols))
	{
		/* For unique indexes full key guarantees single row. */
		return is_unique_idx ? K2PG_SINGLE_ROW_SELECTIVITY
						     : K2PG_SINGLE_KEY_SELECTIVITY;
	}

	return K2PG_HASH_SCAN_SELECTIVITY;
}

void camIndexCostEstimate(IndexPath *path, Selectivity *selectivity,
						  Cost *startup_cost, Cost *total_cost)
{
	Relation	index = RelationIdGetRelation(path->indexinfo->indexoid);
	bool		isprimary = index->rd_index->indisprimary;
	Relation	relation = isprimary ? RelationIdGetRelation(index->rd_index->indrelid) : NULL;
	RelOptInfo *baserel = path->path.parent;
	List	   *qinfos;
	ListCell   *lc;
	bool        is_backwards_scan = path->indexscandir == BackwardScanDirection;
	bool        is_unique = index->rd_index->indisunique;
	bool        is_partial_idx = path->indexinfo->indpred != NIL && path->indexinfo->predOK;
	Bitmapset  *const_quals = NULL;

	/* Primary-index scans are always covered in K2PG (internally) */
	bool       is_uncovered_idx_scan = !index->rd_index->indisprimary &&
	                                   path->path.pathtype != T_IndexOnlyScan;

	CamScanPlanData	scan_plan;
	memset(&scan_plan, 0, sizeof(scan_plan));
	scan_plan.target_relation = isprimary ? relation : index;
	camLoadTableInfo(scan_plan.target_relation, &scan_plan);

	/* Do preliminary analysis of indexquals */
	qinfos = deconstruct_indexquals(path);

	/* Find out the search conditions on the primary key columns */
	foreach(lc, qinfos)
	{
		IndexQualInfo *qinfo = (IndexQualInfo *) lfirst(lc);
		RestrictInfo *rinfo = qinfo->rinfo;
		AttrNumber	 attnum = isprimary ? index->rd_index->indkey.values[qinfo->indexcol]
										: (qinfo->indexcol + 1);
		Expr	   *clause = rinfo->clause;
		Oid			clause_op;
		int			op_strategy;
		int			bms_idx = K2PgAttnumToBmsIndex(scan_plan.target_relation, attnum);

		if (IsA(clause, NullTest))
		{
			NullTest *nt = (NullTest *) clause;
			/* We only support IS NULL (i.e. not IS NOT NULL). */
			if (nt->nulltesttype == IS_NULL)
			{
				const_quals = bms_add_member(const_quals, bms_idx);
				camAddAttributeColumn(&scan_plan, attnum);
			}
		}
		else
		{
			clause_op = qinfo->clause_op;

			if (OidIsValid(clause_op))
			{
				op_strategy = get_op_opfamily_strategy(clause_op,
													   path->indexinfo->opfamily[qinfo->indexcol]);
				Assert(op_strategy != 0);  /* not a member of opfamily?? */

				if (cam_should_pushdown_op(&scan_plan, attnum, op_strategy))
				{
					camAddAttributeColumn(&scan_plan, attnum);
					if (qinfo->other_operand && IsA(qinfo->other_operand, Const))
						const_quals = bms_add_member(const_quals, bms_idx);
				}
			}
		}
	}

	/*
	 * If there is no search condition, or not all of the hash columns have search conditions, it
	 * will be a full-table scan. Otherwise, it will be either a primary key lookup or range scan
	 * on a hash key.
	 */
	*selectivity = camIndexEvalClauseSelectivity(scan_plan.sk_cols,
	                                             is_unique,
	                                             scan_plan.nonprimary_key,
	                                             scan_plan.primary_key);
	path->path.rows = baserel->tuples * (*selectivity);

	/*
	 * For partial indexes, scale down the rows to account for the predicate.
	 * Do this after setting the baserel rows since this does not apply to base rel.
	 * TODO: this should be evaluated based on the index condition in the future.
	 */
	if (is_partial_idx)
	{
		*selectivity *= K2PG_PARTIAL_IDX_PRED_SELECTIVITY;
	}

	camCostEstimate(baserel, *selectivity, is_backwards_scan,
	                is_uncovered_idx_scan, startup_cost, total_cost);

	/*
	 * Try to evaluate the number of rows this baserel might return.
	 * We cannot rely on the join conditions here (e.g. t1.c1 = t2.c2) because
	 * they may not be applied if another join path is chosen.
	 * So only use the t1.c1 = <const_value> quals (filtered above) for this.
	 */
	double const_qual_selectivity = camIndexEvalClauseSelectivity(const_quals,
	                                                              is_unique,
	                                                              scan_plan.nonprimary_key,
	                                                              scan_plan.primary_key);
	double baserel_rows_estimate = const_qual_selectivity * baserel->tuples;
	if (baserel_rows_estimate < baserel->rows)
	{
		baserel->rows = baserel_rows_estimate;
	}


	if (relation)
		RelationClose(relation);

	RelationClose(index);
}

HeapTuple CamFetchTuple(Relation relation, Datum k2pgctid)
{
	K2PgScanHandle* k2pg_stmt;
	TupleDesc      tupdesc = RelationGetDescr(relation);

    K2PgSelectIndexParams index_params {
        .index_oid = RelationGetRelid(relation),
        .index_only_scan = false,
        .use_secondary_index = false
    };

	HandleK2PgStatus(PgGate_NewSelect(K2PgGetDatabaseOid(relation),
																RelationGetRelid(relation),
																index_params,
																&k2pg_stmt));

    std::vector<K2PgConstraintDef> constraints;
    std::vector<int> targets;

	/* Bind k2pgctid to identify the current row. */
    K2PgConstant ctid_const = {
        .type_id = BYTEAOID,
        .attr_size = -1,
        .attr_byvalue = false,
        .datum = k2pgctid,
        .is_null = false
    };
    K2PgConstraintDef ctid_constraint = {
        .attr_num = K2PgTupleIdAttributeNumber,
        .constraint = K2PG_CONSTRAINT_EQ,
        .constants = {ctid_const}
    };
    constraints.push_back(std::move(ctid_constraint));

	/*
	 * Set up the scan targets. For index-based scan we need to return all "real" columns.
	 */
	if (RelationGetForm(relation)->relhasoids)
	{
        targets.push_back(ObjectIdAttributeNumber);
	}
	for (AttrNumber attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
        targets.push_back(attnum);
	}
    targets.push_back(K2PgTupleIdAttributeNumber);

	/*
	 * Execute the select statement.
	 * This select statement fetch the row for a specific K2PGTID
	 */
    K2PgSelectLimitParams limit_params {
        .limit_count = 1,
        .limit_offset = 0,
        .limit_use_default = false
    };
	HandleK2PgStatus(PgGate_ExecSelect(k2pg_stmt, std::move(constraints), std::move(targets), true /* forward */, limit_params));

	HeapTuple tuple    = NULL;
	bool      has_data = false;

	Datum           *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool            *nulls  = (bool *) palloc0(tupdesc->natts * sizeof(bool));
	K2PgSysColumns syscols{};

	/* Fetch one row. */
	HandleK2PgStatus(PgGate_DmlFetch(k2pg_stmt,
															 tupdesc->natts,
															 (uint64_t *) values,
															 nulls,
															 &syscols,
															 &has_data));

	if (has_data)
	{
		tuple = heap_form_tuple(tupdesc, values, nulls);

		if (syscols.oid != InvalidOid)
		{
			HeapTupleSetOid(tuple, syscols.oid);
		}
		if (syscols.k2pgctid != NULL)
		{
			tuple->t_k2pgctid = PointerGetDatum(syscols.k2pgctid);
		}
		tuple->t_tableOid = RelationGetRelid(relation);
	}
	pfree(values);
	pfree(nulls);

	return tuple;
}
