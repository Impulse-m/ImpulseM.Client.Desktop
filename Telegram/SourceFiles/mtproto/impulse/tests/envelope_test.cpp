#include "mtproto/impulse/core/envelope.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using Impulse::Envelope;
using Impulse::ParseEnvelope;

// Base64 "CAE=" decodes to the bytes 08 01.
const std::vector<std::uint8_t> kBodyBytes = { 0x08, 0x01 };

} // namespace

IMPULSE_TEST(EnvelopeNumberedFrame) {
	const Envelope envelope = ParseEnvelope(
		R"({"type":2,"pts":10,"ptsCount":1,"date":1700000000,"data":"CAE="})");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::Numbered);
	IMPULSE_CHECK(envelope.type == 2);
	IMPULSE_CHECK(envelope.pts == 10);
	IMPULSE_CHECK(envelope.ptsCount == 1);
	IMPULSE_CHECK(envelope.date == 1700000000);
	IMPULSE_CHECK(envelope.updatesProto == kBodyBytes);
}

IMPULSE_TEST(EnvelopePtsLessWhenNoPtsMembers) {
	const Envelope envelope = ParseEnvelope(
		R"({"type":3,"date":5,"data":"CAE"})");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::PtsLess);
	IMPULSE_CHECK(envelope.type == 3);
	IMPULSE_CHECK(envelope.pts == 0);
	IMPULSE_CHECK(envelope.ptsCount == 0);
	IMPULSE_CHECK(envelope.date == 5);
	IMPULSE_CHECK(envelope.updatesProto == kBodyBytes);
}

IMPULSE_TEST(EnvelopeUnnumberedWhenPtsAndCountAreZero) {
	const Envelope envelope = ParseEnvelope(
		R"({"type":3,"pts":0,"ptsCount":0,"date":5,"data":"CAE="})");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::Unnumbered);
	IMPULSE_CHECK(envelope.pts == 0);
	IMPULSE_CHECK(envelope.ptsCount == 0);
	IMPULSE_CHECK(envelope.updatesProto == kBodyBytes);
}

IMPULSE_TEST(EnvelopeNumberedKeepsExactInt64Pts) {
	const Envelope envelope = ParseEnvelope(
		R"({"type":2,"pts":9007199254740993,"ptsCount":1,"date":7,"data":"CAE="})");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::Numbered);
	IMPULSE_CHECK(envelope.pts == 9007199254740993LL);
}

IMPULSE_TEST(EnvelopeMissingPtsCountReadsAsZero) {
	const Envelope envelope = ParseEnvelope(
		R"({"type":2,"pts":4,"date":7,"data":"CAE="})");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::Numbered);
	IMPULSE_CHECK(envelope.pts == 4);
	IMPULSE_CHECK(envelope.ptsCount == 0);
}

IMPULSE_TEST(EnvelopeUpdatesTooLongByType) {
	const Envelope envelope = ParseEnvelope(
		R"({"type":221,"date":9,"data":"CAE="})");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::UpdatesTooLong);
	IMPULSE_CHECK(envelope.type == Impulse::kUpdatesTooLongType);
	IMPULSE_CHECK(envelope.pts == 0);
	IMPULSE_CHECK(envelope.date == 9);
	IMPULSE_CHECK(envelope.updatesProto == kBodyBytes);
}

IMPULSE_TEST(EnvelopeLegacySessionRevokedArray) {
	const Envelope envelope = ParseEnvelope(
		R"([{"event_type":"session_revoked","reason":"banned","api_id":4,"session_hash":12}])");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::LegacySessionRevoked);
	IMPULSE_CHECK(envelope.updatesProto.empty());
	IMPULSE_CHECK(envelope.decodable());
}

IMPULSE_TEST(EnvelopeEmptyLegacyArrayIsUndecodable) {
	const Envelope envelope = ParseEnvelope("[]");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(!envelope.decodable());
}

IMPULSE_TEST(EnvelopeLegacyArrayWithNonObjectIsUndecodable) {
	const Envelope envelope = ParseEnvelope(R"([{"reason":"x"}, 5])");
	IMPULSE_CHECK(envelope.kind == Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeNonObjectIsUndecodable) {
	IMPULSE_CHECK(ParseEnvelope(R"("CAE=")").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope("42").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope("null").kind == Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeInvalidJsonIsUndecodable) {
	IMPULSE_CHECK(ParseEnvelope("not json").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope("").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(
		ParseEnvelope(R"({"type":3,"date":5,"data":"CAE="} x)").kind
		== Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeMissingOrEmptyDataIsUndecodable) {
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":""})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":7})").kind == Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeNonBase64DataIsUndecodable) {
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":"C@E="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":"C"})").kind == Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeBase64UrlAlphabetAndWhitespace) {
	const Envelope urlSafe = ParseEnvelope(
		R"({"type":3,"date":5,"data":"CgD_"})");
	const std::vector<std::uint8_t> expected = { 0x0a, 0x00, 0xff };
	IMPULSE_CHECK(urlSafe.kind == Envelope::Kind::PtsLess);
	IMPULSE_CHECK(urlSafe.updatesProto == expected);

	const Envelope spaced = ParseEnvelope(
		R"({"type":3,"date":5,"data":"C A\nE="})");
	IMPULSE_CHECK(spaced.kind == Envelope::Kind::PtsLess);
	IMPULSE_CHECK(spaced.updatesProto == kBodyBytes);
}

IMPULSE_TEST(EnvelopeNonPositiveTypeOrDateIsUndecodable) {
	IMPULSE_CHECK(ParseEnvelope(R"({"type":0,"date":5,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":0,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":-5,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"date":5,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeNonIntegerOrOutOfRangeNumbersAreUndecodable) {
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3.5,"date":5,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":"3","date":5,"data":"CAE="})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":"CAE=","pts":-1,"ptsCount":1})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":"CAE=","pts":1,"ptsCount":2147483648})").kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(ParseEnvelope(R"({"type":3,"date":5,"data":"CAE=","pts":"7","ptsCount":1})").kind == Envelope::Kind::Undecodable);
}

IMPULSE_TEST(EnvelopeFactoriesSetFields) {
	const Envelope numbered = Envelope::Numbered(2, 33, 3, 44, kBodyBytes);
	IMPULSE_CHECK(numbered.kind == Envelope::Kind::Numbered);
	IMPULSE_CHECK(numbered.pts == 33);
	IMPULSE_CHECK(numbered.ptsCount == 3);
	IMPULSE_CHECK(numbered.date == 44);

	const Envelope tooLong = Envelope::UpdatesTooLong(55, kBodyBytes);
	IMPULSE_CHECK(tooLong.kind == Envelope::Kind::UpdatesTooLong);
	IMPULSE_CHECK(tooLong.type == Impulse::kUpdatesTooLongType);
	IMPULSE_CHECK(tooLong.date == 55);

	IMPULSE_CHECK(Envelope::Undecodable().kind == Envelope::Kind::Undecodable);
	IMPULSE_CHECK(!Envelope::Undecodable().decodable());
	IMPULSE_CHECK(Envelope::LegacySessionRevoked().decodable());
}
