#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/loader/package_vfs.h"

namespace scarlettx {

struct XStorageReadRequest {
    std::string virtual_path;
    uint64_t offset = 0;
    size_t size = 0;
    uint8_t* destination = nullptr;
};

struct XStorageReadResult {
    Status status;
    size_t bytes_read = 0;
};

// DirectStorage / XStorage HLE queue for async package asset streaming.
class XStorageQueueHle {
public:
    explicit XStorageQueueHle(const PackageVfs& vfs);

    void EnqueueRead(const XStorageReadRequest& request);
    std::vector<XStorageReadResult> SubmitAndWait();
    size_t PendingCount() const noexcept { return pending_.size(); }

private:
    const PackageVfs& vfs_;
    std::vector<XStorageReadRequest> pending_;
};

}  // namespace scarlettx
