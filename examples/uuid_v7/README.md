## Generating a UUID v7

[Universally unique identifiers](https://en.wikipedia.org/wiki/Universally_unique_identifier) (UUIDs) are a data type frequently used as primary keys due to their uniqueness property. Historically, one of the most popular methods of generating UUIDs was UUID v4, which randomly generated the UUID. However, this often caused poor index locality, as compared to a monotonically increasing integer, and can impact how quickly users can retrieve rows from a database. To improve this experience, [UUID v7](https://datatracker.ietf.org/doc/html/draft-peabody-dispatch-new-uuid-format-04#name-uuid-version-7) was proposed, which stores a portion of a UNIX timestamp in the first 48-bits before allowing for the generation of random data.

In this example, we'll show you how can you build a Trusted Language Extension (TLE) with [PL/Rust](https://github.com/tcdi/plrust) to generate a UUID using the UUID v7 method.

This extension supports 3 operations:
1. Generate a UUID v7 from the system time.
2. Generate a UUID v7 from a user-provided timestamp.
3. Extract a `timestamptz` from a UUID that's generated using the UUID v7 method.


### Prerequisites
---
- [PL/Rust 1.2.0](https://github.com/tcdi/plrust/tree/v1.2.0) and above

    To set up PL/Rust, refer to https://plrust.io/install-plrust.html


### Installation
---
To install the extension, run the `uuid_v7.sql` file in the desired database

```sh
psql -d postgres -f uuid_v7.sql
```

To generate a UUID using UUID v7, you can run the following command:
```sql
postgres=# SELECT generate_uuid_v7();
           generate_uuid_v7
--------------------------------------
 018bbaec-db78-7d42-ab07-9b8055faa6cc
(1 row)
```

You can verify that a more recently generated UUID v7 used a newer timestamp than a previously generated UUID v7:
```sql
postgres=# \set uuidv7 '018bbaec-db78-7d42-ab07-9b8055faa6cc'
postgres=# SELECT generate_uuid_v7() > :'uuidv7';
 ?column?
----------
 t
(1 row)
```

To extract the timestamp from the v7 uuid. Note that since UUID v7 uses millisecond level of precision, the returns timestamp 
```sql
postgres=# SELECT uuid_v7_to_timestamptz('018bbaec-db78-7d42-ab07-9b8055faa6cc');
   uuid_v7_to_timestamptz
----------------------------
 2023-11-10 15:29:26.776-05
(1 row)
```

To generate a UUID using UUID v7, you can run the following command:
```sql
postgres=# SELECT timestamptz_to_uuid_v7('2023-11-10 15:29:26.776-05');
        timestamptz_to_uuid_v7
--------------------------------------
 018bbaec-db78-7afa-b2e6-c328ae861711
(1 row)
```

You can verify that the timestamp that you used to generate the UUID v7 matches the one that you provided:
```sql
postgres=# SELECT uuid_v7_to_timestamptz('018bbaec-db78-7afa-b2e6-c328ae861711');
   uuid_v7_to_timestamptz
----------------------------
 2023-11-10 15:29:26.776-05
(1 row)
```

To uninstall the extension
```sql
postgres=# DROP EXTENSION uuid_v7;
DROP EXTENSION
postgres=# SELECT pgtle.uninstall_extension('uuid_v7');
 uninstall_extension
---------------------
 t
(1 row)
```
