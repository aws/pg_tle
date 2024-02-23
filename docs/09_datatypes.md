# Trusted Language Base types

PostgreSQL provides [`CREATE TYPE`](https://www.postgresql.org/docs/current/sql-createtype.html) command to register a new base type (scalar type) for use in the current database. A base type allows you to customized how the data is stored internally and how to convert it from/to an external textual representation. The support functions `input_function` and `output_function` are required. The `input_function` converts the type's external textual representation to the internal representation. `output_function` performs the reverse transformation. Generally they have to be coded in C or another low-level language. Also, you must be a superuser to create a new base type.

`pg_tle` enables you to build Trusted Language base data types through a set of SQL APIs and use a trusted language to define the support functions. This section of the documentation describes the available APIs and provides examples for how to use them to create your own base data types.

## Scope

The `pg_tle` base data type is scoped to an individual PostgreSQL database (e.g. an object created using `CREATE DATABASE`).

## Functions

### `pgtle.create_shell_type(typenamespace regnamespace, typename name)`

`create_shell_type` provides a way to create a shell type, which is simply a placeholder for a type to be defined later. This is similar to shell type form of [`CREATE TYPE`](https://www.postgresql.org/docs/current/sql-createtype.html).

#### Role

`pgtle_admin`

#### Arguments

* `typenamespace`: The namespace where the new shell type will be created.
* `typename`: The name of the new type.

#### Example

```sql
SELECT pgtle.create_shell_type('public', 'test_citext');
```

### `pgtle.create_shell_type_if_not_exists(typenamespace regnamespace, typename name)`

`create_shell_type_if_not_exists` provides a way to create a shell type, which is simply a placeholder for a type to be defined later. It returns `true` if the type is created, otherwise it returns `false` if the type already exists.

#### Role

`pgtle_admin`

#### Arguments

* `typenamespace`: The namespace where the new shell type will be created.
* `typename`: The name of the new type.

#### Example

```sql
SELECT pgtle.create_shell_type_if_not_exists('public', 'test_citext');
```

### `pgtle.create_base_type(typenamespace regnamespace, typename name, infunc regprocedure, outfunc regprocedure, internallength int4, alignment text default 'int4', storage text default 'plain')`

`create_base_type` provides a way to create a new base data type. The type must be a shell type previously defined by `create_shell_type`. This is similar to base type form of [`CREATE TYPE`](https://www.postgresql.org/docs/current/sql-createtype.html). Internally, a base data type created by `pg_tle` is stored as `bytea`, it can be cast to `bytea` explicitly after creation.

#### Role

`pgtle_admin`

#### Arguments

* `typenamespace`: The namespace where the base data type will be created.
* `typename`: The name of the base data type.
* `infunc`: The name of a previously defined function to convert the type's external textual representation to the internal representation (`bytea`). The function must take one argument of type `text` and return `bytea`. The function must also be declared as `IMMUTABLE` and `STRICT`. It will not be called with a NULL parameter.
* `outfunc`: The name of a previously defined function to convert the type's internal binary representation (`bytea`) to the external textual representation. The function must take one argument of type `bytea` and return `text`. The function must also be declared as `IMMUTABLE` and `STRICT`. It will not be called with a NULL parameter.
* `internallength`: Total length of the base data type as bytes. Base data types can be fixed-length, in which case internallength is a positive integer, or variable-length, in which case internallength is -1.
* `alignment`: The optional alignment parameter specifies the storage alignment requirement of the data type. Allowed values are 'char', 'int2', 'int4', 'double'. The allowed values equate to alignment on 1, 2, 4, or 8 byte boundaries. The default is 'int4', alignment on 4 byte boundaries. A variable-length type (e.g., text) must have a byte-alignment of at least 4 due to the size of its header.
* `storage`: The optional storage parameter allows selection of storage strategies for variable-length data types. Allowed values are 'plain', 'external', 'extended', 'main'. Only 'plain' is allowed for fixed-length types. The default is 'plain'. See PostgreSQL doc on [CREATE TYPE](https://www.postgresql.org/docs/current/sql-createtype.html) and [TOAST](https://www.postgresql.org/docs/current/storage-toast.html) for more details.

#### Example

```sql
-- Create a variable-length data type
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
-- Create a fixed-length (2 bytes) data type
SELECT pgtle.create_base_type('public', 'test_int2', 'test_int2_in(text)'::regprocedure, 'test_int2_out(bytea)'::regprocedure, 2);
-- Create a TOASTable variable-length data type 
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1, storage => 'extended');
```

### `pgtle.create_base_type_if_not_exists(typenamespace regnamespace, typename name, infunc regprocedure, outfunc regprocedure, internallength int4, alignment text default 'int4', storage text default 'plain')`

`create_base_type_if_not_exists` `create_base_type` provides a way to create a new base data type. It returns `true` if the type is created, otherwise it returns `false` if the type already exists. The type must be a shell type previously defined by `create_shell_type`. This is similar to base type form of [`CREATE TYPE`](https://www.postgresql.org/docs/current/sql-createtype.html). Internally, a base data type created by `pg_tle` is stored as `bytea`, it can be cast to `bytea` explicitly after creation.

#### Role

`pgtle_admin`

#### Arguments

* `typenamespace`: The namespace where the base data type will be created.
* `typename`: The name of the base data type.
* `infunc`: The name of a previously defined function to convert the type's external textual representation to the internal representation (`bytea`). The function must take one argument of type `text` and return `bytea`. The function must also be declared as `IMMUTABLE` and `STRICT`. It will not be called with a NULL parameter.
* `outfunc`: The name of a previously defined function to convert the type's internal binary representation (`bytea`) to the external textual representation. The function must take one argument of type `bytea` and return `text`. The function must also be declared as `IMMUTABLE` and `STRICT`. It will not be called with a NULL parameter.
* `internallength`: Total length of the base data type as bytes. Base data types can be fixed-length, in which case internallength is a positive integer, or variable-length, in which case internallength is -1.
* `alignment`: The optional alignment parameter specifies the storage alignment requirement of the data type. Allowed values are 'char', 'int2', 'int4', 'double'. The allowed values equate to alignment on 1, 2, 4, or 8 byte boundaries. The default is 'int4', alignment on 4 byte boundaries. A variable-length type (e.g., text) must have a byte-alignment of at least 4 due to the size of its header.
* `storage`: The optional storage parameter allows selection of storage strategies for variable-length data types. Allowed values are 'plain', 'external', 'extended', 'main'. Only 'plain' is allowed for fixed-length types. The default is 'plain'. See PostgreSQL doc on [CREATE TYPE](https://www.postgresql.org/docs/current/sql-createtype.html) and [TOAST](https://www.postgresql.org/docs/current/storage-toast.html) for more details.

#### Example

```sql
-- Create a variable-length data type
SELECT pgtle.create_base_type_if_not_exists('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
-- Create a fixed-length (2 bytes) data type
SELECT pgtle.create_base_type_if_not_exists('public', 'test_int2', 'test_int2_in(text)'::regprocedure, 'test_int2_out(bytea)'::regprocedure, 2);
-- Create a TOASTable variable-length data type 
SELECT pgtle.create_base_type_if_not_exists('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1, storage => 'extended');
```

### `pgtle.create_operator_func(typenamespace regnamespace, typename name, opfunc regprocedure)`

`create_operator_func` provides a way to create an operator function on the base data type previously defined by `create_base_type`. This function takes an operator function which accepts one or two arguments of type `bytea`, and creates an overloaded version which accpets the base data type as the arguments instead. This is not required to create an operator function, but it can be helpful while working with certain languages such as plrust.

#### Role

`pgtle_admin`

#### Arguments

* `typenamespace`: The namespace where the new operator function will be created. It must be the same namespace where the base data type is.
* `typename`: The name of the base data type.
* `opfunc`: The name of a previously defined operator function. The function must take exactly one or two arguments of type `bytea`.

#### Example

```sql
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
```

### `pgtle.create_operator_func_if_not_exists(typenamespace regnamespace, typename name, opfunc regprocedure)`

`create_operator_func_if_not_exists` provides a way to create an operator function on the base data type previously defined by `create_base_type`. It returns `true` if the operator function is created, otherwise it returns `false` if the operator function already exists. This function is not required to create an operator function, but it can be helpful while working with certain languages such as plrust.

#### Role

`pgtle_admin`

#### Arguments

* `typenamespace`: The namespace where the new operator function will be created. It must be the same namespace where the base data type is.
* `typename`: The name of the base data type.
* `opfunc`: The name of a previously defined operator function. The function must be declared as taking one or two arguments of type `bytea`.

#### Example

```sql
SELECT pgtle.create_operator_func_if_not_exists('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
```

## Examples
The following examples demonstrate how to use `pg_tle` data type API functions to create a base data type. After running this example, a base data type called `test_citext` (case-insentive text) will be available for use in the current database.

### Create shell type
First, use `pgtle.create_shell_type` to create a shell type in the target namespace (PUBLIC in this example).

```sql
SELECT pgtle.create_shell_type('public', 'test_citext');
```

### Create I/O functions
Second, define the input/output function on the data type. Remember the input function takes one argument of type `text` and returns `bytea`; while the output function takes one argument of type `bytea` and returns `text`.

```sql
CREATE FUNCTION public.test_citext_in(input text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_out(input bytea) RETURNS text AS
$$
  SELECT pg_catalog.convert_from(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;
```

### Create base type
We can now use `pgtle.create_base_type` to create the base data type using the I/O functions defined previously. Because we are defining a variable-length type, `-1` is used as `internallength`.

```sql
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
```

After this step, we should be able to use the newly created data type `test_citext`.
```sql
CREATE TABLE public.test_dt(c1 test_citext PRIMARY KEY);
INSERT INTO test_dt VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE');
INSERT INTO test_dt VALUES ('select');
```
`'SELECT'` and `'select'` are considered different values at this moment because we haven't defined related operators and operator class for the type.

### Create operator functions
After the base data type is created, we can define operators and operator class if needed.

The following commands define a set of operator functions on type `test_citext`. We use an explicit cast from `test_citext` to `bytea` so that we can utilize existing binary functions in Postgresql. 
```sql
CREATE FUNCTION public.test_citext_cmp(l test_citext, r test_citext)
RETURNS int AS
$$
BEGIN
  RETURN pg_catalog.bttextcmp(pg_catalog.lower(pg_catalog.convert_from(l::bytea, 'UTF8')), pg_catalog.lower(pg_catalog.convert_from(r::bytea, 'UTF8')));
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_eq(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_cmp(l, r) == 0;
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_ne(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_cmp(l, r) != 0;
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_lt(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_cmp(l, r) < 0;
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_le(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_cmp(l, r) <= 0;
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_gt(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_cmp(l, r) > 0;
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_ge(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_cmp(l, r) >= 0;
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;
```

### Alternative way to operator functions
Alternatively, we can use `pgtle.create_operator_func` to create operator functions without explicit cast. It can be really helpful in certains languages such as plrust where the newly created type is not available.

First, create the operator functions that take `bytea` as argument type:
```sql
CREATE FUNCTION public.test_citext_cmp(l bytea, r bytea) 
RETURNS int AS
$$
  SELECT pg_catalog.bttextcmp(pg_catalog.lower(pg_catalog.convert_from(l, 'UTF8')), pg_catalog.lower(pg_catalog.convert_from(r, 'UTF8')));
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_lt(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) < 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_eq(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) = 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_le(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) <= 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_gt(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) > 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_ge(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) >= 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_ne(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) != 0;
$$ IMMUTABLE STRICT LANGUAGE sql;
```

then use `pgtle.create_operator_func` to convert the functions to operate on type `test_citext`:
```sql
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_lt(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_le(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_eq(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_ne(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_gt(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_ge(bytea, bytea)'::regprocedure);
```

### Create operators / operator class
After defining operator functions, run the following commands to define operators on type `test_citext`.
```sql
CREATE OPERATOR < (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = >,
    NEGATOR = >=,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel,
    PROCEDURE = public.test_citext_lt
);

CREATE OPERATOR <= (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = >=,
    NEGATOR = >,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel,
    PROCEDURE = public.test_citext_le
);

CREATE OPERATOR = (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = =,
    NEGATOR = <>,
    RESTRICT = eqsel,
    JOIN = eqjoinsel,
    HASHES,
    MERGES,
    PROCEDURE = public.test_citext_eq
);

CREATE OPERATOR <> (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <>,
    NEGATOR = =,
    RESTRICT = neqsel,
    JOIN = neqjoinsel,
    PROCEDURE = public.test_citext_ne
);

CREATE OPERATOR > (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <,
    NEGATOR = <=,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel,
    PROCEDURE = public.test_citext_gt
);

CREATE OPERATOR >= (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <=,
    NEGATOR = <,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel,
    PROCEDURE = public.test_citext_ge
);
```

Run following command to create operator class. Note superuser permission is required here. If you are using Amazon RDS, superuser permission is not required.

```sql
CREATE OPERATOR CLASS public.test_citext_ops
    DEFAULT FOR TYPE public.test_citext USING btree AS
        OPERATOR        1       < ,
        OPERATOR        2       <= ,
        OPERATOR        3       = ,
        OPERATOR        4       > ,
        OPERATOR        5       >= ,
        FUNCTION        1       public.test_citext_cmp(public.test_citext, public.test_citext);
```

Now we can run some simply queries to verify the operators are working as expected.

```sql
DROP TABLE IF EXISTS public.test_dt;
CREATE TABLE public.test_dt(c1 test_citext PRIMARY KEY);
INSERT INTO test_dt VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE');
INSERT INTO test_dt VALUES ('select');
ERROR:  duplicate key value violates unique constraint "test_dt_pkey"
```
`'SELECT'` and `'select'` are considered equal according to the `=` operator and violates the unique constraint of `PRIMARY KEY`.
