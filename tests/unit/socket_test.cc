/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2019 Elazar Leibovich
 */

#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/print.hh>
#include <seastar/core/memory.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>

#include <seastar/net/posix-stack.hh>

using namespace seastar;

future<> handle_connection(connected_socket s) {
    auto in = s.input();
    auto out = s.output();
    return do_with(std::move(in), std::move(out), [](auto& in, auto& out) {
        return do_until([&in]() { return in.eof(); },
            [&in, &out] {
                return in.read().then([&out](auto buf) {
                    return out.write(std::move(buf)).then([&out]() { return out.close(); });
                });
            });
    });
}

future<> echo_server_loop() {
    return do_with(
        server_socket(listen(make_ipv4_address({1234}), listen_options{.reuse_address = true})), [](auto& listener) {
              // Connect asynchronously in background.
              (void)connect(make_ipv4_address({"127.0.0.1", 1234})).then([](connected_socket&& socket) {
                  socket.shutdown_output();
              });
              return listener.accept().then(
                  [](accept_result ar) {
                      connected_socket s = std::move(ar.connection);
                      return handle_connection(std::move(s));
                  }).then([l = std::move(listener)]() mutable { return l.abort_accept(); });
        });
}

class my_malloc_allocator : public std::pmr::memory_resource {
public:
    int allocs;
    int frees;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override { allocs++; return malloc(bytes); }
    void do_deallocate(void *ptr, std::size_t bytes, std::size_t alignment) override { frees++; return free(ptr); }
    virtual bool do_is_equal(const std::pmr::memory_resource& __other) const noexcept override { abort(); }
};

my_malloc_allocator malloc_allocator;
std::pmr::polymorphic_allocator<char> allocator{&malloc_allocator};

SEASTAR_TEST_CASE(socket_allocation_test) {
    return echo_server_loop().finally([](){ engine().exit((malloc_allocator.allocs == malloc_allocator.frees) ? 0 : 1); });
}

SEASTAR_TEST_CASE(socket_skip_test) {
    return seastar::async([&] {
        listen_options lo;
        lo.reuse_address = true;
        server_socket ss = seastar::listen(ipv4_addr("127.0.0.1", 1234), lo);

        abort_source as;
        auto client = async([&as] {
            connected_socket socket = connect(ipv4_addr("127.0.0.1", 1234)).get();
            socket.output().write("abc").get();
            socket.shutdown_output();
            try {
                sleep_abortable(std::chrono::seconds(10), as).get();
            } catch (const sleep_aborted&) {
                // expected
                return;
            }
            assert(!"Skipping data from socket is likely stuck");
        });

        accept_result accepted = ss.accept().get();
        input_stream<char> input = accepted.connection.input();
        input.skip(16).get();
        as.request_abort();
        client.get();
    });
}

SEASTAR_TEST_CASE(test_file_desc_fdinfo) {
    auto fd = file_desc::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    auto info = fd.fdinfo();
    BOOST_REQUIRE_EQUAL(info.substr(0, 8), "socket:[");
    return make_ready_future<>();
}
