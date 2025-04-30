#include <postgres.h>
#include <fmgr.h>
#include <catalog/pg_type.h>
#include <executor/functions.h>
#include <nodes/makefuncs.h>
#include <nodes/nodes.h>
#include <nodes/pathnodes.h>
#include <nodes/print.h>
#include <nodes/supportnodes.h>
#include <tcop/tcopprot.h>
#include <utils/builtins.h>
#include <utils/syscache.h>

/*
 * Borrow this from util/adt/ri_triggers.c
 * since we do similar SQL-building to there:
 */
#define MAX_QUOTED_NAME_LEN  (NAMEDATALEN*2+3)
#define MAX_QUOTED_REL_NAME_LEN  (MAX_QUOTED_NAME_LEN*2)

PG_MODULE_MAGIC;

Datum commission_cents_support(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(commission_cents_support);

/*
 * Inline the function call.
 *
 * If we know up front there is no salesperson, the commission will always be $0.
 */
Datum
commission_cents_support(PG_FUNCTION_ARGS)
{
    Node *rawreq = (Node *) PG_GETARG_POINTER(0);
    SupportRequestSimplify *req;
    FuncExpr *expr;
	Node *node;

    /* We only handle Simplify support requests. */
    if (!IsA(rawreq, SupportRequestSimplify))
        PG_RETURN_POINTER(NULL);

    req = (SupportRequestSimplify *) rawreq;
    expr = req->fcall;

    if (list_length(expr->args) != 2)
    {
        ereport(WARNING, (errmsg("commission_cents_support called with %d args but expected 2", list_length(expr->args)))); 
        PG_RETURN_POINTER(NULL);
    }

    /*
     * Extract salesperson id from the func's arguments.
     * It must be Const and INT4.
	 * 
	 * I don't see a way to get access to a ParamListInfo,
	 * but if we added boundParams to SupportRequestSimplify
	 * and set it from the eval_const_expressions_context
	 * in simplify_function, we could use that to detect more constants.
     */
    node = lsecond(expr->args);
	if (IsA(node, Const)) {
		Const *c = (Const *) node;

		if (c->consttype != INT4OID) {
			ereport(WARNING, (errmsg("commission_cents_support called with non-INT4 parameter")));
			PG_RETURN_POINTER(NULL);
		}

		if (c->constisnull) {
			Const *ret = makeConst(
				INT4OID,			// type
				-1,					// typmod
				0,					// collid
				4,					// len
				Int32GetDatum(0),	// value
				false,				// isnull
				true				// byval
			);
			ereport(NOTICE, (errmsg("commission_cents_support inlining a constant zero")));
			PG_RETURN_POINTER(ret);
		}
	}

	ereport(NOTICE, (errmsg("commission_cents_support called with non-constant parameter")));
	PG_RETURN_POINTER(NULL);
}
