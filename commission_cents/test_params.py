#!/usr/bin/env python3

import psycopg

with psycopg.connect() as conn:
    with conn.cursor() as cur:
        # To attach a debugger:
        # cur.execute("SELECT pg_backend_pid()")
        # print(cur.fetchone())
        # input()

        cur.execute("SELECT commission_cents(1, 10000)")
        print(cur.fetchone())

        cur.execute("SELECT commission_cents(1, null)")
        print(cur.fetchone())

        cur.execute("SELECT commission_cents(1, %s)", (None,))
        print(cur.fetchone())
