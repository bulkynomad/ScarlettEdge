#include "scarlettx/kernel/xstorage.h"

#include <algorithm>
#include <cstring>

namespace scarlettx {

XStorageQueueHle::XStorageQueueHle(const PackageVfs& vfs) : vfs_(vfs) {}

void XStorageQueueHle::EnqueueRead(const XStorageReadRequest& request) {
    pending_.push_back(request);
}

std::vector<XStorageReadResult> XStorageQueueHle::SubmitAndWait() {
    std::vector<XStorageReadResult> results;
    results.reserve(pending_.size());

    for (const auto& req : pending_) {
        XStorageReadResult res{};
        if (!req.destination || req.size == 0) {
            res.status = Status::Error(StatusCode::InvalidArgument, "Invalid read destination/size");
            results.push_back(std::move(res));
            continue;
        }
        std::vector<uint8_t> file_bytes;
        Status st = vfs_.ReadFileBytes(req.virtual_path, file_bytes);
        if (!st.ok()) {
            res.status = std::move(st);
            results.push_back(std::move(res));
            continue;
        }
        if (req.offset > file_bytes.size()) {
            res.status = Status::Error(StatusCode::InvalidArgument, "Read offset past EOF");
            results.push_back(std::move(res));
            continue;
        }
        const size_t avail = file_bytes.size() - static_cast<size_t>(req.offset);
        const size_t to_copy = std::min(avail, req.size);
        std::memcpy(req.destination, file_bytes.data() + req.offset, to_copy);
        res.bytes_read = to_copy;
        res.status = Status::Ok();
        results.push_back(std::move(res));
    }

    pending_.clear();
    return results;
}

}  // namespace scarlettx
