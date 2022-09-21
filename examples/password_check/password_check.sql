/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

SELECT pgbc.install_extension
(
 'password_check',
 '1.0',
$_password_check_$
comment = 'A function that can be called from the pg_tle feature passcheck'
default_version = '1.0'
module_pathname = 'pgbc_string'
relocatable = false
superuser = false
trusted = true
requires = 'plperl, uni_api'
$_password_check_$,
  false,
$_password_check_$
CREATE SCHEMA password_check;
REVOKE ALL on SCHEMA password_check FROM public;
GRANT USAGE ON SCHEMA password_check TO PUBLIC;

CREATE TABLE password_check.password_history (
  username     text NOT NULL,
  hash         text NOT NULL,
  change_ts    timestamptz NOT NULL
);
REVOKE ALL on TABLE password_check.password_history FROM public;

CREATE INDEX password_history_username 
  ON password_check.password_history(username);

CREATE FUNCTION password_check.checkpass(text, text, bc.password_types, timestamp, boolean) 
  RETURNS void AS 
$_$
    my ($username, $pwd, $password_type, $validuntil_time, $validuntil_null) = @_;

    unless($password_type == "PASSWORD_TYPE_PLAINTEXT") {
        elog(WARNING, "Password not checked. Password Type is " . $password_type);
        return;
    }
    my $len = length($pwd);

    # passwords less than 8 characters are not allowed
    if($len < 8){
        elog(ERROR, 'Passwords needs to be longer than 8.');
    }

    # passwords less than 16 characters need numbers
    if($len < 16) {
      unless($pwd =~ /\d/ && $pwd =~ /[a-z]/ && $pwd =~ /[A-Z]/) {
          elog(ERROR, 'Passwords less than 16 characters need to have digits, upper and lower case');
      }
    }

    # passwords more than 16 characters only need upper and lower case
    unless($pwd =~ /[a-z]/ && $pwd =~ /[A-Z]/){
        elog(ERROR, 'Passwords need upper and lower case characters');
    }

    my $userpass = $pwd . $username;

    my $plan1 = spi_prepare('SELECT 1 WHERE pg_catalog.md5($1) IN 
                             (SELECT hash 
                                FROM password_check.password_history 
                               WHERE username = $2 
                               ORDER BY change_ts DESC 
                               LIMIT 3)', 'TEXT', 'TEXT');
    my $rv = spi_query_prepared($plan1, $userpass, $username);
    if (defined(my $row = spi_fetchrow($rv))) {
      elog(ERROR, 'The password does not meet the password history requirement');
    }
    spi_cursor_close($rv);
    spi_freeplan($plan1);

    my $plan2 = spi_prepare('INSERT INTO password_check.password_history 
                             VALUES ($1, pg_catalog.md5($2), current_timestamp)', 
                             'TEXT', 'TEXT');
    spi_exec_prepared($plan2, $username, $userpass);
    spi_freeplan($plan2);
$_$
SECURITY DEFINER
LANGUAGE plperl;

GRANT EXECUTE ON FUNCTION password_check.checkpass TO PUBLIC;
$_password_check_$
);
