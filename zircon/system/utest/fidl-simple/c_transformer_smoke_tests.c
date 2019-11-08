// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>
#include <threads.h>
#include <zircon/fidl.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <fidl/test/echo/c/fidl.h>
#include <fidl/test/ctransformer/c/fidl.h>
#include <lib/async-loop/default.h>
#include <lib/async-loop/loop.h>
#include <lib/fidl-async/bind.h>
#include <lib/fidl/coding.h>
#include <lib/fidl/runtime_flag.h>
#include <lib/fidl/txn_header.h>
#include <unittest/unittest.h>

// V1 version of |example/Sandwich4|.
// This excerpt of bytes is taken directly from zircon/system/utest/fidl/transformer_tests.cc.
const uint8_t sandwich4_case1_v1[] = {
    0x01, 0x02, 0x03, 0x04,  // Sandwich4.before
    0x00, 0x00, 0x00, 0x00,  // Sandwich4.before (padding)

    0x04, 0x00, 0x00, 0x00,  // UnionSize36Alignment4.tag, i.e. Sandwich4.the_union
    0x00, 0x00, 0x00, 0x00,  // UnionSize36Alignment4.tag (padding)
    0x20, 0x00, 0x00, 0x00,  // UnionSize36Alignment4.env.num_bytes
    0x00, 0x00, 0x00, 0x00,  // UnionSize36Alignment4.env.num_handle
    0xff, 0xff, 0xff, 0xff,  // UnionSize36Alignment4.env.presence
    0xff, 0xff, 0xff, 0xff,  // UnionSize36Alignment4.env.presence [cont.]

    0x05, 0x06, 0x07, 0x08,  // Sandwich4.after
    0x00, 0x00, 0x00, 0x00,  // Sandwich4.after (padding)

    0xa0, 0xa1, 0xa2, 0xa3,  // UnionSize36Alignment4.data, i.e. Sandwich4.the_union.data
    0xa4, 0xa5, 0xa6, 0xa7,  // UnionSize36Alignment4.data [cont.]
    0xa8, 0xa9, 0xaa, 0xab,  // UnionSize36Alignment4.data [cont.]
    0xac, 0xad, 0xae, 0xaf,  // UnionSize36Alignment4.data [cont.]
    0xb0, 0xb1, 0xb2, 0xb3,  // UnionSize36Alignment4.data [cont.]
    0xb4, 0xb5, 0xb6, 0xb7,  // UnionSize36Alignment4.data [cont.]
    0xb8, 0xb9, 0xba, 0xbb,  // UnionSize36Alignment4.data [cont.]
    0xbc, 0xbd, 0xbe, 0xbf,  // UnionSize36Alignment4.data [cont.]
};

static int xunion_to_union_test_server(void* ctx) {
  zx_handle_t server = *(zx_handle_t*)ctx;
  zx_status_t status = ZX_OK;

  while (status == ZX_OK) {
    zx_signals_t observed;
    status = zx_object_wait_one(server, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                ZX_TIME_INFINITE, &observed);
    if ((observed & ZX_CHANNEL_READABLE) == 0) {
      break;
    }
    ASSERT_EQ(ZX_OK, status, "");
    char msg[1024];
    zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    uint32_t actual_bytes = 0u;
    uint32_t actual_handles = 0u;
    status = zx_channel_read(server, 0, msg, handles, sizeof(msg), ZX_CHANNEL_MAX_MSG_HANDLES,
                             &actual_bytes, &actual_handles);
    ASSERT_EQ(ZX_OK, status, "");
    ASSERT_GE(actual_bytes, sizeof(fidl_message_header_t), "");
    ASSERT_EQ(actual_handles, 0u, "");
    fidl_message_header_t* req = (fidl_message_header_t*)msg;

    // Respond with v1 version of |example/Sandwich4|.
    uint8_t response[sizeof(fidl_message_header_t) + sizeof(sandwich4_case1_v1)] = {};
    fidl_message_header_t* response_hdr = (fidl_message_header_t*)response;
    fidl_init_txn_header(response_hdr, req->txid, req->ordinal);
    // Set the flag indicating unions are encoded as xunions.
    response_hdr->flags[0] |= FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;
    memcpy(&response[sizeof(fidl_message_header_t)], sandwich4_case1_v1,
           sizeof(sandwich4_case1_v1));

    status = zx_channel_write(server, 0, response, sizeof(response), NULL, 0);
    ASSERT_EQ(ZX_OK, status, "");
  }

  zx_handle_close(server);
  return 0;
}

static bool xunion_to_union(void) {
  BEGIN_TEST;

  zx_handle_t client, server;
  zx_status_t status = zx_channel_create(0, &client, &server);
  ASSERT_EQ(ZX_OK, status, "");

  thrd_t thread;
  int rv = thrd_create(&thread, xunion_to_union_test_server, &server);
  ASSERT_EQ(thrd_success, rv, "");

  // Server is responding in v1 wire-format, but we should be able to receive it
  // as the old wire-format.
  example_Sandwich4 sandwich4;
  status = fidl_test_ctransformer_TestReceiveUnion(client, &sandwich4);
  EXPECT_EQ(ZX_OK, status, "");

  EXPECT_EQ(0x04030201, sandwich4.before, "");
  EXPECT_EQ(0x08070605, sandwich4.after, "");
  EXPECT_EQ(example_UnionSize36Alignment4Tag_variant, sandwich4.the_union.tag, "");
  for (uint8_t i = 0; i < sizeof(sandwich4.the_union.variant); i++) {
    EXPECT_EQ(sandwich4.the_union.variant[i], 0xa0 + i, "");
  }

  status = zx_handle_close(client);
  ASSERT_EQ(ZX_OK, status, "");

  int result = 0;
  rv = thrd_join(thread, &result);
  ASSERT_EQ(thrd_success, rv, "");

  END_TEST;
}

zx_status_t union_to_xunion_receive_union(void* ctx, fidl_txn_t* txn) {
  example_Sandwich4 sandwich = {};
  sandwich.before = 0x04030201;
  sandwich.after = 0x08070605;
  sandwich.the_union.tag = example_UnionSize36Alignment4Tag_variant;
  // Initialize the array in the union per the example on-the-wire content.
  for (uint8_t i = 0; i < sizeof(sandwich.the_union.variant); i++) {
    sandwich.the_union.variant[i] = 0xa0 + i;
  }
  return fidl_test_ctransformer_TestReceiveUnion_reply(txn, &sandwich);
}
static const fidl_test_ctransformer_Test_ops_t union_to_xunion_server_ops = {
    .ReceiveUnion = union_to_xunion_receive_union
};

static bool union_to_xunion(void) {
  BEGIN_TEST;

  // Note: Take care to reset the "encode union as xunion" global flag on all return paths.
#define RETURN_IF_FAILURE(expr)                                  \
  do {                                                           \
    zx_status_t _status = (expr);                                \
    EXPECT_EQ(ZX_OK, _status, "");                               \
    if (_status != ZX_OK) {                                      \
      fidl_global_set_should_write_union_as_xunion(old_flag); \
      if (loop) {                                                \
        async_loop_destroy(loop);                                \
      }                                                          \
      return false;                                              \
    }                                                            \
  } while (0);
  const bool old_flag = fidl_global_get_should_write_union_as_xunion();

  async_loop_t* loop = NULL;
  zx_handle_t client, server;
  RETURN_IF_FAILURE(zx_channel_create(0, &client, &server));

  RETURN_IF_FAILURE(async_loop_create(&kAsyncLoopConfigNoAttachToCurrentThread, &loop));
  RETURN_IF_FAILURE(async_loop_start_thread(loop, "union-to-xunion-test-dispatcher", NULL));
  async_dispatcher_t* dispatcher = async_loop_get_dispatcher(loop);
  RETURN_IF_FAILURE(fidl_bind(dispatcher, server,
                              (fidl_dispatch_t*)fidl_test_ctransformer_Test_dispatch, NULL,
                              &union_to_xunion_server_ops));

  // Send a request to the server and manually read out the response.
  // We should get the v1 wire-format because the server is configured to write xunions via the
  // global flag.
  {
    fidl_global_set_should_write_union_as_xunion(true);
    FIDL_ALIGNDECL fidl_test_ctransformer_TestReceiveUnionRequest request = {};
    zx_handle_t handles[1] = {};
    fidl_init_txn_header(&request.hdr, /* txid */ 1,
                         fidl_test_ctransformer_TestReceiveUnionOrdinal);
    RETURN_IF_FAILURE(zx_channel_write(client, 0, &request, sizeof(request), handles, 0));
    FIDL_ALIGNDECL uint8_t response_buf[512] = {};
    zx_signals_t observed;
    RETURN_IF_FAILURE(zx_object_wait_one(client, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                         ZX_TIME_INFINITE, &observed));
    if ((observed & ZX_CHANNEL_READABLE) == 0) {
      RETURN_IF_FAILURE(ZX_ERR_INTERNAL);
    }
    uint32_t actual_bytes = 0;
    uint32_t actual_handles = 0;
    RETURN_IF_FAILURE(zx_channel_read(client, 0, response_buf, handles, sizeof(response_buf), 0,
                                      &actual_bytes, &actual_handles));
    // Compare against golden bytes
    EXPECT_EQ(actual_handles, 0, "");
    EXPECT_EQ(actual_bytes, sizeof(fidl_message_header_t) + sizeof(sandwich4_case1_v1), "");
    for (uint32_t i = 0; i < sizeof(sandwich4_case1_v1); i++) {
      EXPECT_EQ(response_buf[sizeof(fidl_message_header_t) + i], sandwich4_case1_v1[i], "");
    }
  }

  // Send a request to the server and manually read out the response.
  // We should get the old wire-format because the server is configured to no longer write xunions.
  {
    fidl_global_set_should_write_union_as_xunion(false);
    FIDL_ALIGNDECL fidl_test_ctransformer_TestReceiveUnionRequest request = {};
    zx_handle_t handles[1] = {};
    fidl_init_txn_header(&request.hdr, /* txid */ 2,
                         fidl_test_ctransformer_TestReceiveUnionOrdinal);
    RETURN_IF_FAILURE(zx_channel_write(client, 0, &request, sizeof(request), handles, 0));
    FIDL_ALIGNDECL uint8_t response_buf[512] = {};
    zx_signals_t observed;
    RETURN_IF_FAILURE(zx_object_wait_one(client, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                         ZX_TIME_INFINITE, &observed));
    if ((observed & ZX_CHANNEL_READABLE) == 0) {
      RETURN_IF_FAILURE(ZX_ERR_INTERNAL);
    }
    uint32_t actual_bytes = 0;
    uint32_t actual_handles = 0;
    RETURN_IF_FAILURE(zx_channel_read(client, 0, response_buf, handles, sizeof(response_buf), 0,
                                      &actual_bytes, &actual_handles));
    // Attempt to decode the union normally, using coding table for the old wire-format
    RETURN_IF_FAILURE(fidl_decode(&fidl_test_ctransformer_TestReceiveUnionResponseTable,
                                  response_buf, actual_bytes, handles, actual_handles, NULL));
    example_Sandwich4 sandwich4 =
        ((fidl_test_ctransformer_TestReceiveUnionResponse*)response_buf)->sandwich4;
    EXPECT_EQ(example_UnionSize36Alignment4Tag_variant, sandwich4.the_union.tag, "");
    EXPECT_EQ(0x04030201, sandwich4.before, "");
    EXPECT_EQ(0x08070605, sandwich4.after, "");
    for (uint8_t i = 0; i < sizeof(sandwich4.the_union.variant); i++) {
      EXPECT_EQ(sandwich4.the_union.variant[i], 0xa0 + i, "");
    }
  }

  RETURN_IF_FAILURE(zx_handle_close(client));
  async_loop_destroy(loop);

  fidl_global_set_should_write_union_as_xunion(old_flag);

  END_TEST;
}

BEGIN_TEST_CASE(c_transformer_smoke_tests)
RUN_NAMED_TEST("Test xunion -> union transformer integration", xunion_to_union)
RUN_NAMED_TEST("Test union -> xunion transformer integration", union_to_xunion)
END_TEST_CASE(c_transformer_smoke_tests);
