CREATE EXTENSION IF NOT EXISTS pg_tle;
CREATE EXTENSION IF NOT EXISTS plrust;

--
-- Trusted language extension for uuid v7
--
-- To use, execute this file in your desired database to install the extension.
--

SELECT pgtle.install_extension
(
    'uuid_v7',
    '1.0',
    'extension for uuid v7',
$_pg_tle_$
    -- Uuid v7 is a 128-bit binary value generated from unix epoch timestamp in ms (milliseconds) converted
    -- into a u64 in Big-endian format and encoded with a series of random bytes
    CREATE FUNCTION generate_uuid_v7()
        RETURNS uuid
        AS $$
            [dependencies]
            rand = "0.8.5"

            [code]
            // The underlying size of an uuid in bytes is sixteen unsigned char.
            type UuidBytes = [u8; 16];

            fn generate_uuid_v7_bytes() -> UuidBytes {
                // Retrieve the current timestamp as millisecond to compute the uuid
                let now = pgrx::clock_timestamp();

                // Extract the epoch from the timestamp and convert to millisecond
                let epoch_in_millis_numeric: AnyNumeric = now
                    .extract_part(DateTimeParts::Epoch)
                    .expect("Unable to extract epoch from clock timestamp")
                    * 1000;

                // Unfortunately we cannot convert AnyNumeric to an u64 directly, so we first convert to a string then u64
                let epoch_in_millis_normalized = epoch_in_millis_numeric.floor().normalize().to_owned();
                let millis = epoch_in_millis_normalized
                    .parse::<u64>()
                    .expect("Unable to convertg from timestamp from type AnyNumeric to u64");

                generate_uuid_bytes_from_unix_ts_millis(
                    millis,
                    &rng_bytes()[..10]
                        .try_into()
                        .expect("Unable to generate 10 bytes of random u8"),
                )
            }

            // Returns 16 random u8.
            // For this example, we use a thread-local random number generator as suggested by the rand crate.
            // Replace with a different type of random number generator based on your use case 
            // https://rust-random.github.io/book/guide-rngs.html
            fn rng_bytes() -> [u8; 16] {
                rand::random()
            }

            fn generate_uuid_bytes_from_unix_ts_millis(millis: u64, random_bytes: &[u8; 10]) -> UuidBytes {
                let (millis_high, millis_low, random_and_version, d4) =
                    encode_unix_timestamp_millis(millis, random_bytes);

                let bytes: UuidBytes =
                    generate_uuid_bytes_from_fields(millis_high, millis_low, random_and_version, &d4);

                bytes
            }

            // This is copied from the implementation of uuid-rs crate
            // https://github.com/uuid-rs/uuid/blob/1.6.1/src/timestamp.rs#L247-L266
            fn encode_unix_timestamp_millis(millis: u64, random_bytes: &[u8; 10]) -> (u32, u16, u16, [u8; 8]) {
                let millis_high = ((millis >> 16) & 0xFFFF_FFFF) as u32;
                let millis_low = (millis & 0xFFFF) as u16;

                let random_and_version =
                    (random_bytes[1] as u16 | ((random_bytes[0] as u16) << 8) & 0x0FFF) | (0x7 << 12);

                let mut d4 = [0; 8];

                d4[0] = (random_bytes[2] & 0x3F) | 0x80;
                d4[1] = random_bytes[3];
                d4[2] = random_bytes[4];
                d4[3] = random_bytes[5];
                d4[4] = random_bytes[6];
                d4[5] = random_bytes[7];
                d4[6] = random_bytes[8];
                d4[7] = random_bytes[9];

                (millis_high, millis_low, random_and_version, d4)
            }
            
            // This is copied from the implementation of uuid-rs crate
            // https://github.com/uuid-rs/uuid/blob/1.6.1/src/builder.rs#L122-L141
            fn generate_uuid_bytes_from_fields(d1: u32, d2: u16, d3: u16, d4: &[u8; 8]) -> UuidBytes {
                [
                    (d1 >> 24) as u8,
                    (d1 >> 16) as u8,
                    (d1 >> 8) as u8,
                    d1 as u8,
                    (d2 >> 8) as u8,
                    d2 as u8,
                    (d3 >> 8) as u8,
                    d3 as u8,
                    d4[0],
                    d4[1],
                    d4[2],
                    d4[3],
                    d4[4],
                    d4[5],
                    d4[6],
                    d4[7],
                ]
            }
            
            Ok(Some(Uuid::from_bytes(generate_uuid_v7_bytes())))
        $$ LANGUAGE plrust
    STRICT VOLATILE;

    CREATE FUNCTION uuid_v7_to_timestamptz(uuid UUID)
        RETURNS timestamptz
        as $$
            // The timestamp of the uuid is encoded in the first 48 bits.
            // To retrieve the timestamp in milliseconds, convert the first
            // six u8 encoded in Big-endian format into a u64.
            let uuid_bytes = uuid.as_bytes();
            let mut timestamp_bytes = [0u8; 8];
            timestamp_bytes[2..].copy_from_slice(&uuid_bytes[0..6]);
            let millis = u64::from_be_bytes(timestamp_bytes);

            // The postgres to_timestamp function takes a double as argument,
            // whereas the pgrx::to_timestamp takes a f64 as arugment.
            // Since the timestamp in uuid was computed from extracting the unix epoch
            // and multiplying by 1000, here we divide it by 1000 to get the precision we
            // need and convert into a f64.
            let epoch_in_seconds_with_precision = millis as f64 / 1000 as f64;

            Ok(Some(pgrx::to_timestamp(epoch_in_seconds_with_precision)))
        $$ LANGUAGE plrust
    STRICT IMMUTABLE;

    CREATE FUNCTION timestamptz_to_uuid_v7(tz timestamptz)
        RETURNS uuid
        AS $$
            [dependencies]
            rand = "0.8.5"

            [code]
            type UuidBytes = [u8; 16];

            // The implementation is similar to generate_uuid_v7 except we generate uuid based on a given timestamp instead of the current timestmp
            fn generate_uuid_v7_bytes(tz: TimestampWithTimeZone) -> UuidBytes {
                let epoch_numeric: AnyNumeric = tz
                    .extract_part(DateTimeParts::Epoch)
                    .expect("Unable to extract epoch from clock timestamp");
                let epoch_in_millis_numeric: AnyNumeric = epoch_numeric * 1000;
                let epoch_in_millis_normalized = epoch_in_millis_numeric.floor().normalize().to_owned();
                let millis = epoch_in_millis_normalized
                    .parse::<u64>()
                    .expect("Unable to convertg from timestamp from type AnyNumeric to u64");

                generate_uuid_bytes_from_unix_ts_millis(
                    millis,
                    &rng_bytes()[..10]
                        .try_into()
                        .expect("Unable to generate 10 bytes of random u8"),
                )
            }

            fn rng_bytes() -> [u8; 16] {
                rand::random()
            }

            fn generate_uuid_bytes_from_unix_ts_millis(millis: u64, random_bytes: &[u8; 10]) -> UuidBytes {
                let (millis_high, millis_low, random_and_version, d4) =
                    encode_unix_timestamp_millis(millis, random_bytes);
                let bytes: UuidBytes =
                    generate_uuid_bytes_from_fields(millis_high, millis_low, random_and_version, &d4);
                bytes
            }

            fn encode_unix_timestamp_millis(millis: u64, random_bytes: &[u8; 10]) -> (u32, u16, u16, [u8; 8]) {
                let millis_high = ((millis >> 16) & 0xFFFF_FFFF) as u32;
                let millis_low = (millis & 0xFFFF) as u16;

                let random_and_version =
                    (random_bytes[1] as u16 | ((random_bytes[0] as u16) << 8) & 0x0FFF) | (0x7 << 12);

                let mut d4 = [0; 8];

                d4[0] = (random_bytes[2] & 0x3F) | 0x80;
                d4[1] = random_bytes[3];
                d4[2] = random_bytes[4];
                d4[3] = random_bytes[5];
                d4[4] = random_bytes[6];
                d4[5] = random_bytes[7];
                d4[6] = random_bytes[8];
                d4[7] = random_bytes[9];

                (millis_high, millis_low, random_and_version, d4)
            }

            fn generate_uuid_bytes_from_fields(d1: u32, d2: u16, d3: u16, d4: &[u8; 8]) -> UuidBytes {
                [
                    (d1 >> 24) as u8,
                    (d1 >> 16) as u8,
                    (d1 >> 8) as u8,
                    d1 as u8,
                    (d2 >> 8) as u8,
                    d2 as u8,
                    (d3 >> 8) as u8,
                    d3 as u8,
                    d4[0],
                    d4[1],
                    d4[2],
                    d4[3],
                    d4[4],
                    d4[5],
                    d4[6],
                    d4[7],
                ]
            }

            Ok(Some(Uuid::from_bytes(generate_uuid_v7_bytes(tz))))
        $$ LANGUAGE plrust
    STRICT VOLATILE;
$_pg_tle_$
);

CREATE EXTENSION uuid_v7;
