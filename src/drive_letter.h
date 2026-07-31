#pragma once
#include <string>
#include <cstdint>

// Wait for a disk of given size to come online, assign it a drive letter.
// Returns drive letter like "R:" on success, empty string on failure.
std::string AssignDriveLetter(uint64_t diskSizeBytes);
