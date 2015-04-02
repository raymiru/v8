// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/control-reducer.h"
#include "src/compiler/graph-visualizer.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/js-operator.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/node.h"
#include "test/unittests/compiler/graph-unittest.h"
#include "test/unittests/compiler/node-test-utils.h"
#include "testing/gmock-support.h"

using testing::_;
using testing::AllOf;
using testing::Capture;
using testing::CaptureEq;

namespace v8 {
namespace internal {
namespace compiler {

class ControlReducerTest : public TypedGraphTest {
 public:
  ControlReducerTest()
      : TypedGraphTest(1),
        machine_(zone()),
        javascript_(zone()),
        jsgraph_(isolate(), graph(), common(), &javascript_, &machine_) {}

 protected:
  MachineOperatorBuilder machine_;
  JSOperatorBuilder javascript_;
  JSGraph jsgraph_;

  void ReduceGraph() {
    if (FLAG_trace_turbo_graph) {
      OFStream os(stdout);
      os << "-- Graph before control reduction" << std::endl;
      os << AsRPO(*graph());
    }
    ControlReducer::ReduceGraph(zone(), jsgraph(), common());
    if (FLAG_trace_turbo_graph) {
      OFStream os(stdout);
      os << "-- Graph after control reduction" << std::endl;
      os << AsRPO(*graph());
    }
  }

  JSGraph* jsgraph() { return &jsgraph_; }
};


TEST_F(ControlReducerTest, NonTerminatingLoop) {
  Node* loop = graph()->NewNode(common()->Loop(2), graph()->start());
  loop->AppendInput(graph()->zone(), loop);
  ReduceGraph();
  Capture<Node*> branch;
  EXPECT_THAT(
      graph()->end(),
      IsEnd(IsMerge(
          graph()->start(),
          IsReturn(IsUndefinedConstant(), graph()->start(),
                   IsIfFalse(
                       AllOf(CaptureEq(&branch),
                             IsBranch(IsAlways(),
                                      AllOf(loop, IsLoop(graph()->start(),
                                                         IsIfTrue(CaptureEq(
                                                             &branch)))))))))));
}


TEST_F(ControlReducerTest, NonTerminatingLoopWithEffectPhi) {
  Node* loop = graph()->NewNode(common()->Loop(2), graph()->start());
  loop->AppendInput(graph()->zone(), loop);
  Node* ephi = graph()->NewNode(common()->EffectPhi(2), graph()->start());
  ephi->AppendInput(graph()->zone(), ephi);
  ephi->AppendInput(graph()->zone(), loop);
  ReduceGraph();
  Capture<Node*> branch;
  EXPECT_THAT(
      graph()->end(),
      IsEnd(IsMerge(
          graph()->start(),
          IsReturn(IsUndefinedConstant(),
                   AllOf(ephi, IsEffectPhi(graph()->start(), ephi, loop)),
                   IsIfFalse(
                       AllOf(CaptureEq(&branch),
                             IsBranch(IsAlways(),
                                      AllOf(loop, IsLoop(graph()->start(),
                                                         IsIfTrue(CaptureEq(
                                                             &branch)))))))))));
}


TEST_F(ControlReducerTest, NonTerminatingLoopWithTwoEffectPhis) {
  Node* loop = graph()->NewNode(common()->Loop(2), graph()->start());
  loop->AppendInput(graph()->zone(), loop);
  Node* ephi1 = graph()->NewNode(common()->EffectPhi(2), graph()->start());
  ephi1->AppendInput(graph()->zone(), ephi1);
  ephi1->AppendInput(graph()->zone(), loop);
  Node* ephi2 = graph()->NewNode(common()->EffectPhi(2), graph()->start());
  ephi2->AppendInput(graph()->zone(), ephi2);
  ephi2->AppendInput(graph()->zone(), loop);
  ReduceGraph();
  Capture<Node*> branch;
  EXPECT_THAT(
      graph()->end(),
      IsEnd(IsMerge(
          graph()->start(),
          IsReturn(
              IsUndefinedConstant(),
              IsEffectSet(
                  AllOf(ephi1, IsEffectPhi(graph()->start(), ephi1, loop)),
                  AllOf(ephi2, IsEffectPhi(graph()->start(), ephi2, loop))),
              IsIfFalse(AllOf(
                  CaptureEq(&branch),
                  IsBranch(
                      IsAlways(),
                      AllOf(loop, IsLoop(graph()->start(),
                                         IsIfTrue(CaptureEq(&branch)))))))))));
}


TEST_F(ControlReducerTest, NonTerminatingLoopWithDeadEnd) {
  Node* loop = graph()->NewNode(common()->Loop(2), graph()->start());
  loop->AppendInput(graph()->zone(), loop);
  graph()->end()->ReplaceInput(0, graph()->NewNode(common()->Dead()));
  ReduceGraph();
  Capture<Node*> branch;
  EXPECT_THAT(
      graph()->end(),
      IsEnd(IsReturn(
          IsUndefinedConstant(), graph()->start(),
          IsIfFalse(AllOf(
              CaptureEq(&branch),
              IsBranch(IsAlways(),
                       AllOf(loop, IsLoop(graph()->start(),
                                          IsIfTrue(CaptureEq(&branch))))))))));
}


TEST_F(ControlReducerTest, PhiAsInputToBranch_true) {
  Node* p0 = Parameter(0);
  Node* branch1 = graph()->NewNode(common()->Branch(), p0, graph()->start());
  Node* if_true1 = graph()->NewNode(common()->IfTrue(), branch1);
  Node* if_false1 = graph()->NewNode(common()->IfFalse(), branch1);
  Node* merge1 = graph()->NewNode(common()->Merge(2), if_true1, if_false1);
  Node* phi1 = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                jsgraph()->Int32Constant(1),
                                jsgraph()->Int32Constant(2), merge1);

  Node* branch2 = graph()->NewNode(common()->Branch(), phi1, merge1);
  Node* if_true2 = graph()->NewNode(common()->IfTrue(), branch2);
  Node* if_false2 = graph()->NewNode(common()->IfFalse(), branch2);
  Node* merge2 = graph()->NewNode(common()->Merge(2), if_true2, if_false2);
  Node* result = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                  jsgraph()->Int32Constant(11),
                                  jsgraph()->Int32Constant(22), merge2);

  Node* ret =
      graph()->NewNode(common()->Return(), result, graph()->start(), merge2);
  graph()->end()->ReplaceInput(0, ret);

  ReduceGraph();

  // First diamond is not reduced.
  EXPECT_THAT(merge1, IsMerge(IsIfTrue(branch1), IsIfFalse(branch1)));

  // Second diamond should be folded away.
  EXPECT_THAT(graph()->end(),
              IsEnd(IsReturn(IsInt32Constant(11), graph()->start(), merge1)));
}


TEST_F(ControlReducerTest, PhiAsInputToBranch_false) {
  Node* p0 = Parameter(0);
  Node* branch1 = graph()->NewNode(common()->Branch(), p0, graph()->start());
  Node* if_true1 = graph()->NewNode(common()->IfTrue(), branch1);
  Node* if_false1 = graph()->NewNode(common()->IfFalse(), branch1);
  Node* merge1 = graph()->NewNode(common()->Merge(2), if_true1, if_false1);
  Node* phi1 = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                jsgraph()->Int32Constant(0),
                                jsgraph()->BooleanConstant(false), merge1);

  Node* branch2 = graph()->NewNode(common()->Branch(), phi1, merge1);
  Node* if_true2 = graph()->NewNode(common()->IfTrue(), branch2);
  Node* if_false2 = graph()->NewNode(common()->IfFalse(), branch2);
  Node* merge2 = graph()->NewNode(common()->Merge(2), if_true2, if_false2);
  Node* result = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                  jsgraph()->Int32Constant(11),
                                  jsgraph()->Int32Constant(22), merge2);

  Node* ret =
      graph()->NewNode(common()->Return(), result, graph()->start(), merge2);
  graph()->end()->ReplaceInput(0, ret);

  ReduceGraph();

  // First diamond is not reduced.
  EXPECT_THAT(merge1, IsMerge(IsIfTrue(branch1), IsIfFalse(branch1)));

  // Second diamond should be folded away.
  EXPECT_THAT(graph()->end(),
              IsEnd(IsReturn(IsInt32Constant(22), graph()->start(), merge1)));
}


TEST_F(ControlReducerTest, PhiAsInputToBranch_unknown_true) {
  Node* p0 = Parameter(0);
  Node* phi0 = graph()->NewNode(common()->Phi(kMachInt32, 2), p0,
                                jsgraph()->Int32Constant(1), graph()->start());
  Node* branch1 = graph()->NewNode(common()->Branch(), phi0, graph()->start());
  Node* if_true1 = graph()->NewNode(common()->IfTrue(), branch1);
  Node* if_false1 = graph()->NewNode(common()->IfFalse(), branch1);
  Node* merge1 = graph()->NewNode(common()->Merge(2), if_true1, if_false1);
  Node* phi1 = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                jsgraph()->Int32Constant(111),
                                jsgraph()->Int32Constant(222), merge1);

  Node* ret =
      graph()->NewNode(common()->Return(), phi1, graph()->start(), merge1);
  graph()->end()->ReplaceInput(0, ret);

  ReduceGraph();

  // Branch should not be folded.
  EXPECT_THAT(phi1,
              IsPhi(kMachInt32, IsInt32Constant(111), IsInt32Constant(222),
                    IsMerge(IsIfTrue(branch1), IsIfFalse(branch1))));
  EXPECT_THAT(graph()->end(), IsEnd(IsReturn(phi1, graph()->start(), merge1)));
}


TEST_F(ControlReducerTest, RangeAsInputToBranch_true1) {
  Node* p0 = Parameter(Type::Range(1, 2, zone()), 0);
  Node* branch1 = graph()->NewNode(common()->Branch(), p0, graph()->start());
  Node* if_true1 = graph()->NewNode(common()->IfTrue(), branch1);
  Node* if_false1 = graph()->NewNode(common()->IfFalse(), branch1);
  Node* merge1 = graph()->NewNode(common()->Merge(1), if_true1, if_false1);
  Node* result = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                  jsgraph()->Int32Constant(11),
                                  jsgraph()->Int32Constant(44), merge1);

  Node* ret =
      graph()->NewNode(common()->Return(), result, graph()->start(), merge1);
  graph()->end()->ReplaceInput(0, ret);

  ReduceGraph();

  // Diamond should be folded away.
  EXPECT_THAT(
      graph()->end(),
      IsEnd(IsReturn(IsInt32Constant(11), graph()->start(), graph()->start())));
}


TEST_F(ControlReducerTest, RangeAsInputToBranch_true2) {
  Node* p0 = Parameter(Type::Range(-2, -1, zone()), 0);
  Node* branch1 = graph()->NewNode(common()->Branch(), p0, graph()->start());
  Node* if_true1 = graph()->NewNode(common()->IfTrue(), branch1);
  Node* if_false1 = graph()->NewNode(common()->IfFalse(), branch1);
  Node* merge1 = graph()->NewNode(common()->Merge(1), if_true1, if_false1);
  Node* result = graph()->NewNode(common()->Phi(kMachInt32, 2),
                                  jsgraph()->Int32Constant(11),
                                  jsgraph()->Int32Constant(44), merge1);

  Node* ret =
      graph()->NewNode(common()->Return(), result, graph()->start(), merge1);
  graph()->end()->ReplaceInput(0, ret);

  ReduceGraph();

  // Diamond should be folded away.
  EXPECT_THAT(
      graph()->end(),
      IsEnd(IsReturn(IsInt32Constant(11), graph()->start(), graph()->start())));
}


}  // namespace compiler
}  // namespace internal
}  // namespace v8
