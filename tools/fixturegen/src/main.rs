//! Phase 0a: capture a byte-exact reference of what the Rust client puts on the wire,
//! so the C encoder can be tested offline against it.
//!
//! Emits, for one known WriteRequestNominal:
//!   1. the raw protobuf encoding
//!   2. the snappy-framed body actually sent
//!   3. the exact method, URI and header set

use std::collections::HashMap;

use conjure_object::{BearerToken, ResourceIdentifier};
use nominal_streaming::api::tonic::io::nominal::scout::api::proto::{Channel, Points, Series};
use nominal_streaming::client::encode_request;
use nominal_streaming::prelude::*;
use prost::Message;

fn hex(bytes: &[u8]) -> String {
    bytes
        .chunks(16)
        .map(|row| {
            let cells: Vec<String> = row.iter().map(|b| format!("{b:02x}")).collect();
            format!("    {}", cells.join(" "))
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn main() {
    // Deliberately boring, fully specified values so the C side can hard-code them.
    // Timestamp: 2026-01-01T00:00:00Z = 1767225600 seconds since the epoch.
    const T0_SECS: i64 = 1_767_225_600;

    let double_series = |name: &str, tags: &[(&str, &str)], vals: &[(i32, f64)]| Series {
        channel: Some(Channel {
            name: name.to_string(),
        }),
        tags: tags
            .iter()
            .map(|(k, v)| (k.to_string(), v.to_string()))
            .collect::<HashMap<_, _>>(),
        points: Some(Points {
            points_type: Some(PointsType::DoublePoints(DoublePoints {
                points: vals
                    .iter()
                    .map(|(nanos, value)| DoublePoint {
                        timestamp: Some(Timestamp {
                            seconds: T0_SECS,
                            nanos: *nanos,
                        }),
                        value: *value,
                    })
                    .collect(),
            })),
        }),
    };

    let request = WriteRequestNominal {
        series: vec![
            double_series(
                "demo.rpm",
                &[("bank", "1")],
                &[(0, 1500.0), (1_000_000, 1510.5), (2_000_000, 1521.25)],
            ),
            double_series(
                "demo.egt",
                &[],
                &[(0, 700.0), (1_000_000, 701.5), (2_000_000, 703.0)],
            ),
        ],
        session_name: None,
    };

    let raw = request.encode_to_vec();
    println!("== 1. raw protobuf ({} bytes) ==", raw.len());
    println!("{}", hex(&raw));

    // Note: HashMap iteration order is not stable, so a two-tag channel could encode
    // its map entries in either order. Only one tag per channel is used above to keep
    // this fixture reproducible.
    let token = BearerToken::new("fixture-token").expect("token");
    let rid = ResourceIdentifier::new("ri.catalog.gov-staging.dataset.00000000-0000-0000-0000-000000000000")
        .expect("rid");

    let http = encode_request(&raw, &token, &rid).expect("encode_request");

    println!("\n== 2. request line and headers ==");
    println!("    {} {}", http.method(), http.uri());
    for (name, value) in http.headers() {
        println!("    {name}: {}", value.to_str().unwrap_or("<binary>"));
    }

    println!("\n== 3. body as sent ==");
    match http.body() {
        conjure_http::client::AsyncRequestBody::Fixed(bytes) => {
            println!("    ({} bytes, from {} raw)", bytes.len(), raw.len());
            println!("{}", hex(bytes));
        }
        _ => println!("    <streaming body — not Fixed>"),
    }

    // Write the three candidate bodies out for the live encoding probe, and as the
    // golden fixtures the C encoder is tested against.
    let out = std::path::Path::new(FIXTURE_DIR);
    std::fs::create_dir_all(out).expect("create fixture dir");

    let mut snappy = snap::write::FrameEncoder::new(Vec::new());
    std::io::Write::write_all(&mut snappy, &raw).expect("snappy");
    let snappy = snappy.into_inner().expect("snappy finish");

    let zstd = zstd::encode_all(&raw[..], 1).expect("zstd");

    for (name, bytes) in [
        ("body_identity.pb", &raw),
        ("body_zstd.bin", &zstd),
        ("body_snappy.bin", &snappy),
    ] {
        let path = out.join(name);
        std::fs::write(&path, bytes).expect("write fixture");
        println!("\nwrote {} ({} bytes)", path.display(), bytes.len());
    }
}

/// Golden fixtures live in the new repo, not this scratch crate.
const FIXTURE_DIR: &str = "../../cstream/tests/fixtures";
