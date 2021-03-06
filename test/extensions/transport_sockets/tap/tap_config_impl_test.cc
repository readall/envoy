#include "common/network/address_impl.h"

#include "extensions/transport_sockets/tap/tap_config_impl.h"

#include "test/extensions/common/tap/common.h"
#include "test/mocks/network/mocks.h"
#include "test/test_common/simulated_time_system.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace Tap {
namespace {

namespace TapCommon = Extensions::Common::Tap;

class MockSocketTapConfig : public SocketTapConfig {
public:
  PerSocketTapperPtr createPerSocketTapper(const Network::Connection& connection) override {
    return PerSocketTapperPtr{createPerSocketTapper_(connection)};
  }

  Extensions::Common::Tap::PerTapSinkHandleManagerPtr
  createPerTapSinkHandleManager(uint64_t trace_id) override {
    return Extensions::Common::Tap::PerTapSinkHandleManagerPtr{
        createPerTapSinkHandleManager_(trace_id)};
  }

  MOCK_METHOD1(createPerSocketTapper_, PerSocketTapper*(const Network::Connection& connection));
  MOCK_METHOD1(createPerTapSinkHandleManager_,
               Extensions::Common::Tap::PerTapSinkHandleManager*(uint64_t trace_id));
  MOCK_CONST_METHOD0(maxBufferedRxBytes, uint32_t());
  MOCK_CONST_METHOD0(maxBufferedTxBytes, uint32_t());
  MOCK_CONST_METHOD0(createMatchStatusVector,
                     Extensions::Common::Tap::Matcher::MatchStatusVector());
  MOCK_CONST_METHOD0(rootMatcher, const Extensions::Common::Tap::Matcher&());
  MOCK_CONST_METHOD0(streaming, bool());
  MOCK_CONST_METHOD0(timeSource, TimeSource&());
};

class PerSocketTapperImplTest : public testing::Test {
public:
  void setup(bool streaming) {
    connection_.local_address_ =
        std::make_shared<Network::Address::Ipv4Instance>("127.0.0.1", 1000);
    ON_CALL(connection_, id()).WillByDefault(Return(1));
    EXPECT_CALL(*config_, createPerTapSinkHandleManager_(1)).WillOnce(Return(sink_manager_));
    EXPECT_CALL(*config_, createMatchStatusVector())
        .WillOnce(Return(TapCommon::Matcher::MatchStatusVector(1)));
    EXPECT_CALL(*config_, rootMatcher()).WillRepeatedly(ReturnRef(matcher_));
    EXPECT_CALL(matcher_, onNewStream(_))
        .WillOnce(Invoke([this](TapCommon::Matcher::MatchStatusVector& statuses) {
          statuses_ = &statuses;
          statuses[0].matches_ = true;
          statuses[0].might_change_status_ = false;
        }));
    EXPECT_CALL(*config_, streaming()).WillRepeatedly(Return(streaming));
    EXPECT_CALL(*config_, maxBufferedRxBytes()).WillRepeatedly(Return(1024));
    EXPECT_CALL(*config_, maxBufferedTxBytes()).WillRepeatedly(Return(1024));
    EXPECT_CALL(*config_, timeSource()).WillRepeatedly(ReturnRef(time_system_));
    time_system_.setSystemTime(std::chrono::seconds(0));
    tapper_ = std::make_unique<PerSocketTapperImpl>(config_, connection_);
  }

  std::shared_ptr<MockSocketTapConfig> config_{std::make_shared<MockSocketTapConfig>()};
  // Raw pointer, returned via mock to unique_ptr.
  TapCommon::MockPerTapSinkHandleManager* sink_manager_ =
      new TapCommon::MockPerTapSinkHandleManager;
  std::unique_ptr<PerSocketTapperImpl> tapper_;
  std::vector<TapCommon::MatcherPtr> matchers_{1};
  TapCommon::MockMatcher matcher_{matchers_};
  TapCommon::Matcher::MatchStatusVector* statuses_;
  NiceMock<Network::MockConnection> connection_;
  Event::SimulatedTimeSystem time_system_;
};

// Verify the full streaming flow.
TEST_F(PerSocketTapperImplTest, StreamingFlow) {
  EXPECT_CALL(*sink_manager_, submitTrace_(TraceEqual(
                                  R"EOF(
socket_streamed_trace_segment:
  trace_id: 1
  connection:
    local_address:
      socket_address:
        address: 127.0.0.1
        port_value: 1000
    remote_address:
      socket_address:
        address: 10.0.0.3
        port_value: 50000
)EOF")));
  setup(true);

  InSequence s;

  EXPECT_CALL(*sink_manager_, submitTrace_(TraceEqual(
                                  R"EOF(
socket_streamed_trace_segment:
  trace_id: 1
  event:
    timestamp: 1970-01-01T00:00:00Z
    read:
      data:
        as_bytes: aGVsbG8=
)EOF")));
  tapper_->onRead(Buffer::OwnedImpl("hello"), 5);

  EXPECT_CALL(*sink_manager_, submitTrace_(TraceEqual(
                                  R"EOF(
socket_streamed_trace_segment:
  trace_id: 1
  event:
    timestamp: 1970-01-01T00:00:01Z
    write:
      data:
        as_bytes: d29ybGQ=
      end_stream: true
)EOF")));
  time_system_.setSystemTime(std::chrono::seconds(1));
  tapper_->onWrite(Buffer::OwnedImpl("world"), 5, true);

  EXPECT_CALL(*sink_manager_, submitTrace_(TraceEqual(
                                  R"EOF(
socket_streamed_trace_segment:
  trace_id: 1
  event:
    timestamp: 1970-01-01T00:00:02Z
    closed: {}
)EOF")));
  time_system_.setSystemTime(std::chrono::seconds(2));
  tapper_->closeSocket(Network::ConnectionEvent::RemoteClose);
}

} // namespace
} // namespace Tap
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
