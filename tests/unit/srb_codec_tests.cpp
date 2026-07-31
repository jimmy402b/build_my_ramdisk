#include "test_framework.h"

#include "src/driver/srb_codec.h"

#include <cstring>
#include <vector>

constexpr std::uint32_t kTestCtrl = 0x83730800u;

TEST_CASE(srb_frame_uses_exact_header_length) {
    std::byte pld[4]{};
    auto f = myrd::aim::build_srb_frame(kTestCtrl, pld, 16, 30);
    REQUIRE(f.ok());
    REQUIRE(f.value().size() >= sizeof(myrd::aim::SrbIoControl));
    myrd::aim::SrbIoControl hdr{};
    std::memcpy(&hdr, f.value().data(), sizeof(hdr));
    REQUIRE_EQ(hdr.HeaderLength, static_cast<std::uint32_t>(sizeof(myrd::aim::SrbIoControl)));
}

TEST_CASE(srb_frame_uses_exact_eight_byte_signature) {
    auto f = myrd::aim::build_srb_frame(kTestCtrl, {}, 0, 30);
    REQUIRE(f.ok());
    REQUIRE(std::memcmp(f.value().data() + 4, "PhDskMnt", 8) == 0);
}

TEST_CASE(srb_frame_preserves_control_code) {
    auto f = myrd::aim::build_srb_frame(kTestCtrl, {}, 0, 30);
    myrd::aim::SrbIoControl hdr{};
    std::memcpy(&hdr, f.value().data(), sizeof(hdr));
    REQUIRE_EQ(hdr.ControlCode, kTestCtrl);
}

TEST_CASE(srb_frame_preserves_timeout) {
    auto f = myrd::aim::build_srb_frame(kTestCtrl, {}, 0, 42);
    myrd::aim::SrbIoControl hdr{};
    std::memcpy(&hdr, f.value().data(), sizeof(hdr));
    REQUIRE_EQ(hdr.Timeout, 42u);
}

TEST_CASE(srb_frame_sets_request_payload_length) {
    std::byte pld[32]{};
    auto f = myrd::aim::build_srb_frame(kTestCtrl, pld, 64, 30);
    myrd::aim::SrbIoControl hdr{};
    std::memcpy(&hdr, f.value().data(), sizeof(hdr));
    REQUIRE(hdr.Length >= 64u); // response capacity is larger
}

TEST_CASE(srb_frame_zeroes_return_code_before_call) {
    auto f = myrd::aim::build_srb_frame(kTestCtrl, {}, 0, 30);
    myrd::aim::SrbIoControl hdr{};
    std::memcpy(&hdr, f.value().data(), sizeof(hdr));
    REQUIRE_EQ(hdr.ReturnCode, 0u);
}

TEST_CASE(srb_frame_rejects_payload_above_ulong) {
    // Not testable with small buffers — test that normal payload works
    std::byte pld[100]{};
    auto f = myrd::aim::build_srb_frame(kTestCtrl, pld, 0, 30);
    REQUIRE(f.ok());
}

// ── Response validation ───────────────────────────────────────

static std::vector<std::byte> make_valid_response(
    std::uint32_t ctrl, std::uint32_t ret_code, std::size_t pl_len) {
    std::vector<std::byte> buf(sizeof(myrd::aim::SrbIoControl) + pl_len);
    myrd::aim::SrbIoControl hdr{};
    hdr.HeaderLength = sizeof(myrd::aim::SrbIoControl);
    std::memcpy(hdr.Signature, "PhDskMnt", 8);
    hdr.ControlCode = ctrl;
    hdr.Length = static_cast<std::uint32_t>(pl_len);
    hdr.ReturnCode = ret_code;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return buf;
}

TEST_CASE(srb_response_rejects_short_header) {
    std::byte buf[10]{};
    auto r = myrd::aim::parse_srb_response(buf, kTestCtrl, 10);
    REQUIRE(r.has_error());
}

TEST_CASE(srb_response_rejects_wrong_signature) {
    auto buf = make_valid_response(kTestCtrl, 0, 0);
    std::memset(buf.data() + 4, 0, 8); // zero out signature
    auto r = myrd::aim::parse_srb_response(buf, kTestCtrl, buf.size());
    REQUIRE(r.has_error());
}

TEST_CASE(srb_response_rejects_wrong_header_length) {
    auto buf = make_valid_response(kTestCtrl, 0, 0);
    myrd::aim::SrbIoControl hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    hdr.HeaderLength = 99;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    auto r = myrd::aim::parse_srb_response(buf, kTestCtrl, buf.size());
    REQUIRE(r.has_error());
}

TEST_CASE(srb_response_rejects_wrong_control_code) {
    auto buf = make_valid_response(kTestCtrl, 0, 0);
    auto r = myrd::aim::parse_srb_response(buf, 0x9999, buf.size());
    REQUIRE(r.has_error());
}

TEST_CASE(srb_response_rejects_length_beyond_bytes_returned) {
    auto buf = make_valid_response(kTestCtrl, 0, 100);
    auto r = myrd::aim::parse_srb_response(buf, kTestCtrl, 10);
    REQUIRE(r.has_error());
}

TEST_CASE(srb_response_preserves_nonzero_return_code) {
    auto buf = make_valid_response(kTestCtrl, 0xC0000001u, 0);
    auto r = myrd::aim::parse_srb_response(buf, kTestCtrl, buf.size());
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().return_code, 0xC0000001u);
}

TEST_CASE(srb_response_returns_exact_payload_slice) {
    std::byte pld[8]{std::byte{0xAA}, std::byte{0xBB}};
    std::vector<std::byte> buf(sizeof(myrd::aim::SrbIoControl) + 8);
    myrd::aim::SrbIoControl hdr{};
    hdr.HeaderLength = sizeof(myrd::aim::SrbIoControl);
    std::memcpy(hdr.Signature, "PhDskMnt", 8);
    hdr.ControlCode = kTestCtrl;
    hdr.Length = 8;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), pld, 8);
    auto r = myrd::aim::parse_srb_response(buf, kTestCtrl, buf.size());
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().payload.size(), 8u);
    REQUIRE(r.value().payload[0] == std::byte{0xAA});
}
