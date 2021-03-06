#include <iostream>
#include <vector>

#include "testsuite.hpp"

static std::vector<abstract_test_case *> test_case_ptrs;

void abstract_test_case::register_case(abstract_test_case *tcp) {
	test_case_ptrs.push_back(tcp);
}

int main() {
	for(int s = 10; s < 24; s++) {
		int n = 1 << s;
		for(abstract_test_case *tcp : test_case_ptrs) {
			std::cout << "posix-torture: Running " << tcp->name()
					<< " for " << n << " iterations" << std::endl;
			for(int i = 0; i < n; i++)
				tcp->run();
		}
	}
}
