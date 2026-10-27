// CRPC -- Redis scripting tests.
//
// The NOSCRIPT fallback is the whole point: a client that only calls EVALSHA works
// until the server restarts, then fails in production. The cases pin the fallback,
// the derived key count (a wrong count hashes arguments into the wrong cluster slot),
// and the digest check on SCRIPT LOAD.
#include "test_framework.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/redis/scripting.hpp>

#include "redis_test_support.hpp"

namespace {

using crpc::redis::command;
using crpc::redis::resp_value;
using crpc::redis::script;
using crpc::redis::script_runner;
using redis_test::array;
using redis_test::error;
using redis_test::integer;
using redis_test::text;

class recording_executor final : public crpc::redis::command_executor {
public:
    std::deque<resp_value> replies;
    std::vector<command> executed;

    crpc::result<resp_value> execute(const command& cmd) override {
        executed.push_back(cmd);
        if (replies.empty()) {
            return crpc::rpc_error{crpc::rpc_code::internal, "no scripted reply"};
        }
        resp_value reply = std::move(replies.front());
        replies.pop_front();
        return reply;
    }

    crpc::result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override {
        std::vector<resp_value> out;
        out.reserve(commands.size());
        for (const command& cmd : commands) {
            auto reply = execute(cmd);
            if (!reply) {
                return reply.error();
            }
            out.push_back(std::move(reply.value()));
        }
        return out;
    }
};

} // namespace

CRPC_TEST(script_digest_is_sha1_of_the_body) {
    const script body("return 1");
    // Redis identifies a script by the SHA-1 of its exact bytes.
    CRPC_CHECK_EQ(std::size_t{40}, body.digest().size());
    CRPC_CHECK_EQ(std::string("e0e1f9fabfc9d4800c877a703b823ac0578ff8db"), body.digest());
}

CRPC_TEST(script_run_uses_evalsha_first) {
    recording_executor executor;
    executor.replies.push_back(integer(7));

    const script body("return 7");
    script_runner runner(executor);
    auto reply = runner.run(body, {"k1", "k2"}, {"a"});
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(std::int64_t{7}, reply.value().as_integer());
    }

    const command& cmd = executor.executed.at(0);
    CRPC_CHECK_EQ(std::string("EVALSHA"), cmd[0]);
    CRPC_CHECK_EQ(body.digest(), cmd[1]);
    // The key count is derived from the key list, then keys, then argv.
    CRPC_CHECK_EQ(std::string("2"), cmd[2]);
    CRPC_CHECK_EQ(std::string("k1"), cmd[3]);
    CRPC_CHECK_EQ(std::string("k2"), cmd[4]);
    CRPC_CHECK_EQ(std::string("a"), cmd[5]);
}

CRPC_TEST(script_run_falls_back_to_eval_on_noscript) {
    recording_executor executor;
    executor.replies.push_back(
        error("NOSCRIPT No matching script. Please use EVAL."));
    executor.replies.push_back(text("ok"));

    const script body("return 'ok'");
    script_runner runner(executor);
    auto reply = runner.run(body, {"k"}, {});
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(std::string("ok"), reply.value().as_bulk_string());
    }

    // EVALSHA, then EVAL with the body — not a second EVALSHA.
    CRPC_CHECK_EQ(std::size_t{2}, executor.executed.size());
    CRPC_CHECK_EQ(std::string("EVALSHA"), executor.executed[0][0]);
    CRPC_CHECK_EQ(std::string("EVAL"), executor.executed[1][0]);
    CRPC_CHECK_EQ(body.source(), executor.executed[1][1]);
}

CRPC_TEST(script_run_does_not_retry_other_errors) {
    recording_executor executor;
    executor.replies.push_back(error("ERR user_script:1: bad"));
    const script body("bad()");
    script_runner runner(executor);
    auto reply = runner.run(body, {});
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        // A script error is data: the caller decides what it means.
        CRPC_CHECK(reply.value().is_error());
    }
    // No fallback: only NOSCRIPT means "send the body".
    CRPC_CHECK_EQ(std::size_t{1}, executor.executed.size());
}

CRPC_TEST(script_load_checks_the_digest) {
    const script body("return 1");
    {
        recording_executor executor;
        executor.replies.push_back(text(body.digest()));
        script_runner runner(executor);
        auto digest = runner.load(body);
        CRPC_CHECK(digest.ok());
        if (digest.ok()) {
            CRPC_CHECK_EQ(body.digest(), digest.value());
        }
        CRPC_CHECK_EQ(std::string("SCRIPT"), executor.executed[0][0]);
        CRPC_CHECK_EQ(std::string("LOAD"), executor.executed[0][1]);
    }
    {
        // A server that reports a different digest hashed different bytes; a later
        // EVALSHA with our digest would never hit, so this must be an error.
        recording_executor executor;
        executor.replies.push_back(text("0000000000000000000000000000000000000000"));
        script_runner runner(executor);
        CRPC_CHECK_FALSE(runner.load(body).ok());
    }
}

CRPC_TEST(script_exists_and_flush) {
    const script body("return 1");
    {
        recording_executor executor;
        executor.replies.push_back(array({integer(1)}));
        script_runner runner(executor);
        auto present = runner.exists(body);
        CRPC_CHECK(present.ok());
        if (present.ok()) {
            CRPC_CHECK(present.value());
        }
    }
    {
        recording_executor executor;
        executor.replies.push_back(array({integer(0)}));
        script_runner runner(executor);
        auto present = runner.exists(body);
        CRPC_CHECK(present.ok());
        if (present.ok()) {
            CRPC_CHECK_FALSE(present.value());
        }
    }
    {
        recording_executor executor;
        executor.replies.push_back(text("OK"));
        script_runner runner(executor);
        CRPC_CHECK(runner.flush().ok());
        CRPC_CHECK_EQ(std::string("FLUSH"), executor.executed[0][1]);
    }
}

CRPC_TEST(script_run_source_builds_the_script) {
    recording_executor executor;
    executor.replies.push_back(integer(1));
    script_runner runner(executor);
    auto reply = runner.run_source("return 1", {});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK_EQ(std::string("0"), executor.executed[0][2]);
}

CRPC_TEST_MAIN()