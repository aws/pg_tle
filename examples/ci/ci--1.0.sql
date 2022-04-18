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

DROP OPERATOR IF EXISTS ==(text, text);



CREATE OPERATOR ==
  (
    FUNCTION = rust_eq_insensitive,
    LEFTARG = text,
    RIGHTARG = text
  );

