
use clap::{AppSettings, Parser};
use std::fs;
use postgres::{Client};
use openssl::ssl::{SslConnector, SslMethod, SslVerifyMode};
use postgres_openssl::MakeTlsConnector;

#[derive(Parser)]
#[clap(name = env!("CARGO_PKG_NAME"), author = env!("CARGO_PKG_AUTHORS"), version = env!("CARGO_PKG_VERSION"), about = env!("CARGO_PKG_DESCRIPTION"), long_about = None)]
#[clap(global_setting(AppSettings::DeriveDisplayOrder))]

struct Args {
    #[clap(name = "PG Connection", short = 'c', long = "pgconn", help = "PostgreSQL connection string (Key=Value or URI format)", value_parser)]
    pg_conn: String,

    #[clap(name = "Extension Path", short = 'p', long = "extpath", help = "Local path of the extension", value_parser)]
    ext_path: String,

    #[clap(name = "Extension Name", short = 'n', long = "extname", help = "Name of the extension", value_parser)]
    ext_name: String,

    #[clap(name = "Extension Revision", short = 'r', long = "extrev", help = "Extension revision to install", value_parser)]
    ext_rev: String,

    #[clap(short = 'a', long, help = "CA Pem cert", default_value_t = String::from("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem") )]
    ca_file: String,

}

fn main() {
    let args = Args::parse();

    let cntrl_file = format!("{}/{}.control",args.ext_path,args.ext_name);
    let func_file = format!("{}/{}--{}.sql",args.ext_path,args.ext_name,args.ext_rev);

    println!("Loading {} version {} from {} using {}", args.ext_name, args.ext_rev, args.ext_path, args.pg_conn);
    println!("cntrl_file is {}",cntrl_file);
    println!("func_file is {}",func_file);

    let mut builder = SslConnector::builder(SslMethod::tls()).unwrap();
    //builder.set_ca_file("/home/sharyogi/tls/root.crt").unwrap();
    builder.set_ca_file(args.ca_file).unwrap();
    builder.set_verify(SslVerifyMode::NONE);
    let connector = MakeTlsConnector::new(builder.build());

    let cntrl_content = fs::read_to_string(cntrl_file)
        .expect("Should have been able to read the file");
    let func_content = fs::read_to_string(func_file)
        .expect("Should have been able to read the file");
    let mut client = Client::connect(&args.pg_conn, connector).unwrap();
    client.execute("SELECT * FROM pg_tle.install_extension( $1, $2, $3, $4, $5 )", &[ &args.ext_name, &args.ext_rev, &cntrl_content, &false, &func_content ] ).unwrap();

}

