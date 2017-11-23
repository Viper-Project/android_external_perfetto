/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ftrace_reader/ftrace_controller.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cpu_reader.h"
#include "ftrace_procfs.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "proto_translation_table.h"

using testing::_;
using testing::Return;
using testing::ByMove;
using testing::AnyNumber;

namespace perfetto {

namespace {

using Table = ProtoTranslationTable;

class MockTaskRunner : public base::TaskRunner {
 public:
  MOCK_METHOD1(PostTask, void(std::function<void()>));
  MOCK_METHOD2(PostDelayedTask, void(std::function<void()>, int delay_ms));
  MOCK_METHOD2(AddFileDescriptorWatch, void(int fd, std::function<void()>));
  MOCK_METHOD1(RemoveFileDescriptorWatch, void(int fd));
};

class MockDelegate : public perfetto::FtraceSink::Delegate {
 public:
  MOCK_METHOD1(
      GetBundleForCpu,
      protozero::ProtoZeroMessageHandle<pbzero::FtraceEventBundle>(size_t));
  MOCK_METHOD2(
      OnBundleComplete_,
      void(size_t,
           protozero::ProtoZeroMessageHandle<pbzero::FtraceEventBundle>&));

  void OnBundleComplete(
      size_t cpu,
      protozero::ProtoZeroMessageHandle<pbzero::FtraceEventBundle> bundle) {
    OnBundleComplete_(cpu, bundle);
  }
};

class MockCpuReader : public CpuReader {
 public:
  MockCpuReader(size_t cpu, int fd)
      : CpuReader(nullptr, cpu, base::ScopedFile(fd)) {}
  MockCpuReader(MockCpuReader&&) = default;

 private:
  MockCpuReader(const MockCpuReader&) = delete;
  MockCpuReader& operator=(const MockCpuReader&) = delete;
};

std::unique_ptr<Table> FakeTable() {
  using Event = Table::Event;
  using Field = Table::Field;

  std::vector<Field> common_fields;
  std::vector<Event> events;

  {
    Event event;
    event.name = "foo";
    event.group = "group";
    event.ftrace_event_id = 1;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "bar";
    event.group = "group";
    event.ftrace_event_id = 10;
    events.push_back(event);
  }

  return std::unique_ptr<Table>(new Table(events, std::move(common_fields)));
}

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(1));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());
  }

  MOCK_METHOD2(WriteToFile,
               bool(const std::string& path, const std::string& str));
  MOCK_CONST_METHOD0(NumberOfCpus, size_t());
};

class TestFtraceController : public FtraceController {
 public:
  TestFtraceController(std::unique_ptr<MockFtraceProcfs> ftrace_procfs,
                       base::TaskRunner* runner,
                       std::unique_ptr<Table> table)
      : FtraceController(std::move(ftrace_procfs), runner, std::move(table)) {}

  //  MOCK_METHOD3(CreateCpuReader, CpuReader(const ProtoTranslationTable*,
  //  size_t cpu, const std::string& path));

 private:
  TestFtraceController(const TestFtraceController&) = delete;
  TestFtraceController& operator=(const TestFtraceController&) = delete;
};

}  // namespace

TEST(FtraceControllerTest, NoExistentEventsDontCrash) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config;
  config.AddEvent("not_an_event");

  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);
}

TEST(FtraceControllerTest, OneSink) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config({"foo"});

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/foo/enable", "1"));
  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/foo/enable", "0"));
  sink.reset();
}

TEST(FtraceControllerTest, MultipleSinks) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;

  FtraceConfig configA({"foo"});
  FtraceConfig configB({"foo", "bar"});

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/foo/enable", "1"));
  std::unique_ptr<FtraceSink> sinkA = controller.CreateSink(configA, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/bar/enable", "1"));
  std::unique_ptr<FtraceSink> sinkB = controller.CreateSink(configB, &delegate);

  sinkA.reset();

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/foo/enable", "0"));
  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/bar/enable", "0"));
  sinkB.reset();
}

TEST(FtraceControllerTest, ControllerMayDieFirst) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  std::unique_ptr<TestFtraceController> controller(new TestFtraceController(
      std::move(ftrace_procfs), &task_runner, FakeTable()));

  MockDelegate delegate;
  FtraceConfig config({"foo"});

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/foo/enable", "1"));
  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs,
              WriteToFile("/root/events/group/foo/enable", "0"));
  controller.reset();

  sink.reset();
}

TEST(FtraceControllerTest, StartStop) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // Stopping before we start does nothing.
  controller.Stop();

  EXPECT_CALL(task_runner, AddFileDescriptorWatch(_, _));
  controller.Start();
  // Double start does nothing.
  controller.Start();

  EXPECT_CALL(task_runner, RemoveFileDescriptorWatch(_));
  controller.Stop();
  // Double stop does nothing.
  controller.Stop();
}

}  // namespace perfetto