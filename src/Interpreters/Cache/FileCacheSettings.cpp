#include "FileCacheSettings.h"

#include <Poco/Util/AbstractConfiguration.h>
#include <Common/Exception.h>
#include <IO/ReadHelpers.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

void FileCacheSettings::loadFromConfig(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    if (!config.has(config_prefix + ".path"))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Expected cache path (`path`) in configuration");

    base_path = config.getString(config_prefix + ".path");

    if (!config.has(config_prefix + ".max_size"))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Expected cache size (`max_size`) in configuration");

    max_size = parseWithSizeSuffix<uint64_t>(config.getString(config_prefix + ".max_size"));
    if (max_size == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Expected non-zero size for cache configuration");

    auto path = config.getString(config_prefix + ".path", "");
    if (path.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk Cache requires non-empty `path` field (cache base path) in config");

    max_elements = config.getUInt64(config_prefix + ".max_elements", REMOTE_FS_OBJECTS_CACHE_DEFAULT_MAX_ELEMENTS);
    if (config.has(config_prefix + ".max_file_segment_size"))
        max_file_segment_size = parseWithSizeSuffix<uint64_t>(config.getString(config_prefix + ".max_file_segment_size"));
    else
        max_file_segment_size = REMOTE_FS_OBJECTS_CACHE_DEFAULT_MAX_FILE_SEGMENT_SIZE;

    cache_on_write_operations = config.getUInt64(config_prefix + ".cache_on_write_operations", false);
    enable_filesystem_query_cache_limit = config.getUInt64(config_prefix + ".enable_filesystem_query_cache_limit", false);
    enable_cache_hits_threshold = config.getUInt64(config_prefix + ".enable_cache_hits_threshold", REMOTE_FS_OBJECTS_CACHE_ENABLE_HITS_THRESHOLD);

    enable_bypass_cache_with_threashold = config.getUInt64(config_prefix + ".enable_bypass_cache_with_threashold", false);

    if (config.has(config_prefix + ".bypass_cache_threashold"))
        bypass_cache_threashold = parseWithSizeSuffix<uint64_t>(config.getString(config_prefix + ".bypass_cache_threashold"));
    else
         bypass_cache_threashold = REMOTE_FS_OBJECTS_CACHE_BYPASS_THRESHOLD;

    do_not_evict_index_and_mark_files = config.getUInt64(config_prefix + ".do_not_evict_index_and_mark_files", false);
}

}
