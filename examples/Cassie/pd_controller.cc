#include "pd_controller.h"
#include "datatypes/cassie_names.h"
#include "cassie_controller_lcm.h"

namespace drake{

using systems::BasicVector;
using systems::Context;
using Eigen::VectorXd;

CassiePDController::CassiePDController() {
  num_joints_ = cassieJointNames.size();
  state_input_port_ = this->DeclareInputPort(systems::kVectorValued, 2*num_joints_).get_index();
  config_input_port_ = this->DeclareVectorInputPort(CassiePDConfig(num_joints_)).get_index();

  this->DeclareVectorOutputPort(BasicVector<double>(num_joints_), &CassiePDController::CalcControl);

  q_des_ = VectorXd::Zero(num_joints_);
  v_des_ = VectorXd::Zero(num_joints_);
  kp_ = VectorXd::Zero(num_joints_);
  kd_ = VectorXd::Zero(num_joints_);
}

void CassiePDController::CalcControl(const Context<double>& context, BasicVector<double>* output) const {
    const Eigen::VectorBlock<const VectorXd> state =
      this->EvalEigenVectorInput(context, state_input_port_);

    const CassiePDConfig* config =
      dynamic_cast<const CassiePDConfig*>(this->EvalVectorInput(context, config_input_port_));

    VectorXd u(num_joints_);

    for (int i = 0; i < num_joints_; i++) {
      u(i) = config->getKp(i)*(config->getDesiredPosition(i) - state(i)) +
             config->getKd(i)*(config->getDesiredVelocity(i) - state(i + num_joints_));
    }

    output->SetFromVector(u);
}


}