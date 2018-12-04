#!/boot/bin/sh

# !!! IF YOU CHANGE THIS FILE !!! ... please ensure it's in sync with
# topaz/bin/fidl_compatibility_test/run_fidl_compatibility_test_topaz.sh. (This
# file should be similar to that, but omit the Dart server.)

export FIDL_COMPATIBILITY_TEST_SERVERS=fidl_compatibility_test_server_cpp,fidl_compatibility_test_server_go,fidl_compatibility_test_server_rust
/pkgfs/packages/fidl_compatibility_test_bin/0/bin/app
