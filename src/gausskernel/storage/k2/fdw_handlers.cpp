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

#include "libintl.h"
#include "postgres.h"
#include "funcapi.h"
#include "access/reloptions.h"
#include "access/transam.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <skvhttp/dto/SKVRecord.h>
#include <skvhttp/client/SKVClient.h>
#include <skvhttp/dto/Expression.h>

#include "commands/dbcommands.h"
#include "catalog/pg_foreign_table.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "commands/tablecmds.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/date.h"
#include "utils/syscache.h"
#include "utils/partitionkey.h"
#include "utils/palloc.h"
#include "catalog/heap.h"
#include "optimizer/var.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/subselect.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "knl/knl_thread.h"
#include "utils/lsyscache.h"
#include "access/k2/pg_gate_api.h"
#include "access/k2/storage.h"
#include "access/k2/k2pg_aux.h"
#include "error_reporting.h"
#include "parse.h"
#include "log.h"

namespace k2fdw {
namespace sh=skv::http;

// Intermediate storage for push-down projection and expressions, as decided when we're asked to generate a plan
struct K2FdwPushDownState {
    /* Projection: list of attribute (column) numbers that we need to fetch from K2. */
    List *target_attrs{0};
    /*
     * Restriction clauses, divided into safe and unsafe to pushdown subsets.
     */
    List *remote_conds{0}; // conditions to be evaluated by k2
    List *local_conds{0};  // conditions to be evaluated by PG on the returned records
};

struct K2FdwExecState {
    /* The handle for the internal K2PG Select statement. */

    // parameters required for call to ExecSelect
    std::vector<K2PgConstraintDef> constraints;
    std::vector<int> targets_attrnum;
    bool forward_scan{true};
    K2PgSelectLimitParams limit_params;

    K2PgScanHandle* k2_handle{0};     /* the handle generated by pggate */
};


/*
 * k2GetForeignPaths
 * Step 0: Create possible access paths for a scan on the foreign table, which is the full
 * table scan plus available index paths (including the primary key scan path if any).
 */
void k2GetForeignPaths(PlannerInfo *root,
                       RelOptInfo *baserel,
                       Oid foreigntableid) {
    K2LOG_D(log::fdw, "k2GetForeignPaths ftoid:", foreigntableid);
    /* Create a ForeignPath node and it as the scan path */
    add_path(root, baserel,
             (Path *)create_foreignscan_path(root,
                                             baserel,
                                             0.001,  // From MOT
                                             0.0,    // TODO cost: test to see if things work fine with these values
                                             NIL,    /* no pathkeys */
                                             NULL,   /* no outer rel either */
                                             NULL,   /* no extra plan */
                                             0 /* no options yet */));

    /* Add primary key and secondary index paths also */
    create_index_paths(root, baserel);
}

/*
 * k2GetForeignRelSize
 *      Step 1 in the scan setup
 *      Obtain relation size estimates for a foreign table
 */
void k2GetForeignRelSize(PlannerInfo *root,
                         RelOptInfo *baserel,
                         Oid foreigntableid) {
    K2LOG_D(log::fdw, "k2GetForeignRelSize ftoid: ", foreigntableid);
    K2FdwPushDownState *pushdown_state = (K2FdwPushDownState *)palloc0(sizeof(K2FdwPushDownState));
    pushdown_state->target_attrs = NIL;
    pushdown_state->remote_conds = NIL;
    pushdown_state->local_conds = NIL;

    /* Set the estimate for the total number of rows (tuples) in this table. */
    baserel->tuples = 1000;

    /*
     * Initialize the estimate for the number of rows returned by this query.
     * This does not yet take into account the restriction clauses, but it will
     * be updated later by camIndexCostEstimate once it inspects the clauses.
     */
    baserel->rows = baserel->tuples;

    baserel->fdw_private = (void *)pushdown_state;

    ListCell *lc;
    K2LOG_D(log::fdw, "k2GetForeignRelSizebase restrictinfos: {}", list_length(baserel->baserestrictinfo));

    foreach (lc, baserel->baserestrictinfo) {
        RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
        if (is_foreign_expr(root, baserel, ri->clause)) {
            K2LOG_D(log::fdw, "classified as remote baserestrictinfo: {}", nodeToString(ri));
            pushdown_state->remote_conds = lappend(pushdown_state->remote_conds, ri);
        } else {
            K2LOG_D(log::fdw, "classified as local baserestrictinfo: {}", nodeToString(ri));
            pushdown_state->local_conds = lappend(pushdown_state->local_conds, ri);
        }
    }
    K2LOG_D(log::fdw, "classified remote_conds: {}", list_length(pushdown_state->remote_conds));

    /*
     * Test any indexes of rel for applicability also.
     */

    // check_index_predicates(root, baserel);
    check_partial_indexes(root, baserel);
}

/*
 * k2GetForeignPlan
 *       Step 2: Create a ForeignScan plan node for scanning the foreign table
 *       here we can evaluate the fields which the planner wants projected by this FDW
 */
ForeignScan *
k2GetForeignPlan(PlannerInfo *root,
                 RelOptInfo *baserel,
                 Oid foreigntableid,
                 ForeignPath *best_path,
                 List *tlist,
                 List *scan_clauses) {
    K2LOG_D(log::fdw, "ftoid: ", foreigntableid);

    K2FdwPushDownState *pushdown_state = (K2FdwPushDownState *)baserel->fdw_private;
    Index scan_relid;
    ListCell *lc;

    // these are lists we build to return back to PG as part of the plan
    List *local_exprs = NIL;
    List *remote_exprs = NIL;

    K2LOG_D(log::fdw, "fdw_private {} remote_conds and {} local_conds for foreign relation {}",
            list_length(pushdown_state->remote_conds), list_length(pushdown_state->local_conds), foreigntableid);

    if (IS_SIMPLE_REL(baserel)) {
        scan_relid = baserel->relid;
        /*
         * Separate the restrictionClauses into those that can be executed remotely
         * and those that can't.  baserestrictinfo clauses that were previously
         * determined to be safe or unsafe are shown in fpinfo->remote_conds and
         * fpinfo->local_conds.  Anything else in the restrictionClauses list will
         * be a join clause, which we have to check for remote-safety.
         */
        K2LOG_D(log::fdw, "GetForeignPlan with {} scan_clauses for simple relation {}", list_length(scan_clauses), scan_relid);
        foreach (lc, scan_clauses) {
            RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);
            K2LOG_D(log::fdw, "classifying scan_clause: {}", nodeToString(rinfo));

            /* Ignore pseudoconstants, they are dealt with elsewhere */
            if (rinfo->pseudoconstant) {
                K2LOG_D(log::fdw, "pseudoconstant scan_clause");
                continue;
            }

            // the list_member_ptr ops are linear scans. Probably fine for query use cases since the number of conditions
            // should be small, but it does technically make this method O(N^2)
            if (list_member_ptr(pushdown_state->remote_conds, rinfo)) {
                K2LOG_D(log::fdw, "remote expr scan_clause");
                remote_exprs = lappend(remote_exprs, rinfo->clause);
            } else if (list_member_ptr(pushdown_state->local_conds, rinfo)) {
                K2LOG_D(log::fdw, "local expr scan_clause");
                local_exprs = lappend(local_exprs, rinfo->clause);
            } else if (is_foreign_expr(root, baserel, rinfo->clause)) {
                K2LOG_D(log::fdw, "foreign(remote) scan_clause");
                remote_exprs = lappend(remote_exprs, rinfo->clause);
            } else {
                K2LOG_D(log::fdw, "default(local) scan_clause");
                local_exprs = lappend(local_exprs, rinfo->clause);
            }
        }
        K2LOG_D(log::fdw, "classified {} scan_clauses for relation {}: remote_exprs: {}, local_exprs: {}",
                list_length(scan_clauses), scan_relid, list_length(remote_exprs), list_length(local_exprs));
    } else {
        K2LOG_D(log::fdw, "non-simple relation");

        /*
         * Join relation or upper relation - set scan_relid to 0.
         */
        scan_relid = 0;
        /*
         * For a join rel, baserestrictinfo is NIL and we are not considering
         * parameterization right now, so there should be no scan_clauses for
         * a joinrel or an upper rel either.
         */
        Assert(!scan_clauses);

        /*
         * Instead we get the conditions to apply from the fdw_private
         * structure.
         */
        remote_exprs = extract_actual_clauses(pushdown_state->remote_conds, false);
        local_exprs = extract_actual_clauses(pushdown_state->local_conds, false);
    }

    scan_clauses = extract_actual_clauses(scan_clauses, false);

    /* Get the target columns that need to be retrieved from K2 platform into a bitmapset*/
    // TODO taken from MOT - test to make sure this works
    Bitmapset* target_attr_bitmap{0};
    pull_varattnos((Node *)baserel->reltargetlist, baserel->relid, &target_attr_bitmap);

    K2LOG_D(log::fdw, "setting scan targets");
    /* Process the above bitmapset to setup the scan targets (projection) */
    bool wholerow = false;
    for (AttrNumber attnum = baserel->min_attr; attnum <= baserel->max_attr; attnum++) {
        int bms_idx = attnum - baserel->min_attr + 1;
        // if we want the wholerow, or this attribute is in the bitmapset, then use it
        if (wholerow || bms_is_member(bms_idx, target_attr_bitmap)) {
            switch (attnum) {
                case InvalidAttrNumber:
                    /*
                     * Postgres repurposes InvalidAttrNumber to represent the "wholerow"
                     * junk attribute.
                     */
                    K2LOG_D(log::fdw, "wholerow select due to invalid attnum {}", attnum);
                    wholerow = true;
                    break;
                default: /* valid column - ask for it */
                {
                    K2LOG_D(log::fdw, "new target for regular column attnum {}", attnum);
                    TargetEntry *target = makeNode(TargetEntry);
                    target->resno = attnum;
                    pushdown_state->target_attrs = lappend(pushdown_state->target_attrs, target);
                }
            }
        }
    }

    /* Create the ForeignScan node */
    return make_foreignscan(tlist,        /* target list */
                            scan_clauses, /* ideally we should use local_exprs here, still use the whole list in case the FDW cannot process some remote exprs*/
                            scan_relid,
                            remote_exprs,                /* expressions K2 may evaluate */
                            pushdown_state->target_attrs); /* store the computed list of target attributes */
                                                         // nullptr,
    // nullptr,
    // nullptr);

    // After this call, we would have a complete scan plan created which for now just holds our K2FdwPushDownState
}

/*
* Step 3. Initiate the scan
*/
void
k2BeginForeignScan(ForeignScanState *node, int eflags)
{
    K2LOG_D(log::fdw, "BeginForeignScan");
    Relation relation = node->ss.ss_currentRelation;

    // created in k2GetForeignPlan...
    ForeignScan *foreignScan = (ForeignScan *) node->ss.ps.plan;

    /* Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL. */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;


    /* Allocate and initialize K2PG scan state. */
	K2FdwExecState *k2pg_state = new K2FdwExecState();

    node->fdw_state = (void *)k2pg_state;

    ListCell *lc{0};
    // go over the target attribute numbers we stored before in the fdw_private
    // and add them to the pggate's targets vector
    foreach (lc, foreignScan->fdw_private) {
        TargetEntry *target = (TargetEntry *)lfirst(lc);
        K2LOG_D(log::fdw, "projecting target attribute {}", target->resno);
        k2pg_state->targets_attrnum.push_back(target->resno);
    }

    // parse push-down clauses
    ParamListInfo paramLI = node->ss.ps.state->es_param_list_info;
    parse_conditions(foreignScan->fdw_exprs, paramLI, k2pg_state->constraints);

    K2PgSelectIndexParams index_params;

    switch (nodeTag(node)) {
        case T_IndexScan: {
            K2LOG_D(log::fdw, "index scan");
            index_params.index_only_scan = false;
            k2pg_state->forward_scan = ((IndexScan*) node)->indexorderdir == ForwardScanDirection;
            index_params.index_oid = ((IndexScan *)node)->indexid;
            index_params.use_secondary_index = true;
            break;
        }
        case T_IndexOnlyScan: {
            K2LOG_D(log::fdw, "index-only scan");
            index_params.index_only_scan = true;
            k2pg_state->forward_scan = ((IndexOnlyScan *)node)->indexorderdir == ForwardScanDirection;
            index_params.index_oid = ((IndexOnlyScan *)node)->indexid;
            index_params.use_secondary_index = true;
            break;
        }
        default: {
            K2LOG_D(log::fdw, "default forward scan true");
            index_params.index_only_scan = false;
            k2pg_state->forward_scan = true;
            index_params.index_oid = InvalidOid;
            index_params.use_secondary_index = false;
        }
    }

    k2pg_state->limit_params.limit_count = 0;  // TODO the value of SELECT ... LIMIT
    k2pg_state->limit_params.limit_offset = 0;    // TODO the value of SELECT ... OFFSET
    k2pg_state->limit_params.limit_use_default = true;

    HandleK2PgStatus(PgGate_NewSelect(K2PgGetDatabaseOid(relation), RelationGetRelid(relation),
                                      std::move(index_params), &k2pg_state->k2_handle));

    // TODO Add this back when we consolidate PGStatement and K2PGScanHandle
    /* Set the current syscatalog version (will check that we are up to date) */
    // HandleK2PgStatus(PgGate_SetCatalogCacheVersion(k2pg_state->k2_handle,
    //                                                    k2pg_catalog_cache_version));
    K2LOG_D(log::fdw, "foreign_scan for relation {}, fdw_exprs: {}", relation->rd_id, list_length(foreignScan->fdw_exprs));

    K2LOG_D(log::fdw, "BeginForeignScan done");
}

/*
 * k2IterateForeignScan
 *        Step 4: Read next record from the data file and store it into the
 *        ScanTupleSlot as a virtual tuple
 */
TupleTableSlot *
k2IterateForeignScan(ForeignScanState *node)
{
    K2LOG_D(log::fdw, "IterateForeignScan");
    TupleTableSlot *slot= nullptr;
    K2FdwExecState *k2pg_state = (K2FdwExecState *) node->fdw_state;
    Relation relation = node->ss.ss_currentRelation;

    HandleK2PgStatus(PgGate_ExecSelect(k2pg_state->k2_handle, k2pg_state->constraints,
                    k2pg_state->targets_attrnum, k2pg_state->forward_scan, k2pg_state->limit_params));

    /* Clear tuple slot before starting */
    slot = node->ss.ss_ScanTupleSlot;
    ExecClearTuple(slot);

    K2LOG_D(log::fdw, "IterateForeignScan tuple prep done");

    /* Fetch one row. */
    bool            has_data   = false;
    TupleDesc       tupdesc = slot->tts_tupleDescriptor;
    Datum           *values = slot->tts_values;
    bool            *isnull = slot->tts_isnull;
    K2PgSysColumns    syscols;
    HandleK2PgStatus(PgGate_DmlFetch(k2pg_state->k2_handle,
                                          tupdesc->natts,
                                          (uint64_t *) values,
                                          isnull,
                                          &syscols,
                                          &has_data));

    /* If we have result(s) update the tuple slot. */
    if (has_data) {
        HeapTuple tuple = heap_form_tuple(tupdesc, values, isnull);
        if (syscols.oid != InvalidOid) {
            HeapTupleSetOid(tuple, syscols.oid);
        }

        slot = ExecStoreTuple(tuple, slot, InvalidBuffer, false);

        /* Setup special columns in the slot */
        slot->tts_k2pgctid = PointerGetDatum(syscols.k2pgctid);
    }

    return slot;
}

/*
 * Step 5. Done with scan
 */
void k2EndForeignScan(ForeignScanState *node) {
    K2FdwExecState *k2pg_state = (K2FdwExecState *) node->fdw_state;
    if (k2pg_state != NULL) {
	    delete k2pg_state;
    }

    K2LOG_D(log::fdw, "End foreignscan called");
}

}  // namespace k2fdw
