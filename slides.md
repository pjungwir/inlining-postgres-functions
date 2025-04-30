# Inlining Postgres Functions Now and Then

Paul A. Jungwirth

12 May 2025

Notes:

- Hi, I'm Paul Jungwirth, a freelance programmer from Portland, Oregon.
- I've been using Postgres for at least 15 years, and building extensions for most of that time.



# Lineup

- Inlining SQL Set-Returning Functions
- Inlining Non-SRF SQL Functions
- Inlining Non-SQL Set-Returning Functions

Notes:

- I want to talk about how Postgres inlines functions.
- I don't mean inlined C functions, I mean the functions you call from SQL.
    - Mostly I'm talking about User-Defined Functions.
    - Postgres could inline its own built-in functions too, but getting *your* functions inlined is more interesting.
- I've got these three categories I want to cover:
    - Set-Returning Functions (SRFs as I like to call them) implemented in SQL
    - Other functions implemented in SQL
    - SRFs not implemented in SQL
- These are all interesting to a Postgres developer and an extension author.
- For extension authors, I think the last one is the most interesting.



# SRFs

```
CREATE OR REPLACE FUNCTION visible_sales(user_id INT)
RETURNS SETOF sales
AS $$
  SELECT * FROM sales
  WHERE vendor_id IN (SELECT company_id
                      FROM   memberships
                      WHERE  user_id = $1);
$$ LANGUAGE sql STABLE;
```

Notes:

- In case you don't know, a Set-Returning Function is a function that returns more than one result: typically more than one row.
- Let's say we have a multi-tenant e-commerce application.
    - There are users and companies and sales.
    - A user can belong to more than one company.
    - Can everyone see all the sales? No way!
    - You can only see the sales from the companies you belong to.
    - It is convenient to wrap up this logic into a function: `visible_sales`.
    - You can think of this function as a parameterized view.
      - It returns sales records, but only the ones you can see.
      - Unlike a regular view, it accepts an input: the user id.
    - This example is based on several real customers I've worked with, btw---
      except their visibility functions were much more complicated and costly.
      - You probably have permissions logic too.
        - Wouldn't it be nice to encapsulate it?



# Inlining SQL SRFs

```console[|2-3||7|7-10]
=# EXPLAIN ANALYZE SELECT  *
FROM    visible_sales_slow(1) AS s
WHERE   vendor_id = 5000;
               QUERY PLAN
-------------------------------------------------
 Function Scan on visible_sales_slow s
   (cost=0.25..12.75 rows=5 width=56)
   (actual time=57.415..57.670 rows=2 loops=1)
   Filter: (vendor_id = 5000)
   Rows Removed by Filter: 51688
 Planning Time: 0.129 ms
 Execution Time: 57.925 ms
(5 rows)
```

Notes:

- Ideally our `visible_sales` function would compose well with the other parts of our query.
- Encapsulation shouldn't cost us performance.
- Here we're looking up user 1's visible sales, but only for a specific vendor.
  - This plan is not what we want.
- Postgres will call our function to find all the visible sales, then filter them.
- Postgres thinks the function will get 5 rows, but actually user 1 can see thousands of sales. Oops!
- On the other hand, vendor 5000 only has two sales!
  - We threw out over 50 thousand rows.
  - Can't we just go straight to the ones that matter?
  - Our query time was 58 milliseconds.



# Inlining SQL SRFs

```console[|7|4-7|8-11]
 Nested Loop
   (cost=0.84..32.53 rows=7 width=27)
   (actual time=0.044..0.048 rows=2 loops=1)
   ->  Index Only Scan using idx_memberships_company_user on memberships
         (cost=0.42..4.44 rows=1 width=4)
         (actual time=0.025..0.026 rows=1 loops=1)
         Index Cond: ((company_id = 5000) AND (user_id = 1))
   ->  Index Scan using uq_sales_po_number on sales
       (cost=0.42..28.02 rows=7 width=27)
       (actual time=0.013..0.016 rows=2 loops=1)
         Index Cond: (vendor_id = 5000)
 Planning Time: 0.258 ms
 Execution Time: 0.071 ms
```

Notes:

- This would be a lot better. Less than a tenth of a millisecond, compared to 50+.
    - Instead of calling the function and going over all the sales,
      Postgres merges it into the rest of the query.
      - Basically the planner removed the `FuncCall` node and replaced it with a `Query` node based on the function definition.
      - So first of all there is no function call overhead.
      - But that's just the beginning.
        - The planner thinks we just have a subquery here, and it can apply all its tricks.
        - Its estimates are much more accurate: none of this "I guess a function returns about 5 rows, right?"
        - And best of all, it can rearrange things.
        - Look at this: our company filter is pushed down into the query on memberships!
        - And btw that is an index-only scan, because we only need the company id.
        - For each of those we'll find the sales with an index.
            - Postgres estimates 7 and gets 2, pretty close.

- As long as we can declare our function as `STABLE`, this is what we get.
    - `STABLE` means that our result might depend on what the tables contain, but otherwise it will give the same result for the same inputs:
        - There is no randomness or time-dependence.
        - We don't modify anything.

- Btw this is not very well-known, I think.
    - It's not in the Postgres docs (but I'll submit a patch).
    - There is a wiki page about it.
    - I forget how I found out about that.
    - It's even missing from several books I know about optimizing Postgres SQL queries.

- So now we needn't fear performance when encapsulating logic inside functions.
- This is great news for SQL developers, and maybe extension developers too.
- But note this only works for SQL functions: not PL/pgSQL functions, not C functions, Python functions, etc.
    - Postgres has to be able to "see into" your function.
    - It has to be able to convert it into a plan tree.
    - Postgres can only do this for SQL functions, because sadly, we have not yet solved the Entsheidungsproblem. TODO sp?
        - Some so-called "computer scientists" claim that we never will.



# Inlining SQL SRFs

```console[|3|22-23|11-15,22-23|18-21]
=# EXPLAIN (ANALYZE) SELECT  *
FROM    visible_sales(1) AS s
WHERE   vendor_id = 2 LIMIT 10;
                   QUERY PLAN
-----------------------------------------------------
 Limit  (cost=0.42..5.84 rows=10 width=27)
        (actual time=0.972..1.010 rows=10 loops=1)
   ->  Nested Loop Semi Join
         (cost=0.42..2722.73 rows=5020 width=27)
         (actual time=0.970..1.004 rows=10 loops=1)
         ->  Seq Scan on sales
               (cost=0.00..2655.54 rows=5020 width=27)
               (actual time=0.868..0.896 rows=10 loops=1)
               Filter: (vendor_id = 2)
               Rows Removed by Filter: 134
         ->  Materialize  (cost=0.42..4.44 rows=1 width=4)
                          (actual time=0.010..0.010 rows=1 loops=10)
               ->  Index Only Scan using idx_memberships_company_user on memberships
                     (cost=0.42..4.44 rows=1 width=4)
                     (actual time=0.092..0.092 rows=1 loops=1)
                     Index Cond: ((company_id = 2) AND (user_id = 1))
 Planning Time: 0.515 ms
 Execution Time: 1.058 ms
(11 rows)
```

Notes:

- Here's another example, with two changes:
    - We're looking at a company that *does* have a lot of sales.
    - But we're paginating the results, showing only 10 per page.
    - It's still fast!
    - We scan all of sales, but we quit after 10 rows.
    - The permission check is just an index-only scan.
    - Again, these benefits come from inlining.



# `temporal_semijoin`
<!-- .element class="r-fit-text" -->

```sql
SELECT  a.id,
        UNNEST(multirange(a.valid_at) * j.valid_at) AS valid_at
FROM    a
JOIN (
  SELECT  b.id, range_agg(b.valid_at) AS valid_at
  FROM    b
  GROUP BY b.id
) AS j
ON a.id = j.id AND a.valid_at && j.valid_at;
```
<!-- .element style="width:100%" -->

from [](https://github.com/pjungwir/temporal_ops)

Notes:

- TODO: print and linkify without repeating myself
- Here is a query I wish I could generalize then inline.
- It implements a semijoin between two temporal tables.
- You may know that the SQL:2011 standard introduces a bunch of new features for "temporal" tables: tables that keep a history of their subject over time.
    - But the standard doesn't give you anything for temporal outer join, semi-join, anti-join, aggregates, union, intersect, or except.
    - I've got a github repo with SQL-based implementations for those, but ideally you'd wrap them up in functions, like this one.....



# `temporal_semijoin`
<!-- .element class="r-fit-text" -->

```sql
CREATE OR REPLACE FUNCTION temporal_semijoin(
  left_table text, left_id_col text, left_valid_col text,
  right_table text, right_id_col text, right_valid_col text
)
RETURNS SETOF RECORD AS $$
DECLARE
  subquery TEXT := 'j';
BEGIN
  IF left_table = 'j' OR right_table = 'j' THEN
    subquery := 'j1';
    IF left_table = 'j1' OR right_table = 'j1' THEN
      subquery := 'j2';
    END IF;
  END IF;
  RETURN QUERY EXECUTE format($j$
    SELECT  %1$I.%2$I, UNNEST(multirange(%1$I.%3$I) * %7$I.%6$I) AS %3$I
    FROM    %1$I
    JOIN (
      SELECT  %4$I.%5$I, range_agg(%4$I.%6$I) AS %6$I
      FROM    %4$I
      GROUP BY %4$I.%5$I
    ) AS %7$I
    ON %1$I.%2$I = %7$I.%5$I AND %1$I.%3$I && %7$I.%6$I;
  $j$, left_table, left_id_col, left_valid_col, right_table, right_id_col, right_valid_col, subquery);
END;
$$ STABLE LEAKPROOF PARALLEL SAFE SUPPORT temporal_semijoin_support LANGUAGE plpgsql;
```
<!-- .element style="margin-top:0px; margin-bottom:0px; font-size:0.35em" -->

Notes:
- This function is great: you give it the table and column names,
  and it builds the last query with them.
- The problem is, then your performance crashes.
- If only Postgres could build this SQL, then inline it!
- We are sooo close. Let me show you something else....



# Inlining Non-SRFs

```sql
CREATE OR REPLACE FUNCTION commission_cents(
  _sale_id INTEGER, _salesperson_id INTEGER
)
RETURNS INTEGER
AS $$
  SELECT  total_price_cents * COALESCE(commission_percent, 0)
  FROM    sales AS s
  LEFT JOIN memberships AS m
  ON      m.company_id = s.vendor_id
  AND     m.user_id = _salesperson_id
  WHERE   s.id = _sale_id;
$$ LANGUAGE sql STABLE;
```

Notes:

- Now there *is* a way for functions to advertise a plan tree that is equivalent to calling the function.
- Let's say we have this function, to compute the commission a sales person should receive.
  - We get the sale price from the `sales` table, and the commission rate from the `memberships` table.
- But a sale might not have a salesperson, and in that case the commission should be zero.
  - In that case we don't really have to look up anything.
  - Postgres has a way to let us do that.



# Support Procs

```console[|13]
=# \d pg_proc
                   Table "pg_catalog.pg_proc"
     Column      |     Type     | Collation | Nullable | Default
-----------------+--------------+-----------+----------+---------
 oid             | oid          |           | not null |
 proname         | name         |           | not null |
 pronamespace    | oid          |           | not null |
 proowner        | oid          |           | not null |
 prolang         | oid          |           | not null |
 procost         | real         |           | not null |
 prorows         | real         |           | not null |
 provariadic     | oid          |           | not null |
 prosupport      | regproc      |           | not null |
 prokind         | "char"       |           | not null |
 prosecdef       | boolean      |           | not null |
 proleakproof    | boolean      |           | not null |
 proisstrict     | boolean      |           | not null |
 proretset       | boolean      |           | not null |
 provolatile     | "char"       |           | not null |
 proparallel     | "char"       |           | not null |
 pronargs        | smallint     |           | not null |
 pronargdefaults | smallint     |           | not null |
 prorettype      | oid          |           | not null |
 proargtypes     | oidvector    |           | not null |
 proallargtypes  | oid[]        |           |          |
 proargmodes     | "char"[]     |           |          |
 proargnames     | text[]       | C         |          |
 proargdefaults  | pg_node_tree | C         |          |
 protrftypes     | oid[]        |           |          |
 prosrc          | text         | C         | not null |
 probin          | text         | C         |          |
 prosqlbody      | pg_node_tree | C         |          |
 proconfig       | text[]       | C         |          |
 proacl          | aclitem[]    |           |          |
```

Notes:

- Here is `pg_proc`, from the catalog.
- Every function can carry around a helper function, called a "support function".
- Support functions answer various questions at plan time.



# Support Requests

- `SupportRequestRows`
- `SupportRequestSelectivity`
- `SupportRequestCost`
- `SupportRequestIndexCondition`
- `SupportRequestWFuncMonotonic`
- `SupportRequestOptimizeWindowClause`
- `SupportRequestModifyInPlace`
- `SupportRequestSimplify`

Notes:

- Each kind of question is a "SupportRequest".
- If a support function is present, it will get asked all these questions.
- If it doesn't know how to answer a certain kind of question, it just returns NULL.
- There are eight support requests. Here they are.
    - The first few help the planner make decisions:
        - How many rows will you return?
        - What is your selectivity?
        - What is your cost?
    - Then there are some fancy ones:
        - What is an index condition to pre-filter rows going into this function? I think PostGIS uses this a lot.
        - I feel like you could write some date functions that take advantage of this.
            - It's easy to accidentally write date conditions that don't use indexes.
        - There are a couple to help window functions.
        - There is one to modify composite values in-place, like arrays or json.
            - If you have a fancy custom type, this might help you.
    - Then the fanciest of all is `SupportRequestSimplify`.
        - This lets a function replace itself with a plan tree.
        - In the docs this is used for mathematical identities, like "anything plus zero" should just be anything.
            - That's our commission example.




# Inlining Non-SRFs

```sql
SELECT  total_price_cents,
        commission_cents(id, salesperson_id)
FROM    sales
WHERE   salesperson_id = $1
AND     sold_at BETWEEN start_of_month($2)
                    AND end_of_month($2)
```

Notes:

- Now suppose we are computing the monthly commission for each salesperson, to write each one a check.
- This query gives us a report for any month and any salesperson.
- But we also want to know the sales with *no* salesperson.
  - This function can do that too: just set `$1` to null.
- In that case the commission should be zero.
- But maybe there are a lot of those sales.
    - Do we really want to call the function over & over just to get zero every time?
    - We know at *plan time* that we're always passing NULL.
- It might be worth it to give this function a support proc that can handle `SupportRequestSimplify`.



# `commission_cents_support`
<!-- .element class="r-fit-text" -->

```sql[|13]
CREATE OR REPLACE FUNCTION commission_cents(
  _sale_id INTEGER, _salesperson_id INTEGER
)
RETURNS INTEGER
AS $$
  SELECT  total_price_cents * COALESCE(commission_percent, 0)
  FROM    sales AS s
  LEFT JOIN memberships AS m
  ON      m.company_id = s.vendor_id
  AND     m.user_id = _salesperson_id
  WHERE   s.id = _sale_id;
$$ LANGUAGE sql STABLE
SUPPORT commission_cents_support;
```

Notes:

- Here is the same function, but with a new last line.
  - We're declaring a support function.



# `commission_cents_support`
<!-- .element class="r-fit-text" -->

```sql
CREATE OR REPLACE FUNCTION commission_cents_support(INTERNAL)
RETURNS INTERNAL
AS 'commission_cents', 'commission_cents_support'
LANGUAGE C;
```

Notes:

- Here is the declaration for that function.
- Of course all the details are in C.



# `commission_cents_support`
<!-- .element class="r-fit-text" -->

```c[|2|2-6|8-10]
Datum commission_cents_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *) PG_GETARG_POINTER(0);
  SupportRequestSimplify *req;

  if (!IsA(req, SupportRequestSimplify)) {
    PG_RETURN_POINTER(NULL);

  req = (SupportRequestSimplify *) rawreq;

  FuncExpr *expr = req->fcall;

  ...
```

Notes:

- Here is roughly the C code for this.
- You can find the complete code in the github repo for these slides.
- The first argument is always the support request struct, which is a Node.
	- It could be any of the support request types, so we have to make sure it's one we handle.
	- It gives us the parse tree for this function call, so we grab that.



# `commission_cents_support`
<!-- .element class="r-fit-text" -->

```c
typedef struct SupportRequestSimplify
{   
    NodeTag     type;
    
    struct PlannerInfo *root;
    FuncExpr   *fcall;
} SupportRequestSimplify;
```

Notes:

- Here is the `SupportRequestSimplify` struct btw.
- We have to use what's here to see if we can rewrite the function.
- We have the function call, including its arguments,
- and we have a PlannerInfo, which gives us some context if we need it.



# `commission_cents_support`
<!-- .element class="r-fit-text" -->

```c[|1|2-15|17]
Node *node = lsecond(expr->args);
if (IsA(node, Const)) {
  Const *c = (Const *) node;
  if (c->constisnull) {
    Const *ret = makeConst(
      INT4OID,          /* type */
      -1,               /* typmod */
      0,                /* collid */
      4,                /* len */
      Int32GetDatum(0), /* value */
      false,            /* isnull */
      true              /* byval */
    );
    PG_RETURN_POINTER(ret);
  }
}
PG_RETURN_POINTER(NULL);
```

Notes:

- Okay back to our support function....
- I'm skipping lots of error checking.
- We get the function's second argument, the sales person id.
- If it's a Const node and it's NULL,
  - then our result will always be a 0.
- We can replace the whole function call with that.
- Otherwise we return NULL, saying we can't make any simplifications.

- Note that I'm not supporting Param nodes.
    - That's a $1, $2, etc., such as you'd use to call this from an application.
    - This is a shame, because those may be constant too.
	- I tried to make that work, but I couldn't get access to their bound values from the support function.
    - It seems like this should be available from a few accessible structs, but when I tried the `ParamListInfo` was always null:
        - in the `PlannerGlobal` you can get off the `root` `PlannerInfo` passed to your support function,
        - and even the `context` passed to `simplify_function`!
        - Maybe those just haven't been set up yet?
        - In principle we should have access to them, because they are passed when the client sends the query (unless we're in a prepared statement).

- Now, your support function doesn't *have* to return a `Const`.
- That's what this feature was designed for, I guess, but you could build any node tree and return that.
    - So does this let us inline `temporal_semijoin`? What if we just return whole a `Query` node?
    - Sadly no, because the planner doesn't use this to simplify set-returning functions.
    - The work to inline SQL-language functions happens in a completely different place.



# There's a Patch for That!
<!-- .element class="r-fit-text" -->

TODO: commitfest link? Commit message? Diff snippet?

Notes:

- Okay, but what if we patched Postgres?
- Over in that place where we inline SQL-language SRFs, let's accept a new Support Request to inline a function in any language.
- So I have a patch for this.
    - It got neglected while I was fixing bugs in temporal foreign keys, but I'm starting to pick it up again.
    - I think it just needs some refactoring.



# `SupportRequestInlineSRF`
<!-- .element class="r-fit-text" -->

```c[|1|2|3-8|9-10]
char *sql = "....";
List *parsed = pg_parse_query(sql);
List *analyzed = pg_analyze_and_rewrite_with_cb(
  linitial(parsed),
  sql,
  (ParserSetupHook) sql_fn_parser_setup,
  pinfo,
  NULL);
Query *q = linitial(analyzed);
PG_RETURN_POINTER(q);
```

Notes:

- Here you go.
    - This is what you would write as the support function author.
- Once again error checking is wildly absent.
- I'm not even checking for `Const` inputs.
- Somehow you build your SQL string.
- Then Postgres gives us everything we need.
    - You parse it.
    - You do analysis and rewriting.
    - That's your result.
- This is more or less what the SQL-inlining code is doing.



# Caveats

Notes:

- Support functions have to be written in C.
    - This makes it hard to users to access, since so many people these days are in the Cloud.
    - You could have some kind of C shim that dispatches to a user-supplied plpgsql function.
        - This would allay Cloud vendors' exploit fears, but not necessarily all the security fears of the database owner.



# Generalizing

```sql[|15-27]
CREATE OR REPLACE FUNCTION temporal_semijoin(
  left_table text, left_id_col text, left_valid_col text,
  right_table text, right_id_col text, right_valid_col text
)
RETURNS SETOF RECORD AS $$
DECLARE
  subquery TEXT := 'j';
BEGIN
  IF left_table = 'j' OR right_table = 'j' THEN
    subquery := 'j1';
    IF left_table = 'j1' OR right_table = 'j1' THEN
      subquery := 'j2';
    END IF;
  END IF;
  RETURN QUERY EXECUTE format($qqq$
    SELECT  %1$I.%2$I, UNNEST(multirange(%1$I.%3$I) * %7$I.%6$I) AS %3$I
    FROM    %1$I
    JOIN (
      SELECT  %4$I.%5$I, range_agg(%4$I.%6$I) AS %6$I
      FROM    %4$I
      GROUP BY %4$I.%5$I
    ) AS %7$I
    ON %1$I.%2$I = %7$I.%5$I AND %1$I.%3$I && %7$I.%6$I;
  $qqq$,
  left_table, left_id_col, left_valid_col,
  right_table, right_id_col, right_valid_col,
  subquery);
END;
$$ STABLE LEAKPROOF PARALLEL SAFE SUPPORT temporal_semijoin_support LANGUAGE plpgsql;
```
<!-- .element style="margin-top:0px; margin-bottom:0px; font-size:0.35em" -->

Notes:

- And maybe we can push things further.
- Go back to my `temporal_semijoin` function.
- We end with a `RETURN QUERY EXECUTE`
    - This has got to be a really common pattern.
    - Could plpgsql notice that your function does that, and that it's `STABLE`, and automatically attach a support function that:
        - Checks for all-`Const` arguments,
        - Runs the plpgsql up 'til the `RETURN QUERY EXECUTE`,
        - Uses the string input to build a query tree but not run it?
        - I think this might be feasible.
    - To really dream, what if we *did* accept non-`Const` parameters, as long as they just got passed as parameters to the `EXECUTE`, and we plumbed those all the way back to their source when we inlined the query? SQL-language inlining does this already.

- Anyway, I'm excited to see what people do with this, if it gets into core.
  - It's the kind of feature that lets people do things we haven't thought of yet.
  - Postgres lore is that we started out in Lisp, and our C is still kind of Lispy.
  - So what if we gave our users macros?



# Thank you!

TODO: github for this talk


Notes:

- Thanks for coming! Here is the github for this talk.
- I'm happy for questions, feedback, and flames.



# Bibliography

TODO: github for this talk
TODO: wiki page on sql inlining
TODO: commitfest for this patch
TODO: temporal_ops github repo
