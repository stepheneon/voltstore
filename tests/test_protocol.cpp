#include "test_harness.h"
#include "voltstore/protocol.h"

TEST_CASE(protocol_put_preserves_spaces) {
    const auto command = voltstore::parse_command("PUT key 100 hello world");
    REQUIRE(command.type == voltstore::CommandType::Put);
    REQUIRE(command.key == "key");
    REQUIRE(command.value == "hello world");
    REQUIRE(command.ttl_ms.value() == 100);
}

TEST_CASE(protocol_rejects_bad_ttl) {
    const auto command = voltstore::parse_command("PUT key nope value");
    REQUIRE(command.type == voltstore::CommandType::Invalid);
    REQUIRE(!command.error.empty());
}

TEST_CASE(protocol_scan_limit) {
    const auto command = voltstore::parse_command("SCAN sensor: 12");
    REQUIRE(command.type == voltstore::CommandType::Scan);
    REQUIRE(command.limit == 12);
}

TEST_CASE(protocol_rejects_empty_and_malformed_commands) {
    REQUIRE(voltstore::parse_command(" ").type == voltstore::CommandType::Invalid);
    REQUIRE(voltstore::parse_command("GET").type == voltstore::CommandType::Invalid);
    REQUIRE(voltstore::parse_command("SCAN prefix 0").type == voltstore::CommandType::Invalid);
    REQUIRE(voltstore::parse_command("SCAN prefix 10001").type == voltstore::CommandType::Invalid);
    REQUIRE(voltstore::parse_command("PUT key -1 value").type == voltstore::CommandType::Invalid);
    REQUIRE(voltstore::parse_command("PING extra").type == voltstore::CommandType::Invalid);
}
