// macOS trash via NSFileManager, which also records "Put Back" information for Finder.
#import <Foundation/Foundation.h>

#include "storage/core/filesystem.hpp"

namespace storage {

std::error_code local::moveToTrash(const std::string& path, std::string& location) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        NSURL* result = nil;
        NSError* error = nil;
        if (![[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:&result error:&error]) {
            NSError* underlying = error.userInfo[NSUnderlyingErrorKey];
            if (underlying && [underlying.domain isEqualToString:NSPOSIXErrorDomain])
                return std::error_code(static_cast<int>(underlying.code), std::system_category());
            return std::make_error_code(std::errc::io_error);
        }
        location = result ? std::string(result.fileSystemRepresentation) : std::string();
        return {};
    }
}

}  // namespace storage
