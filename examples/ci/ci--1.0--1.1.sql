--------------------------------------------------------------------------------
-- FUNCTION - rust_eq_insensitive(text, text)
--
--    Returns a boolean indicating whether the two arguments are equal,
--    ignoring differences in case.

CREATE OR REPLACE FUNCTION rust_eq_insensitive(leftarg text, rightarg text)
  RETURNS boolean
AS $$
  Some(str::to_lowercase(leftarg) == str::to_lowercase(rightarg))
$$ STRICT LANGUAGE plrust;

--------------------------------------------------------------------------------
-- FUNCTION - rust_cmp_insensitive(text, text)
--
--  Returns
--    -1  to indicate that the left argument should sort before the right
--        argument, ignoring differences in case.
--     0  to indicate that the two arguments are equal, ignoring differences
--        in case
--     1  to indicate that the left argument should sort after the right
--        argument, ignoring differences in case.

CREATE OR REPLACE FUNCTION rust_cmp_insensitive(leftarg text, rightarg text)
  RETURNS int4
AS $$
  if str::to_lowercase(leftarg) < str::to_lowercase(rightarg)
  {
    Some(-1)
  }
  else if str::to_lowercase(leftarg) > str::to_lowercase(rightarg)
  {
    Some(1)
  }
  else
  {
    Some(0)
  }
$$ STRICT LANGUAGE plrust;

--------------------------------------------------------------------------------

-- OPERATOR ==(text, text)
--
-- Creates an operator named '==' that returns true if the given operands are
-- equal, ignoring differences in case. Once we have this operator, youo can
-- write code such as:
-- SELECT 'abc' == 'Abc' — returns true
-- SELECT 'abc' == '123' — returns false

DROP OPERATOR IF EXISTS ==(text, text);

CREATE OPERATOR ==
 (
 FUNCTION = rust_eq_insensitive,
 LEFTARG = text,
 RIGHTARG = text
 );

--------------------------------------------------------------------------------
-- OPERATOR CLASS rust_opclass
--
-- Creates an operator class that you can use to create a btree index using
-- rust_cmp_insensitive(text, text). The operator class states that the
-- optimizer may use the index to evaluate any comparisons based on the
-- given given operators (<, <=, ==, >=, >), such as:
--     WHERE name < 'Jones'
--     WHERE name >= 'Jones'
--     WHERE name == 'Jones'

CREATE OPERATOR CLASS rust_opclass FOR TYPE text USING btree AS
 OPERATOR 1 < ,
 OPERATOR 2 <= ,
 OPERATOR 3 == ,
 OPERATOR 4 >= ,
 OPERATOR 5 > ,
 FUNCTION 1 rust_cmp_insensitive(text, text);

--------------------------------------------------------------------------------
-- OPERATOR CLASS rust_opclass2
--
--  Creates a second operator class that only supports tests for equality, such
--  as:
--     WHERE name == 'Jones'

CREATE OPERATOR CLASS rust_opclass2 FOR TYPE  text USING btree AS
  OPERATOR 3  == ,
  FUNCTION 1 rust_cmp_insensitive(text, text);
