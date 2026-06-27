#include <scylladb/alternator/attribute_value.h>

#include <gtest/gtest.h>

using scylladb::alternator::AttributeValueType;
using scylladb::alternator::HashAttributeValue;
using scylladb::alternator::HashBinaryAttributeValue;

TEST(AttributeValueHash, MatchesCrossLanguageVectors) {
    EXPECT_EQ(HashAttributeValue(AttributeValueType::String, "hello"), 8815023923555918238LL);
    EXPECT_EQ(HashAttributeValue(AttributeValueType::String, ""), 8849112093580131862LL);
    EXPECT_EQ(HashAttributeValue(AttributeValueType::String, "user_123"), -4025731529809423594LL);
    EXPECT_EQ(HashAttributeValue(AttributeValueType::String, u8"こんにちは"), -8746014667889746860LL);

    EXPECT_EQ(HashAttributeValue(AttributeValueType::Number, "42"), -5061732451827723051LL);
    EXPECT_EQ(HashAttributeValue(AttributeValueType::Number, "-12345"), 2496798676881075539LL);
    EXPECT_EQ(HashAttributeValue(AttributeValueType::Number, "3.14159"), 2139945193071104172LL);
    EXPECT_EQ(HashAttributeValue(AttributeValueType::Number, "1.23E10"), -8571981415737439826LL);

    EXPECT_EQ(HashBinaryAttributeValue({0x01, 0x02, 0x03}), 5026299041734804437LL);
    EXPECT_EQ(HashBinaryAttributeValue({}), 8244620721157455449LL);
    EXPECT_EQ(HashBinaryAttributeValue({0xFF, 0x00, 0x80}), 14533934253577680LL);
}

TEST(AttributeValueHash, PreventsTypeCollisions) {
    const auto string_hash = HashAttributeValue(AttributeValueType::String, "12345");
    const auto number_hash = HashAttributeValue(AttributeValueType::Number, "12345");
    const auto binary_hash = HashBinaryAttributeValue({'1', '2', '3', '4', '5'});

    EXPECT_EQ(string_hash, -6122888897254035317LL);
    EXPECT_EQ(number_hash, -3190731486301745196LL);
    EXPECT_EQ(binary_hash, -3752463870508600385LL);
    EXPECT_NE(string_hash, number_hash);
    EXPECT_NE(string_hash, binary_hash);
    EXPECT_NE(number_hash, binary_hash);
}
