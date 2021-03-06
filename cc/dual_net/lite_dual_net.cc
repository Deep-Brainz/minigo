// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cc/dual_net/lite_dual_net.h"

#include <sys/sysinfo.h>
#include <fstream>
#include <iostream>

#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cc/check.h"
#include "cc/constants.h"
#include "tensorflow/contrib/lite/context.h"
#include "tensorflow/contrib/lite/interpreter.h"
#include "tensorflow/contrib/lite/kernels/register.h"
#include "tensorflow/contrib/lite/model.h"

using tflite::FlatBufferModel;
using tflite::InterpreterBuilder;
using tflite::ops::builtin::BuiltinOpResolver;

namespace minigo {
namespace {

class LiteDualNet : public DualNet {
 public:
  explicit LiteDualNet(std::string graph_path);

  void RunMany(std::vector<const BoardFeatures*> features,
               std::vector<Output*> outputs, std::string* model) override;

 private:
  template <typename T>
  void RunMany(std::vector<const BoardFeatures*> features,
               std::vector<Output*> outputs, T* feature_data,
               const T* policy_data, const T* value_data);

  std::unique_ptr<tflite::FlatBufferModel> model_;
  std::unique_ptr<tflite::Interpreter> interpreter_;

  TfLiteTensor* input_ = nullptr;
  TfLiteTensor* policy_ = nullptr;
  TfLiteTensor* value_ = nullptr;

  std::string graph_path_;
};

minigo::LiteDualNet::LiteDualNet(std::string graph_path)
    : graph_path_(graph_path) {
  if (!std::ifstream(graph_path).good()) {
    graph_path = absl::StrCat(graph_path, ".tflite");
  }

  model_ = FlatBufferModel::BuildFromFile(graph_path.c_str());
  MG_CHECK(model_ != nullptr);

  BuiltinOpResolver resolver;
  InterpreterBuilder(*model_, resolver)(&interpreter_);
  MG_CHECK(interpreter_ != nullptr);

  // Let's just use all the processors we can.
  interpreter_->SetNumThreads(get_nprocs());

  // Initialize input.
  const auto& inputs = interpreter_->inputs();
  MG_CHECK(inputs.size() == 1);
  absl::string_view input_name = interpreter_->GetInputName(0);
  MG_CHECK(input_name == "pos_tensor");

  input_ = interpreter_->tensor(inputs[0]);
  MG_CHECK(input_ != nullptr);
  MG_CHECK(input_->data.raw != nullptr);
  MG_CHECK(input_->dims->size == 4);
  MG_CHECK(input_->dims->data[1] == kN);
  MG_CHECK(input_->dims->data[2] == kN);
  MG_CHECK(input_->dims->data[3] == kNumStoneFeatures);

  // Initialize outputs.
  const auto& outputs = interpreter_->outputs();
  MG_CHECK(outputs.size() == 2);
  absl::string_view output_0_name = interpreter_->GetOutputName(0);
  absl::string_view output_1_name = interpreter_->GetOutputName(1);
  if (output_0_name == "policy_output") {
    MG_CHECK(output_1_name == "value_output") << output_1_name;
    policy_ = interpreter_->tensor(outputs[0]);
    value_ = interpreter_->tensor(outputs[1]);
  } else {
    MG_CHECK(output_1_name == "policy_output") << output_1_name;
    MG_CHECK(output_0_name == "value_output") << output_0_name;
    policy_ = interpreter_->tensor(outputs[1]);
    value_ = interpreter_->tensor(outputs[0]);
  }

  MG_CHECK(policy_ != nullptr);
  MG_CHECK(policy_->type == input_->type);
  MG_CHECK(policy_->data.raw != nullptr);
  MG_CHECK(policy_->dims->size == 2);
  MG_CHECK(policy_->dims->data[0] == input_->dims->data[0]);
  MG_CHECK(policy_->dims->data[1] == kNumMoves);

  MG_CHECK(value_ != nullptr);
  MG_CHECK(value_->type == input_->type);
  MG_CHECK(value_->data.raw != nullptr);
  MG_CHECK(value_->dims->size == 1);
  MG_CHECK(value_->dims->data[0] == input_->dims->data[0]);

  MG_CHECK(interpreter_->AllocateTensors() == kTfLiteOk);
}

void minigo::LiteDualNet::RunMany(
    std::vector<const DualNet::BoardFeatures*> features,
    std::vector<DualNet::Output*> outputs, std::string* model) {
  if (model != nullptr) {
    *model = graph_path_;
  }

  MG_CHECK(features.size() <= static_cast<size_t>(input_->dims->data[0]));

  switch (input_->type) {
    case kTfLiteFloat32:
      return RunMany(features, outputs, input_->data.f, policy_->data.f,
                     value_->data.f);
    case kTfLiteUInt8:
      return RunMany(features, outputs, input_->data.uint8, policy_->data.uint8,
                     value_->data.uint8);
    default:
      MG_FATAL() << "Unsupported input type";
  }
}

template <typename T, typename S>
T Convert(const TfLiteQuantizationParams&, const S& s) {
  return static_cast<T>(s);
}

// Dequantize.
template <>
float Convert<float, uint8_t>(const TfLiteQuantizationParams& params,
                              const uint8_t& u) {
  return (u - params.zero_point) * params.scale;
};

// Quantize.
template <>
uint8_t Convert<uint8_t, float>(const TfLiteQuantizationParams& params,
                                const float& f) {
  return static_cast<uint8_t>(f / params.scale + params.zero_point);
};

template <typename T>
void minigo::LiteDualNet::RunMany(std::vector<const BoardFeatures*> features,
                                  std::vector<Output*> outputs, T* feature_data,
                                  const T* policy_data, const T* value_data) {
  int batch_size = static_cast<int>(features.size());

  // Allow a smaller batch size than we run inference on because the first
  // inference made when starting the game has batch size 1 (instead of the
  // normal 8) to initialized the tree search.
  MG_CHECK(batch_size <= input_->dims->data[0]);

  // TODO(tommadams): Make BoardFeatures a uint8_t array and memcpy here.
  const auto input_params = input_->params;
  for (size_t j = 0; j < features.size(); ++j) {
    const auto& board = *features[j];
    for (size_t i = 0; i < board.size(); ++i) {
      // TODO(csigg): Apply dequantization parameters?
      feature_data[j * kNumStoneFeatures + i] =
          Convert<T>(input_params, board[i]);
    }
  }

  MG_CHECK(interpreter_->Invoke() == kTfLiteOk);

  const auto& policy_params = policy_->params;
  const auto& value_params = value_->params;
  for (int j = 0; j < batch_size; ++j) {
    for (int i = 0; i < kNumMoves; ++i) {
      outputs[j]->policy[i] =
          Convert<float>(policy_params, policy_data[j * batch_size + i]);
    }
    outputs[j]->value = Convert<float>(value_params, value_data[j]);
  }
}
}  // namespace

std::unique_ptr<DualNet> NewLiteDualNet(const std::string& model_path) {
  return absl::make_unique<LiteDualNet>(model_path);
}
}  // namespace minigo
