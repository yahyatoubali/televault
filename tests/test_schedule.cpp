#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "schedule/schedule.hpp"
#include "schedule/systemd.hpp"

using namespace tv;
namespace fs = std::filesystem;

namespace {

std::string make_sandbox() {
    auto dir = fs::temp_directory_path() /
               ("tvt_sched_test_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    return dir.string();
}

} // namespace

TEST(ScheduleTest, IntervalRoundTrip) {
    Interval iv = Interval::Daily;
    EXPECT_TRUE(interval_from_string("hourly", iv));
    EXPECT_EQ(iv, Interval::Hourly);
    EXPECT_TRUE(interval_from_string("WEEKLY", iv));
    EXPECT_EQ(iv, Interval::Weekly);
    EXPECT_TRUE(interval_from_string("m", iv));
    EXPECT_EQ(iv, Interval::Monthly);
    EXPECT_FALSE(interval_from_string("yearly", iv));
    EXPECT_EQ(interval_to_string(Interval::Daily), "daily");
}

TEST(ScheduleTest, CrudRoundTrip) {
    make_sandbox();
    ScheduleManager mgr;

    ScheduleEntry e;
    e.name = "nightly_docs";
    e.path = "/tmp/some_docs";
    e.interval = Interval::Daily;
    e.password = "s3cret";
    e.incremental = true;
    EXPECT_TRUE(mgr.create(e));

    // Password companion stored with strict permissions
    auto env = fs::path(std::getenv("XDG_CONFIG_HOME")) /
               "televault" / "schedules" / "nightly_docs.env";
    ASSERT_TRUE(fs::exists(env));
    EXPECT_EQ(fs::status(env).permissions() & fs::perms::others_all,
              fs::perms::none);

    ScheduleEntry loaded;
    ASSERT_TRUE(mgr.load("nightly_docs", loaded));
    EXPECT_EQ(loaded.path, "/tmp/some_docs");
    EXPECT_EQ(loaded.interval, Interval::Daily);
    EXPECT_EQ(loaded.password, "s3cret");
    EXPECT_TRUE(loaded.incremental);

    auto all = mgr.list();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].name, "nightly_docs");

    EXPECT_TRUE(mgr.remove("nightly_docs"));
    EXPECT_FALSE(mgr.load("nightly_docs", loaded));
    EXPECT_TRUE(mgr.list().empty());
    EXPECT_FALSE(mgr.remove("nightly_docs"));
}

TEST(ScheduleTest, RejectsBadNames) {
    make_sandbox();
    ScheduleManager mgr;
    ScheduleEntry e;
    e.path = "/tmp";
    e.name = "../evil";
    EXPECT_FALSE(mgr.create(e));
    e.name = "";
    EXPECT_FALSE(mgr.create(e));
    e.name = "ok-name_1";
    e.path = "";
    EXPECT_FALSE(mgr.create(e));
}

TEST(ScheduleTest, CronEntries) {
    ScheduleManager mgr;
    ScheduleEntry e;
    e.name = "job";
    e.path = "/data/docs";
    e.interval = Interval::Hourly;
    auto line = mgr.generate_cron_entry(e);
    EXPECT_NE(line.find("@hourly"), std::string::npos);
    EXPECT_NE(line.find("backup create"), std::string::npos);
    EXPECT_NE(line.find("/data/docs"), std::string::npos);
    e.interval = Interval::Monthly;
    EXPECT_NE(mgr.generate_cron_entry(e).find("@monthly"), std::string::npos);
}

TEST(ScheduleTest, SystemdUnits) {
    // Pure generators, no systemd needed.
    auto timer = generate_timer_unit("nightly", "daily");
    EXPECT_NE(timer.find("OnCalendar=daily"), std::string::npos);
    EXPECT_NE(timer.find("televault-sched-nightly.service"), std::string::npos);
    auto svc = generate_service_unit("nightly", "televault backup create /x");
    EXPECT_NE(svc.find("ExecStart=televault backup create /x"), std::string::npos);
    EXPECT_NE(svc.find("Type=oneshot"), std::string::npos);
    // Names are sanitized for unit file safety.
    EXPECT_NE(generate_timer_unit("a/b", "daily").find("televault-sched-a_b.service"),
              std::string::npos);
}
