#![cfg(not(all(target_arch = "wasm32", feature = "js")))]

//! `set_process_time_zone`, the host's side of a time zone override.
//!
//! The zone is process-wide, so this test has a binary to itself. What the
//! isolates see is covered by the provider's
//! `test_71_unofficial_intl_overrides`.

use wasmer_napi::{InvalidTimeZone, set_process_time_zone};

#[test]
fn takes_known_iana_ids_and_none() {
    assert_eq!(
        set_process_time_zone(Some("Not/A_Zone")),
        Err(InvalidTimeZone)
    );
    assert_eq!(
        set_process_time_zone(Some("Etc/Unknown")),
        Err(InvalidTimeZone)
    );
    // ICU takes custom ids, but they are not IANA ids.
    assert_eq!(
        set_process_time_zone(Some("GMT+05:00")),
        Err(InvalidTimeZone)
    );
    assert_eq!(set_process_time_zone(Some("Asia/Tokyo")), Ok(()));
    // None: the host's zone again.
    assert_eq!(set_process_time_zone(None), Ok(()));
}
