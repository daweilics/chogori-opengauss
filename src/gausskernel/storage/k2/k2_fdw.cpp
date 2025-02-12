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


#include "k2_fdw.h"
#include "error_reporting.h"

#include "postgres.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_table.h"

#include "fdw_handlers.h"

namespace k2fdw {
/*
 * SQL functions
 */
extern "C" Datum k2_fdw_handler(PG_FUNCTION_ARGS);
extern "C" Datum k2_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(k2_fdw_handler);
PG_FUNCTION_INFO_V1(k2_fdw_validator);

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 * This func is called by the FDW system, when the FDW is loaded. The name of the function
 * is specified in the k2--1.0.sql DDL file and will be loaded by the load_plpgsql_function() func.
 */
Datum k2_fdw_handler(PG_FUNCTION_ARGS)
{
    static FdwRoutine routine{
        .type = T_FdwRoutine,
        /* Functions for scanning foreign tables */
        .GetForeignRelSize = k2GetForeignRelSize,
        .GetForeignPaths = k2GetForeignPaths,
        .GetForeignPlan = k2GetForeignPlan,
        .BeginForeignScan = k2BeginForeignScan,
        .IterateForeignScan = k2IterateForeignScan,
        .ReScanForeignScan = NULL,
        .EndForeignScan = k2EndForeignScan,

        /* Functions for updating foreign tables */
        .AddForeignUpdateTargets = NULL,
        .PlanForeignModify = NULL,
        .BeginForeignModify = NULL,
        .ExecForeignInsert = NULL,
        .ExecForeignUpdate = NULL,
        .ExecForeignDelete = NULL,
        .EndForeignModify = NULL,
        .IsForeignRelUpdatable = NULL,

        /* Support functions for EXPLAIN */
        .ExplainForeignScan = NULL,
        .ExplainForeignModify = NULL,

        /* Support functions for ANALYZE */
        .AnalyzeForeignTable = NULL,
        .AcquireSampleRows = NULL,

        .VecIterateForeignScan = NULL,
        .GetFdwType = NULL,
        .ValidateTableDef = NULL,
        .PartitionTblProcess = NULL,
        .BuildRuntimePredicate = NULL,
        .TruncateForeignTable = NULL,
        .VacuumForeignTable = NULL,
        .GetForeignRelationMemSize = NULL,
        .GetForeignMemSize = NULL,
        .GetForeignSessionMemSize = NULL,
        .NotifyForeignConfigChange = NULL};

    PG_RETURN_POINTER(&routine);
}

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct K2FdwOption {
    const char* m_optname;
    Oid m_optcontext; /* Oid of catalog in which option may appear */
};

/*
 * Valid options for file_fdw.
 * These options are based on the options for COPY FROM command.
 * But note that force_not_null is handled as a boolean option attached to
 * each column, not as a table option.
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileGetOptions(), which currently doesn't bother to look at user mappings.
 */
static const struct K2FdwOption valid_options[] = {

    {"null", ForeignTableRelationId},
    {"encoding", ForeignTableRelationId},
    {"force_not_null", AttributeRelationId},

    /* Sentinel */
    {NULL, InvalidOid}
};

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool is_valid_option(const char* option, Oid context)
{
    const struct K2FdwOption* opt;

    for (opt = valid_options; opt->m_optname; opt++) {
        if (context == opt->m_optcontext && strcmp(opt->m_optname, option) == 0)
            return true;
    }
    return false;
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses k2_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum k2_fdw_validator(PG_FUNCTION_ARGS)
{
    List* optionsList = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid catalog = PG_GETARG_OID(1);
    ListCell* cell = nullptr;

    foreach (cell, optionsList) {
        DefElem* def = (DefElem*)lfirst(cell);

        if (!is_valid_option(def->defname, catalog)) {
            const struct K2FdwOption* opt = nullptr;
            StringInfoData buf;

            /*
             * Unknown option specified, complain about it. Provide a hint
             * with list of valid options for the object.
             */
            initStringInfo(&buf);
            for (opt = valid_options; opt->m_optname; opt++) {
                if (catalog == opt->m_optcontext)
                    appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "", opt->m_optname);
            }

            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                    errmsg("invalid option \"%s\"", def->defname),
                    buf.len > 0 ? errhint("Valid options in this context are: %s", buf.data)
                                : errhint("There are no valid options in this context.")));
        }
    }

    PG_RETURN_VOID();
}


} // ns
