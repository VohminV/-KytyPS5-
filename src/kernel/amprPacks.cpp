#include "kernel/amprPacks.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kernel/fileSystem.h"

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::AmprPacks {

namespace {

constexpr uint64_t CHUNK_SIZE        = 256 * 1024;
constexpr uint32_t CHUNK_SHIFT       = 18;
constexpr uint32_t INDEX_HEADER_SIZE = 128;
constexpr uint32_t FILE_RECORD_SIZE  = 32;
constexpr uint32_t CHUNK_RECORD_SIZE = 8;
constexpr uint32_t VOLUME_RECORD_SIZE = 8;
constexpr uint64_t MAX_INDEX_SIZE    = 512ull * 1024 * 1024;
constexpr uint64_t VOLUME_LIMIT      = (uint64_t {1} << 32) - (uint64_t {1} << 16);

struct Chunk {
	uint32_t offset      = 0;
	uint32_t stored_size = 0;
	uint32_t volume      = 0;
	uint8_t  codec       = 0;
};

struct Record {
	std::string path;
	uint64_t    size        = 0;
	uint32_t    first_chunk = 0;
};

struct State {
	bool                 initialized = false;
	bool                 available   = false;
	std::filesystem::path root;
	std::vector<Record>   records;
	std::vector<Chunk>    chunks;
	std::vector<uint64_t> volumes;
	std::unordered_map<std::string, uint32_t> files;
	std::unordered_map<std::string, std::vector<Common::File::DirEntry>> dirs;
	std::vector<std::unique_ptr<Common::File>> volume_files;
	std::mutex           mutex;
};

State g_state;
std::once_flag g_once;

uint32_t ReadU32(const uint8_t* data, size_t size, size_t offset) {
	if (offset > size || size - offset < 4) {
		return 0;
	}
	uint32_t value = 0;
	std::memcpy(&value, data + offset, sizeof(value));
	return value;
}

uint64_t ReadU64(const uint8_t* data, size_t size, size_t offset) {
	if (offset > size || size - offset < 8) {
		return 0;
	}
	uint64_t value = 0;
	std::memcpy(&value, data + offset, sizeof(value));
	return value;
}

char FoldChar(char c) {
	if (c == '\\') {
		return '/';
	}
	if (c >= 'A' && c <= 'Z') {
		return static_cast<char>(c - 'A' + 'a');
	}
	return c;
}

std::string NormalizePath(const std::string& path) {
	std::string result;
	result.reserve(path.size() + 1);
	if (path.empty() || path.front() != '/') {
		result.push_back('/');
	}
	for (const char c: path) {
		const char folded = FoldChar(c);
		if (folded == '/' && !result.empty() && result.back() == '/') {
			continue;
		}
		result.push_back(folded);
	}
	while (result.size() > 1 && result.back() == '/') {
		result.pop_back();
	}
	return result;
}

bool EqualChild(const std::string& a, const std::string& b) {
	return a.size() == b.size() &&
	       std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		       return FoldChar(x) == FoldChar(y);
	       });
}

void AddChild(std::vector<Common::File::DirEntry>* entries, const std::string& name, bool is_file) {
	if (entries == nullptr || name.empty()) {
		return;
	}
	for (const auto& entry: *entries) {
		if (EqualChild(entry.name, name)) {
			return;
		}
	}
	entries->push_back({name, is_file});
}

void AddPath(State* state, const std::string& path) {
	std::vector<std::string> parts;
	size_t                   begin = path.front() == '/' ? 1 : 0;
	while (begin <= path.size()) {
		const size_t end = path.find('/', begin);
		const auto   len = (end == std::string::npos ? path.size() : end) - begin;
		if (len != 0) {
			parts.push_back(path.substr(begin, len));
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	if (parts.empty()) {
		return;
	}

	std::string parent;
	for (size_t i = 0; i + 1 < parts.size(); i++) {
		parent += "/";
		parent += parts[i];
		AddChild(&state->dirs[NormalizePath(parent)], parts[i + 1], i + 2 == parts.size());
	}
}

void Fail(const char* reason) {
	LOGF("AMPR: can't use ampr_zstd.index: %s\n", reason);
}

void Load(State* state) {
	const auto index_path = LibKernel::FileSystem::GetRealFilename("/app0/ampr_zstd.index");
	if (index_path.empty() || !Common::File::IsFileExisting(index_path)) {
		return;
	}

	Common::File file;
	if (!file.Open(index_path, Common::File::Mode::Read)) {
		Fail("index open failed");
		return;
	}
	const auto index_size = file.Size();
	if (index_size < INDEX_HEADER_SIZE || index_size > MAX_INDEX_SIZE) {
		Fail("index size");
		return;
	}
	auto storage = file.ReadWholeBuffer();
	file.Close();
	if (storage.size() != index_size) {
		Fail("index read failed");
		return;
	}
	const auto* data = reinterpret_cast<const uint8_t*>(storage.data());

	static constexpr char MAGIC[] = "AMPRZPK1";
	if (std::memcmp(data, MAGIC, 8) != 0 || ReadU32(data, index_size, 8) != 1 ||
	    ReadU32(data, index_size, 12) != INDEX_HEADER_SIZE) {
		Fail("bad header");
		return;
	}
	if (data[16] != CHUNK_SHIFT || data[17] < 1 || data[17] > 22) {
		Fail("unsupported chunk size or level");
		return;
	}
	const auto volume_count = ReadU32(data, index_size, 20);
	const auto file_count   = ReadU64(data, index_size, 40);
	const auto chunk_count  = ReadU64(data, index_size, 48);
	const auto files_off    = ReadU64(data, index_size, 56);
	const auto chunks_off   = ReadU64(data, index_size, 64);
	const auto volumes_off  = ReadU64(data, index_size, 72);
	const auto strings_off  = ReadU64(data, index_size, 80);
	const auto strings_len  = ReadU64(data, index_size, 88);
	if (volume_count == 0 || volume_count > 4096 || file_count == 0 || chunk_count == 0 ||
	    chunk_count > UINT32_MAX || file_count > UINT32_MAX) {
		Fail("bad counts");
		return;
	}
	if (files_off < INDEX_HEADER_SIZE || files_off > index_size ||
	    file_count > (index_size - files_off) / FILE_RECORD_SIZE ||
	    chunks_off > index_size || chunk_count > (index_size - chunks_off) / CHUNK_RECORD_SIZE ||
	    volumes_off > index_size ||
	    volume_count > (index_size - volumes_off) / VOLUME_RECORD_SIZE ||
	    strings_off > index_size || strings_len > index_size - strings_off) {
		Fail("bad table range");
		return;
	}

	state->records.reserve(static_cast<size_t>(file_count));
	state->chunks.reserve(static_cast<size_t>(chunk_count));
	state->volumes.reserve(volume_count);
	state->volume_files.resize(volume_count);

	for (uint32_t i = 0; i < volume_count; i++) {
		const auto volume_size = ReadU64(data, index_size, volumes_off + i * VOLUME_RECORD_SIZE);
		if (volume_size <= 64 || volume_size > VOLUME_LIMIT) {
			Fail("bad volume size");
			return;
		}
		char name[32] {};
		std::snprintf(name, sizeof(name), "ampr_zstd-%03u.pak", i);
		const auto volume_path = index_path.parent_path() / name;
		if (!Common::File::IsFileExisting(volume_path) ||
		    Common::File::Size(volume_path) < volume_size) {
			Fail("missing or short volume");
			return;
		}
		state->volumes.push_back(volume_size);
	}

	for (size_t i = 0; i < chunk_count; i++) {
		const auto word = ReadU64(data, index_size, chunks_off + i * CHUNK_RECORD_SIZE);
		Chunk       chunk {};
		chunk.offset      = static_cast<uint32_t>(word & 0xffffffffu);
		chunk.volume      = static_cast<uint32_t>((word >> 32) & 0xfffu);
		chunk.stored_size = static_cast<uint32_t>(((word >> 44) & 0x3ffffu) + 1);
		chunk.codec       = static_cast<uint8_t>(word >> 62);
		if (chunk.codec > 1 || chunk.volume >= state->volumes.size() ||
		    chunk.stored_size > CHUNK_SIZE || chunk.offset < 64 ||
		    static_cast<uint64_t>(chunk.offset) + chunk.stored_size > state->volumes[chunk.volume]) {
			Fail("bad chunk");
			return;
		}
		state->chunks.push_back(chunk);
	}

	for (uint64_t i = 0; i < file_count; i++) {
		const auto base = files_off + i * FILE_RECORD_SIZE;
		const auto size = ReadU64(data, index_size, base + 8);
		const auto first_chunk = ReadU32(data, index_size, base + 16);
		const auto path_offset = ReadU32(data, index_size, base + 20);
		const auto path_length = static_cast<uint32_t>(ReadU64(data, index_size, base + 24) & 0xffffu);
		if (path_length == 0 || path_offset > strings_len ||
		    path_length > strings_len - path_offset || path_length >= 1024) {
			Fail("bad file path");
			return;
		}
		const auto expected_chunks = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
		if (expected_chunks > UINT32_MAX || first_chunk > chunk_count ||
		    expected_chunks > chunk_count - first_chunk) {
			Fail("bad file chunk range");
			return;
		}
		const auto* path_data = data + strings_off + path_offset;
		std::string path(reinterpret_cast<const char*>(path_data), path_length);
		if (path.find('\0') != std::string::npos || path.find('\\') != std::string::npos ||
		    path.empty() || path.front() == '/') {
			Fail("invalid file path");
			return;
		}
		const auto guest_path = "/app0/" + path;
		const auto key        = NormalizePath(guest_path);
		if (state->files.contains(key)) {
			continue;
		}
		Record record {};
		record.path        = guest_path;
		record.size        = size;
		record.first_chunk = first_chunk;
		state->files.emplace(key, static_cast<uint32_t>(state->records.size()));
		state->records.push_back(std::move(record));
		AddPath(state, guest_path);
	}

	for (auto& [key, entries]: state->dirs) {
		(void)key;
		std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
			return Common::ToLower(a.name) < Common::ToLower(b.name);
		});
	}

	state->root      = index_path.parent_path();
	state->available = true;
	LOGF("AMPR: loaded %" PRIu64 " files, %" PRIu64 " chunks from %u zstd volumes\n", file_count,
	     chunk_count, volume_count);
}

void EnsureInitialized() {
	std::call_once(g_once, [] { Load(&g_state); g_state.initialized = true; });
}

} // namespace

bool FindFile(const std::string& guest_path, FileInfo* out) {
	if (out == nullptr) {
		return false;
	}
	EnsureInitialized();
	if (!g_state.available) {
		return false;
	}
	const auto key = NormalizePath(guest_path);
	std::scoped_lock lock(g_state.mutex);
	const auto       it = g_state.files.find(key);
	if (it == g_state.files.end()) {
		return false;
	}
	const auto& record = g_state.records[it->second];
	out->path          = record.path;
	out->size          = record.size;
	out->first_chunk   = record.first_chunk;
	return true;
}

bool Stat(const std::string& guest_path, StatInfo* out) {
	if (out == nullptr) {
		return false;
	}
	EnsureInitialized();
	if (!g_state.available) {
		return false;
	}
	const auto key = NormalizePath(guest_path);
	std::scoped_lock lock(g_state.mutex);
	const auto       file = g_state.files.find(key);
	if (file != g_state.files.end()) {
		out->is_dir = false;
		out->size   = g_state.records[file->second].size;
		return true;
	}
	const auto dir = g_state.dirs.find(key);
	if (dir != g_state.dirs.end()) {
		out->is_dir = true;
		out->size   = 0;
		return true;
	}
	return false;
}

std::vector<Common::File::DirEntry> ListDirectory(const std::string& guest_path) {
	EnsureInitialized();
	if (!g_state.available) {
		return {};
	}
	const auto key = NormalizePath(guest_path);
	std::scoped_lock lock(g_state.mutex);
	const auto       it = g_state.dirs.find(key);
	if (it == g_state.dirs.end()) {
		return {};
	}
	return it->second;
}

bool Read(const FileInfo& file, uint64_t offset, void* buffer, uint64_t size,
          uint64_t* bytes_read) {
	if (bytes_read == nullptr || (buffer == nullptr && size != 0)) {
		return false;
	}
	*bytes_read = 0;
	EnsureInitialized();
	if (!g_state.available || offset >= file.size || size == 0) {
		return true;
	}

	const uint64_t readable = std::min(size, file.size - offset);
	thread_local std::vector<uint8_t> compressed;
	thread_local std::vector<uint8_t> decoded;
	compressed.resize(static_cast<size_t>(CHUNK_SIZE));
	decoded.resize(static_cast<size_t>(CHUNK_SIZE));

	uint64_t done = 0;
	while (done < readable) {
		const uint64_t file_chunk = (offset + done) / CHUNK_SIZE;
		const uint64_t in_chunk   = (offset + done) % CHUNK_SIZE;
		Chunk          chunk {};
		{
			std::scoped_lock lock(g_state.mutex);
			const uint64_t chunk_index = static_cast<uint64_t>(file.first_chunk) + file_chunk;
			if (chunk_index >= g_state.chunks.size()) {
				return false;
			}
			chunk = g_state.chunks[chunk_index];
			auto& volume_file = g_state.volume_files[chunk.volume];
			if (!volume_file) {
				char name[32] {};
				std::snprintf(name, sizeof(name), "ampr_zstd-%03u.pak", chunk.volume);
				volume_file = std::make_unique<Common::File>();
				if (!volume_file->Open(g_state.root / name, Common::File::Mode::Read)) {
					volume_file.reset();
					return false;
				}
			}
			if (!volume_file->Seek(chunk.offset)) {
				return false;
			}
			uint32_t got = 0;
			volume_file->Read(compressed.data(), chunk.stored_size, &got);
			if (got != chunk.stored_size) {
				return false;
			}
		}

		size_t decoded_size = 0;
		if (chunk.codec == 0) {
			std::memcpy(decoded.data(), compressed.data(), chunk.stored_size);
			decoded_size = chunk.stored_size;
		} else {
			decoded_size = ZSTD_decompress(decoded.data(), decoded.size(), compressed.data(),
			                               chunk.stored_size);
			if (ZSTD_isError(decoded_size)) {
				static std::atomic<uint32_t> errors {0};
				if (errors.fetch_add(1, std::memory_order_relaxed) < 8) {
					LOGF("AMPR: zstd decode failed for %s: %s\n", file.path.c_str(),
					     ZSTD_getErrorName(decoded_size));
				}
				return false;
			}
		}

		const uint64_t expected =
		    std::min(CHUNK_SIZE, file.size - file_chunk * CHUNK_SIZE);
		if (decoded_size < expected) {
			std::memset(decoded.data() + decoded_size, 0, static_cast<size_t>(expected - decoded_size));
			decoded_size = static_cast<size_t>(expected);
		}
		if (in_chunk >= decoded_size) {
			break;
		}
		const uint64_t available = std::min<uint64_t>(decoded_size - in_chunk, readable - done);
		if (available == 0) {
			break;
		}
		std::memcpy(static_cast<uint8_t*>(buffer) + done, decoded.data() + in_chunk,
		            static_cast<size_t>(available));
		done += available;
	}

	*bytes_read = done;
	return true;
}

} // namespace Libs::AmprPacks
