password_check
==============

A package that creates a function to check the complexity of a password. It can 
be used with the *passcheck* feature of pg_tle. The function enforces the 
following complexity rules:

* The password must be at least 8 characters
* Passwords between 8 and 16 characters must contain digits, lower and upper 
  case characters
* Passwords longer than 16 characters must contain lower and upper case 
  characters
* At least 3 unique passwords need to be used before the password can be repeated

Installation
------------
Installing this extension is very simple:

    psql -f password_check.sql postgres
    psql -c "CREATE EXTENSION password_check" postgres
    psql -c "SELECT bc.bc_feature_info_sql_insert('password_check.checkpass', 'passcheck')" postgres
