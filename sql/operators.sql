-- This file and its contents are licensed under the Apache License 2.0.
-- Please see the included NOTICE for copyright information and
-- LICENSE-APACHE for a copy of the license.

-- This file contains operators and associated functions that are
-- defined as part of TimescaleDB.

-- Functions to compare timestamps with ranges.
--
-- These are used to extend the timestamp ranges to also allow a
-- timestamp to denote a (very small) range in comparisons.
CREATE FUNCTION cmp_ts_rng(TIMESTAMP, TSRANGE)
RETURNS boolean
AS $$ SELECT TSRANGE($1,$1, '[]') << $2 $$
LANGUAGE SQL STABLE;

CREATE FUNCTION cmp_tstz_rng(TIMESTAMPTZ, TSTZRANGE)
RETURNS boolean
AS $$ SELECT TSTZRANGE($1,$1, '[]') << $2 $$
LANGUAGE SQL STABLE;

CREATE OPERATOR <<(procedure = cmp_ts_rng, leftarg = timestamp, rightarg = tsrange);
CREATE OPERATOR <<(procedure = cmp_tstz_rng, leftarg = timestamptz, rightarg = tstzrange);

