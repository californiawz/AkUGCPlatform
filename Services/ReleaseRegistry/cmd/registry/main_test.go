package main

import "testing"

func TestLocalRegistryAddress(t *testing.T) {
	t.Parallel()
	testCases := []struct {
		name       string
		configured string
		expected   string
		valid      bool
	}{
		{name: "default", expected: "127.0.0.1:8088", valid: true},
		{name: "IPv4 loopback", configured: "127.0.0.2:9000", expected: "127.0.0.2:9000", valid: true},
		{name: "IPv6 loopback", configured: "[::1]:9000", expected: "[::1]:9000", valid: true},
		{name: "localhost", configured: "localhost:9000", expected: "localhost:9000", valid: true},
		{name: "all IPv4", configured: "0.0.0.0:9000"},
		{name: "all interfaces", configured: ":9000"},
		{name: "remote hostname", configured: "registry.example:9000"},
		{name: "missing port", configured: "127.0.0.1"},
	}
	for _, testCase := range testCases {
		testCase := testCase
		t.Run(testCase.name, func(t *testing.T) {
			t.Parallel()
			actual, err := localRegistryAddress(testCase.configured)
			if testCase.valid {
				if err != nil || actual != testCase.expected {
					t.Fatalf("address = %q, err = %v", actual, err)
				}
				return
			}
			if err == nil {
				t.Fatalf("non-loopback address %q was accepted as %q", testCase.configured, actual)
			}
		})
	}
}
