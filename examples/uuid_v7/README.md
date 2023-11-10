## Uuid v7 example

This example demonstrates how users can create a Trusted Language Extension (TLE) with [PL/Rust](https://github.com/tcdi/plrust) to generate uuid v7.

This extension supports 3 operations:
1. Generate a v7 uuid for the current time
2. Generate a v7 uuid from a given timstamptz
3. Extract the timestamptz from a given v7 uuid


### Pre-requisites
---
- [PL/Rust 1.2.0](https://github.com/tcdi/plrust/tree/v1.2.0) and above

    To set up PL/Rust, refer to https://plrust.io/install-plrust.html


### Installation
---
To install the extension, run the `uuid_v7.sql` file in the desired database and install the extension.

```
postgres=# create extension uuid_v7;
CREATE EXTENSION
```

To generate a v7 uuid
```
postgres=# SELECT generate_uuid_v7();
           generate_uuid_v7
--------------------------------------
 018bbaec-db78-7d42-ab07-9b8055faa6cc
(1 row)
```

Newly generated v7 uuid is older than ones generated previously
```
postgres=# \set uuidv7 '018bbaec-db78-7d42-ab07-9b8055faa6cc'
postgres=# SELECT generate_uuid_v7() > :'uuidv7';
 ?column?
----------
 t
(1 row)
```

To extract the timestamp from the v7 uuid
```
postgres=# SELECT uuid_v7_to_timestamptz('018bbaec-db78-7d42-ab07-9b8055faa6cc');
   uuid_v7_to_timestamptz
----------------------------
 2023-11-10 15:29:26.776-05
(1 row)
```

To generate a v7 uuid with a given timestamp
```
postgres=# SELECT timestamptz_to_uuid_v7('2023-11-10 15:29:26.776-05');
        timestamptz_to_uuid_v7
--------------------------------------
 018bbaec-db78-7afa-b2e6-c328ae861711
(1 row)
```

The extracted timestamp from the uuid is the same as the timestamp that was given to generate the uuid 
```
postgres=# SELECT uuid_v7_to_timestamptz('018bbaec-db78-7afa-b2e6-c328ae861711');
   uuid_v7_to_timestamptz
----------------------------
 2023-11-10 15:29:26.776-05
(1 row)
```
