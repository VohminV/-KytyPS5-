#ifndef KYTY_KERNEL_AMPR_PACKS_H_
#define KYTY_KERNEL_AMPR_PACKS_H_

#include "common/file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::AmprPacks {

struct FileInfo {
	std::string path;
	uint64_t    size        = 0;
	uint32_t    first_chunk = 0;
};

struct StatInfo {
	bool     is_dir = false;
	uint64_t size   = 0;
};

bool FindFile(const std::string& guest_path, FileInfo* out);
bool Stat(const std::string& guest_path, StatInfo* out);
bool Read(const FileInfo& file, uint64_t offset, void* buffer, uint64_t size,
          uint64_t* bytes_read);

std::vector<Common::File::DirEntry> ListDirectory(const std::string& guest_path);

} // namespace Libs::AmprPacks

#endif /* KYTY_KERNEL_AMPR_PACKS_H_ */
