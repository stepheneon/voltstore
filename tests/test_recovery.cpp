#include "test_harness.h"
#include "voltstore/storage_engine.h"
#include "voltstore/wal.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

TEST_CASE(wal_restart_recovery) {
    test::Workspace workspace("restart");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("a", "alpha");
        engine.put("b", "beta");
        engine.erase("b");
    }
    {
        voltstore::StorageEngine recovered({path, 8, true});
        REQUIRE(recovered.get("a") == "alpha");
        REQUIRE(!recovered.get("b"));
        REQUIRE(recovered.replay_report().applied >= 3);
    }
}

TEST_CASE(snapshot_then_restart) {
    test::Workspace workspace("snapshot");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("a", "1");
        engine.put("b", "2");
        engine.snapshot();
        REQUIRE(engine.stats().wal_bytes == 0);
        engine.put("c", "3");
    }
    {
        voltstore::StorageEngine recovered({path, 8, true});
        REQUIRE(recovered.get("a") == "1");
        REQUIRE(recovered.get("b") == "2");
        REQUIRE(recovered.get("c") == "3");
    }
}

TEST_CASE(partial_wal_tail_is_ignored) {
    test::Workspace workspace("partial");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("safe", "value");
    }
    {
        std::ofstream output(path / "wal.vlt", std::ios::binary | std::ios::app);
        output.write("XYZ", 3);
    }
    {
        voltstore::StorageEngine recovered({path, 8, true});
        REQUIRE(recovered.get("safe") == "value");
        REQUIRE(recovered.replay_report().ignored_partial_tail);
    }
}

TEST_CASE(complete_corrupt_wal_record_is_rejected) {
    test::Workspace workspace("corrupt");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("safe", "value");
    }
    {
        std::fstream file(path / "wal.vlt", std::ios::binary | std::ios::in |
                                                  std::ios::out);
        REQUIRE(file.good());
        constexpr std::streamoff value_offset = 24 + 4; // Header plus key "safe".
        file.seekg(value_offset);
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file.gcount() == 1);
        byte ^= 1;
        file.seekp(value_offset);
        file.write(&byte, 1);
        file.flush();
    }

    bool rejected = false;
    try {
        voltstore::StorageEngine recovered({path, 8, true});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(partial_wal_tail_is_repaired_before_new_appends) {
    test::Workspace workspace("partial-repair");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("before", "kept");
    }
    {
        std::ofstream output(path / "wal.vlt", std::ios::binary | std::ios::app);
        output.write("broken", 6);
    }
    {
        voltstore::StorageEngine recovered({path, 8, true});
        REQUIRE(recovered.replay_report().ignored_partial_tail);
        recovered.put("after", "also-kept");
    }
    {
        voltstore::StorageEngine recovered({path, 8, true});
        REQUIRE(recovered.get("before") == "kept");
        REQUIRE(recovered.get("after") == "also-kept");
        REQUIRE(!recovered.replay_report().ignored_partial_tail);
    }
}

TEST_CASE(snapshot_trailing_bytes_are_detected) {
    test::Workspace workspace("snapshot-trailing");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("key", "value");
        engine.snapshot();
    }
    {
        std::ofstream output(path / "snapshot.vlt", std::ios::binary | std::ios::app);
        output.write("unexpected", 10);
    }

    bool rejected = false;
    try {
        voltstore::StorageEngine recovered({path, 8, true});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(snapshot_truncation_is_detected) {
    test::Workspace workspace("snapshot-truncated");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("key", "value");
        engine.snapshot();
    }
    std::filesystem::resize_file(path / "snapshot.vlt", 15);

    bool rejected = false;
    try {
        voltstore::StorageEngine recovered({path, 8, true});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(wal_unreadable_on_restart_is_not_silently_ignored) {
    test::Workspace workspace("wal-unreadable");
    const auto wal_path = workspace.path() / "wal.vlt";
    {
        voltstore::StorageEngine engine({workspace.path(), 8, true});
        engine.put("persisted", "value");
    }

    // Run the permission test as a non-root user; restore permissions afterward.
    if (::geteuid() == 0) {
        return;
    }
    REQUIRE(::chmod(wal_path.c_str(), S_IWUSR) == 0);

    bool rejected = false;
    try {
        voltstore::StorageEngine recovered({workspace.path(), 8, true});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    REQUIRE(::chmod(wal_path.c_str(), S_IRUSR | S_IWUSR) == 0);
    REQUIRE(rejected);

    voltstore::StorageEngine recovered({workspace.path(), 8, true});
    REQUIRE(recovered.get("persisted") == "value");
}

TEST_CASE(wal_size_query_propagates_missing_file_error) {
    test::Workspace workspace("wal-size-error");
    const auto path = workspace.path() / "wal.vlt";
    voltstore::Wal wal(path, false);
    std::filesystem::rename(path, workspace.path() / "moved.wal");

    bool rejected = false;
    try {
        (void)wal.size_bytes();
    } catch (const std::filesystem::filesystem_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(snapshot_temp_file_failure_preserves_wal_recovery) {
    test::Workspace workspace("snapshot-temp-error");
    const auto& path = workspace.path();
    {
        voltstore::StorageEngine engine({path, 8, true});
        engine.put("before", "kept");
        engine.snapshot();
        engine.put("after", "from-wal");

        // A directory cannot be replaced by O_TRUNC file creation.
        std::filesystem::create_directory(path / "snapshot.vlt.tmp");
        bool rejected = false;
        try {
            engine.snapshot();
        } catch (const std::exception&) {
            rejected = true;
        }
        REQUIRE(rejected);
        std::filesystem::remove(path / "snapshot.vlt.tmp");
    }
    voltstore::StorageEngine recovered({path, 8, true});
    REQUIRE(recovered.get("before") == "kept");
    REQUIRE(recovered.get("after") == "from-wal");
}

TEST_CASE(data_directory_lock_rejects_second_engine_and_reopens_after_close) {
    test::Workspace workspace("directory-lock");
    const auto directory = workspace.path() / "storage";
    {
        voltstore::StorageEngine first({directory, 8, true});
        first.put("record", "saved");

        bool rejected = false;
        try {
            voltstore::StorageEngine second({directory, 8, true});
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("already in use") !=
                       std::string::npos;
        }
        REQUIRE(rejected);
        REQUIRE(first.get("record") == "saved");
    }
    voltstore::StorageEngine reopened({directory, 8, true});
    REQUIRE(reopened.get("record") == "saved");
    REQUIRE(std::filesystem::exists(directory / ".voltstore.lock"));
}

TEST_CASE(post_wal_allocation_failure_rejects_stale_reads_until_recovery) {
    test::Workspace workspace("post-wal-failure");
    bool inject_failure = false;
    voltstore::EngineConfig config{workspace.path(), 8, true};
    config.after_wal_append_hook = [&] {
        if (inject_failure) {
            inject_failure = false;
            throw std::bad_alloc();
        }
    };
    {
        voltstore::StorageEngine engine(config);
        engine.put("key", "old");
        inject_failure = true;
        bool failed = false;
        try {
            engine.put("key", "recovered");
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        REQUIRE(failed);

        const auto rejects_access = [](auto&& operation) {
            bool rejected = false;
            try {
                operation();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            REQUIRE(rejected);
        };
        rejects_access([&] { (void)engine.get("key"); });
        rejects_access([&] { engine.put("later", "value"); });
        rejects_access([&] { (void)engine.scan_prefix("k"); });
        rejects_access([&] { engine.snapshot(); });
        rejects_access([&] { (void)engine.stats(); });
    }
    voltstore::StorageEngine restored({workspace.path(), 8, true});
    REQUIRE(restored.get("key") == "recovered");
    REQUIRE(!restored.get("later"));
}

TEST_CASE(post_wal_delete_failure_recovers_delete_after_restart) {
    test::Workspace workspace("post-delete-failure");
    bool inject_failure = false;
    voltstore::EngineConfig config{workspace.path(), 8, true};
    config.after_wal_append_hook = [&] {
        if (inject_failure) {
            inject_failure = false;
            throw std::bad_alloc();
        }
    };
    {
        voltstore::StorageEngine engine(config);
        engine.put("remove", "original");
        inject_failure = true;
        bool failed = false;
        try {
            (void)engine.erase("remove");
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        REQUIRE(failed);
        bool rejected = false;
        try {
            (void)engine.exists("remove");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
    voltstore::StorageEngine restored({workspace.path(), 8, true});
    REQUIRE(!restored.get("remove"));
}

TEST_CASE(data_directory_lock_rejects_symlink_alias) {
    test::Workspace workspace("directory-alias");
    const auto directory = workspace.path() / "data";
    {
        voltstore::StorageEngine first({directory, 8, true});
        const auto alias = workspace.path() / "alias";
        std::filesystem::create_directory_symlink(directory, alias);
        bool rejected = false;
        try {
            voltstore::StorageEngine second({alias, 8, true});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
    voltstore::StorageEngine reopened({directory, 8, true});
    REQUIRE(reopened.stats().live_keys == 0);
}

TEST_CASE(data_directory_lock_file_cannot_be_symlink) {
    test::Workspace workspace("directory-lock-symlink");
    const auto directory = workspace.path() / "data";
    std::filesystem::create_directory(directory);
    const auto protected_file = workspace.path() / "protected";
    {
        std::ofstream output(protected_file);
        output << "preserve";
    }
    std::filesystem::create_symlink(protected_file, directory / ".voltstore.lock");

    bool rejected = false;
    try {
        voltstore::StorageEngine engine({directory, 8, true});
    } catch (const std::system_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
    std::ifstream input(protected_file);
    std::string value;
    input >> value;
    REQUIRE(value == "preserve");
}
