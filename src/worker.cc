/************************************************************
*
* Licensed to the Apache Software Foundation (ASF) under one
* or more contributor license agreements.  See the NOTICE file
* distributed with this work for additional information
* regarding copyright ownership.  The ASF licenses this file
* to you under the Apache License, Version 2.0 (the
* "License"); you may not use this file except in compliance
* with the License.  You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied.  See the License for the
* specific language governing permissions and limitations
* under the License.
*
*************************************************************/

#include "singa/worker.h"

#include <glog/logging.h>
#include <chrono>
#include <thread>
#include <typeinfo>
#include "singa/utils/cluster.h"
#include "singa/utils/factory.h"
#include "singa/utils/singleton.h"

namespace singa {

using std::string;

Worker* Worker::Create(const AlgProto& conf) {
  auto factory = Singleton<Factory<singa::Worker>>::Instance();
  Worker* worker = nullptr;
  if (conf.has_user_alg())
    worker = factory->Create(conf.user_alg());
  else
    worker = factory->Create(conf.alg());
  return worker;
}

void Worker::Setup(int grp_id, int id, const JobProto& conf,
    NeuralNet* train_net, NeuralNet* val_net, NeuralNet* test_net) {
  grp_id_ = grp_id;
  id_ = id;
  job_conf_ = conf;
  train_net_ = train_net;
  val_net_ = val_net;
  test_net_ = test_net;
  layer_dealer_ = dealer_ = nullptr;
}

Worker::~Worker() {
  if (layer_dealer_)
    delete layer_dealer_;
  if (dealer_)
    delete dealer_;
}

void Worker::InitNetParams(const JobProto& job_conf, NeuralNet* net) {
  // for each server grp, its first subscriber worker grp does the param init
  if (grp_id_ % Cluster::Get()->nworker_groups_per_server_group() == 0) {
    // extract params that should be initialized by this worker
    // must gen a name for each param if the user doesn't config it
    std::unordered_map<string, Param*> name2param;
    for (auto layer : net->layers()) {
      if (layer->partition_id() == id_) {
        for (auto param : layer->GetParams()) {
          // only owners fill the memory of parameter values.
          if (param->owner() == param->id()) {
            CHECK(name2param.find(param->name()) == name2param.end());
            name2param[param->name()] = param;
          }
        }
      }
    }
    // load from checkpoints. get param blob based on param name.
    // the param from previous checkpoint files will be overwritten by
    // the param with the same name in later checkpoint files.
    for (const auto path : job_conf.checkpoint_path()) {
      LOG(ERROR) << "Load from checkpoint file " << path;
      BlobProtos bps;
      ReadProtoFromBinaryFile(path.c_str(), &bps);
      for (int i = 0; i < bps.name_size(); i++) {
        if (name2param.find(bps.name(i)) != name2param.end()) {
          name2param.at(bps.name(i))->FromProto(bps.blob(i));
          //  if load from pre-training params, reset version to start step
          if (job_conf.reset_param_version())
            name2param.at(bps.name(i))->set_version(job_conf.step());
          else  // if resume training, use the same version as last checkpoint
            name2param.at(bps.name(i))->set_version(bps.version(i));
        }
      }
    }
    // init other params who do not have checkpoint version
    for (auto entry : name2param)
      if (entry.second->version() < 0) {
        entry.second->InitValues(job_conf.step());
        if (!job_conf.reset_param_version())
          LOG(ERROR) << "better reset version of params from checkpoints "
            << "to the same as other newly initialized params!";
      }

    // warmup training before put params to servers
    for (; step_ < job_conf.warmup_steps(); step_++)
      TrainOneBatch(step_, net);
    for (auto layer : net->layers()) {
      if (layer->partition_id() == id_)
        for (auto param : layer->GetParams())
          if (param->owner() == param->id())
            Put(param->version(), param);
    }
  }
  // wait owners in the same procs init params, then no get requests sent
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  for (auto layer : net->layers()) {
    if (layer->partition_id() == id_)
      for (auto param : layer->GetParams())
        Get(job_conf.warmup_steps(), param);
  }
}

void ConnectStub(int grp, int id, Dealer* dealer, EntityType entity) {
  dealer->Connect(kInprocRouterEndpoint);
  Msg* ping = new Msg(Addr(grp, id, entity), Addr(-1, -1, kStub));
  ping->set_type(kConnect);
  dealer->Send(&ping);
}

void Worker::Run() {
  LOG(ERROR) << "Worker (group = " << grp_id_ <<", id = " << id_ << ") start";
  auto cluster = Cluster::Get();
  int svr_grp = grp_id_ / cluster->nworker_groups_per_server_group();
  CHECK(cluster->runtime()->JoinSGroup(grp_id_, id_, svr_grp));
  // TODO(wangsh): provide a unique sock id from cluster
  dealer_ = new Dealer(0);
  ConnectStub(grp_id_, id_, dealer_, kWorkerParam);
  for (auto layer : train_net_->layers()) {
    if (layer->partition_id() == id_) {
      if (typeid(layer) == typeid(BridgeDstLayer)
          || typeid(layer) == typeid(BridgeSrcLayer)) {
        // TODO(wangsh): provide a unique socket id from cluster
        layer_dealer_ = new Dealer(1);
        ConnectStub(grp_id_, id_, layer_dealer_, kWorkerLayer);
        break;
      }
    }
  }

  step_ = job_conf_.step();
  InitNetParams(job_conf_, train_net_);
  while (!StopNow(step_)) {
    if (ValidateNow(step_) && val_net_ != nullptr) {
      CollectAll(step_, val_net_);
      for (int step = 0; step < job_conf_.validate_steps(); step++)
        TestOneBatch(step, kVal, val_net_);
      Display(kVal, "Validation @ step " + std::to_string(step_), val_net_);
    }
    if (TestNow(step_) && test_net_ != nullptr) {
      CollectAll(step_, test_net_);
      for (int step = 0; step < job_conf_.test_steps(); step++)
        TestOneBatch(step, kTest, test_net_);
      Display(kTest, "Test @ step " + std::to_string(step_), test_net_);
    }
    if (CheckpointNow(step_) && grp_id_ == 0) {
      CollectAll(step_, train_net_);
      Checkpoint(step_, Cluster::Get()->checkpoint_folder(), train_net_);
      job_conf_.set_step(step_);
    }
    TrainOneBatch(step_, train_net_);
    if (DisplayNow(step_) && grp_id_ == 0 && id_ == 0)
      Display(kTrain, "Train @ step " + std::to_string(step_), train_net_);
    step_++;
  }

  // save the model
  if (grp_id_ == 0)
    Checkpoint(step_, Cluster::Get()->checkpoint_folder(), train_net_);
  // clean up
  cluster->runtime()->LeaveSGroup(grp_id_, id_, svr_grp);
  // notify the stub on worker stop
  Msg* msg = new Msg(Addr(grp_id_, id_, kWorkerParam), Addr(-1, -1, kStub));
  msg->set_type(kStop);
  dealer_->Send(&msg);  // use param dealer to send the stop msg
  LOG(ERROR) << "Worker (group = " <<grp_id_ << ", id = " << id_ << ") stops";
}

void Worker::Checkpoint(int step, const std::string& folder, NeuralNet* net) {
  BlobProtos bps;
  for (auto layer : net->layers()) {
    if (layer->partition_id() == id_) {
      for (auto param : layer->GetParams()) {
        // only owners fill the memory of parameter values.
        if (param->owner() == param->id()) {
          auto *blob = bps.add_blob();
          param->ToProto(blob);
          bps.add_version(param->version());
          bps.add_name(param->name());
        }
      }
    }
  }
  char buf[256];
  snprintf(buf, sizeof(buf), "%s/step%d-worker%d", folder.c_str(), step, id_);
  LOG(INFO) << "checkpoint to " << buf;
  WriteProtoToBinaryFile(bps, buf);
}

int Worker::Put(int step, Param* param) {
  if (dealer_ == nullptr) {
    LOG(ERROR) << "Null dealer in worker (" << grp_id_ << ", " << id_ << ")";
    return 1;
  }
  Msg* msg = new Msg(Addr(grp_id_, id_, kWorkerParam), Addr(-1, -1, kStub));
  msg->set_trgt(ParamTrgt(param->owner(), 0), step);
  msg->set_type(kPut);
  dealer_->Send(&msg);
  return 1;
}

int Worker::Get(int step, Param* param) {
  if (param->version() >= step)
    return 1;
  if (dealer_ == nullptr) {
    LOG(ERROR) << "Null dealer in worker (" << grp_id_ << ", " << id_ << ")";
    return 1;
  }
  Msg* msg = new Msg(Addr(grp_id_, id_, kWorkerParam), Addr(-1, -1, kStub));
  msg->set_trgt(ParamTrgt(param->owner(), 0), step);
  msg->set_type(kGet);
  dealer_->Send(&msg);
  return 1;
}

int Worker::Update(int step, Param* param) {
  param->set_local_version(param->version());
  if (dealer_ == nullptr) {
    LOG(ERROR) << "Null dealer in worker (" << grp_id_ << ", " << id_ << ")";
    return 1;
  }
  Msg* msg = new Msg(Addr(grp_id_, id_, kWorkerParam), Addr(-1, -1, kStub));
  msg->set_trgt(ParamTrgt(param->owner(), 0), step);
  msg->set_type(kUpdate);
  dealer_->Send(&msg);
  return 1;
}

int Worker::CollectAll(int step, NeuralNet* net) {
  auto& layers = net->layers();
  for (auto& layer : layers) {
    if (layer->partition_id() == id_) {
      for (Param* p : layer->GetParams()) {
        Collect(step, p);
      }
    }
  }
  return 1;
}

int Worker::Collect(int step, Param* param) {
  while (param->version() <= param->local_version())
    std::this_thread::sleep_for(std::chrono::milliseconds(kCollectSleepTime));
  return 1;
}

void Worker::Display(int flag, const std::string& prefix, NeuralNet* net) {
  for (auto layer : net->layers()) {
    if (layer->partition_id() == id_) {
      const string& disp = layer->ToString(false, flag);
      if (disp.length())
        LOG(ERROR) << prefix << ": " << disp;
      if (job_conf_.debug()) {
        const string& info = layer->ToString(true, flag);
        if (info.length()) {
          LOG(INFO) <<  prefix << info;
        }
      }
    }
  }
}

void Worker::ReceiveBlobs(bool data, bool grad, BridgeLayer* layer,
                          NeuralNet* net) {
  if (layer_dealer_ == nullptr) {
    LOG(ERROR) << "Null dealer in worker (" << grp_id_ << ", " << id_ << ")";
  }
  while (!layer->ready()) {
    auto msg = layer_dealer_->Receive();
    CHECK_EQ(AddrGrp(msg->src()), grp_id_);
    string name(static_cast<char*>(msg->FrameData()), msg->FrameSize());
    auto receive_layer = net->name2layer(name);
    auto data = receive_layer->mutable_data(nullptr);
    msg->NextFrame();
    memcpy(data->mutable_cpu_data(), msg->FrameData(), msg->FrameSize());
    dynamic_cast<BridgeLayer*>(receive_layer)->set_ready(true);
    delete msg;
  }
}

void Worker::SendBlobs(bool data, bool grad, BridgeLayer* layer,
                       NeuralNet* net) {
  if (layer_dealer_ == nullptr) {
    LOG(ERROR) << "Null dealer in worker (" << grp_id_ << ", " << id_ << ")";
  }
  auto dst = net->srclayers(layer).at(0);
  Msg *msg = new Msg();
  msg->set_src(Addr(grp_id_, id_, kWorkerLayer));
  msg->set_dst(Addr(grp_id_, dst->partition_id(), kWorkerLayer));
  msg->AddFrame(dst->name().c_str(), dst->name().length());
  auto const & blob = layer->data(nullptr);
  msg->AddFrame(blob.cpu_data(), blob.count() * sizeof(float));
  layer_dealer_->Send(&msg);
}

/****************************BPWorker**********************************/
void BPWorker::TrainOneBatch(int step, NeuralNet* net) {
  Forward(step, kTrain, net);
  Backward(step, net);
}

void BPWorker::TestOneBatch(int step, Phase phase, NeuralNet* net) {
  Forward(step, phase, net);
}

void BPWorker::Forward(int step, Phase phase, NeuralNet* net) {
  for (auto& layer : net->layers()) {
    if (layer->partition_id() == id_) {
      // TODO(wangwei): enable this for model partition
      // recv data from other workers
      // if (typeid(*layer) == typeid(BridgeDstLayer))
      //   ReceiveBlobs(true, false, dynamic_cast<BridgeLayer*>(layer), net);
      if (phase == kTrain) {
        // wait until param is updated
        for (Param* p : layer->GetParams()) {
          Collect(step, p);
        }
      }
      layer->ComputeFeature(phase | kForward, net->srclayers(layer));
      // TODO(wangwei): enable this for model partition
      // send data to other workers
      // if (typeid(*layer) == typeid(BridgeSrcLayer))
      //   SendBlobs(true, false, dynamic_cast<BridgeLayer*>(layer), net);
    }
  }
}

void BPWorker::Backward(int step, NeuralNet* net) {
  auto& layers = net->layers();
  for (auto it = layers.rbegin(); it != layers.rend(); it++) {
    Layer* layer = *it;
    if (layer->partition_id() == id_) {
      // TODO(wangwei): enable this for model partition
      // send data to other workers
      // if (typeid(layer) == typeid(BridgeSrcLayer))
      //   ReceiveBlobs(false, true, layer, net);
      layer->ComputeGradient(kTrain | kBackward, net->srclayers(layer));
      for (Param* p : layer->GetParams())
        Update(step, p);
      // TODO(wangwei): enable this for model partition
      // recv data from other workers
      // if (typeid(layer) == typeid(BridgeDstLayer))
      //   SendBlobs(false, true, dynamic_cast<BridgeDstLayer*>(layer), net);
    }
  }
}

/****************************CDWorker**********************************/
void CDWorker::TrainOneBatch(int step, NeuralNet* net) {
  const auto& layers = net->layers();
  for (auto* layer : layers) {
    for (Param* p : layer->GetParams())  // wait until param is updated
      Collect(step, p);
    layer->ComputeFeature(kPositive, net->srclayers(layer));
  }
  for (auto* layer : layers)
    if (typeid(*layer) == typeid(RBMVisLayer)
          || typeid(*layer) == typeid(RBMHidLayer))
      layer->ComputeFeature(kNegative | kTest, net->srclayers(layer));
  for (int i = 1; i < job_conf_.train_one_batch().cd_conf().cd_k(); i++) {
    for (auto* layer : layers) {
      if (typeid(*layer) == typeid(RBMVisLayer)
          || typeid(*layer) == typeid(RBMHidLayer))
      layer->ComputeFeature(kNegative, net->srclayers(layer));
    }
  }
  for (auto* layer : layers) {
    if (typeid(*layer) == typeid(RBMVisLayer)
        || typeid(*layer) == typeid(RBMHidLayer)) {
      layer->ComputeGradient(kTrain, net->srclayers(layer));
      for (Param* p : layer->GetParams()) {
        Update(step, p);
      }
    }
  }
}

void CDWorker::TestOneBatch(int step, Phase phase, NeuralNet* net) {
  auto& layers = net->layers();
  for (auto *layer : layers)
    layer->ComputeFeature(kPositive, net->srclayers(layer));
  for (auto *layer : layers)
    if (typeid(*layer) == typeid(RBMVisLayer))
      layer->ComputeFeature(kNegative | kTest, net->srclayers(layer));
}

}  // namespace singa
