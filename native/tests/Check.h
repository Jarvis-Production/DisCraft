#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

// A tiny test harness: TEST registers, CHECK records failures and keeps going.
namespace check
{
	struct Test
	{
		const char*           name;
		std::function<void()> fn;
	};

	inline std::vector<Test>& Registry()
	{
		static std::vector<Test> tests;
		return tests;
	}

	inline int& Failures()
	{
		static int failures = 0;
		return failures;
	}

	template <class T>
	std::string Show(const T& a_value)
	{
		std::ostringstream out;
		if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
			out << static_cast<long long>(a_value) << " (0x" << std::hex << static_cast<unsigned long long>(a_value) << ")";
		} else {
			out << a_value;
		}
		return out.str();
	}

	struct Register
	{
		Register(const char* a_name, std::function<void()> a_fn) { Registry().push_back({ a_name, std::move(a_fn) }); }
	};
}

#define DC_CONCAT2(a, b) a##b
#define DC_CONCAT(a, b) DC_CONCAT2(a, b)
#define TEST(name)                                                              \
	static void           name();                                               \
	static check::Register DC_CONCAT(reg_, name)(#name, name);                  \
	static void           name()

#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
			++check::Failures();                                                       \
		}                                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                                                     \
	do {                                                                                                   \
		const auto va = (a);                                                                               \
		const auto vb = (b);                                                                               \
		if (!(va == vb)) {                                                                                 \
			std::printf("  FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a, #b,                \
				check::Show(va).c_str(), check::Show(vb).c_str());                                         \
			++check::Failures();                                                                           \
		}                                                                                                  \
	} while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
	do {                                                                                           \
		const double va = (a);                                                                     \
		const double vb = (b);                                                                     \
		if (!(std::fabs(va - vb) <= (eps))) {                                                      \
			std::printf("  FAIL %s:%d: %s ~= %s (%f vs %f)\n", __FILE__, __LINE__, #a, #b, va, vb); \
			++check::Failures();                                                                   \
		}                                                                                          \
	} while (0)
