#include "kvdb/crc32.hpp"
#include "kvdb/record.hpp"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define REQUIRE(cond, msg)                                                        \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << msg \
                      << "\n";                                                    \
            return 1;                                                             \
        }                                                                         \
    } while (0)

int main() {
    using namespace kvdb;

    // 1. Verify IEEE 802.3 CRC32 standard check vector ("123456789" -> 0xCBF43926)
    const char* check_str = "123456789";
    std::uint32_t crc = Crc32::compute(check_str, 9);
    REQUIRE(crc == 0xCBF43926u, "IEEE CRC32 check vector failed");

    // 2. Verify 12-byte File Header encode/decode and corruption detection
    std::uint8_t fhdr_buf[kFileHeaderSize];
    encode_file_header(fhdr_buf);
    REQUIRE(fhdr_buf[0] == 'K' && fhdr_buf[1] == 'V' && fhdr_buf[2] == 'D' && fhdr_buf[3] == 'B',
            "File header magic bytes mismatch");

    FileHeader decoded_fhdr{};
    Status st = decode_and_validate_file_header(fhdr_buf, decoded_fhdr);
    REQUIRE(st.ok(), "Valid file header failed validation");
    REQUIRE(decoded_fhdr.version == 1 && decoded_fhdr.flags == 0, "File header fields mismatch");

    // Flip 1 bit in file header and confirm CORRUPT
    fhdr_buf[5] ^= 0x01u;
    st = decode_and_validate_file_header(fhdr_buf, decoded_fhdr);
    REQUIRE(st.is_corrupt(), "Corrupted file header was not rejected");

    // 3. Verify 32-byte Record Header encode/decode with 0-byte empty value
    std::vector<std::uint8_t> rec_buf;
    serialize_record(RecordType::PUT, kFlagNone, 42, 0, 1, "alpha_key", "", rec_buf);
    REQUIRE(rec_buf.size() == kRecordHeaderSize + 9, "Serialized record size mismatch");

    RecordHeader rhdr{};
    st = decode_record_header(rec_buf.data(), rhdr);
    REQUIRE(st.ok(), "Valid record header failed decode");
    REQUIRE(rhdr.magic == kRecordMagic, "Record magic mismatch");
    REQUIRE(rhdr.type == RecordType::PUT, "Record type mismatch");
    REQUIRE(rhdr.txn_id == 42, "Record txn_id mismatch");
    REQUIRE(rhdr.key_len == 9 && rhdr.value_len == 0, "Record key/value length mismatch");

    // 4. Verify boundary limits (1..1024 key bytes, 0..1,000,000 value bytes)
    REQUIRE(validate_key_size(0).is_invalid_argument(), "0-byte key must be rejected");
    REQUIRE(validate_key_size(1).ok(), "1-byte key must be accepted");
    REQUIRE(validate_key_size(1024).ok(), "1024-byte key must be accepted");
    REQUIRE(validate_key_size(1025).is_too_large(), "1025-byte key must be rejected");

    REQUIRE(validate_value_size(0).ok(), "0-byte value must be accepted");
    REQUIRE(validate_value_size(1000000).ok(), "1,000,000-byte value must be accepted");
    REQUIRE(validate_value_size(1000001).is_too_large(), "1,000,001-byte value must be rejected");

    // 5. Verify 36-byte Batch COMMIT marker encoding/decoding
    std::uint8_t commit_buf[kRecordHeaderSize + 4];
    serialize_commit_marker(99, 10, 0x12345678u, commit_buf);
    RecordHeader chdr{};
    st = decode_record_header(commit_buf, chdr);
    REQUIRE(st.ok(), "Valid COMMIT marker header failed decode");
    REQUIRE(chdr.type == RecordType::COMMIT, "COMMIT type mismatch");
    REQUIRE(chdr.op_index == 10 && chdr.op_count == 10, "COMMIT op_count mismatch");
    REQUIRE(read_le32(commit_buf + kRecordHeaderSize) == 0x12345678u, "COMMIT payload CRC mismatch");

    std::cout << "[PASS] test_record passed all assertions.\n";
    return 0;
}