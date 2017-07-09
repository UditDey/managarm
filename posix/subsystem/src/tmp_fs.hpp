
#ifndef POSIX_SUBSYSTEM_TMP_FS_HPP
#define POSIX_SUBSYSTEM_TMP_FS_HPP

#include "vfs.hpp"

namespace tmp_fs {

std::shared_ptr<Node> createMemoryNode(std::string path);

std::shared_ptr<Link> createRoot();

} // namespace tmp_fs

#endif // POSIX_SUBSYSTEM_TMP_FS_HPP

