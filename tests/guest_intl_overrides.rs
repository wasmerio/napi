//! A guest's `unofficial_napi_set_default_locale` and
//! `unofficial_napi_refresh_intl_defaults` imports reach its env through real
//! Wasmer dispatch and the C++ bridge. The engine side is covered by
//! `tests/runners/test_71_unofficial_intl_overrides.cc`.
//!
//! A process takes a bounded number of distinct locales, so this binary has
//! one test to itself.

#![cfg(not(target_arch = "wasm32"))]

use std::sync::mpsc;

use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, NAPI_INVALID_ARG, compile};

/// `napi_generic_failure`.
const NAPI_GENERIC_FAILURE: i32 = 9;
/// The most distinct locales a process takes: ICU never frees one.
const MAX_DEFAULT_LOCALE_IDS: usize = 1024;

fn default_locale_is(guest: &mut Guest, tag: &str) -> bool {
    let source = format!("new Intl.NumberFormat().resolvedOptions().locale === '{tag}' ? 1 : 0");
    guest.number(&source) == 1
}

#[test]
fn guest_sets_its_default_locale_through_the_import() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();

    let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
    guest.create();
    // A locale the host is not already using.
    let tag = if default_locale_is(&mut guest, "de-DE") {
        "fr-FR"
    } else {
        "de-DE"
    };

    assert_eq!(guest.set_default_locale(tag), 0);
    assert!(default_locale_is(&mut guest, tag));
    // A malformed tag, and one longer than any tag (refused before guest
    // memory is read), change nothing.
    assert_eq!(guest.set_default_locale("de_DE"), NAPI_INVALID_ARG);
    assert_eq!(guest.set_default_locale(&"a".repeat(257)), NAPI_INVALID_ARG);
    // NULL with a length, as in the C API, even with a tag at address 0.
    let other = if tag == "de-DE" { "fr-FR" } else { "de-DE" };
    assert_eq!(guest.set_default_locale_at(0, other), NAPI_INVALID_ARG);
    assert!(default_locale_is(&mut guest, tag));
    // Length 0: the process default again.
    assert_eq!(guest.set_default_locale(""), 0);
    assert!(!default_locale_is(&mut guest, tag));
    // No time zone change is pending.
    assert_eq!(guest.refresh_intl_defaults(), 0);

    // New tags until the process refuses one: `tag` took the first place.
    let mut accepted = 1;
    let mut status = 0;
    for i in 0..MAX_DEFAULT_LOCALE_IDS {
        status = guest.set_default_locale(&format!("en-x-{i:08x}"));
        if status != 0 {
            break;
        }
        accepted += 1;
    }
    assert_eq!(status, NAPI_GENERIC_FAILURE);
    assert_eq!(accepted, MAX_DEFAULT_LOCALE_IDS);
    // Tags it already took are still accepted.
    assert_eq!(guest.set_default_locale("en-x-00000000"), 0);
    assert_eq!(guest.set_default_locale(tag), 0);
    assert!(default_locale_is(&mut guest, tag));
    assert_eq!(guest.set_default_locale(""), 0);

    assert_eq!(guest.release(), 0);
}
