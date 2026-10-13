#include "test.hpp"
#include "webviewcpp/json.hpp"

using wvc::Json;

TEST(json_primitives) {
    CHECK(Json().is_null());
    CHECK(Json(true).as_bool());
    CHECK_EQ(Json(42).as_int(), 42);
    CHECK(Json(3.5).as_double() > 3.4);
    CHECK(Json("hi").is_string());
    CHECK_EQ(Json("hi").as_string(), "hi");
}

TEST(json_build_and_dump) {
    Json obj;
    obj["name"] = "WebViewCpp";
    obj["version"] = 1;
    obj["enabled"] = true;
    obj["tags"].push_back("a");
    obj["tags"].push_back("b");

    const std::string text = obj.dump();
    CHECK(text.find("\"name\":\"WebViewCpp\"") != std::string::npos);
    CHECK(text.find("\"tags\":[\"a\",\"b\"]") != std::string::npos);
}

TEST(json_roundtrip) {
    const std::string src =
        R"({"n":1,"f":2.5,"s":"he said \"hi\"\n","arr":[1,2,3],"nested":{"x":null},"u":"\u00e9"})";
    Json parsed = Json::parse(src);
    CHECK_EQ(parsed["n"].as_int(), 1);
    CHECK(parsed["f"].as_double() > 2.4);
    CHECK_EQ(parsed["arr"].size(), std::size_t(3));
    CHECK(parsed["nested"]["x"].is_null());
    CHECK_EQ(parsed["u"].as_string(), std::string("\xc3\xa9"));  // é in UTF-8

    Json again = Json::parse(parsed.dump());
    CHECK_EQ(again["s"].as_string(), parsed["s"].as_string());
}

TEST(json_missing_keys_are_null) {
    Json obj = Json::parse(R"({"a":1})");
    CHECK(obj["missing"].is_null());
    CHECK(obj["missing"]["deeper"].is_null());
    CHECK_EQ(obj["a"].int_or(0), 1);
    CHECK_EQ(obj["missing"].int_or(7), 7);
}

TEST(json_errors) {
    CHECK_THROWS(Json::parse("{"));
    CHECK_THROWS(Json::parse("[1,2"));
    CHECK_THROWS(Json::parse("nope"));
    CHECK(Json::parse_or("{bad", Json(0)).is_int());
}
