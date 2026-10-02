#include "Check.h"

int main()
{
	int failedTests = 0;
	for (const auto& t : check::Registry()) {
		const int before = check::Failures();
		std::printf("%s\n", t.name);
		t.fn();
		if (check::Failures() != before) {
			++failedTests;
		}
	}
	std::printf("%zu tests, %d failed\n", check::Registry().size(), failedTests);
	return failedTests == 0 ? 0 : 1;
}
