#include <gmock/gmock.h>
#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include "src/common/base/test_utils.h"
#include "src/shared/metadata/metadata_state.h"

namespace pl {
namespace md {

using ::testing::UnorderedElementsAre;

constexpr char kPod0UpdateTxt[] = R"(
  uid: "pod0"
  name: "pod0"
  namespace: "ns0"
  start_timestamp_ns: 101
  stop_timestamp_ns: 103
  container_ids: "container0"
  container_ids: "container1"
  container_names: "container0"
  container_names: "container1"
  qos_class: QOS_CLASS_GUARANTEED
  phase: RUNNING
  conditions: {
    type: READY
    status: STATUS_TRUE
  }
  node_name: "a_node"
  hostname: "a_host"
  pod_ip: "1.2.3.4"
  host_ip: "5.6.7.8"
  message: "a pod message"
  reason: "a pod reason"
)";

constexpr char kContainer0UpdateTxt[] = R"(
  cid: "container0"
  name: "container0"
  namespace: "ns0"
  start_timestamp_ns: 100
  stop_timestamp_ns: 102
  pod_id: "pod0"
  pod_name: "pod0"
  container_state: CONTAINER_STATE_RUNNING
  message: "a container message"
  reason: "a container reason"
)";

constexpr char kRunningServiceUpdatePbTxt[] = R"(
  uid: "3_uid"
  name: "running_service"
  namespace: "ns0"
  start_timestamp_ns: 7
  stop_timestamp_ns: 8
  pod_ids: "pod0"
  pod_ids: "pod1"
  pod_names: "pod0"
  pod_names: "pod1"
)";

constexpr char kRunningNamespaceUpdatePbTxt[] = R"(
  uid: "4_uid"
  name: "ns0"
  start_timestamp_ns: 7
  stop_timestamp_ns: 8
)";

TEST(K8sMetadataStateTest, CloneCopiedCIDR) {
  K8sMetadataState state;

  CIDRBlock pod_cidr0;
  CIDRBlock pod_cidr1;
  ASSERT_OK(ParseCIDRBlock("1.2.3.4/10", &pod_cidr0));
  ASSERT_OK(ParseCIDRBlock("16.17.18.19/10", &pod_cidr1));
  std::vector<CIDRBlock> pod_cidrs = {pod_cidr0, pod_cidr1};
  state.set_pod_cidrs(pod_cidrs);

  CIDRBlock service_cidr;
  ASSERT_OK(ParseCIDRBlock("10.64.0.0/16", &service_cidr));
  state.set_service_cidr(service_cidr);

  auto state_copy = state.Clone();

  auto& state_copy_pod_cidrs = state_copy->pod_cidrs();
  ASSERT_EQ(state_copy_pod_cidrs.size(), 2);
  EXPECT_EQ(pod_cidrs[0].ip_addr.AddrStr(), state_copy_pod_cidrs[0].ip_addr.AddrStr());
  EXPECT_EQ(pod_cidrs[0].prefix_length, state_copy_pod_cidrs[0].prefix_length);
  EXPECT_EQ(pod_cidrs[1].ip_addr.AddrStr(), state_copy_pod_cidrs[1].ip_addr.AddrStr());
  EXPECT_EQ(pod_cidrs[1].prefix_length, state_copy_pod_cidrs[1].prefix_length);

  ASSERT_TRUE(state_copy->service_cidr().has_value());
  EXPECT_EQ(service_cidr.ip_addr.AddrStr(), state_copy->service_cidr()->ip_addr.AddrStr());
  EXPECT_EQ(service_cidr.prefix_length, state_copy->service_cidr()->prefix_length);
}

TEST(K8sMetadataStateTest, HandleContainerUpdate) {
  K8sMetadataState state;

  K8sMetadataState::ContainerUpdate update;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kContainer0UpdateTxt, &update))
      << "Failed to parse proto";

  EXPECT_OK(state.HandleContainerUpdate(update));
  auto info = state.ContainerInfoByID("container0");
  EXPECT_NE(nullptr, info);
  EXPECT_EQ("container0", info->cid());
  EXPECT_EQ("container0", info->name());
  // Shouldn't be set until the pod update.
  EXPECT_EQ("", info->pod_id());
  EXPECT_EQ(100, info->start_time_ns());
  EXPECT_EQ(102, info->stop_time_ns());
  EXPECT_EQ(ContainerState::kRunning, info->state());
  EXPECT_EQ("a container message", info->state_message());
  EXPECT_EQ("a container reason", info->state_reason());
}

TEST(K8sMetadataStateTest, HandlePodUpdate) {
  // 1 missing ContainerUpdate (should be skipped).
  // 1 present ContainerUpdate (should be handled before PodUpdate).
  K8sMetadataState state;

  K8sMetadataState::ContainerUpdate container_update;
  ASSERT_TRUE(
      google::protobuf::TextFormat::MergeFromString(kContainer0UpdateTxt, &container_update))
      << "Failed to parse proto";

  K8sMetadataState::PodUpdate pod_update;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kPod0UpdateTxt, &pod_update))
      << "Failed to parse proto";

  // We expect container updates will be run before the pod update.
  EXPECT_OK(state.HandleContainerUpdate(container_update));

  auto container_info = state.ContainerInfoByID("container0");
  EXPECT_NE(nullptr, container_info);
  EXPECT_EQ("", container_info->pod_id());

  EXPECT_OK(state.HandlePodUpdate(pod_update));

  auto pod_info = state.PodInfoByID("pod0");
  EXPECT_NE(nullptr, pod_info);
  EXPECT_EQ("pod0", pod_info->uid());
  EXPECT_EQ("pod0", pod_info->name());
  EXPECT_EQ("ns0", pod_info->ns());
  EXPECT_EQ(PodQOSClass::kGuaranteed, pod_info->qos_class());
  EXPECT_EQ(PodPhase::kRunning, pod_info->phase());
  EXPECT_EQ(1, pod_info->conditions().size());
  EXPECT_EQ(PodConditionStatus::kTrue, pod_info->conditions()[PodConditionType::kReady]);
  EXPECT_EQ(PodQOSClass::kGuaranteed, pod_info->qos_class());
  EXPECT_EQ(101, pod_info->start_time_ns());
  EXPECT_EQ(103, pod_info->stop_time_ns());
  EXPECT_EQ("a pod message", pod_info->phase_message());
  EXPECT_EQ("a pod reason", pod_info->phase_reason());
  EXPECT_EQ("a_node", pod_info->node_name());
  EXPECT_EQ("a_host", pod_info->hostname());
  EXPECT_EQ("1.2.3.4", pod_info->pod_ip());
  EXPECT_THAT(pod_info->containers(), UnorderedElementsAre("container0"));

  // Check that the container info pod ID got set.
  EXPECT_EQ("pod0", container_info->pod_id());
}

TEST(K8sMetadataStateTest, HandleServiceUpdate) {
  // 1 missing PodUpdate (should be skipped).
  // 1 present PodUpdate (should be handled before ServiceUpdate).
  K8sMetadataState state;

  K8sMetadataState::PodUpdate pod_update;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kPod0UpdateTxt, &pod_update))
      << "Failed to parse proto";

  K8sMetadataState::ServiceUpdate service_update;
  ASSERT_TRUE(
      google::protobuf::TextFormat::MergeFromString(kRunningServiceUpdatePbTxt, &service_update))
      << "Failed to parse proto";

  EXPECT_OK(state.HandlePodUpdate(pod_update));

  auto pod_info = state.PodInfoByID("pod0");
  EXPECT_NE(nullptr, pod_info);
  EXPECT_EQ(0, pod_info->services().size());

  EXPECT_OK(state.HandleServiceUpdate(service_update));

  auto service_info = state.ServiceInfoByID("3_uid");
  EXPECT_NE(nullptr, service_info);
  EXPECT_EQ("3_uid", service_info->uid());
  EXPECT_EQ("running_service", service_info->name());
  EXPECT_EQ("ns0", service_info->ns());
  EXPECT_EQ(7, service_info->start_time_ns());
  EXPECT_EQ(8, service_info->stop_time_ns());

  // Check that the pod info service got set.
  EXPECT_THAT(pod_info->services(), UnorderedElementsAre("3_uid"));
}

TEST(K8sMetadataStateTest, HandleNamespaceUpdate) {
  K8sMetadataState state;

  K8sMetadataState::NamespaceUpdate update;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kRunningNamespaceUpdatePbTxt, &update))
      << "Failed to parse proto";

  EXPECT_OK(state.HandleNamespaceUpdate(update));
  auto info = state.NamespaceInfoByID("4_uid");
  EXPECT_NE(nullptr, info);
  EXPECT_EQ("4_uid", info->uid());
  EXPECT_EQ("ns0", info->name());
  EXPECT_EQ(7, info->start_time_ns());
  EXPECT_EQ(8, info->stop_time_ns());
}

}  // namespace md
}  // namespace pl
