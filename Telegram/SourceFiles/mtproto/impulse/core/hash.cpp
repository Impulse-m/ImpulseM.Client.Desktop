/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/hash.h"

#include <algorithm>
#include <cstddef>

namespace Impulse {
namespace {

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kLengthFieldSize = 8;

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] std::span<const std::uint8_t> AsBytes(std::string_view text) {
	return std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t *>(text.data()),
		text.size());
}

[[nodiscard]] constexpr std::uint32_t RotateLeft(
		std::uint32_t value,
		unsigned int bits) {
	return (value << bits) | (value >> (32U - bits));
}

[[nodiscard]] constexpr std::uint32_t RotateRight(
		std::uint32_t value,
		unsigned int bits) {
	return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] constexpr std::uint32_t LoadBigEndian(const std::uint8_t *bytes) {
	return (static_cast<std::uint32_t>(bytes[0]) << 24)
		| (static_cast<std::uint32_t>(bytes[1]) << 16)
		| (static_cast<std::uint32_t>(bytes[2]) << 8)
		| static_cast<std::uint32_t>(bytes[3]);
}

// Feeds every 64-byte block of the message, including the padded tail
// (0x80, zeros, then the 64-bit big-endian bit length), to processBlock.
template <typename BlockFunction>
void ProcessMessage(
		std::span<const std::uint8_t> data,
		BlockFunction &&processBlock) {
	const std::size_t fullBlocks = data.size() / kBlockSize;
	for (std::size_t index = 0; index < fullBlocks; ++index) {
		processBlock(data.data() + index * kBlockSize);
	}

	const std::size_t rest = data.size() - fullBlocks * kBlockSize;
	std::array<std::uint8_t, 2 * kBlockSize> tail = {};
	std::copy_n(data.data() + fullBlocks * kBlockSize, rest, tail.begin());
	tail[rest] = 0x80;

	const std::size_t tailBlocks = (rest + 1 + kLengthFieldSize <= kBlockSize)
		? 1
		: 2;
	const std::size_t tailSize = tailBlocks * kBlockSize;
	const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8U;
	for (std::size_t byte = 0; byte < kLengthFieldSize; ++byte) {
		tail[tailSize - 1 - byte] = static_cast<std::uint8_t>(
			bitLength >> (8U * byte));
	}

	for (std::size_t index = 0; index < tailBlocks; ++index) {
		processBlock(tail.data() + index * kBlockSize);
	}
}

void Sha1Compress(std::array<std::uint32_t, 5> &state, const std::uint8_t *block) {
	std::array<std::uint32_t, 80> words = {};
	for (std::size_t index = 0; index < 16; ++index) {
		words[index] = LoadBigEndian(block + 4 * index);
	}
	for (std::size_t index = 16; index < 80; ++index) {
		words[index] = RotateLeft(
			words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16],
			1U);
	}

	std::uint32_t a = state[0];
	std::uint32_t b = state[1];
	std::uint32_t c = state[2];
	std::uint32_t d = state[3];
	std::uint32_t e = state[4];
	for (std::size_t index = 0; index < 80; ++index) {
		std::uint32_t f = 0;
		std::uint32_t k = 0;
		if (index < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999U;
		} else if (index < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1U;
		} else if (index < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCU;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6U;
		}
		const std::uint32_t temp = RotateLeft(a, 5U) + f + e + k + words[index];
		e = d;
		d = c;
		c = RotateLeft(b, 30U);
		b = a;
		a = temp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

void Sha256Compress(std::array<std::uint32_t, 8> &state, const std::uint8_t *block) {
	std::array<std::uint32_t, 64> words = {};
	for (std::size_t index = 0; index < 16; ++index) {
		words[index] = LoadBigEndian(block + 4 * index);
	}
	for (std::size_t index = 16; index < 64; ++index) {
		const std::uint32_t w15 = words[index - 15];
		const std::uint32_t w2 = words[index - 2];
		const std::uint32_t sigma0 = RotateRight(w15, 7U)
			^ RotateRight(w15, 18U)
			^ (w15 >> 3U);
		const std::uint32_t sigma1 = RotateRight(w2, 17U)
			^ RotateRight(w2, 19U)
			^ (w2 >> 10U);
		words[index] = sigma1 + words[index - 7] + sigma0 + words[index - 16];
	}

	std::uint32_t a = state[0];
	std::uint32_t b = state[1];
	std::uint32_t c = state[2];
	std::uint32_t d = state[3];
	std::uint32_t e = state[4];
	std::uint32_t f = state[5];
	std::uint32_t g = state[6];
	std::uint32_t h = state[7];
	for (std::size_t index = 0; index < 64; ++index) {
		const std::uint32_t bigSigma1 = RotateRight(e, 6U)
			^ RotateRight(e, 11U)
			^ RotateRight(e, 25U);
		const std::uint32_t choose = (e & f) ^ (~e & g);
		const std::uint32_t temp1 = h + bigSigma1 + choose
			+ kSha256Constants[index] + words[index];
		const std::uint32_t bigSigma0 = RotateRight(a, 2U)
			^ RotateRight(a, 13U)
			^ RotateRight(a, 22U);
		const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		const std::uint32_t temp2 = bigSigma0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

} // namespace

Sha1Digest Sha1(std::span<const std::uint8_t> data) {
	std::array<std::uint32_t, 5> state = {
		0x67452301U,
		0xEFCDAB89U,
		0x98BADCFEU,
		0x10325476U,
		0xC3D2E1F0U,
	};
	ProcessMessage(data, [&state](const std::uint8_t *block) {
		Sha1Compress(state, block);
	});

	Sha1Digest digest = {};
	for (std::size_t word = 0; word < state.size(); ++word) {
		for (std::size_t byte = 0; byte < 4; ++byte) {
			digest[word * 4 + byte] = static_cast<std::uint8_t>(
				state[word] >> (24U - 8U * byte));
		}
	}
	return digest;
}

Sha1Digest Sha1(std::string_view text) {
	return Sha1(AsBytes(text));
}

Sha256Digest Sha256(std::span<const std::uint8_t> data) {
	std::array<std::uint32_t, 8> state = {
		0x6a09e667U,
		0xbb67ae85U,
		0x3c6ef372U,
		0xa54ff53aU,
		0x510e527fU,
		0x9b05688cU,
		0x1f83d9abU,
		0x5be0cd19U,
	};
	ProcessMessage(data, [&state](const std::uint8_t *block) {
		Sha256Compress(state, block);
	});

	Sha256Digest digest = {};
	for (std::size_t word = 0; word < state.size(); ++word) {
		for (std::size_t byte = 0; byte < 4; ++byte) {
			digest[word * 4 + byte] = static_cast<std::uint8_t>(
				state[word] >> (24U - 8U * byte));
		}
	}
	return digest;
}

Sha256Digest Sha256(std::string_view text) {
	return Sha256(AsBytes(text));
}

} // namespace Impulse
