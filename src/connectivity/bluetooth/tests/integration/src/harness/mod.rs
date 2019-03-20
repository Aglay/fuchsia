// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {failure::Error, futures::Future};

#[macro_use]
pub mod control;

#[macro_use]
pub mod host_driver;

#[macro_use]
pub mod low_energy_central;

/// Trait for a Harness that we can run tests with
pub trait TestHarness: Sized {
    fn run_with_harness<F, Fut>(test_func: F) -> Result<(), Error>
    where
        F: FnOnce(Self) -> Fut,
        Fut: Future<Output = Result<(), Error>>;
}

pub fn run_test<F, H, Fut>(test_func: F) -> Result<(), Error>
where
    F: FnOnce(H) -> Fut,
    Fut: Future<Output = Result<(), Error>>,
    H: TestHarness,
{
    let result = H::run_with_harness(test_func);
    if let Err(err) = &result {
        println!("\x1b[31mFAILED\x1b[0m");
        println!("Error running test: {}", err);
    } else {
        println!("\x1b[32mPASSED\x1b[0m");
    }
    result
}

// Prints out the test name and runs the test.
macro_rules! run_test {
    ($name:ident) => {{
        print!("{}...", stringify!($name));
        std::io::stdout().flush().unwrap();
        run_test($name)
    }};
}
